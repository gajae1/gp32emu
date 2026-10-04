/* Exercise the real U8 PCM conversion sites, including private HLE mixers.
 * Run with -fsanitize=shift to catch signed shifts below the U8 midpoint. */
#include "../src/gp32.c"

static const uint8_t input[] = {0, 1, 127, 128, 129, 254, 255};
static const int16_t expected[] = {-32768, -32512, -256, 0, 256, 32256, 32512};

static gp32_t *sdk_stream_fixture(uint32_t half_samples) {
    gp32_t *g = gp32_create(NULL);
    if (!g) return NULL;
    const uint32_t mixer = GP32_RAM_BASE + 0x4000u;
    const uint32_t cursor = mixer - 0x100u;
    const uint32_t buffer = GP32_RAM_BASE + 0x8000u;
    const uint32_t object = GP32_RAM_BASE + 0xa000u;
    const uint32_t fill = GP32_RAM_BASE + 0xc000u;
    const uint32_t code[] = {
        0xe5903000u, 0xe2833001u, 0xe5803000u, 0xe5813000u, 0xe12fff1eu,
    }; /* Increment object word, copy it into the released half, BX lr. */
    direct_set_fxe_mode(g, 1u);
    direct_install_stubs(g);
    gp32_set_jit(g, 1);
    s3c2400_write32(g->soc, 0x14800004u, 0x3000u);
    s3c2400_write32(g->soc, 0x14800014u, 0u);
    g->direct_hle_sdk_sndmixer_addr = mixer;
    g->direct_hle_sdk_rate = 44100u;
    s3c2400_write32(g->soc, mixer, buffer);
    s3c2400_write32(g->soc, mixer + 4u, buffer);
    s3c2400_write32(g->soc, mixer + 8u, half_samples * 2u);
    s3c2400_write32(g->soc, mixer + 12u, half_samples * 2u);
    s3c2400_write32(g->soc, mixer + 16u, 1u);
    s3c2400_write32(g->soc, cursor, mixer + 4u);
    s3c2400_write32(g->soc, cursor + 8u, 1u); /* two bytes per sample */
    s3c2400_write32(g->soc, cursor + 12u, buffer);
    s3c2400_write32(g->soc, cursor + 24u, half_samples);
    s3c2400_write32(g->soc, cursor + 28u, object);
    s3c2400_write32(g->soc, cursor + 32u, fill);
    s3c2400_write32(g->soc, object, 0x80008000u);
    for (unsigned i = 0; i < half_samples * 2u; ++i) s3c2400_write16(g->soc, buffer + i * 2u, 0x8000u);
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(code); ++i)
        s3c2400_write32(g->soc, fill + i * 4u, code[i]);
    return g;
}

static int check_sdk_refill_slicing(uint32_t half_samples) {
    gp32_t *batch = sdk_stream_fixture(half_samples), *split = sdk_stream_fixture(half_samples);
    if (!batch || !split) { gp32_destroy(batch); gp32_destroy(split); return 0; }
    uint32_t clock = direct_run_clock_hz(batch);
    direct_sdk_sound_tick(batch, clock / 100u, clock);
    for (unsigned i = 0; i < 100u; ++i) direct_sdk_sound_tick(split, clock / 10000u, clock);
    uint64_t a_frames = 0, b_frames = 0;
    uint32_t a_rate = 0, b_rate = 0;
    const int16_t *a = s3c2400_audio_samples(batch->soc, &a_frames, &a_rate);
    const int16_t *b = s3c2400_audio_samples(split->soc, &b_frames, &b_rate);
    int ok = clock == 66000000u && a && b && a_frames == 441u && b_frames == a_frames &&
        a_rate == 44100u && b_rate == a_rate &&
        !memcmp(a, b, (size_t)a_frames * 2u * sizeof(*a)) &&
        a[half_samples * 4u] == 1 && a[half_samples * 6u] == 2 &&
        s3c2400_debug_read32(batch->soc, GP32_RAM_BASE + 0xa000u) == 0x80008000u + 441u / half_samples &&
        s3c2400_debug_read32(split->soc, GP32_RAM_BASE + 0xa000u) == 0x80008000u + 441u / half_samples;
    if (!ok) fprintf(stderr, "FAIL: SDK refill must follow buffer progress independent of cycle slicing\n");
    gp32_destroy(batch);
    gp32_destroy(split);
    return ok;
}

