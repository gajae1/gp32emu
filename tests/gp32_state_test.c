/* Transactional savestate regression: a truncated or otherwise rejected load
 * must leave the live machine byte-identical, including CPU, RAM, SmartMedia
 * and queued PCM. The v0005 stream is
 * magic | gp32 image | elapsed time | frame time | arm920t image | s3c2400 image | ram | smc | audio | spans, and
 * a cut past the CPU image used to rewind the CPU while the caller still saw
 * GP32_ERR_IO, with the mounted SmartMedia committed before the trailing PCM
 * had been read. Synthetic fixture, no ROM needed; GP32_SOURCE can select a
 * saved pre-change source for the same fixture. */
#ifndef GP32_SOURCE
#define GP32_SOURCE "../src/gp32.c"
#endif
#include GP32_SOURCE

static int failures;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr, "FAIL: %s (%d)\n", (msg), __LINE__); ++failures; } } while (0)

/* IIS byte DMA on channel 2, as in tests/s3c2400_timing_test.c: one 500-cycle
 * tick pushes one stereo frame into the captured PCM buffer. */
static void queue_audio(gp32_t *g, unsigned frames) {
    s3c2400_t *soc = g->soc;
    s3c2400_write32(soc, 0x14600040u, 0x0c000000u); /* DISRC2: RAM, incrementing */
    s3c2400_write32(soc, 0x14600044u, 0x35508010u); /* DIDST2: IISFIF, fixed */
    s3c2400_write32(soc, 0x14600048u, 0x10800008u); /* DCON2: IRQ|HW req|IIS, byte, tc=8 */
    s3c2400_write32(soc, 0x14600058u, 2u);          /* DMASKTRIG2: channel on */
    s3c2400_write32(soc, 0x15508000u, 1u);          /* IISCON: start */
    for (unsigned i = 0; i < frames; ++i) {
        s3c2400_tick(soc, 500u);
        if (s3c2400_read32(soc, 0x14400000u) & 0x00080000u)
            s3c2400_write32(soc, 0x14400000u, 0x00080000u);
    }
}

/* Clock a READID command and two ID bytes through the GPIO SmartMedia pins,
 * so the live card holds a mode and byte address the other card never had. */
static void smc_read_id(gp32_t *g) {
    s3c2400_t *soc = g->soc;
    s3c2400_write32(soc, 0x15600008u, 0x1u);  /* READ line idle (active low) */
    s3c2400_write32(soc, 0x15600030u, 0x8u);  /* CLE/ALE low, write strobe off */
    s3c2400_write32(soc, 0x15600024u, 0x0u);  /* card selected, output enabled */
    s3c2400_write32(soc, 0x1560000cu, 0x90u); /* data bus: READID */
    s3c2400_write32(soc, 0x15600030u, 0x28u); /* command latch -> 0x90 */
    s3c2400_write32(soc, 0x15600030u, 0x8u);
    for (unsigned i = 0; i < 2u; ++i) {
        s3c2400_write32(soc, 0x15600008u, 0x0u); /* read strobe */
        s3c2400_write32(soc, 0x15600008u, 0x1u);
    }
}

/* Exact whole-machine state image, or NULL. The loader consumes the same
 * bytes, so equality here means a rejected load left the machine untouched. */
static uint8_t *capture_state(gp32_t *g, size_t *size) {
    *size = gp32_state_size(g);
    uint8_t *buf = (uint8_t *)malloc(*size ? *size : 1u);
    if (!buf || !*size || gp32_save_state_data(g, buf, *size) != GP32_OK) {
        free(buf);
        *size = 0;
        return NULL;
    }
    return buf;
}

/* CPU wire image length from the live saver: exactly the bytes the
 * whole-machine stream carries, without depending on where the wire struct is
 * declared, so a frozen baseline gp32.c compiles against its own headers. */
static size_t cpu_image_bytes(const gp32_t *g) {
    state_io_t io = state_io_counter();
    return arm920t_state_save_io(g->cpu, &io) ? io.pos : 0u;
}

/* Load the first `cut` bytes of `image` (memory buffer, or the same bytes
 * written to `path`) and require rejection plus a machine byte-identical to
 * the pristine `live` image captured before the first attempt, so an earlier
 * mutated case cannot mask the next one. */
