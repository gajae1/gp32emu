/* SWI #0x1FF: the SDK CRT trampoline against the firmware's selector-0xFF
 * service, on real CPU/hardware and both execution backends.
 *
 * Retail firmware v1.6.6 dispatches the high SWI range with imm24 & 0xff as
 * the selector (ROM 0x1004 -> 0x2888 -> 0x69d0), so 0x1FF reaches the routine
 * at ROM 0x6b2c.  That routine resets the machine (stacks from the table at
 * 0x1058, clock from 0x1090, default 8-bpp LCD plus rebuilt palette, the three
 * launcher fields at 0x0C7B0C00/0x0C7B0D00/0x0C7B0E00) and then continues at
 * the address the caller published in FIQ-mode r12, with IRQ and FIQ disabled
 * in supervisor mode.  All 28 commercial payloads contain the same caller
 * trampoline ("svc #0x1ff" at 0x0C000190), whose whole purpose is to publish
 * its continuation through r12_fiq before the SWI.
 *
 * Direct-FXE HLE must implement that service: declining it sends the CPU into
 * the zero-filled direct-mode vector page. */
#include "../src/gp32.c"

static int failures;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s (%d)\n", m, __LINE__); ++failures; } } while (0)

static const uint32_t tramp = GP32_RAM_BASE + 0x2000u;
static const uint32_t marker = GP32_RAM_BASE + 0x4000u;
static const uint32_t caller = GP32_RAM_BASE + 0x20000u;

static void code(gp32_t *g, uint32_t addr, const uint32_t *words, size_t n) {
    for (size_t i = 0; i < n; ++i) s3c2400_write32(g->soc, addr + (uint32_t)i * 4u, words[i]);
}

/* The trampoline is the exact idiom from the commercial payloads: mask the
 * requested CPSR into r0, derive the FIQ/SVC copies, enter FIQ mode only to
 * publish the continuation in r12_fiq, return to supervisor mode, svc #0x1ff.
 * The continuation stores a marker, then returns to the caller's address. */
static gp32_t *fixture(int jit, int publish_continuation) {
    gp32_t *g = gp32_create(NULL);
    CHECK(g != NULL, "create SWI 0x1ff core");
    if (!g) return NULL;
    direct_set_fxe_mode(g, 1u);
    direct_install_stubs(g);
    gp32_set_jit(g, jit);
    /* Deterministic interrupt state: nothing may preempt the trampoline. */
    s3c2400_write32(g->soc, 0x14400008u, 0xffffffffu);
    s3c2400_write32(g->soc, 0x1440000cu, 0u);
    const uint32_t words[] = {
        0xe3c0001fu, /* bic r0, r0, #0x1f */
        0xe3801011u, /* orr r1, r0, #0x11 */
        0xe3802013u, /* orr r2, r0, #0x13 */
        0xe12ff001u, /* msr cpsr_fsxc, r1 */
        publish_continuation ? 0xe3a0c004u : 0xe3a0c000u, /* mov ip, #4 | mov ip, #0 */
        publish_continuation ? 0xe08cc00fu : 0xe1a00000u, /* add ip, ip, pc | nop */
        0xe12ff002u, /* msr cpsr_fsxc, r2 */
        0xef0001ffu, /* svc #0x1ff */
        0xe3a0004du, /* mov r0, #77 */
        0xe5840000u, /* str r0, [r4] */
        0xe1a0f00eu, /* mov pc, lr */
    };
    code(g, tramp, words, GP32_ARRAY_COUNT(words));
    s3c2400_write32(g->soc, marker, 0u);
    s3c2400_write32(g->soc, caller, 0xeafffffeu);
    /* Supervisor mode with N and V set and IRQ/FIQ open; r0 carries the CPSR
       the CRT asks the firmware to install. */
    arm920t_set_cpsr(g->cpu, 0x90000013u);
    arm920t_set_reg(g->cpu, 0u, 0x90000013u);
    arm920t_set_reg(g->cpu, 4u, marker);
    arm920t_set_reg(g->cpu, 13u, GP32_RAM_BASE + 0x1f000u);
    arm920t_set_reg(g->cpu, 14u, caller);
    arm920t_set_reg(g->cpu, 15u, tramp);
    return g;
}

static void dirty_display_state(gp32_t *g) {
    /* A 16-bpp surface with a foreign palette and stale launcher fields, the
       state a game would be in when it asks the firmware to reinitialise. */
    g->direct_fxe_fb_addr = GP32_RAM_BASE + 0x40000u;
    g->direct_fxe_bpp = 16u;
    g->direct_fxe_lcd_enabled = 0u;
    s3c2400_write32(g->soc, 0x14a00000u, 0u);
    s3c2400_write32(g->soc, 0x14a00010u, 0u);
    s3c2400_write32(g->soc, 0x0c7b0c00u, 0xdeadbeefu);
    s3c2400_write32(g->soc, 0x0c7b0d00u, 0x1234u);
    s3c2400_write32(g->soc, 0x0c7b0e00u, 0x5678u);
}