static int check_hle_pcm_clock_domains(void) {
    /* Equal wall time must yield the same stream under different CPU/bus
     * ratios, including the high-PLL effective instruction-budget clock. */
    const uint32_t pll[] = {0x3000u, 0x3000u, 0xe000u};
    const uint32_t div[] = {0u, 2u, 2u};
    const uint32_t run_hz[] = {66000000u, 33000000u, 48000000u};
    uint8_t sef[8u + 512u] = {0};
    for (unsigned i = 8u; i < sizeof(sef); ++i) sef[i] = (uint8_t)i;
    fpk_asset_t asset = {0};
    asset.data = sef;
    asset.size = sizeof(sef);
    int16_t reference[441u * 2u];
    for (unsigned k = 0; k < GP32_ARRAY_COUNT(pll); ++k) {
        gp32_t *g = gp32_create(NULL);
        if (!g) return 0;
        direct_set_fxe_mode(g, 1u);
        s3c2400_write32(g->soc, 0x14800004u, pll[k]);
        s3c2400_write32(g->soc, 0x14800014u, div[k]);
        g->direct_hle_audio_asset = &asset;
        g->direct_hle_audio_size = 512u;
        g->direct_hle_audio_rate = 22050u;
        const uint32_t source = GP32_RAM_BASE + 0x1000u;
        s3c2400_write32(g->soc, source, 0x40ff8000u);
        g->direct_hle_pcm_ch[0].active = 1u;
        g->direct_hle_pcm_ch[0].src_addr = source;
        g->direct_hle_pcm_ch[0].size_bytes = 4u;
        g->direct_hle_pcm_ch[0].bits = 8u;
        g->direct_hle_pcm_ch[0].rate = 11025u;
        g->direct_hle_pcm_ch[0].repeat = 1u;
        uint32_t budget = direct_run_clock_hz(g) / 100u;
        g->direct_vblank_wait_cycles = budget;
        gp32_status_t status = gp32_run_cycles(g, budget);
        uint64_t frames = 0;
        uint32_t rate = 0;
        const int16_t *pcm = s3c2400_audio_samples(g->soc, &frames, &rate);
        int ok = status == GP32_OK && direct_run_clock_hz(g) == run_hz[k] &&
            pcm && frames == 441u && rate == 44100u;
        if (ok && k == 0u) memcpy(reference, pcm, sizeof(reference));
        else if (ok) ok = !memcmp(reference, pcm, sizeof(reference));
        if (!ok) fprintf(stderr, "FAIL: HLE PCM clock ratio %u: frames=%llu rate=%u\n",
                         k, (unsigned long long)frames, rate);
        g->direct_hle_audio_asset = NULL;
        gp32_destroy(g);
        if (!ok) return 0;
    }
    return 1;
}

