/* Drive the actual GP32 GPIO wiring and observe queued PCM, not codec internals. */
#include "../src/gp32.c"

static void pin(gp32_t *g, uint32_t value) {
    s3c2400_write32(g->soc, 0x15600030u, value);
}

static void bits(gp32_t *g, unsigned data_mode, uint8_t value, unsigned first, unsigned end) {
    for (unsigned i = first; i < end; ++i) {
        uint32_t v = (data_mode ? 0x400u : 0u) | (((value >> i) & 1u) << 11);
        pin(g, v);
        pin(g, v | 0x200u);
    }
}

static void command(gp32_t *g, uint8_t address, uint8_t data) {
    pin(g, 0u);
    pin(g, 0x200u); /* Retail BIOS primes CLOCK before the address bits. */
    bits(g, 0, address, 0, 8);
    pin(g, 0x400u);
    bits(g, 1, data, 0, 8);
}

static int frame_is(gp32_t *g, uint64_t at, int16_t left, int16_t right) {
    uint64_t n = 0;
    const int16_t *p = s3c2400_audio_samples(g->soc, &n, NULL);
    int ok = p && n > at && p[at * 2u] == left && p[at * 2u + 1u] == right;
    if (!ok) fprintf(stderr, "frame %llu/%llu: got %d,%d expected %d,%d\n",
        (unsigned long long)at, (unsigned long long)n, p && n > at ? p[at * 2u] : 0,
        p && n > at ? p[at * 2u + 1u] : 0, left, right);
    return ok;
}

static void append(gp32_t *g) {
    s3c2400_audio_append_s16_stereo(g->soc, 10000, -10000, 44100);
}

/* Direct-FXE HLE SWI #0x17 (GpControlVolume), observed only through the PCM
   the SoC captures: r0 wraps modulo 64 into VC5..VC0 (85 == 21, not clamped),
   every call ends with the 0x80 control byte that clears a previously latched
   mute, and the inlined service must leave r0-r3 and lr untouched. A BIOS-mode
   CPU has no installed HLE and takes the ordinary vector-8 path instead. */
static int swi_volume_call(gp32_t *g, uint32_t at, uint32_t arg) {
    arm920t_set_reg(g->cpu, 0, arg);
    arm920t_set_reg(g->cpu, 1, 0xa1a1a1a1u);
    arm920t_set_reg(g->cpu, 2, 0x5a5a5a5au);
    arm920t_set_reg(g->cpu, 3, 0xdeadbe00u);
    arm920t_set_reg(g->cpu, 14, 0x0c00f00du);
    arm920t_set_reg(g->cpu, 15, at);
    uint32_t ran = arm920t_run(g->cpu, 1u);
    int ok = ran == 1u && arm920t_get_pc(g->cpu) == at + 4u &&
        arm920t_get_reg(g->cpu, 0) == arg &&
        arm920t_get_reg(g->cpu, 1) == 0xa1a1a1a1u &&
        arm920t_get_reg(g->cpu, 2) == 0x5a5a5a5au &&
        arm920t_get_reg(g->cpu, 3) == 0xdeadbe00u &&
        arm920t_get_reg(g->cpu, 14) == 0x0c00f00du;
    if (!ok) fprintf(stderr, "swi#0x17 r0=%u: ran=%u pc=%08x r0=%08x r1=%08x r2=%08x r3=%08x lr=%08x\n",
        (unsigned)arg, (unsigned)ran, arm920t_get_pc(g->cpu),
        arm920t_get_reg(g->cpu, 0), arm920t_get_reg(g->cpu, 1),
        arm920t_get_reg(g->cpu, 2), arm920t_get_reg(g->cpu, 3),
        arm920t_get_reg(g->cpu, 14));
    return ok;
}

static int check_direct_swi_volume(void) {
    const uint32_t at = GP32_RAM_BASE + 0x40000u;
    uint8_t code[8];
    gp32_st32le(code, 0xef000017u);      /* svc #0x17 */
    gp32_st32le(code + 4u, 0xeafffffeu); /* parked after the call */
    int ok = 1;
    for (int jit = 0; jit <= 1; ++jit) {
        gp32_t *g = gp32_create(NULL);
        if (!g) return 0;
        gp32_set_jit(g, jit);

        /* BIOS mode: no installed HLE, so the same opcode takes vector 8 and
           leaves the codec at its unity reset gain. */
        s3c2400_write32(g->soc, at, 0xef000017u);
        arm920t_set_cpsr(g->cpu, 0xd3u);
        arm920t_set_reg(g->cpu, 0, 21u);
        arm920t_set_reg(g->cpu, 15, at);
        int vectored = arm920t_run(g->cpu, 1u) == 1u && arm920t_get_pc(g->cpu) == 8u;
        if (!vectored) fprintf(stderr, "bios swi#0x17: pc=%08x expected 00000008\n",
            arm920t_get_pc(g->cpu));
        append(g);
        ok &= vectored && frame_is(g, 0, 10000, -10000);

        fxe_image_t img = {0};
        img.payload = code; img.payload_size = sizeof(code);
        img.load_addr = img.entry_addr = at;
        if (gp32_load_fxe_image_internal(g, &img, 0, 0, 0, 0) != GP32_OK) {
            fputs("FAIL: direct FXE fixture load\n", stderr);
            gp32_destroy(g);
            return 0;
        }
        s3c2400_write32(g->soc, 0x1560002cu, 0x540000u); /* GPE9..11 outputs */
        gp32_clear_audio(g);
        arm920t_set_cpsr(g->cpu, 0xd3u);
        ok &= swi_volume_call(g, at, 21u);
        append(g);
        ok &= swi_volume_call(g, at, 85u);
        append(g);
        ok &= swi_volume_call(g, at, 63u);
        append(g);
        command(g, 0x14u, 0x84u); /* latch the mute bit over the L3 wire */
        ok &= swi_volume_call(g, at, 21u);
        append(g);
        ok &= frame_is(g, 0, 1000, -1000) && frame_is(g, 1, 1000, -1000) &&
            frame_is(g, 2, 0, 0) && frame_is(g, 3, 1000, -1000);
        gp32_destroy(g);
    }
    if (!ok) fputs("FAIL: direct-FXE SWI #0x17 GpControlVolume\n", stderr);
    return ok;
}

