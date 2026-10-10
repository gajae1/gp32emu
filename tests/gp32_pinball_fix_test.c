/* ROM-independent startup-volume regression, using real L3 writes, PCM,
 * CPU/JIT execution, option changes and portable saved states. No card needed. */
#ifndef GP32_SOURCE
#define GP32_SOURCE "../src/gp32.c"
#endif
#include GP32_SOURCE

static int failures;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr, "FAIL: %s (%d)\n", (msg), __LINE__); ++failures; } } while (0)

#define PROBE_L    10000
#define PROBE_R    (-10000)
#define PROBE_RATE 44100u
#define NO_CORRUPT UINT32_MAX

/* The codec tuple the captured startup sequence leaves behind. */
#define CAPTURE_STATUS  0x08u
#define CAPTURE_CONTROL 0x90u
#define CAPTURE_VOLUME  63u

/* The instruction the fix owns inside the recorded routine. */
#define PATCH_INDEX ((GP32_PINBALL_FIX_PC - GP32_PINBALL_FIX_FUNC) / 4u)

typedef struct fixture {
    gp32_t *g;
} fixture_t;

typedef struct state_blob {
    uint8_t *data;
    size_t size;
} state_blob_t;

static uint32_t ram_word(const fixture_t *f, uint32_t addr) {
    return s3c2400_debug_read32(f->g->soc, addr);
}

static void ram_put(const fixture_t *f, uint32_t addr, uint32_t word) {
    s3c2400_write32(f->g->soc, addr, word);
}

/* GPE9..GPE11 carry L3CLOCK/L3MODE/L3DATA. The model only services them while
 * GPECON programs those pins as outputs, the way the firmware leaves them. */
static void wire_pin(gp32_t *g, uint32_t value) {
    s3c2400_write32(g->soc, 0x15600030u, value);
}

static void wire_bits(gp32_t *g, unsigned data_mode, uint8_t value, unsigned first, unsigned end) {
    for (unsigned i = first; i < end; ++i) {
        uint32_t v = (data_mode ? 0x400u : 0u) | (((value >> i) & 1u) << 11);
        wire_pin(g, v);
        wire_pin(g, v | 0x200u);
    }
}

/* One complete L3 transfer: address byte with MODE low, then the data byte. */
static void wire_command(gp32_t *g, uint8_t address, uint8_t data) {
    wire_pin(g, 0u);
    wire_pin(g, 0x200u); /* firmware primes CLOCK before the address bits */
    wire_bits(g, 0, address, 0, 8);
    wire_pin(g, 0x400u);
    wire_bits(g, 1, data, 0, 8);
}

/* Program the codec through the real wire. The captured tuple is status 0x08,
 * control 0x90 (no mute) and VC 63; the mismatch cases reuse this with one
 * register changed. */
static void program_tuple(gp32_t *g, uint8_t status, uint8_t control, uint8_t volume) {
    s3c2400_write32(g->soc, 0x1560002cu, 0x540000u); /* GPE9..11 outputs */
    wire_command(g, 0x16u, status);
    wire_command(g, 0x14u, control);
    wire_command(g, 0x14u, volume);
}

/* One stereo frame at the gain the codec holds right now. */
static void append_probe(gp32_t *g) {
    s3c2400_audio_append_s16_stereo(g->soc, PROBE_L, PROBE_R, PROBE_RATE);
}

/* Observe queued PCM the way a frontend does. */
static void expect_frame(gp32_t *g, uint64_t at, int16_t left, int16_t right, const char *what) {
    gp32_audio_desc_t desc = {0};
    if (gp32_get_audio(g, &desc) != GP32_OK || !desc.samples_s16_interleaved || desc.frame_count <= at) {
        fprintf(stderr, "FAIL: %s: no frame %llu (have %llu)\n", what,
                (unsigned long long)at, (unsigned long long)desc.frame_count);
        ++failures;
        return;
    }
    int16_t got_l = desc.samples_s16_interleaved[at * 2u];
    int16_t got_r = desc.samples_s16_interleaved[at * 2u + 1u];
    if (got_l != left || got_r != right) {
        fprintf(stderr, "FAIL: %s: frame %llu is %d,%d, expected %d,%d\n", what,
                (unsigned long long)at, got_l, got_r, left, right);
        ++failures;
    }
}

