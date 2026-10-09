/* Host-detector regression for generic fast loading: the opt-in 4x CPU boost
 * that fills silent bulk SmartMedia reads.  The machine is synthetic - an
 * active 240x320 TFT surface over static RAM, a guest parked on a self-branch
 * and NAND data reads faked as GPIO RE strobes - so no ROM, card image or BIOS
 * is needed.  Everything is observed through the public runtime: the host pump
 * (gp32_get_cpu_speed_percent inside a delivery), gp32_get_cycles,
 * s3c2400_iis_running and the savestate data API.
 *
 * Each case is red against the pre-guard src/gp32.c (run it with
 * -DGP32_SOURCE=\"...gp32-before.c\"):
 *   1. a machine that never ran audio must never arm the boost (the old
 *      last_iis = 0 made frame 1 look "recently audio");
 *   2. gp32_load_state_data must drop the host history, or a replayed state
 *      keeps boosting with no new evidence;
 *   3. a scanout layout (LCDSADDR3) or palette change is display activity even
 *      when the hashed pixel span is byte-identical;
 *   4. a real guest store that starts IIS mid-frame must end the boost before
 *      the next CPU slice, not after the whole frame;
 *   5. the HLE mixer is part of the no-audio guard, not just IISCON bit 0. */
#ifndef GP32_SOURCE
#define GP32_SOURCE "../src/gp32.c"
#endif
#include GP32_SOURCE

static int failures;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr, "FAIL: %s (%d)\n", (msg), __LINE__); ++failures; } } while (0)

#define FAST_LCD     0x14a00000u
#define FAST_IISCON  0x15508000u
#define FAST_LOOP    (GP32_RAM_BASE + 0x1000u)     /* b . */
#define FAST_CODE    (GP32_RAM_BASE + 0x1100u)     /* starts IIS, then spins */
#define FAST_SURFACE (GP32_RAM_BASE + 0x200000u)
#define FAST_READS   4096u

/* What the host pump saw: the speed the next CPU slice will run at, how many
 * deliveries ran at nominal speed, and the cycles the frame retired. */
typedef struct pump_log {
    unsigned calls;
    unsigned nominal;
    unsigned max_speed;
    uint64_t cycles;
} pump_log_t;

static pump_log_t log_st;

/* Only the address matters: the guard treats an active SEF stream as audio.
 * The zeroed stream is stopped, so nothing ever dereferences its data. */
static fpk_asset_t fake_asset;

static void pump_cb(gp32_t *g, void *user) {
    pump_log_t *p = (pump_log_t *)user;
    unsigned speed = gp32_get_cpu_speed_percent(g);
    p->calls++;
    if (speed > p->max_speed) p->max_speed = speed;
    if (speed <= 100u) p->nominal++;
}

/* One NAND data byte as the bit-bang driver reads it: card selected, both
 * latches low and one RE strobe.  This is the only path that advances
 * smc_bytes_read. */
static void reads(gp32_t *g, unsigned bytes) {
    s3c2400_t *soc = g->soc;
    s3c2400_write32(soc, 0x15600008u, 0x1u);  /* RE line idle */
    s3c2400_write32(soc, 0x15600030u, 0x8u);  /* CLE/ALE low, write strobe off */
    s3c2400_write32(soc, 0x15600024u, 0x0u);  /* card selected, read direction */
    for (unsigned i = 0; i < bytes; ++i) {
        s3c2400_write32(soc, 0x15600008u, 0x0u); /* RE asserted */
        s3c2400_write32(soc, 0x15600008u, 0x1u);
    }
}

static uint64_t run_frame(gp32_t *g, unsigned bytes) {
    reads(g, bytes);
    uint64_t before = gp32_get_cycles(g);
    memset(&log_st, 0, sizeof log_st);
    log_st.max_speed = 100u;
    CHECK(gp32_run_frame(g) == GP32_OK, "frame retires");
    log_st.cycles = gp32_get_cycles(g) - before;
    return log_st.cycles;
}