static void expect_rejected_load(gp32_t *g, const uint8_t *live, size_t live_size,
                                 const uint8_t *image, size_t cut, const char *path, const char *what) {
    if (path) {
        FILE *f = fopen(path, "wb");
        int written = f && fwrite(image, 1, cut, f) == cut;
        if (f && fclose(f) != 0) written = 0;
        CHECK(written, what);
        if (written) CHECK(gp32_load_state(g, path) != GP32_OK, what);
    } else {
        CHECK(gp32_load_state_data(g, image, cut) != GP32_OK, what);
    }
    size_t after_size = 0;
    uint8_t *after = capture_state(g, &after_size);
    CHECK(after != NULL && live_size > 0 && after_size == live_size &&
          memcmp(live, after, live_size) == 0, what);
    free(after);
}

static void check_swi_route(gp32_t *g, uint32_t pc, int direct) {
    s3c2400_write32(g->soc, pc, 0xef000011u); /* direct surface service: r0=1 */
    arm920t_set_cpsr(g->cpu, 0xd3u);
    arm920t_set_reg(g->cpu, 0u, 0u);
    arm920t_set_reg(g->cpu, 15u, pc);
    CHECK(arm920t_run(g->cpu, 1u) == 1u, "one SWI instruction");
    CHECK(arm920t_get_pc(g->cpu) == (direct ? pc + 4u : 8u), "mode selects HLE or BIOS vector");
    CHECK(arm920t_get_reg(g->cpu, 0u) == (direct ? 1u : 0u), "only direct mode handles the service");
}

static void check_swi_lifecycle(void) {
    const uint32_t pc = GP32_RAM_BASE + 0x4000u;
    uint8_t code[8];
    gp32_st32le(code, 0xef000011u);
    gp32_st32le(code + 4u, 0xeafffffeu);
    for (int jit = 0; jit <= 1; ++jit) {
        gp32_t *g = gp32_create(NULL);
        CHECK(g != NULL, "create SWI lifecycle fixture");
        if (!g) continue;
        gp32_set_jit(g, jit);
        check_swi_route(g, pc, 0);
        size_t bios_size = 0, direct_size = 0;
        uint8_t *bios = capture_state(g, &bios_size), *direct = NULL;
        CHECK(bios != NULL, "save BIOS mode");
        fxe_image_t img = {0};
        img.payload = code; img.payload_size = sizeof(code);
        img.load_addr = img.entry_addr = pc;
        CHECK(gp32_load_fxe_image_internal(g, &img, 0, 0, 0, 0) == GP32_OK,
              "FXE loader installs direct services");
        check_swi_route(g, pc, 1);
        direct = capture_state(g, &direct_size);
        CHECK(direct != NULL, "save direct mode");
        CHECK(gp32_reset(g) == GP32_OK, "reset without retained FXE image");
        check_swi_route(g, pc, 0);
        if (bios && direct) {
            CHECK(gp32_load_state_data(g, direct, direct_size) == GP32_OK, "restore direct over BIOS");
            check_swi_route(g, pc, 1);
            CHECK(gp32_load_state_data(g, bios, bios_size) == GP32_OK, "restore BIOS over direct");
            check_swi_route(g, pc, 0);
        }
        free(bios); free(direct); gp32_destroy(g);
    }
}

