/* Firmware services of direct-FXE HLE on real CPU/hardware and both execution
 * backends: SWI #0x1FF (the SDK CRT trampoline against the selector-0xFF
 * service), SWI #0x0D plus the clock tree a direct load starts from (the
 * retail BIOS clock service, ROM 0x200c) and SWI #0x0F selector 1 (the
 * application path query).
 *
 * SWI #0x1FF:
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
    direct_apply_fw_clock(g, 0x47022u, 1u); /* a title's own 59.25 MHz clock */
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
    /* The retail firmware's 8-bpp TFT words. LCDCON1 read-back carries the live
       line count in bits 27:18 and LCDCON5 the live VSTATUS/HSTATUS in 20:17. */
    CHECK(gp32_get_fclk_hz(g) == 67800000u && s3c2400_hclk_hz(g->soc) == 33900000u,
          "clock back at the default table of ROM 0x1090");
    CHECK((s3c2400_debug_read32(g->soc, 0x14a00000u) & 0x3ffffu) == 0x377u, "LCDCON1 shows enabled 8-bpp TFT mode");
    CHECK((s3c2400_debug_read32(g->soc, 0x14a00010u) & ~(0xfu << 17)) == 0x702u, "8-bpp TFT LCDCON5 restored");
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
    CHECK(gp32_get_fclk_hz(g) == 59250000u, "declined service leaves the clock alone");
    CHECK(s3c2400_debug_read32(g->soc, 0x0c7b0c00u) == 0xdeadbeefu, "declined service leaves launcher fields alone");
    CHECK((gp32_get_cpsr(g) & 0x1fu) == 0x13u && (gp32_get_cpsr(g) & 0x80u) != 0u,
          "ordinary SWI exception still enters supervisor mode with IRQ masked");
    printf("swi-1ff-declined jit=%d pc=%08x cpsr=%08x\n", jit, gp32_get_pc(g), gp32_get_cpsr(g));
    gp32_destroy(g);
}

/* Direct loads start from the clock tree the retail BIOS leaves at the first
 * instruction of a title (FCLK 67.8 MHz, HCLK 33.9 MHz, PCLK 16.95 MHz, REFRESH
 * counter 0x5fd as read back in BIOS boots), and SWI #0x0d is the clock service
 * the SDK calls to leave it. r0 points at {FCLK Hz, MPLLCON, CLKDIVN}; the ROM
 * epilogue (0x1250) hands every register back unchanged. */
static gp32_t *load_direct(const uint32_t *words, unsigned count, int jit) {
    uint8_t image[16];
    for (unsigned i = 0; i < count; ++i) gp32_st32le(image + i * 4u, words[i]);
    gp32_t *g = gp32_create(NULL);
    CHECK(g != NULL, "create direct clock core");
    if (!g) return NULL;
    gp32_set_jit(g, jit);
    fxe_image_t img = {0};
    img.payload = image;
    img.payload_size = count * 4u;
    img.load_addr = img.entry_addr = GP32_RAM_BASE + 0x40000u;
    CHECK(gp32_load_fxe_image_internal(g, &img, 0, 0, 0, 0) == GP32_OK, "direct load");
    return g;
}

static uint64_t iis_rate_after(gp32_t *g) {
    uint32_t rate = 0;
    uint64_t frames = 0;
    s3c2400_write32(g->soc, 0x15508010u, 0x00010001u);
    (void)s3c2400_audio_samples(g->soc, &frames, &rate);
    return frames ? rate : 0u;
}