static int check_sdk_channel_mix(void) {
    /* Four slots: a three-sample loop, a one-shot pair, an invalid source,
     * and one silent sample. Exercise unaligned words and a partial table. */
    const int16_t want[] = {-5461, -8192, 32767, -32768, 0};
    for (unsigned edge = 0; edge < 2u; ++edge) {
        gp32_t *g = gp32_create(NULL);
        if (!g) return 0;
        uint32_t end = GP32_RAM_BASE + (uint32_t)s3c2400_ram_size(g->soc);
        uint32_t mixer = edge ? end - 76u : GP32_RAM_BASE + 0x4001u;
        uint32_t source = GP32_RAM_BASE + 0x1001u;
        uint32_t status = GP32_RAM_BASE + 0x2000u;
        const uint16_t data[] = {0u, 32768u, 65535u, 49152u, 16384u, 32768u};
        for (unsigned i = 0; i < GP32_ARRAY_COUNT(data); ++i)
            s3c2400_write16(g->soc, source + i * 2u, data[i]);
        const uint32_t channel[][5] = {
            {source, source, 3u, 3u, 1u},
            {source + 6u, source + 6u, 2u, 2u, 0u},
            {end - 1u, end - 1u, 1u, 1u, 1u},
            {source + 10u, source + 10u, 1u, 1u, 0u},
        };
        for (unsigned ch = 0; ch < 4u; ++ch)
            for (unsigned w = 0; w < 5u; ++w)
                direct_write32_if_ram(g, mixer + ch * 20u + w * 4u, channel[ch][w]);
        g->direct_hle_sdk_sndmixer_addr = mixer;
        g->direct_hle_sdk_sndsrcexist_addr = status;
        uint32_t budget = (uint32_t)(((uint64_t)direct_run_clock_hz(g) * 5u + 44099u) / 44100u);
        direct_sdk_sound_tick(g, budget, direct_run_clock_hz(g));
        uint64_t frames = 0;
        uint32_t rate = 0;
        const int16_t *pcm = s3c2400_audio_samples(g->soc, &frames, &rate);
        int ok = pcm && frames == GP32_ARRAY_COUNT(want) && rate == 44100u &&
            direct_read32_if_ram(g, status) == 1u &&
            direct_read32_if_ram(g, mixer + 4u) == source + 4u &&
            direct_read32_if_ram(g, mixer + 12u) == 1u;
        for (unsigned i = 0; ok && i < GP32_ARRAY_COUNT(want); ++i)
            ok = pcm[i * 2u] == want[i] && pcm[i * 2u + 1u] == want[i];
        for (unsigned ch = 1; ok && ch < 4u; ++ch)
            ok = !direct_read32_if_ram(g, mixer + ch * 20u) &&
                 !direct_read32_if_ram(g, mixer + ch * 20u + 4u) &&
                 !direct_read32_if_ram(g, mixer + ch * 20u + 12u);
        s3c2400_write8(g->soc, end - 1u, 0xabu);
        s3c2400_write8(g->soc, GP32_RAM_BASE, 0xcdu);
        ok = ok && direct_read_u16_if_ram(g, end - 1u) == 0x00abu &&
            direct_read_u16_if_ram(g, GP32_RAM_BASE - 1u) == 0xcd00u;
        if (!ok) fprintf(stderr, "FAIL: SDK channel mixing/cursors at RAM edge %u\n", edge);
        gp32_destroy(g);
        if (!ok) return 0;
    }
    return 1;
}