static void check_reinit_service(int jit) {
    gp32_t *g = fixture(jit, 1);
    if (!g) return;
    dirty_display_state(g);

    int rc = GP32_OK;
    for (unsigned i = 0; i < 64u && !s3c2400_debug_read32(g->soc, marker); ++i)
        rc = gp32_run_cycles(g, 8u);
    CHECK(rc == GP32_OK, "service completes inside the run budget");
    CHECK(s3c2400_debug_read32(g->soc, marker) == 77u, "continuation published through r12_fiq runs");

    arm920t_register_context_t ctx;
    arm920t_get_register_context(g->cpu, &ctx);
    CHECK((gp32_get_cpsr(g) & 0x1fu) == 0x13u, "supervisor mode after the service");
    CHECK((gp32_get_cpsr(g) & 0xc0u) == 0xc0u, "IRQ and FIQ disabled after the service");
    CHECK((gp32_get_cpsr(g) & 0xf0000000u) == 0x90000000u, "NZCV preserved from the requested CPSR");
    CHECK(ctx.spsr_svc == 0x90000013u, "pre-service CPSR left in SPSR_svc");
    CHECK(g->direct_fxe_bpp == 8u && g->direct_fxe_lcd_enabled == 1u, "display reset to 8 bpp and enabled");
    CHECK(g->direct_fxe_fb_addr == direct_default_surface_addr(0u), "default LCD surface restored");
    /* LCDCON1 read-back carries the live line count in bits 27:18. */
    CHECK((s3c2400_debug_read32(g->soc, 0x14a00000u) & 0x3ffffu) == 0x17u, "LCDCON1 shows enabled 8-bpp mode");
    CHECK(s3c2400_debug_read32(g->soc, 0x14a00010u) == 2u, "8-bpp TFT LCDCON5 restored");
    CHECK(g->direct_fxe_palette_initialized == 1u, "standard palette installed");
    CHECK(s3c2400_debug_read32(g->soc, direct_palette_sw_addr(g) + 255u * 4u) != 0u, "palette entries are non-zero");
    CHECK(s3c2400_debug_read32(g->soc, 0x0c7b0c00u) == 0u && s3c2400_debug_read32(g->soc, 0x0c7b0d00u) == 0u &&
          s3c2400_debug_read32(g->soc, 0x0c7b0e00u) == 0u, "launcher fields cleared");

    /* The continuation ran to its own caller: nothing was pushed onto a stack
       the emulator does not own, and the guest's banked stacks are intact. */
    for (unsigned i = 0; i < 16u && gp32_get_pc(g) != caller; ++i)
        CHECK(gp32_run_cycles(g, 4u) == GP32_OK, "continuation returns to its caller");
    CHECK(gp32_get_pc(g) == caller, "continuation returned through lr");
    CHECK(gp32_get_cpu_reg(g, 13u) == GP32_RAM_BASE + 0x1f000u, "guest stack pointer untouched");
    printf("swi-1ff jit=%d pc=%08x cpsr=%08x spsr=%08x\n", jit, gp32_get_pc(g), gp32_get_cpsr(g), ctx.spsr_svc);

    /* The same trampoline without a published continuation must fall back to
       ordinary SWI semantics, so it cannot silently rewrite machine state. */
    gp32_destroy(g);
}

static void check_declined_when_no_continuation(int jit) {
    gp32_t *g = fixture(jit, 0);
    if (!g) return;
    dirty_display_state(g);
    for (unsigned i = 0; i < 32u; ++i)
        CHECK(gp32_run_cycles(g, 4u) == GP32_OK, "declined service keeps running");
    CHECK(s3c2400_debug_read32(g->soc, marker) == 0u, "declined service does not reach the continuation");
    CHECK(g->direct_fxe_bpp == 16u && g->direct_fxe_lcd_enabled == 0u, "declined service leaves display state alone");
    CHECK(s3c2400_debug_read32(g->soc, 0x0c7b0c00u) == 0xdeadbeefu, "declined service leaves launcher fields alone");
    CHECK((gp32_get_cpsr(g) & 0x1fu) == 0x13u && (gp32_get_cpsr(g) & 0x80u) != 0u,
          "ordinary SWI exception still enters supervisor mode with IRQ masked");
    printf("swi-1ff-declined jit=%d pc=%08x cpsr=%08x\n", jit, gp32_get_pc(g), gp32_get_cpsr(g));
    gp32_destroy(g);
}

int main(void) {
    for (int jit = 0; jit < 2; ++jit) {
        check_reinit_service(jit);
        check_declined_when_no_continuation(jit);
    }
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("gp32_swi_1ff: all checks passed\n");
    return 0;
}