static void check_handoff_clock(int jit) {
    static const uint32_t park[] = {0xeafffffeu};
    gp32_t *g = load_direct(park, 1u, jit);
    if (!g) return;
    CHECK(gp32_get_fclk_hz(g) == 67800000u && s3c2400_hclk_hz(g->soc) == 33900000u &&
          s3c2400_pclk_hz(g->soc) == 16950000u && gp32_get_run_clock_hz(g) == 33900000u,
          "direct load starts at FCLK 67.8 / HCLK 33.9 / PCLK 16.95 MHz");
    CHECK((s3c2400_debug_read32(g->soc, 0x14000024u) & 0x7ffu) == 0x5fdu, "REFRESH counter of the 33.9 MHz HCLK");
    CHECK((s3c2400_debug_read32(g->soc, 0x14a00000u) & 0x3ffffu) == 0x377u, "LCD divider 3 of the 33.9 MHz HCLK");
    direct_sync_fwinfo(g);
    CHECK(s3c2400_debug_read32(g->soc, direct_fwinfo_addr(g) + 0u) == 67800000u &&
          s3c2400_debug_read32(g->soc, direct_fwinfo_addr(g) + 4u) == 33900000u &&
          s3c2400_debug_read32(g->soc, direct_fwinfo_addr(g) + 8u) == 16950000u,
          "SWI 0x0b selector 4 block holds FCLK, HCLK and PCLK");
    /* The IIS prescaler the BIOS leaves (0xa5, 256fs) gives 11,035 Hz at that
       PCLK; a title's own IISPSR 0x42 / IISMOD 0x99 gives 22,070 Hz, where the
       former 48 MHz direct clock gave 62.5 kHz. */
    CHECK(iis_rate_after(g) == 11035u, "handoff IIS prescaler yields the BIOS rate");
    gp32_destroy(g);
    g = load_direct(park, 1u, jit);
    if (!g) return;
    s3c2400_write32(g->soc, 0x15508008u, 0x42u);
    s3c2400_write32(g->soc, 0x15508004u, 0x99u);
    CHECK(iis_rate_after(g) == 22070u, "title-programmed IIS prescaler follows PCLK");
    gp32_destroy(g);
}

static void check_clock_service(int jit) {
    static const uint32_t words[] = {
        0xef00000du, /* svc #0x0d */
        0xe3a04007u, /* mov r4, #7 */
        0xeafffffeu, /* b . */
    };
    gp32_t *g = load_direct(words, GP32_ARRAY_COUNT(words), jit);
    if (!g) return;
    const uint32_t params = GP32_RAM_BASE + 0x1f000u;
    s3c2400_write32(g->soc, params, 59250000u);
    s3c2400_write32(g->soc, params + 4u, 0x47022u);
    s3c2400_write32(g->soc, params + 8u, 1u);
    arm920t_set_reg(g->cpu, 0u, params);
    arm920t_set_reg(g->cpu, 1u, 0x12345678u);
    for (unsigned i = 0; i < 8u && gp32_get_cpu_reg(g, 4u) != 7u; ++i)
        CHECK(gp32_run_cycles(g, 8u) == GP32_OK, "clock service runs");
    CHECK(gp32_get_cpu_reg(g, 4u) == 7u, "execution continues after the clock service");
    CHECK(gp32_get_cpu_reg(g, 0u) == params && gp32_get_cpu_reg(g, 1u) == 0x12345678u, "r0 and r1 preserved");
    CHECK(gp32_get_fclk_hz(g) == 59250000u && s3c2400_hclk_hz(g->soc) == 59250000u &&
          s3c2400_pclk_hz(g->soc) == 29625000u && gp32_get_run_clock_hz(g) == 59250000u,
          "MPLLCON 0x47022 with CLKDIVN 1 gives 59.25 / 59.25 / 29.625 MHz");
    CHECK((s3c2400_debug_read32(g->soc, 0x14000024u) & 0x7ffu) == 0x47bu, "REFRESH counter follows the new HCLK");
    CHECK(s3c2400_debug_read32(g->soc, direct_fwinfo_addr(g) + 4u) == 59250000u &&
          s3c2400_debug_read32(g->soc, direct_fwinfo_addr(g) + 8u) == 29625000u,
          "clock block published without another SWI 0x0b");
    /* Display services other than a mode switch keep the panel divider (ROM
       0x1a64-0x1d44 never touch LCDCON1); the mode switch recomputes it from
       the clock (ROM 0x1804 via 0x1fd4: 3 at 33.9 MHz, 5 at 59.25 MHz). */
    direct_set_lcd_8bpp(g, direct_default_surface_addr(0u), 0u, 0);
    CHECK(((s3c2400_debug_read32(g->soc, 0x14a00000u) >> 8) & 0x3ffu) == 3u, "surface service keeps LCD divider 3");
    direct_set_lcd_8bpp(g, direct_default_surface_addr(0u), 0u, 1);
    CHECK(((s3c2400_debug_read32(g->soc, 0x14a00000u) >> 8) & 0x3ffu) == 5u, "mode switch takes LCD divider 5 at 59.25 MHz");
    /* A parameter block outside RAM leaves the clock alone. */
    arm920t_set_reg(g->cpu, 0u, 0x40u);
    arm920t_set_reg(g->cpu, 15u, GP32_RAM_BASE + 0x40000u);
    arm920t_set_reg(g->cpu, 4u, 0u);
    for (unsigned i = 0; i < 8u && gp32_get_cpu_reg(g, 4u) != 7u; ++i)
        CHECK(gp32_run_cycles(g, 8u) == GP32_OK, "clock service with a bad block runs");
    CHECK(gp32_get_cpu_reg(g, 4u) == 7u && gp32_get_run_clock_hz(g) == 59250000u, "bad parameter block ignored");
    gp32_destroy(g);
}