static int check_callback_clock_audio(int sdk) {
    gp32_t *g = sdk ? sdk_stream_fixture(70u) : gp32_create(NULL);
    if (!g) return 0;
    direct_set_fxe_mode(g, 1u);
    direct_install_stubs(g);
    gp32_set_jit(g, 1);
    s3c2400_write32(g->soc, 0x14800004u, 0x3000u);
    s3c2400_write32(g->soc, 0x14800014u, 0u);
    if (!sdk) {
        const uint32_t source = GP32_RAM_BASE + 0x1000u;
        s3c2400_write32(g->soc, source, 0x80808080u);
        g->direct_hle_pcm_ch[0].active = 1u;
        g->direct_hle_pcm_ch[0].src_addr = source;
        g->direct_hle_pcm_ch[0].size_bytes = 4u;
        g->direct_hle_pcm_ch[0].bits = 8u;
        g->direct_hle_pcm_ch[0].rate = 44100u;
        g->direct_hle_pcm_ch[0].repeat = 1u;
    }
    const uint32_t callback = GP32_RAM_BASE + 0x2000u;
    const uint32_t counter_fn = GP32_RAM_BASE + 0x2400u;
    const uint32_t counter = GP32_RAM_BASE + 0x2800u;
    const uint32_t change[] = {0xe59f0008u, 0xe3a01002u, 0xe5801000u, 0xe12fff1eu, 0x14800014u};
    const uint32_t count[] = {0xe59f000cu, 0xe5901000u, 0xe2811001u, 0xe5801000u, 0xe12fff1eu, counter};
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(change); ++i)
        s3c2400_write32(g->soc, callback + i * 4u, change[i]);
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(count); ++i)
        s3c2400_write32(g->soc, counter_fn + i * 4u, count[i]);
    g->direct_hle_gpos_timers_enabled = 1u;
    for (unsigned i = 0; i < 2u; ++i) {
        g->direct_hle_gpos_timer[i].configured = 1u;
        g->direct_hle_gpos_timer[i].enabled = 1u;
        g->direct_hle_gpos_timer[i].tps = 1000u;
        g->direct_hle_gpos_timer[i].callback = i ? counter_fn : callback;
    }
    const uint32_t budgets[] = {32768u, 16500u, 66000u};
    const uint32_t want[] = {21u, 22u, 45u};
    int ok = 1;
    for (unsigned step = 0; step < 3u && ok; ++step) {
        if (step < 2u) {
            g->direct_hle_gpos_timer[0].accum = direct_run_clock_hz(g) - budgets[step] * 1000u;
            s3c2400_write32(g->soc, callback + 4u, step ? 0xe3a01000u : 0xe3a01002u);
            arm920t_flush_jit(g->cpu);
        } else g->direct_hle_gpos_timer[0].enabled = 0u;
        g->direct_vblank_wait_cycles = budgets[step];
        ok = gp32_run_cycles(g, budgets[step]) == GP32_OK;
        uint64_t frames = 0;
        uint32_t rate = 0;
        (void)s3c2400_audio_samples(g->soc, &frames, &rate);
        ok = ok && frames == want[step] && rate == 44100u &&
            direct_run_clock_hz(g) == (step ? 66000000u : 33000000u) &&
            s3c2400_debug_read32(g->soc, counter) == (step == 2u ? 1u : 0u);
        if (!ok) fprintf(stderr, "FAIL: clock-changing callback SDK=%d step=%u frames=%llu\n",
                         sdk, step, (unsigned long long)frames);
        gp32_clear_audio(g);
        if (ok && step == 0u) {
            size_t size = gp32_state_size(g);
            uint8_t *state = malloc(size);
            ok = state && gp32_save_state_data(g, state, size) == GP32_OK &&
                gp32_load_state_data(g, state, size) == GP32_OK;
            free(state);
        }
    }
    gp32_destroy(g);
    return ok;
}

static int check_iis_queued_rate(void) {
    gp32_t *g = gp32_create(NULL);
    if (!g) return 0;
    s3c2400_t *s = g->soc;
    s3c2400_audio_append_s16_stereo(s, 1234, -5678, 11025u);
    s3c2400_write32(s, 0x15508004u, 0u);
    s3c2400_write32(s, 0x15508008u, 3u << 5); /* Configure 46,875 Hz. */
    s3c2400_write32(s, 0x15508000u, 1u);
    s3c2400_tick(s, 4096u); /* No DMA source: no new PCM. */
    uint64_t frames;
    uint32_t rate;
    const int16_t *pcm = s3c2400_audio_samples(s, &frames, &rate);
    int ok = frames == 1u && rate == 11025u && pcm[0] == 1234 && pcm[1] == -5678;
    if (!ok) fprintf(stderr, "FAIL: IIS setup retags queued PCM: frames=%llu rate=%u\n", (unsigned long long)frames, rate);
    s3c2400_audio_clear(s);
    s3c2400_write16(s, 0x15508010u, 1234u);
    s3c2400_audio_samples(s, &frames, &rate);
    if (frames != 0u) ok = 0; /* A halfword alone is not a stereo frame. */
    s3c2400_write16(s, 0x15508010u, (uint16_t)-5678);
    pcm = s3c2400_audio_samples(s, &frames, &rate);
    if (frames != 1u || rate != 46875u || pcm[0] != 1234 || pcm[1] != -5678) {
        fputs("FAIL: completed IIS FIFO frame must carry the configured rate\n", stderr);
        ok = 0;
    }
    gp32_destroy(g);
    return ok;
}