/* Load the game's initialization routine into RAM. The patch word is always
 * the game's own "mov r0, #63" unless the caller corrupts that slot, so the
 * fixture never starts from a value the fix itself would have written. */
static void install_own_code(fixture_t *f, uint32_t corrupt_index, uint32_t corrupt_value) {
    const uint32_t n = (uint32_t)GP32_ARRAY_COUNT(direct_pinball_fix_code);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t word = i == PATCH_INDEX ? GP32_PINBALL_FIX_ORIG : direct_pinball_fix_code[i];
        if (corrupt_index != NO_CORRUPT && i == corrupt_index) word = corrupt_value;
        ram_put(f, GP32_PINBALL_FIX_FUNC + i * 4u, word);
    }
}

static int open_fixture(fixture_t *f, int jit, uint32_t corrupt_index, uint32_t corrupt_value) {
    memset(f, 0, sizeof *f);
    f->g = gp32_create(NULL);
    CHECK(f->g != NULL, "create core");
    if (!f->g) return 0;
    /* The BIOS-mode idle filler would interleave silence frames; this fixture
     * queues its own probes and wants stable frame indices. */
    s3c2400_set_audio_idle(f->g->soc, 0);
    CHECK(gp32_set_jit(f->g, jit) == GP32_OK, "select execution backend");
    install_own_code(f, corrupt_index, corrupt_value);
    return 1;
}

static void close_fixture(fixture_t *f) {
    gp32_destroy(f->g);
    f->g = NULL;
}

/* One run slice as a frontend drives it: the scan for the routine and the
 * repair happen inside gp32_run_cycles. The CPU is parked on the reset vector
 * so the slice executes an ordinary instruction. */
static int run_slice(fixture_t *f) {
    arm920t_set_cpsr(f->g->cpu, 0xd3u);
    for (uint32_t r = 0; r < 16u; ++r) arm920t_set_reg(f->g->cpu, r, 0u);
    return gp32_run_cycles(f->g, 1u) == GP32_OK;
}

/* Drive the word at the site through the real instruction path. */
static uint32_t run_site_instruction(fixture_t *f, uint32_t r0) {
    arm920t_set_cpsr(f->g->cpu, 0xd3u);
    arm920t_set_reg(f->g->cpu, 0u, r0);
    arm920t_set_reg(f->g->cpu, 15u, GP32_PINBALL_FIX_PC);
    return arm920t_run(f->g->cpu, 3u); /* MOV, BL, inlined callee self-branch */
}

static int capture_state(fixture_t *f, state_blob_t *blob) {
    blob->size = gp32_state_size(f->g);
    blob->data = blob->size ? (uint8_t *)malloc(blob->size) : NULL;
    return blob->data && gp32_save_state_data(f->g, blob->data, blob->size) == GP32_OK;
}

/* The exact resident routine is patched and its captured mute repaired; a
 * routine that differs anywhere else keeps the game's word and silence, and
 * so does a site that holds neither ORIG nor NEW. */