/* GpGraphicModeSet (SWI #8 selector 0) after the clock change reaches ROM
 * 0x1804 and takes the divider of the new clock; the same call before it leaves 3. */
static void check_mode_switch_divider(int jit) {
    static const uint32_t words[] = {
        0xef00000du, /* svc #0x0d */
        0xe3a00008u, /* mov r0, #8: bpp */
        0xe3a01000u, /* mov r1, #0: palette */
        0xe3a02000u, /* mov r2, #0: selector 0 */
        0xef000008u, /* svc #8 */
        0xe3a04007u, /* mov r4, #7 */
        0xeafffffeu, /* b . */
    };
    gp32_t *g = load_direct(words, GP32_ARRAY_COUNT(words), jit);
    if (!g) return;
    const uint32_t params = GP32_RAM_BASE + 0x1f000u;
    s3c2400_write32(g->soc, params, 59250000u);
    s3c2400_write32(g->soc, params + 4u, 0x47022u);
    s3c2400_write32(g->soc, params + 8u, 1u);
    arm920t_set_reg(g->cpu, 0u, params);
    for (unsigned i = 0; i < 16u && gp32_get_cpu_reg(g, 4u) != 7u; ++i)
        CHECK(gp32_run_cycles(g, 8u) == GP32_OK, "clock service and mode switch run");
    CHECK(gp32_get_cpu_reg(g, 4u) == 7u, "execution continues after the mode switch");
    CHECK(((s3c2400_debug_read32(g->soc, 0x14a00000u) >> 8) & 0x3ffu) == 5u, "GpGraphicModeSet after the clock change takes divider 5");
    gp32_destroy(g);
}

/* SWI #0x0F selector 1 (ROM 0x1458-0x1490) returns the application directory and
 * stores its length through r0. The SDK startup stub (b 0x32c-0x338) copies
 * length + 1 bytes with that count, so a word left untouched ran the copy off the
 * end of RAM. */
static void check_app_path_query(int jit) {
    static const uint32_t words[] = {
        0xef00000fu, /* svc #0x0f */
        0xe3a05007u, /* mov r5, #7 */
        0xeafffffeu, /* b . */
    };
    gp32_t *g = load_direct(words, GP32_ARRAY_COUNT(words), jit);
    if (!g) return;
    const char *path = "gp:\\game\\TEST";
    snprintf(g->direct_smc_game_dir, sizeof(g->direct_smc_game_dir), "%s", path);
    const uint32_t buf = GP32_RAM_BASE + 0x1f000u;
    s3c2400_write32(g->soc, buf, 0x7fffffffu);
    arm920t_set_reg(g->cpu, 0u, buf);
    arm920t_set_reg(g->cpu, 4u, 1u);
    for (unsigned i = 0; i < 8u && gp32_get_cpu_reg(g, 5u) != 7u; ++i)
        CHECK(gp32_run_cycles(g, 8u) == GP32_OK, "path query runs");
    CHECK(gp32_get_cpu_reg(g, 5u) == 7u, "execution continues after the path query");
    CHECK(s3c2400_debug_read32(g->soc, buf) == (uint32_t)strlen(path), "string length stored through r0");
    uint32_t str = gp32_get_cpu_reg(g, 0u);
    CHECK(str == direct_app_arg_addr(g), "r0 points at the application directory");
    int same = 1;
    for (size_t i = 0; i <= strlen(path); ++i) same &= s3c2400_read8(g->soc, str + (uint32_t)i) == (uint8_t)path[i];
    CHECK(same, "string and its terminator are in RAM");
    gp32_destroy(g);
}

int main(void) {
    for (int jit = 0; jit < 2; ++jit) {
        check_reinit_service(jit);
        check_declined_when_no_continuation(jit);
        check_handoff_clock(jit);
        check_clock_service(jit);
        check_mode_switch_divider(jit);
        check_app_path_query(jit);
    }
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("gp32_swi_1ff: all checks passed\n");
    return 0;
}