static int check_iis_dma_word_order(void) {
    gp32_t *g = gp32_create(NULL);
    if (!g) return 0;
    s3c2400_t *s = g->soc;
    int16_t reference[6];
    int ok = 1;
    for (unsigned pending = 0; pending < 2u; ++pending) {
        for (unsigned fast = 0; fast < 2u; ++fast) {
            s3c2400_reset(s);
            s3c2400_write32(s, 0x15508008u, 3u << 5);
            s3c2400_write32(s, GP32_RAM_BASE, 0xabcd1234u);
            s3c2400_write32(s, GP32_RAM_BASE + 4u, 0xef015678u);
            if (pending) s3c2400_write16(s, 0x15508010u, 0x1122u);
            uint32_t dma = 0x14600000u + (fast ? 0x40u : 0u);
            s3c2400_write32(s, dma, GP32_RAM_BASE);
            s3c2400_write32(s, dma + 4u, 0x35508010u);
            s3c2400_write32(s, dma + 8u, 0x04600002u); /* Whole-service, stop, 32-bit, tc=2. */
            s3c2400_write32(s, dma + 24u, 3u); /* Software request. */
            if (pending) s3c2400_write16(s, 0x15508010u, 0x3344u);
            uint64_t frames;
            uint32_t rate;
            const int16_t *pcm = s3c2400_audio_samples(s, &frames, &rate);
            if (frames != 2u + pending || rate != 46875u) { ok = 0; break; }
            if (!fast) memcpy(reference, pcm, (size_t)frames * 2u * sizeof(*pcm));
            else if (memcmp(reference, pcm, (size_t)frames * 2u * sizeof(*pcm))) {
                fprintf(stderr, "FAIL: IIS fast DMA word order differs from bus writes, pending=%u\n", pending);
                ok = 0;
            }
        }
    }
    gp32_destroy(g);
    return ok;
}

/* Channel 0 uses ordinary FIFO writes; channel 2 may bulk-copy PCM. Compare
 * their output and DMA cursors, including the next FIFO write's pairing. */
static int check_iis_dma_halfword_copy(void) {
    const uint32_t counts[] = {2u, 3u, 31u, 32u};
    gp32_t *g = gp32_create(NULL);
    if (!g) return 0;
    int ok = 1;
    for (unsigned shape = 0; ok && shape < 8u; ++shape)
        for (unsigned n = 0; ok && n < GP32_ARRAY_COUNT(counts); ++n) {
            unsigned pending = shape & 1u, fixed = (shape >> 1) & 1u;
            uint32_t source = GP32_RAM_BASE + ((shape & 4u) ? 0x7ffffbu : 0x1001u);
            int16_t reference[36u * 2u];
            uint32_t cursor[4];
            for (unsigned fast = 0; ok && fast < 2u; ++fast) {
                s3c2400_t *s = g->soc;
                s3c2400_reset(s);
                s3c2400_write32(s, 0x14800004u, 0u);
                s3c2400_write32(s, 0x14800014u, 0u);
                s3c2400_write32(s, 0x15508008u, 3u << 5);
                for (uint32_t i = 0; i < 70u && source + i < GP32_RAM_BASE + 0x800000u; ++i)
                    s3c2400_write8(s, source + i, (uint8_t)(i * 71u + 29u));
                if (pending) s3c2400_write16(s, 0x15508010u, 0x8123u);
                uint32_t dma = 0x14600000u + (fast ? 0x40u : 0u);
                s3c2400_write32(s, dma, source | (fixed ? 1u << 29 : 0u));
                s3c2400_write32(s, dma + 4u, 0x35508010u);
                s3c2400_write32(s, dma + 8u, 0x04500000u | counts[n]);
                s3c2400_write32(s, dma + 24u, 3u);
                s3c2400_write16(s, 0x15508010u, 0x3456u);
                s3c2400_write16(s, 0x15508010u, 0xa987u);
                uint64_t frames;
                uint32_t rate;
                const int16_t *pcm = s3c2400_audio_samples(s, &frames, &rate);
                ok = frames == (counts[n] + pending + 2u) / 2u && rate == 46875u;
                for (unsigned r = 0; ok && r < 4u; ++r) {
                    uint32_t v = s3c2400_read32(s, dma + 12u + r * 4u);
                    if (!fast) cursor[r] = v;
                    else ok = cursor[r] == v;
                }
                if (ok && !fast) memcpy(reference, pcm, (size_t)frames * 2u * sizeof(*pcm));
                else if (ok) ok = !memcmp(reference, pcm, (size_t)frames * 2u * sizeof(*pcm));
            }
        }
    gp32_destroy(g);
    if (!ok) fputs("FAIL: IIS halfword DMA differs from ordinary FIFO writes\n", stderr);
    return ok;
}