static void check_signature(int jit) {
    fixture_t f;
    if (!open_fixture(&f, jit, NO_CORRUPT, 0u)) return;
    program_tuple(f.g, CAPTURE_STATUS, CAPTURE_CONTROL, CAPTURE_VOLUME);
    append_probe(f.g);
    expect_frame(f.g, 0u, 0, 0, "the game's own VC 63 leaves queued PCM silent");
    CHECK(ram_word(&f, GP32_PINBALL_FIX_PC) == GP32_PINBALL_FIX_ORIG,
          "the game's own instruction is resident before any run");
    CHECK(run_slice(&f), "run slice succeeds");
    CHECK(ram_word(&f, GP32_PINBALL_FIX_PC) == GP32_PINBALL_FIX_NEW,
          "the exact routine's site now holds mov r0, #0");
    append_probe(f.g);
    expect_frame(f.g, 0u, 0, 0, "the correction preserves already queued silence");
    expect_frame(f.g, 1u, PROBE_L, PROBE_R, "the repaired codec makes newly queued PCM audible");
    close_fixture(&f);

    const uint32_t corrupt_at[] = { 5u, (uint32_t)GP32_ARRAY_COUNT(direct_pinball_fix_code) - 1u };
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(corrupt_at); ++i) {
        CHECK(corrupt_at[i] != PATCH_INDEX, "corruption probe avoids the patch word");
        if (!open_fixture(&f, jit, corrupt_at[i], 0xe1a00000u)) return;
        program_tuple(f.g, CAPTURE_STATUS, CAPTURE_CONTROL, CAPTURE_VOLUME);
        append_probe(f.g);
        CHECK(run_slice(&f), "run slice succeeds");
        CHECK(ram_word(&f, GP32_PINBALL_FIX_PC) == GP32_PINBALL_FIX_ORIG,
              "a foreign routine keeps the game's word at the site");
        append_probe(f.g);
        expect_frame(f.g, 0u, 0, 0, "a foreign routine keeps the captured mute");
        expect_frame(f.g, 1u, 0, 0, "a foreign routine queues no audible PCM");
        close_fixture(&f);
    }

    /* "mov r0, #62" is not the game's own word, so nothing may overwrite it. */
    if (!open_fixture(&f, jit, PATCH_INDEX, 0xe3a0003eu)) return;
    program_tuple(f.g, CAPTURE_STATUS, CAPTURE_CONTROL, CAPTURE_VOLUME);
    CHECK(run_slice(&f), "run slice succeeds");
    CHECK(ram_word(&f, GP32_PINBALL_FIX_PC) == 0xe3a0003eu, "a near-miss instruction is left alone");
    append_probe(f.g);
    expect_frame(f.g, 0u, 0, 0, "a near-miss instruction queues no audible PCM");
    close_fixture(&f);
}

/* With the host option off the game's own startup is untouched: the site keeps
 * its instruction and the captured mute stands. */
static void check_fixes_disabled(int jit) {
    fixture_t f;
    if (!open_fixture(&f, jit, NO_CORRUPT, 0u)) return;
    CHECK(gp32_set_game_fixes(f.g, 0) == GP32_OK, "disable accepted");
    program_tuple(f.g, CAPTURE_STATUS, CAPTURE_CONTROL, CAPTURE_VOLUME);
    append_probe(f.g);
    CHECK(run_slice(&f), "run slice succeeds");
    CHECK(run_slice(&f), "a second run slice succeeds");
    CHECK(ram_word(&f, GP32_PINBALL_FIX_PC) == GP32_PINBALL_FIX_ORIG,
          "disabled: the game's word stays at the site");
    append_probe(f.g);
    expect_frame(f.g, 0u, 0, 0, "disabled: the captured startup PCM stays silent");
    expect_frame(f.g, 1u, 0, 0, "disabled: no repaired volume is forced");
    close_fixture(&f);
}

/* Disabling after the fact restores the original instruction and nothing else:
 * the codec the game already initialized keeps the volume it reached, so
 * playback does not fall back into the startup mute. */
static void check_disable_keeps_codec(int jit) {
    fixture_t f;
    if (!open_fixture(&f, jit, NO_CORRUPT, 0u)) return;
    program_tuple(f.g, CAPTURE_STATUS, CAPTURE_CONTROL, CAPTURE_VOLUME);
    CHECK(run_slice(&f), "run slice installs the fix");
    append_probe(f.g);
    expect_frame(f.g, 0u, PROBE_L, PROBE_R, "the repaired codec plays");

    CHECK(gp32_set_game_fixes(f.g, 0) == GP32_OK, "disable accepted");
    CHECK(ram_word(&f, GP32_PINBALL_FIX_PC) == GP32_PINBALL_FIX_ORIG,
          "disable gives the game's own instruction back");
    append_probe(f.g);
    expect_frame(f.g, 1u, PROBE_L, PROBE_R, "disable does not force the startup mute back on");
    CHECK(run_slice(&f), "a run slice after the disable succeeds");
    append_probe(f.g);
    expect_frame(f.g, 2u, PROBE_L, PROBE_R, "a disabled host never mutes an initialized codec");

    CHECK(gp32_set_game_fixes(f.g, 1) == GP32_OK, "re-enable accepted");
    CHECK(ram_word(&f, GP32_PINBALL_FIX_PC) == GP32_PINBALL_FIX_NEW,
          "re-enable patches the site again");
    append_probe(f.g);
    expect_frame(f.g, 3u, PROBE_L, PROBE_R, "re-enable keeps the codec's volume");
    close_fixture(&f);
}