/* 240x320 TFT scanout over static RAM, 16 or 8 bpp, 4 KiB-page stride 0. */
static void program_lcd(gp32_t *g, unsigned bpp) {
    s3c2400_t *soc = g->soc;
    uint32_t words = (240u / (bpp == 8u ? 4u : 2u)) * 320u;
    uint32_t end = FAST_SURFACE + words * 4u;
    uint32_t mode = bpp == 8u ? 0x0bu : 0x0cu;
    s3c2400_write32(soc, FAST_LCD + 0x00u, 1u | (mode << 1) | (2u << 5));
    s3c2400_write32(soc, FAST_LCD + 0x04u, 319u << 14);
    s3c2400_write32(soc, FAST_LCD + 0x08u, 239u << 8);
    s3c2400_write32(soc, FAST_LCD + 0x14u, FAST_SURFACE >> 1);
    s3c2400_write32(soc, FAST_LCD + 0x18u, end >> 1);
    s3c2400_write32(soc, FAST_LCD + 0x1cu, 0u);
    for (uint32_t i = 0; i < words; ++i) s3c2400_write32(soc, FAST_SURFACE + i * 4u, 0x11223344u + i);
}

static gp32_t *fixture(int jit, unsigned bpp) {
    gp32_t *g = gp32_create(NULL);
    CHECK(g != NULL, "create core");
    if (!g) return NULL;
    CHECK(gp32_set_jit(g, jit) == GP32_OK, "select backend");
    program_lcd(g, bpp);
    /* Write both guest programs before either runs, so no execution backend
     * can hold a fetched copy of them. */
    s3c2400_write32(g->soc, FAST_LOOP, 0xeafffffeu);
    const uint32_t iis_start[] = { 0xe3a00001u, 0xe59f1004u, 0xe5810000u, 0xeafffffeu, FAST_IISCON };
    for (unsigned i = 0; i < 5u; ++i) s3c2400_write32(g->soc, FAST_CODE + i * 4u, iis_start[i]);
    arm920t_set_cpsr(g->cpu, 0xd3u);
    for (unsigned i = 0; i < 15u; ++i) arm920t_set_reg(g->cpu, i, 0u);
    arm920t_set_reg(g->cpu, 15u, FAST_LOOP);
    gp32_set_host_pump(g, pump_cb, &log_st);
    return g;
}

/* One frame of audio at the SoC level, then silence: the detector has to have
 * seen audio before a frozen bulk read may arm it. */
static void observe_audio(gp32_t *g) {
    s3c2400_write32(g->soc, FAST_IISCON, 1u);
    run_frame(g, 0u);
    s3c2400_write32(g->soc, FAST_IISCON, 0u);
}

/* Audio seen, then four frozen bulk-read frames: one to seed the frame hash
 * and three busy frames to fill the entry streak.  The boost takes effect on
 * the frame after this helper returns, because the detector updates at the
 * end of a frame. */
static void arm(gp32_t *g) {
    observe_audio(g);
    run_frame(g, FAST_READS);
    run_frame(g, FAST_READS);
    run_frame(g, FAST_READS);
    run_frame(g, FAST_READS);
}

/* 1. No audio has ever run, so nothing may arm - the old detector treated
 * "frame - 0 <= 32" as recent audio and boosted from frame 4. */
static void check_silent_machine_never_arms(int jit) {
    gp32_t *g = fixture(jit, 16u);
    if (!g) return;
    unsigned boosted = 0;
    for (unsigned i = 0; i < 8u; ++i) {
        run_frame(g, FAST_READS);
        if (log_st.max_speed != 100u) boosted++;
    }
    CHECK(boosted == 0u, "a machine that never ran audio must not boost");
    gp32_destroy(g);
}

/* 2. A state load is a new machine: the host detector must not carry the
 * previous run's arm across it. */
static void check_state_load_drops_the_arm(void) {
    gp32_t *g = fixture(0, 16u);
    if (!g) return;
    arm(g);
    run_frame(g, FAST_READS);
    CHECK(log_st.max_speed == 400u, "armed machine boosts (control)");
    size_t size = gp32_state_size(g);
    uint8_t *data = size ? malloc(size) : NULL;
    CHECK(data != NULL, "allocate state");
    if (data) {
        CHECK(gp32_save_state_data(g, data, size) == GP32_OK, "save state");
        CHECK(gp32_load_state_data(g, data, size) == GP32_OK, "load state");
        run_frame(g, FAST_READS);
        CHECK(log_st.max_speed == 100u, "a reloaded state must not keep boosting");
        free(data);
    }
    gp32_destroy(g);
}