static int check_iis_dma_reload_slicing(void) {
    const uint32_t counts[] = {3u, 96u};
    int ok = 1;
    for (unsigned mode = 0; ok && mode < 4u; ++mode)
        for (unsigned n = 0; ok && n < GP32_ARRAY_COUNT(counts); ++n) {
            s3c2400_t *s[2] = {s3c2400_create(0x800000u), s3c2400_create(0x800000u)};
            if (!s[0] || !s[1]) { s3c2400_destroy(s[0]); s3c2400_destroy(s[1]); return 0; }
            for (unsigned split = 0; split < 2u; ++split) {
                s3c2400_t *soc = s[split];
                s3c2400_write32(soc, 0x14800004u, 0u);
                s3c2400_write32(soc, 0x14800014u, 0u);
                s3c2400_write32(soc, 0x15508008u, 3u << 5);
                for (uint32_t i = 0; i < 96u; ++i)
                    s3c2400_write16(soc, GP32_RAM_BASE + i * 2u, (uint16_t)(i * 1537u));
                s3c2400_write32(soc, 0x14600040u, GP32_RAM_BASE);
                s3c2400_write32(soc, 0x14600044u, 0x35508010u);
                /* IRQ enabled; single/whole service and auto-reload/stop. */
                s3c2400_write32(soc, 0x14600048u, 0x10900000u | counts[n] |
                    ((mode & 1u) ? 1u << 26 : 0u) | ((mode & 2u) ? 1u << 22 : 0u));
                s3c2400_write32(soc, 0x14600058u, 2u);
                s3c2400_write32(soc, 0x15508000u, 1u);
                if (!split) s3c2400_tick(soc, 211u * 1024u + 511u);
                else {
                    for (unsigned i = 0; i < 211u; ++i) s3c2400_tick(soc, 1024u);
                    s3c2400_tick(soc, 511u);
                }
                s3c2400_tick(soc, 513u); /* carry the partial final period */
                s3c2400_write16(soc, 0x15508010u, 0x4567u);
                s3c2400_write16(soc, 0x15508010u, 0x89abu);
            }
            uint64_t frames[2]; uint32_t rates[2];
            const int16_t *a = s3c2400_audio_samples(s[0], &frames[0], &rates[0]);
            const int16_t *b = s3c2400_audio_samples(s[1], &frames[1], &rates[1]);
            ok = frames[0] && frames[0] == frames[1] && rates[0] == rates[1] &&
                !memcmp(a, b, (size_t)frames[0] * 2u * sizeof(*a));
            for (uint32_t off = 12u; ok && off <= 24u; off += 4u)
                ok = s3c2400_read32(s[0], 0x14600040u + off) == s3c2400_read32(s[1], 0x14600040u + off);
            ok &= s3c2400_read32(s[0], 0x14400000u) == s3c2400_read32(s[1], 0x14400000u);
            s3c2400_destroy(s[0]); s3c2400_destroy(s[1]);
        }
    if (!ok) fputs("FAIL: IIS reload batching changes PCM, DMA or IRQ state\n", stderr);
    return ok;
}

