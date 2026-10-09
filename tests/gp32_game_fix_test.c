/* ROM-independent regression test for the ADPCM compatibility fix. Astonishia
 * Story R's IMA-ADPCM decoder (0x0c0112a4) picks the low or high nibble of a
 * byte from the parity of the stream-wide sample counter (r7), so every odd
 * block decodes with swapped nibbles. When exactly that routine is resident,
 * the fix replaces its "tst r7, #1" with a host trap that reports the in-block
 * parity instead: the same machine state has to end up with different flags,
 * and disabling the fix has to give the game's own flags back.
 *
 * The fixture is the routine's own ARM words plus the block metadata the trap
 * reads, so every case runs through the public API and the real CPU seam:
 * gp32_run_cycles, gp32_set_game_fixes, gp32_reset, gp32_save_state_data,
 * gp32_load_state_data and arm920t_run. No ROM, card image or BIOS is needed,
 * and no second ADPCM decoder is used as an oracle: the expectations are the
 * CPU flags, the word left at the site and the trap being consumed in place.
 *
 * The stream is two 505-sample blocks. The second one starts at stream
 * position 505, so inside it the stream parity and the in-block parity differ
 * and the reported flags tell the two rules apart. The trap word records which
 * parity the block in progress uses and changes only at a safe in-block
 * position, so a case has to walk its samples from the block start, in order. */
#ifndef GP32_SOURCE
#define GP32_SOURCE "../src/gp32.c"
#endif
#include GP32_SOURCE

static int failures;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr, "FAIL: %s (%d)\n", (msg), __LINE__); ++failures; } } while (0)

#define FIXTURE_SPB   505u
#define FIXTURE_TOTAL (2u * FIXTURE_SPB)
#define NO_CORRUPT    UINT32_MAX

/* The trap stands in for "tst r7, #1": N clears, Z reports whether the sample
 * is even, C and V survive, and the mode stays the supervisor ARM state. All
 * cases enter with N, Z, C and V set so the checks can see all of that. */
#define TRAP_CPSR_IN 0xf00000d3u

typedef struct fixture {
    gp32_t *g;
    uint32_t state;     /* block metadata the trap reads */
    uint32_t trap_pc;   /* the instruction the fix replaces */
    uint32_t own_word;  /* what the game's routine holds there */
} fixture_t;

static uint32_t ram_word(const fixture_t *f, uint32_t addr) {
    return s3c2400_debug_read32(f->g->soc, addr);
}

static void ram_put(const fixture_t *f, uint32_t addr, uint32_t word) {
    s3c2400_write32(f->g->soc, addr, word);
}

/* One run slice as a frontend drives it: the scan for the routine and the
 * per-run refresh of the patch happen inside gp32_run_cycles. The CPU is parked
 * on the reset vector so the slice executes an ordinary instruction. */
static int run_slice(fixture_t *f) {
    arm920t_set_cpsr(f->g->cpu, 0xd3u);
    for (uint32_t r = 0; r < 16u; ++r) arm920t_set_reg(f->g->cpu, r, 0u);
    return gp32_run_cycles(f->g, 1u) == GP32_OK;
}

/* Load the game's decoder routine into RAM. corrupt_index replaces one word of
 * it with corrupt_value, which is what a foreign routine looks like. */
static void install_own_code(fixture_t *f, uint32_t corrupt_index, uint32_t corrupt_value) {
    const uint32_t n = (uint32_t)(sizeof(direct_adpcm_fix_code) / sizeof(direct_adpcm_fix_code[0]));
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t word = direct_adpcm_fix_code[i];
        if (corrupt_index != NO_CORRUPT && i == corrupt_index) word = corrupt_value;
        ram_put(f, GP32_ADPCM_FIX_FUNC + i * 4u, word);
    }
}

static int open_fixture(fixture_t *f, int jit, uint32_t corrupt_index, uint32_t corrupt_value) {
    memset(f, 0, sizeof *f);
    f->trap_pc = GP32_ADPCM_FIX_PC;
    f->state = GP32_RAM_BASE + 0x80000u;
    f->g = gp32_create(NULL);
    CHECK(f->g != NULL, "create core");
    if (!f->g) return 0;
    CHECK(gp32_set_jit(f->g, jit) == GP32_OK, "select execution backend");
    install_own_code(f, corrupt_index, corrupt_value);
    f->own_word = ram_word(f, f->trap_pc);
    CHECK(run_slice(f), "run slice succeeds");
    return 1;
}