int main(int argc, char **argv) {
    check_swi_lifecycle();
    const char *path = argc > 1 ? argv[1] : NULL;
    gp32_t *source = gp32_create(NULL);
    gp32_t *target = gp32_create(NULL);
    if (!source || !target) return 2;

    /* Advanced, different live machines: distinct cycles, RAM bytes,
     * SmartMedia contents/card state, and queued PCM. */
    CHECK(gp32_run_cycles(source, 4096u) == GP32_OK, "advance source machine");
    CHECK(gp32_run_cycles(target, 12000u) == GP32_OK, "advance target machine");
    CHECK(gp32_get_cycles(source) != gp32_get_cycles(target), "machines advanced differently");

    uint8_t src_pattern[256], dst_pattern[256];
    memset(src_pattern, 0xa5, sizeof(src_pattern));
    memset(dst_pattern, 0x5a, sizeof(dst_pattern));
    CHECK(s3c2400_load_ram_image(source->soc, 0x0c000000u, src_pattern, sizeof(src_pattern), NULL, 0), "source ram pattern");
    CHECK(s3c2400_load_ram_image(target->soc, 0x0c000000u, dst_pattern, sizeof(dst_pattern), NULL, 0), "target ram pattern");

    uint8_t src_media[2112 * 4], dst_media[2112 * 8];
    memset(src_media, 0x3c, sizeof(src_media));
    memset(dst_media, 0xc3, sizeof(dst_media));
    CHECK(s3c2400_load_smartmedia_buffer(source->soc, src_media, sizeof(src_media), NULL, 0), "source smartmedia");
    CHECK(s3c2400_load_smartmedia_buffer(target->soc, dst_media, sizeof(dst_media), NULL, 0), "target smartmedia");
    smc_read_id(target);
    queue_audio(source, 8u);
    queue_audio(target, 3u);

    size_t src_size = 0;
    uint8_t *src = capture_state(source, &src_size);
    uint64_t src_frames = 0;
    (void)s3c2400_audio_samples(source->soc, &src_frames, NULL);
    CHECK(src != NULL, "capture source state");
    CHECK(src_frames > 0, "source queued audio nonempty");
    size_t audio_bytes = (size_t)src_frames * 4u; /* stereo s16 */

    /* Reference for a committed load: the same machine with the host-facing
     * PCM buffer cleared, which is what gp32_state_loaded applies last. */
    CHECK(gp32_clear_audio(source) == GP32_OK, "clear source audio");
    size_t ref_size = 0;
    uint8_t *ref = capture_state(source, &ref_size);
    CHECK(ref != NULL, "capture reference state");
    CHECK(src_size > ref_size && src_size - ref_size == audio_bytes, "uniform PCM adds no rate boundaries");
    if (!src || !ref) {
        printf("gp32_state: FAIL\n");
        return 2;
    }

    /* Truncation boundaries from the live component sizes, not a copied wire
     * layout: gp32 header, CPU image, ram blob, then smc tail and PCM tail. */
    const size_t time_off = sizeof(gp32_state_magic) + sizeof(gp32_state_image_t);
    const size_t frame_off = time_off + sizeof(gp32_elapsed_time_t);
    const size_t soc_off = frame_off + sizeof(gp32_frame_time_t);
    const size_t cpu_len = cpu_image_bytes(source);
    const size_t ram_len = s3c2400_ram_size(target->soc);
    const size_t cpu_cut = soc_off + cpu_len / 2u;
    const size_t ram_cut = soc_off + cpu_len + ram_len / 2u;
    const size_t span_bytes = sizeof(uint32_t); /* no closed spans in this fixture */
    const size_t smc_cut = src_size - span_bytes - audio_bytes - 1u;
    const size_t gap_cut = src_size - span_bytes - audio_bytes;
    CHECK(cpu_len > 0, "cpu wire image length");
    CHECK(cpu_cut > soc_off && cpu_cut < soc_off + cpu_len, "cpu cut lands inside the cpu image");
    /* The SoC header holds a 512 KiB BIOS mirror plus a 300 KiB framebuffer,
     * well below the 4 MiB half-blob offset used here. */
    CHECK(ram_cut > soc_off + cpu_len && ram_cut < gap_cut, "ram cut lands inside the ram blob");
    CHECK(smc_cut > ram_cut && smc_cut < gap_cut, "smc cut lands inside the smc blob");

    /* Pristine advanced target; every rejected read must leave exactly these
     * bytes, whichever section the truncation lands in. */
    size_t live_size = 0;
    uint8_t *live = capture_state(target, &live_size);
    CHECK(live != NULL, "capture live target state");
    if (!live) return 2;

    expect_rejected_load(target, live, live_size, src, frame_off - 1u, NULL, "elapsed time cut rejected without mutation");
    expect_rejected_load(target, live, live_size, src, soc_off - 1u, NULL, "frame time cut rejected without mutation");
    gp32_frame_time_t bad_frame = {0u, 60u, 1u};
    memcpy(src + frame_off, &bad_frame, sizeof(bad_frame));
    expect_rejected_load(target, live, live_size, src, src_size, NULL, "invalid frame fraction rejected without mutation");
    memset(src + frame_off, 0, sizeof(bad_frame));
    uint8_t saved_time[sizeof(gp32_elapsed_time_t)];
    memcpy(saved_time, src + time_off, sizeof(saved_time));
    gp32_elapsed_time_t invalid_time = {1u, 0u, 0u};
    memcpy(src + time_off, &invalid_time, sizeof(invalid_time));
    expect_rejected_load(target, live, live_size, src, src_size, NULL, "invalid elapsed time rejected without mutation");
    memcpy(src + time_off, saved_time, sizeof(saved_time));
    expect_rejected_load(target, live, live_size, src, cpu_cut, NULL, "cpu image cut rejected with byte-identical state");
    expect_rejected_load(target, live, live_size, src, ram_cut, NULL, "ram blob cut rejected with byte-identical state");
    expect_rejected_load(target, live, live_size, src, smc_cut, NULL, "smc tail cut rejected with byte-identical state");
    expect_rejected_load(target, live, live_size, src, gap_cut, NULL, "audio gap cut rejected with byte-identical state");
    if (audio_bytes >= 8u)
        expect_rejected_load(target, live, live_size, src, src_size - audio_bytes / 2u, NULL, "audio mid cut rejected with byte-identical state");
    expect_rejected_load(target, live, live_size, src, src_size - 1u, NULL, "audio tail cut rejected with byte-identical state");
    if (path)
        expect_rejected_load(target, live, live_size, src, gap_cut, path, "file path cut rejected with byte-identical state");

    uint32_t bad_count = UINT32_MAX;
    memcpy(src + src_size - span_bytes, &bad_count, sizeof(bad_count));
    expect_rejected_load(target, live, live_size, src, src_size, NULL, "invalid span count rejected without mutation");
    bad_count = 0;
    memcpy(src + src_size - span_bytes, &bad_count, sizeof(bad_count));

    /* A complete image still loads through both entry points and lands on the
     * reference bytes. */
    CHECK(gp32_load_state_data(target, src, src_size) == GP32_OK, "valid memory load");
    size_t got_size = 0;
    uint8_t *got = capture_state(target, &got_size);
    CHECK(got && got_size == ref_size && memcmp(ref, got, ref_size) == 0, "valid memory load restores exact state");
    free(got);
    if (path) {
        FILE *f = fopen(path, "wb");
        int written = f && fwrite(src, 1, src_size, f) == src_size;
        if (f && fclose(f) != 0) written = 0;
        CHECK(written, "write full state file");
        CHECK(gp32_load_state(target, path) == GP32_OK, "valid file load");
        got = capture_state(target, &got_size);
        CHECK(got && got_size == ref_size && memcmp(ref, got, ref_size) == 0, "valid file load restores exact state");
        free(got);
    }

    /* v4 predates frame pacing; v3 also predates spans, v2 elapsed time. */
    size_t v4_size = src_size - sizeof(gp32_frame_time_t);
    uint8_t *v4 = malloc(v4_size);
    CHECK(v4 != NULL, "allocate v4 state");
    if (v4) {
        memcpy(v4, src, frame_off);
        memcpy(v4, gp32_state_magic_v4, sizeof(gp32_state_magic_v4));
        memcpy(v4 + frame_off, src + soc_off, src_size - soc_off);
        CHECK(gp32_load_state_data(target, v4, v4_size) == GP32_OK, "v4 state remains readable");
        CHECK(!target->frame_time.valid, "legacy frame pacing starts at loaded time");
        memcpy(v4, gp32_state_magic_v3, sizeof(gp32_state_magic_v3));
        CHECK(gp32_load_state_data(target, v4, v4_size - span_bytes) == GP32_OK, "v3 state remains readable");
        free(v4);
    }
    size_t legacy_size = src_size - sizeof(gp32_elapsed_time_t) - sizeof(gp32_frame_time_t) - span_bytes;
    uint8_t *legacy = malloc(legacy_size);
    CHECK(legacy != NULL, "allocate legacy state");
    if (legacy) {
        memcpy(legacy, src, time_off);
        memcpy(legacy, gp32_state_magic_v2, sizeof(gp32_state_magic_v2));
        memcpy(legacy + time_off, src + soc_off, src_size - soc_off - span_bytes);
        CHECK(gp32_load_state_data(target, legacy, legacy_size) == GP32_OK, "v2 state remains readable");
        uint32_t old_ms = (uint32_t)(gp32_get_cycles(source) * 1000u / direct_run_clock_hz(source));
        CHECK(direct_elapsed_ms(target) == old_ms, "v2 migration starts at former observable time");
        CHECK(gp32_get_cycles(target) == gp32_get_cycles(source), "v2 migration preserves CPU time");
        free(legacy);
    }
    free(src);
    free(ref);
    free(live);
    gp32_destroy(source);
    gp32_destroy(target);
    printf("gp32_state: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