static int check_mixed_rate_queue(void) {
    gp32_t *mixed = gp32_create(NULL);
    if (!mixed) return 0;
    s3c2400_audio_append_s16_stereo(mixed->soc, 1, -1, 11025u);
    s3c2400_audio_append_s16_stereo(mixed->soc, 2, -2, 11025u);
    s3c2400_audio_append_s16_stereo(mixed->soc, 3, -3, 22050u);
    gp32_audio_desc_t span;
    if (gp32_get_audio(mixed, &span) != GP32_OK || span.frame_count != 2u || span.sample_rate_hz != 11025u) {
        fputs("FAIL: mixed-rate queue must expose its original first span\n", stderr);
        gp32_destroy(mixed);
        return 0;
    }
    int ok = gp32_consume_audio(mixed, 3u) == GP32_ERR_INVALID_ARGUMENT;
    ok &= gp32_consume_audio(mixed, 1u) == GP32_OK;
    ok &= gp32_get_audio(mixed, &span) == GP32_OK && span.frame_count == 1u &&
          span.sample_rate_hz == 11025u && span.samples_s16_interleaved[0] == 2;
    /* Persist unread data, not the already consumed prefix. Component state
     * retains PCM; a complete machine load intentionally discards host audio. */
    state_io_t count = state_io_counter();
    ok &= s3c2400_state_save_io(mixed->soc, &count);
    uint8_t *data = malloc(count.pos);
    if (!data) { gp32_destroy(mixed); return 0; }
    state_io_t writer = state_io_writer(data, count.pos);
    ok &= s3c2400_state_save_io(mixed->soc, &writer);
    gp32_clear_audio(mixed);
    state_io_t reader = state_io_reader(data, count.pos);
    ok &= s3c2400_state_load_io(mixed->soc, &reader, 1);
    free(data);
    ok &= gp32_get_audio(mixed, &span) == GP32_OK && span.frame_count == 1u &&
          span.sample_rate_hz == 11025u && span.samples_s16_interleaved[0] == 2;
    ok &= gp32_consume_audio(mixed, 1u) == GP32_OK;
    ok &= gp32_get_audio(mixed, &span) == GP32_OK && span.frame_count == 1u &&
          span.sample_rate_hz == 22050u && span.samples_s16_interleaved[0] == 3;
    /* Refill after partial consumption forces PCM compaction (loaded capacity
     * is only two frames) and then repeated rate metadata reuse. */
    s3c2400_audio_append_s16_stereo(mixed->soc, 4, -4, 22050u);
    s3c2400_audio_append_s16_stereo(mixed->soc, 5, -5, 44100u);
    ok &= gp32_get_audio(mixed, &span) == GP32_OK && span.frame_count == 2u &&
          span.sample_rate_hz == 22050u && span.samples_s16_interleaved[2] == 4;
    ok &= gp32_consume_audio(mixed, 2u) == GP32_OK;
    for (unsigned i = 0; i < 32u; ++i) {
        uint32_t rate = i & 1u ? 44100u : 11025u;
        s3c2400_audio_append_s16_stereo(mixed->soc, (int16_t)(6u + i), 0, rate);
        ok &= gp32_consume_audio(mixed, 1u) == GP32_OK;
        ok &= gp32_get_audio(mixed, &span) == GP32_OK && span.frame_count == 1u &&
              span.sample_rate_hz == rate && span.samples_s16_interleaved[0] == (int16_t)(6u + i);
    }
    gp32_clear_audio(mixed);
    ok &= gp32_get_audio(mixed, &span) == GP32_OK && span.frame_count == 0;
    gp32_destroy(mixed);
    if (!ok) fputs("FAIL: rate spans, consumption, state or refill\n", stderr);
    return ok;
}