static void close_fixture(fixture_t *f) {
    gp32_destroy(f->g);
    f->g = NULL;
}

/* Drive the word at the site through the real instruction path. r4 points at
 * the block metadata, r7 is the stream position and r9 is the next block header
 * clipped to the stream length, so r9 == total marks the final block. */
static uint32_t trap_step(fixture_t *f, uint32_t spb, uint32_t total, uint32_t pos, uint32_t end) {
    ram_put(f, f->state + 0x14u, total);
    ram_put(f, f->state + 0x18u, spb);
    arm920t_set_cpsr(f->g->cpu, TRAP_CPSR_IN);
    arm920t_set_reg(f->g->cpu, 4u, f->state);
    arm920t_set_reg(f->g->cpu, 7u, pos);
    arm920t_set_reg(f->g->cpu, 9u, end);
    arm920t_set_reg(f->g->cpu, 15u, f->trap_pc);
    return arm920t_run(f->g->cpu, 1u);
}

/* A handled trap is consumed in place: the following instruction runs and no
 * SWI exception is taken. */
static void expect_consumed(const fixture_t *f, const char *what) {
    CHECK(arm920t_get_pc(f->g->cpu) == f->trap_pc + 4u, what);
    CHECK((arm920t_get_cpsr(f->g->cpu) & 0x1fu) == 0x13u, what);
}

static void expect_flags(const fixture_t *f, int even, const char *what) {
    char buf[192];
    uint32_t cpsr = arm920t_get_cpsr(f->g->cpu);
    snprintf(buf, sizeof buf, "%s: Z reports an %s sample", what, even ? "even" : "odd");
    CHECK((cpsr & 0x40000000u) == (even ? 0x40000000u : 0u), buf);
    snprintf(buf, sizeof buf, "%s: N cleared, C and V kept, supervisor ARM state", what);
    CHECK((cpsr & 0xb00000ffu) == 0x300000d3u, buf);
}

/* One sample of a block: the flags show which parity the site reported. */
static void expect_trap_sample(fixture_t *f, uint32_t spb, uint32_t total, uint32_t pos,
                               int even, const char *what) {
    CHECK(trap_step(f, spb, total, pos, total) == 1u, what);
    expect_consumed(f, what);
    expect_flags(f, even, what);
}

/* The game's own word must still be an ordinary "tst r7, #1": the flags follow
 * the plain ARM rule for r7 and nothing is intercepted. */
static void expect_game_tst(fixture_t *f, uint32_t r7, const char *what) {
    arm920t_set_cpsr(f->g->cpu, TRAP_CPSR_IN);
    arm920t_set_reg(f->g->cpu, 7u, r7);
    arm920t_set_reg(f->g->cpu, 15u, f->trap_pc);
    CHECK(arm920t_run(f->g->cpu, 1u) == 1u, what);
    expect_consumed(f, what);
    expect_flags(f, !(r7 & 1u), what);
}

/* A routine that is not the game's own must be left alone: the site keeps the
 * game's word and still executes as an ordinary test. */
static void check_signature_rejection(int jit) {
    fixture_t f;
    if (!open_fixture(&f, jit, 5u, 0xe1a00000u)) return; /* "ldr r7, [r1, #0x10]" -> mov r0, r0 */
    CHECK(ram_word(&f, f.trap_pc) == f.own_word, "foreign routine keeps the game's word at the site");
    expect_game_tst(&f, 507u, "foreign routine: an odd sample is not turned even");
    expect_game_tst(&f, 508u, "foreign routine: an even sample is not turned odd");
    close_fixture(&f);
}

/* The trap must report the parity inside the 505-sample block. The block starts
 * at stream position 505, so inside it the two rules disagree: 506 is even in
 * the stream but its in-block position 1 is odd, and 507 is odd in the stream
 * but its in-block position 2 is even. A trap that kept using r7 would report
 * 506 as even and 507 as odd. */