static int check_clock_edge_timing(void) {
    gp32_t *g = gp32_create(NULL);
    if (!g) return 0;
    s3c2400_write32(g->soc, 0x1560002cu, 0x540000u);
    pin(g, 0u); bits(g, 0, 0x14u, 0, 8);
    pin(g, 0x400u); bits(g, 1, 21u, 0, 7); pin(g, 0x400u);
    s3c2400_write32(g->soc, GP32_RAM_BASE + 0x40000u, 0x2710d8f0u);
    s3c2400_write32(g->soc, 0x14600040u, 0x2c040000u); /* Fixed source. */
    s3c2400_write32(g->soc, 0x14600044u, 0x35508010u);
    s3c2400_write32(g->soc, 0x14600048u, 0x10a00002u); /* IIS word requests, reload. */
    s3c2400_write32(g->soc, 0x14600058u, 2u);
    s3c2400_write32(g->soc, 0x15508008u, 3u << 5);
    s3c2400_write32(g->soc, 0x15508000u, 0x21u);
    for (unsigned i = 0; i < 2000u; ++i)
        s3c2400_write32(g->soc, GP32_RAM_BASE + i * 4u, 0xe1a02002u);
    s3c2400_write32(g->soc, GP32_RAM_BASE + 8000u, 0xe5810000u); /* STR r0,[r1]: final L3 edge. */
    arm920t_set_reg(g->cpu, 0, 0x600u);
    arm920t_set_reg(g->cpu, 1, 0x15600030u);
    arm920t_set_reg(g->cpu, 15, GP32_RAM_BASE);
    gp32_clear_audio(g);
    s3c2400_run_cpu(g->soc, 10000u);
    uint64_t frames = 0;
    const int16_t *pcm = s3c2400_audio_samples(g->soc, &frames, NULL);
    int ok = frames > 0;
    for (uint64_t i = 0; i < frames; ++i)
        ok &= pcm[i * 2u] == 10000 && pcm[i * 2u + 1u] == -10000;
    gp32_clear_audio(g);
    s3c2400_tick(g->soc, 4000u);
    ok &= frame_is(g, 0, 1000, -1000);
    gp32_destroy(g);
    if (!ok) fputs("FAIL: L3 gain change retroactively affects elapsed PCM\n", stderr);
    return ok;
}

int main(void) {
    if (!check_clock_edge_timing()) return 1;
    if (!check_direct_swi_volume()) return 1;
    gp32_t *g = gp32_create(NULL);
    if (!g) return 2;
    s3c2400_write32(g->soc, 0x1560002cu, 0x540000u); /* GPE9..11 outputs */
    append(g); /* Legacy/unprogrammed codec is unity. */
    command(g, 0x14u, 21u); /* -20 dB amplitude = 1/10. */
    append(g);
    command(g, 0x14u, 0x84u); /* Mute control must not overwrite VC. */
    append(g);
    command(g, 0x14u, 0x80u);
    command(g, 0x15u, 63u); /* Wrong address is ignored. */
    append(g);
    int ok = frame_is(g, 0, 10000, -10000) && frame_is(g, 1, 1000, -1000) &&
        frame_is(g, 2, 0, 0) && frame_is(g, 3, 1000, -1000);

    /* A save halfway through a byte must preserve both the old gain and the
       unfinished wire transaction; the new volume latches at bit eight. */
    pin(g, 0u);
    bits(g, 0, 0x14u, 0, 8);
    pin(g, 0x400u);
    bits(g, 1, 62u, 0, 4);
    size_t size = gp32_state_size(g);
    uint8_t *saved = malloc(size);
    ok &= saved && gp32_save_state_data(g, saved, size) == GP32_OK;
    bits(g, 1, 62u, 4, 8);
    append(g);
    ok &= frame_is(g, 4, 0, 0);
    if (saved) {
        ok &= gp32_load_state_data(g, saved, size) == GP32_OK;
        append(g);
        bits(g, 1, 62u, 4, 8);
        append(g);
        ok &= frame_is(g, 0, 1000, -1000) && frame_is(g, 1, 0, 0);
    }
    free(saved);

    /* Exercise the optimized DMA append with non-unity gain. */
    gp32_clear_audio(g);
    command(g, 0x14u, 21u);
    s3c2400_write32(g->soc, GP32_RAM_BASE, 0x2710d8f0u); /* IIS word sends high halfword first. */
    s3c2400_write32(g->soc, 0x14600040u, GP32_RAM_BASE);
    s3c2400_write32(g->soc, 0x14600044u, 0x35508010u);
    s3c2400_write32(g->soc, 0x14600048u, 0x04600001u); /* Whole-service, stop, word, one unit. */
    s3c2400_write32(g->soc, 0x14600058u, 3u); /* Software request. */
    ok &= frame_is(g, 0, 1000, -1000);
    gp32_destroy(g);
    if (!ok) { fputs("FAIL: L3 codec gain, queued audio, DMA or partial-byte restore\n", stderr); return 1; }
    puts("PASS: GPIO L3 volume/mute, queued PCM, DMA and partial-byte restore");
    return 0;
}