/* The site is the routine's business, the codec tuple is the game's: only the
 * captured triple may be repaired, and a different one is never overridden. */
static void check_tuple_mismatch(int jit) {
    struct mismatch { uint8_t status, control, volume; int16_t left, right; const char *what; };
    const struct mismatch cases[] = {
        { CAPTURE_STATUS, CAPTURE_CONTROL, 21u,           1000,  -1000, "a game-set VC 21 is not forced" },
        { 0x00u,          CAPTURE_CONTROL, CAPTURE_VOLUME, 0,     0,    "a foreign status byte is not forced" },
        { CAPTURE_STATUS, 0x80u,           CAPTURE_VOLUME, 0,     0,    "a foreign control byte is not forced" },
    };
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(cases); ++i) {
        fixture_t f;
        if (!open_fixture(&f, jit, NO_CORRUPT, 0u)) return;
        program_tuple(f.g, cases[i].status, cases[i].control, cases[i].volume);
        CHECK(run_slice(&f), "run slice succeeds");
        CHECK(ram_word(&f, GP32_PINBALL_FIX_PC) == GP32_PINBALL_FIX_NEW,
              "the resident routine is still patched");
        append_probe(f.g);
        expect_frame(f.g, 0u, cases[i].left, cases[i].right, cases[i].what);
        close_fixture(&f);
    }
}

/* After the repair the guest owns the codec again: its own later writes, VC 63
 * included, must stay in effect and survive further runs and toggles. */
static void check_guest_volume_after_repair(int jit) {
    fixture_t f;
    if (!open_fixture(&f, jit, NO_CORRUPT, 0u)) return;
    program_tuple(f.g, CAPTURE_STATUS, CAPTURE_CONTROL, CAPTURE_VOLUME);
    CHECK(run_slice(&f), "run slice installs the fix");
    append_probe(f.g);
    expect_frame(f.g, 0u, PROBE_L, PROBE_R, "the repaired codec plays");

    s3c2400_audio_set_volume(f.g->soc, 63u); /* the guest's own GpControlVolume(63) */
    append_probe(f.g);
    expect_frame(f.g, 1u, 0, 0, "a later guest VC 63 mute is honoured");
    CHECK(run_slice(&f), "a run slice after the guest mute succeeds");
    append_probe(f.g);
    expect_frame(f.g, 2u, 0, 0, "the fix does not unmute a muted codec");

    s3c2400_audio_set_volume(f.g->soc, 21u);
    append_probe(f.g);
    expect_frame(f.g, 3u, 1000, -1000, "the guest's own volume stays in effect");

    CHECK(gp32_set_game_fixes(f.g, 0) == GP32_OK, "disable accepted");
    CHECK(gp32_set_game_fixes(f.g, 1) == GP32_OK, "re-enable accepted");
    append_probe(f.g);
    expect_frame(f.g, 4u, 1000, -1000, "re-enabling over the guest's tuple repairs nothing");
    close_fixture(&f);
}

/* A repair between bits of a later volume command must not reset the wire. */
static void check_partial_l3_command(int jit) {
    fixture_t f;
    if (!open_fixture(&f, jit, NO_CORRUPT, 0u)) return;
    program_tuple(f.g, CAPTURE_STATUS, CAPTURE_CONTROL, CAPTURE_VOLUME);
    wire_pin(f.g, 0u);
    wire_pin(f.g, 0x200u);
    wire_bits(f.g, 0, 0x14u, 0, 8);
    wire_pin(f.g, 0x400u);
    wire_bits(f.g, 1, 21u, 0, 4);
    CHECK(run_slice(&f), "repair while a command is incomplete");
    wire_bits(f.g, 1, 21u, 4, 8);
    append_probe(f.g);
    expect_frame(f.g, 0u, 1000, -1000, "the completed wire command retains its own gain");
    close_fixture(&f);
}

/* An already executed "mov r0, #63" must not come back from a compiled block
 * once the run has patched the site. */