static void check_odd_block_parity(int jit) {
    fixture_t f;
    if (!open_fixture(&f, jit, NO_CORRUPT, 0u)) return;
    CHECK(ram_word(&f, f.trap_pc) != f.own_word, "resident routine: the fix owns the site");
    expect_trap_sample(&f, FIXTURE_SPB, FIXTURE_TOTAL, FIXTURE_SPB + 0u, 0,
                       "block start: 505 still runs on the game's parity");
    expect_trap_sample(&f, FIXTURE_SPB, FIXTURE_TOTAL, FIXTURE_SPB + 1u, 0,
                       "506 is odd in the block although it is even in the stream");
    expect_trap_sample(&f, FIXTURE_SPB, FIXTURE_TOTAL, FIXTURE_SPB + 2u, 1,
                       "507 is even in the block although it is odd in the stream");
    expect_trap_sample(&f, FIXTURE_SPB, FIXTURE_TOTAL, FIXTURE_SPB + 3u, 0,
                       "508 is odd in the block");
    close_fixture(&f);

    /* A last block shorter than the stride starts at total - remainder, so its
     * in-block parity alternates from 8 and not from total - spb. */
    if (!open_fixture(&f, jit, NO_CORRUPT, 0u)) return;
    expect_trap_sample(&f, 8u, 11u, 9u, 0, "short final block: 9 is its odd sample");
    expect_trap_sample(&f, 8u, 11u, 10u, 1, "short final block: 10 is its even sample");
    close_fixture(&f);
}

/* Disabling gives the game's own instruction back; enabling patches the site
 * again, and both halves keep reporting what they always did. */
static void check_enable_disable(int jit) {
    fixture_t f;
    if (!open_fixture(&f, jit, NO_CORRUPT, 0u)) return;
    expect_trap_sample(&f, FIXTURE_SPB, FIXTURE_TOTAL, FIXTURE_SPB + 0u, 0,
                       "with the fix on the block start still runs on the game's parity");

    CHECK(gp32_set_game_fixes(f.g, 0) == GP32_OK, "disable accepted");
    CHECK(ram_word(&f, f.trap_pc) == f.own_word, "disable returns the game's word to the site");
    expect_game_tst(&f, 507u, "disabled: 507 is odd for the game's own test");

    CHECK(gp32_set_game_fixes(f.g, 1) == GP32_OK, "re-enable accepted");
    CHECK(ram_word(&f, f.trap_pc) != f.own_word, "re-enable patches the site again");
    expect_trap_sample(&f, FIXTURE_SPB, FIXTURE_TOTAL, FIXTURE_SPB + 0u, 0,
                       "re-enabled: the block starts on the game's parity again");
    expect_trap_sample(&f, FIXTURE_SPB, FIXTURE_TOTAL, FIXTURE_SPB + 1u, 0,
                       "re-enabled: 506 is odd in the block");
    expect_trap_sample(&f, FIXTURE_SPB, FIXTURE_TOTAL, FIXTURE_SPB + 2u, 1,
                       "re-enabled: 507 is even in the block again");
    close_fixture(&f);
}

/* Disabling in the middle of a block must not rewind what the block has already
 * decoded: the trap keeps reporting the parity the block is running with,
 * switches back at the next safe in-block position, and only then does the
 * game's own instruction return. */
static void check_staged_restore(int jit) {
    fixture_t f;
    if (!open_fixture(&f, jit, NO_CORRUPT, 0u)) return;
    expect_trap_sample(&f, FIXTURE_SPB, FIXTURE_TOTAL, FIXTURE_SPB + 0u, 0,
                       "block start runs on the game's parity");
    expect_trap_sample(&f, FIXTURE_SPB, FIXTURE_TOTAL, FIXTURE_SPB + 1u, 0,
                       "506 is the first in-block position the fix may switch at");

    CHECK(gp32_set_game_fixes(f.g, 0) == GP32_OK, "disable accepted");
    CHECK(ram_word(&f, f.trap_pc) != f.own_word, "disable keeps the trap inside a running block");
    expect_trap_sample(&f, FIXTURE_SPB, FIXTURE_TOTAL, FIXTURE_SPB + 2u, 1,
                       "running block: 507 is still even in the block");
    expect_trap_sample(&f, FIXTURE_SPB, FIXTURE_TOTAL, FIXTURE_SPB + 3u, 1,
                       "running block: 508 switches the block back to the game's parity");
    expect_trap_sample(&f, FIXTURE_SPB, FIXTURE_TOTAL, FIXTURE_SPB + 4u, 0,
                       "running block: 509 is odd in the stream the game now uses");

    CHECK(run_slice(&f), "next run slice succeeds");
    CHECK(ram_word(&f, f.trap_pc) == f.own_word, "the next run returns the game's word");
    expect_game_tst(&f, 509u, "restored site runs the game's own test again");
    close_fixture(&f);
}

/* Reset wipes RAM; a later run must not keep claiming the old patch and must
 * re-derive the fix from the routine that is resident afterwards. */