/* 3. Display programming is display activity.  The pixel span is untouched,
 * so only a detector that reads the LCD registers and palette can see it. */
static unsigned run_read_frames(gp32_t *g, unsigned n) {
    unsigned top = 100u;
    for (unsigned i = 0; i < n; ++i) {
        run_frame(g, FAST_READS);
        if (log_st.max_speed > top) top = log_st.max_speed;
    }
    return top;
}

static void check_display_change_stops_the_streak(unsigned bpp, int change_lcd) {
    gp32_t *g = fixture(0, bpp);
    if (!g) return;
    observe_audio(g);
    /* Two identical frames carry the streak to one, far short of the entry. */
    CHECK(run_read_frames(g, 2u) == 100u, "two identical frames do not boost yet");
    if (change_lcd) s3c2400_write32(g->soc, FAST_LCD + 0x1cu, 120u); /* pagewidth */
    else s3c2400_write32(g->soc, FAST_LCD + 0x400u, 0x001fu);        /* palette[0] */
    /* A detector that missed the change would already be armed here. */
    CHECK(run_read_frames(g, 3u) == 100u, "a display change must restart the streak");
    CHECK(run_read_frames(g, 3u) == 400u, "the new programming still arms later");
    gp32_destroy(g);
}

/* 4. Real guest code starts IIS during a boosted frame.  The store is a
 * deferred guest store, so IIS is only running after the first CPU slice:
 * the boost has to stop before the next one. */
static void check_guest_iis_ends_the_boost_midframe(int jit) {
    gp32_t *g = fixture(jit, 16u);
    if (!g) return;
    arm(g);
    uint64_t full = run_frame(g, FAST_READS);
    CHECK(log_st.max_speed == 400u, "boosted frame before the guest starts IIS (control)");
    arm920t_set_reg(g->cpu, 0u, 0u);
    arm920t_set_reg(g->cpu, 15u, FAST_CODE);
    uint64_t mid = run_frame(g, FAST_READS);
    CHECK(log_st.max_speed == 400u, "the frame still starts boosted");
    CHECK(s3c2400_iis_running(g->soc), "the guest store really started IIS");
    CHECK(log_st.nominal > 0u, "IIS resumed mid-frame: nominal speed before the next slice");
    CHECK(mid * 2u < full, "only the silent head of the frame runs boosted");
    CHECK(!g->fast_load_active, "the arm ends with the load");
    gp32_destroy(g);
}

/* 5. Direct boot can mix audio without guest IIS: the HLE mixer mirror is the
 * second half of the guard (direct_hle_audio_asset is the raw SEF path).  The
 * frame a stream is noticed in may still start boosted, but it has to end the
 * arm and the next frame must be back at nominal speed. */
static void check_hle_audio_ends_the_arm(int use_asset, const char *what) {
    gp32_t *g = fixture(0, 16u);
    if (!g) return;
    arm(g);
    if (use_asset) g->direct_hle_audio_asset = &fake_asset;
    else g->direct_hle_pcm_active = 1u;
    run_frame(g, FAST_READS);
    CHECK(!g->fast_load_active, what);
    run_frame(g, FAST_READS);
    CHECK(log_st.max_speed == 100u, what);
    gp32_destroy(g);
}

int main(void) {
    for (int jit = 0; jit <= 1; ++jit) check_silent_machine_never_arms(jit);
    check_state_load_drops_the_arm();
    check_display_change_stops_the_streak(16u, 1);
    check_display_change_stops_the_streak(8u, 0);
    for (int jit = 0; jit <= 1; ++jit) check_guest_iis_ends_the_boost_midframe(jit);
    check_hle_audio_ends_the_arm(0, "HLE mixing must end the arm");
    check_hle_audio_ends_the_arm(1, "SEF asset playback must end the arm");
    printf("gp32_fast_load: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