static void check_site_instruction(int jit) {
    fixture_t f;
    if (!open_fixture(&f, jit, NO_CORRUPT, 0u)) return;
    program_tuple(f.g, CAPTURE_STATUS, CAPTURE_CONTROL, CAPTURE_VOLUME);
    ram_put(&f, 0x0c22a30cu, 0xeafffffeu); /* park at the BL destination */

    CHECK(run_site_instruction(&f, 0xdeadbeefu) == 3u, "the original call trace runs");
    CHECK(arm920t_get_reg(f.g->cpu, 0u) == 63u, "with the game's code resident the site runs mov r0, #63");
    CHECK(arm920t_get_pc(f.g->cpu) == 0x0c22a30cu, "the original block reaches its BL destination");

    CHECK(run_slice(&f), "run slice installs the fix");
    CHECK(ram_word(&f, GP32_PINBALL_FIX_PC) == GP32_PINBALL_FIX_NEW, "the site is patched");

    CHECK(run_site_instruction(&f, 0xdeadbeefu) == 3u, "the corrected call trace runs");
    CHECK(arm920t_get_reg(f.g->cpu, 0u) == 0u, "the site now runs mov r0, #0");
    gp32_cpu_profile_t profile = {0};
    arm920t_get_cpu_profile(f.g->cpu, &profile);
    if (jit && profile.supported && profile.native_backend)
        CHECK(profile.native_arm_insns >= 6u, "the original and corrected blocks execute native code");
    close_fixture(&f);
}

/* Savestates carry both halves: RAM holds the site word, the codec section
 * holds the tuple. Loading reconciles the word with the host option and never
 * rewrites the gain the state already carries. */
static void check_state_cycle(int jit) {
    fixture_t a, b;
    state_blob_t original = {0}, repaired = {0}, unmuted = {0};
    if (!open_fixture(&a, jit, NO_CORRUPT, 0u)) return;
    program_tuple(a.g, CAPTURE_STATUS, CAPTURE_CONTROL, CAPTURE_VOLUME);
    CHECK(capture_state(&a, &original), "capture the game's own startup state");

    CHECK(run_slice(&a), "run slice installs the fix");
    append_probe(a.g);
    expect_frame(a.g, 0u, PROBE_L, PROBE_R, "the repaired machine plays");
    CHECK(capture_state(&a, &repaired), "capture the repaired machine");

    /* The repaired machine's codec with the host option off: the word returns
     * to the game's own, the volume the machine reached is kept. */
    CHECK(gp32_set_game_fixes(a.g, 0) == GP32_OK, "disable accepted");
    CHECK(ram_word(&a, GP32_PINBALL_FIX_PC) == GP32_PINBALL_FIX_ORIG,
          "the disabled machine's site is the game's word again");
    CHECK(capture_state(&a, &unmuted), "capture the disabled machine");
    close_fixture(&a);
    if (!original.data || !repaired.data || !unmuted.data) {
        free(original.data); free(repaired.data); free(unmuted.data);
        return;
    }
    /* Queued PCM makes state lengths variable. Restore each captured buffer
     * with its own length; successful public loads below verify compatibility. */

    if (!open_fixture(&b, jit, NO_CORRUPT, 0u)) {
        free(original.data); free(repaired.data); free(unmuted.data);
        return;
    }
    CHECK(gp32_load_state_data(b.g, repaired.data, repaired.size) == GP32_OK,
          "load the repaired state");
    CHECK(ram_word(&b, GP32_PINBALL_FIX_PC) == GP32_PINBALL_FIX_NEW,
          "the loaded state's patched word stays patched");
    append_probe(b.g);
    expect_frame(b.g, 0u, PROBE_L, PROBE_R, "the loaded repaired state plays");

    CHECK(gp32_load_state_data(b.g, original.data, original.size) == GP32_OK,
          "load the game's own startup state on an enabled host");
    CHECK(ram_word(&b, GP32_PINBALL_FIX_PC) == GP32_PINBALL_FIX_NEW,
          "an enabled host patches the loaded unpatched state");
    append_probe(b.g);
    expect_frame(b.g, 0u, PROBE_L, PROBE_R, "the migrated state plays");

    CHECK(gp32_set_game_fixes(b.g, 0) == GP32_OK, "disable accepted");
    CHECK(gp32_load_state_data(b.g, repaired.data, repaired.size) == GP32_OK,
          "load the repaired state on a disabled host");
    CHECK(ram_word(&b, GP32_PINBALL_FIX_PC) == GP32_PINBALL_FIX_ORIG,
          "a disabled host restores the loaded patched word");
    append_probe(b.g);
    expect_frame(b.g, 0u, PROBE_L, PROBE_R, "the disabled host keeps the repaired volume, not a forced mute");

    CHECK(gp32_load_state_data(b.g, unmuted.data, unmuted.size) == GP32_OK,
          "load the disabled machine on a disabled host");
    CHECK(ram_word(&b, GP32_PINBALL_FIX_PC) == GP32_PINBALL_FIX_ORIG,
          "its unpatched word stays unpatched");
    append_probe(b.g);
    expect_frame(b.g, 0u, PROBE_L, PROBE_R, "its already repaired volume stays repaired");

    CHECK(gp32_load_state_data(b.g, original.data, original.size) == GP32_OK,
          "load the game's own startup state on a disabled host");
    append_probe(b.g);
    expect_frame(b.g, 0u, 0, 0, "a disabled host keeps the loaded startup mute");

    CHECK(gp32_set_game_fixes(b.g, 1) == GP32_OK, "re-enable accepted");
    CHECK(ram_word(&b, GP32_PINBALL_FIX_PC) == GP32_PINBALL_FIX_NEW,
          "re-enabling repairs the loaded startup state");
    append_probe(b.g);
    expect_frame(b.g, 0u, 0, 0, "re-enabling preserves already queued silence");
    expect_frame(b.g, 1u, PROBE_L, PROBE_R, "new audio in the re-enabled state plays");

    free(unmuted.data);
    free(repaired.data);
    free(original.data);
    close_fixture(&b);
}