int main(void) {
    if (!check_mixed_rate_queue()) return 1;
    int queued_rate_ok = check_iis_queued_rate();
    int dma_order_ok = check_iis_dma_word_order();
    if (!queued_rate_ok || !dma_order_ok || !check_iis_dma_halfword_copy() ||
        !check_iis_dma_reload_slicing()) return 1;
    if (!check_callback_clock_audio(0) || !check_callback_clock_audio(1)) return 1;
    if (!check_sdk_channel_mix()) return 1;
    if (!check_hle_pcm_clock_domains()) return 1;
    if (!check_sdk_refill_slicing(32u) || !check_sdk_refill_slicing(64u) ||
        !check_sdk_refill_slicing(70u)) return 1;
    gp32_t *g = gp32_create(NULL);
    if (!g) return 2;
    s3c2400_audio_clear(g->soc);
    s3c2400_audio_append_u8_mono(g->soc, input, GP32_ARRAY_COUNT(input), 11025u);
    uint64_t frames = 0;
    uint32_t rate = 0;
    const int16_t *pcm = s3c2400_audio_samples(g->soc, &frames, &rate);
    if (!pcm || frames != GP32_ARRAY_COUNT(input) || rate != 11025u) return 1;
    for (size_t i = 0; i < GP32_ARRAY_COUNT(input); ++i)
        if (pcm[i * 2u] != expected[i] || pcm[i * 2u + 1u] != expected[i]) return 1;

    uint8_t sef[8u + sizeof(input)] = {0};
    memcpy(sef + 8u, input, sizeof(input));
    fpk_asset_t asset = {0};
    asset.data = sef;
    asset.size = sizeof(sef);
    g->direct_hle_audio_asset = &asset;
    g->direct_hle_audio_size = sizeof(input);
    g->direct_hle_audio_rate = 11025u;
    for (size_t i = 0; i < GP32_ARRAY_COUNT(input); ++i) {
        int32_t left = 0, right = 0;
        if (!direct_hle_sef_sample(g, 11025u, &left, &right) ||
            left != expected[i] || right != expected[i]) return 1;
    }
    g->direct_hle_audio_asset = NULL;

    for (unsigned stereo = 0; stereo <= 1u; ++stereo) {
        for (size_t i = 0; i < GP32_ARRAY_COUNT(input); ++i) {
            s3c2400_write8(g->soc, GP32_RAM_BASE + 0x1000u + (uint32_t)i * (1u + stereo), input[i]);
            if (stereo)
                s3c2400_write8(g->soc, GP32_RAM_BASE + 0x1001u + (uint32_t)i * 2u, input[GP32_ARRAY_COUNT(input) - 1u - i]);
        }
        memset(&g->direct_hle_pcm_ch[0], 0, sizeof(g->direct_hle_pcm_ch[0]));
        g->direct_hle_pcm_ch[0].active = 1u;
        g->direct_hle_pcm_ch[0].src_addr = GP32_RAM_BASE + 0x1000u;
        g->direct_hle_pcm_ch[0].size_bytes = sizeof(input) * (1u + stereo);
        g->direct_hle_pcm_ch[0].bits = 8u;
        g->direct_hle_pcm_ch[0].stereo = stereo;
        g->direct_hle_pcm_ch[0].rate = 11025u;
        for (size_t i = 0; i < GP32_ARRAY_COUNT(input); ++i) {
            int32_t left = 0, right = 0;
            int16_t want_right = expected[stereo ? GP32_ARRAY_COUNT(input) - 1u - i : i];
            if (!direct_pcm_channel_sample(g, 0u, 11025u, &left, &right) ||
                left != expected[i] || right != want_right) return 1;
        }
    }
    gp32_destroy(g);
    puts("PASS: U8 PCM endpoints, midpoint and channel polarity (SoC/SEF/mono/stereo)");
    return 0;
}