static void check_reset(int jit) {
    fixture_t f;
    if (!open_fixture(&f, jit, NO_CORRUPT, 0u)) return;
    CHECK(ram_word(&f, f.trap_pc) != f.own_word, "patched before the reset");

    CHECK(gp32_reset(f.g) == GP32_OK, "reset accepted");
    CHECK(ram_word(&f, f.trap_pc) == 0u, "reset clears the RAM the patch lived in");
    CHECK(run_slice(&f), "run after reset succeeds");
    CHECK(ram_word(&f, f.trap_pc) == 0u, "a run over cleared RAM installs nothing");

    install_own_code(&f, NO_CORRUPT, 0u);
    CHECK(run_slice(&f), "run with the routine loaded again succeeds");
    CHECK(ram_word(&f, f.trap_pc) != f.own_word, "the fix is re-derived after a reset");
    expect_trap_sample(&f, FIXTURE_SPB, FIXTURE_TOTAL, FIXTURE_SPB + 1u, 0,
                       "after reset: 506 is odd in the block");
    expect_trap_sample(&f, FIXTURE_SPB, FIXTURE_TOTAL, FIXTURE_SPB + 2u, 1,
                       "after reset: 507 is even in the block");
    close_fixture(&f);
}

/* The host option is not part of the machine state; the loaded RAM word is, and
 * it is re-examined against the option in effect after the load. */
static void check_state_load(int jit) {
    fixture_t f;
    if (!open_fixture(&f, jit, NO_CORRUPT, 0u)) return;
    CHECK(ram_word(&f, f.trap_pc) != f.own_word, "patched before the save");

    size_t size = gp32_state_size(f.g);
    uint8_t *patched = (uint8_t *)malloc(size ? size : 1u);
    uint8_t *plain = (uint8_t *)malloc(size ? size : 1u);
    CHECK(size && patched && plain && gp32_save_state_data(f.g, patched, size) == GP32_OK,
          "capture the patched machine");
    CHECK(gp32_set_game_fixes(f.g, 0) == GP32_OK, "disable accepted");
    CHECK(ram_word(&f, f.trap_pc) == f.own_word, "the disabled machine holds the game's word");
    CHECK(gp32_save_state_data(f.g, plain, size) == GP32_OK, "capture the machine with the fix off");

    CHECK(gp32_set_game_fixes(f.g, 1) == GP32_OK, "re-enable accepted");
    CHECK(gp32_load_state_data(f.g, patched, size) == GP32_OK, "load the patched state");
    CHECK(ram_word(&f, f.trap_pc) != f.own_word, "the loaded trap word stays a trap");
    expect_trap_sample(&f, FIXTURE_SPB, FIXTURE_TOTAL, FIXTURE_SPB + 1u, 0,
                       "loaded state: 506 is odd in the block");
    expect_trap_sample(&f, FIXTURE_SPB, FIXTURE_TOTAL, FIXTURE_SPB + 2u, 1,
                       "loaded state: 507 is even in the block");

    CHECK(gp32_set_game_fixes(f.g, 0) == GP32_OK, "disable accepted");
    CHECK(gp32_load_state_data(f.g, patched, size) == GP32_OK, "load the patched state while disabled");
    CHECK(ram_word(&f, f.trap_pc) == f.own_word, "a disabled host converts the loaded trap word");
    CHECK(gp32_load_state_data(f.g, plain, size) == GP32_OK, "load the state saved with the fix off");
    CHECK(ram_word(&f, f.trap_pc) == f.own_word, "loading it keeps the game's word");

    CHECK(gp32_set_game_fixes(f.g, 1) == GP32_OK, "re-enable accepted");
    CHECK(ram_word(&f, f.trap_pc) != f.own_word, "an enabled host patches the state saved with the fix off");
    expect_trap_sample(&f, FIXTURE_SPB, FIXTURE_TOTAL, FIXTURE_SPB + 1u, 0,
                       "state cycle: 506 is odd in the block");
    expect_trap_sample(&f, FIXTURE_SPB, FIXTURE_TOTAL, FIXTURE_SPB + 2u, 1,
                       "state cycle: 507 is even in the block");

    free(plain);
    free(patched);
    close_fixture(&f);
}

int main(void) {
    for (int jit = 0; jit <= 1; ++jit) {
        check_signature_rejection(jit);
        check_odd_block_parity(jit);
        check_enable_disable(jit);
        check_staged_restore(jit);
        check_reset(jit);
        check_state_load(jit);
    }
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("gp32 game-fix tests passed (jit=0,1)\n");
    return 0;
}