/* Reset clears RAM, the codec and the queue: a run over the cleared image
 * installs nothing, and the fix re-derives from the routine loaded again. */
static void check_reset(int jit) {
    fixture_t f;
    if (!open_fixture(&f, jit, NO_CORRUPT, 0u)) return;
    program_tuple(f.g, CAPTURE_STATUS, CAPTURE_CONTROL, CAPTURE_VOLUME);
    CHECK(run_slice(&f), "run slice installs the fix");
    CHECK(ram_word(&f, GP32_PINBALL_FIX_PC) == GP32_PINBALL_FIX_NEW, "patched before the reset");

    CHECK(gp32_reset(f.g) == GP32_OK, "reset accepted");
    CHECK(ram_word(&f, GP32_PINBALL_FIX_PC) == 0u, "reset clears the RAM the routine lived in");
    CHECK(run_slice(&f), "run after reset succeeds");
    CHECK(ram_word(&f, GP32_PINBALL_FIX_PC) == 0u, "a run over cleared RAM installs nothing");

    install_own_code(&f, NO_CORRUPT, 0u);
    program_tuple(f.g, CAPTURE_STATUS, CAPTURE_CONTROL, CAPTURE_VOLUME);
    append_probe(f.g);
    expect_frame(f.g, 0u, 0, 0, "the re-loaded game code is muted again");
    CHECK(run_slice(&f), "run with the routine loaded again succeeds");
    CHECK(ram_word(&f, GP32_PINBALL_FIX_PC) == GP32_PINBALL_FIX_NEW, "the fix is re-derived after a reset");
    append_probe(f.g);
    expect_frame(f.g, 1u, PROBE_L, PROBE_R, "and the repaired codec plays again");
    close_fixture(&f);
}

int main(void) {
    for (int jit = 0; jit <= 1; ++jit) {
        check_signature(jit);
        check_fixes_disabled(jit);
        check_disable_keeps_codec(jit);
        check_tuple_mismatch(jit);
        check_guest_volume_after_repair(jit);
        check_partial_l3_command(jit);
        check_site_instruction(jit);
        check_state_cycle(jit);
        check_reset(jit);
    }
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("gp32 pinball codec-volume fix tests passed (jit=0,1)\n");
    return 0;
}
