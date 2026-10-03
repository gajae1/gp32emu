/* A synthetic ARM program reads the real GP32 GPIO input into RAM. Verify
 * press/release reaches that program before the same retro_run presents video.
 * This measures core scheduling only, not controller-to-screen latency. */
#include "../src/libretro/libretro.c"
#include "common.h"

static unsigned pressed, polled, presented;
static uint32_t observed_gpio;
static void poll_input(void) { ++polled; }
static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
    return port == 0 && device == RETRO_DEVICE_JOYPAD && index == 0 &&
           id == RETRO_DEVICE_ID_JOYPAD_A ? (int16_t)pressed : 0;
}
static void present(const void *data, unsigned width, unsigned height, size_t pitch) {
    (void)data; (void)width; (void)height; (void)pitch;
    observed_gpio = gp32_debug_read32(emu, 0x0c000000u);
    ++presented;
}

/* Optional joypad bitmask negotiation. Frontend modes: 0 = does not know
 * RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, 1 = handles it but reports no bitmask
 * support, 2 = bitmask-capable. */
static int frontend_bitmask_mode;
static unsigned bitmask_queries, individual_queries;
static uint16_t simulated_mask;

static bool bitmask_env(unsigned cmd, void *data) {
    if (cmd != RETRO_ENVIRONMENT_GET_INPUT_BITMASKS) return false;
    if (frontend_bitmask_mode == 0) return false;
    if (data) *(bool *)data = (frontend_bitmask_mode == 2);
    return true;
}

/* Feeds the same simulated pad through both protocols so the supported and
 * fallback paths must reach the guest with identical GPIO. */
static int16_t bitmask_input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (port != 0 || device != RETRO_DEVICE_JOYPAD || index != 0) return 0;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) { ++bitmask_queries; return (int16_t)simulated_mask; }
    ++individual_queries;
    return id < 16u ? (int16_t)((simulated_mask >> id) & 1u) : 0;
}

/* GP32 GPIO input register 0x1560000c, active low, as stored in RAM by the
 * synthetic program. Unmapped pad ids must leave every bit untouched. */
static const struct { unsigned id; uint32_t in0_bit; } pad_gpio[] = {
    { RETRO_DEVICE_ID_JOYPAD_LEFT, 0x0100u },
    { RETRO_DEVICE_ID_JOYPAD_DOWN, 0x0200u },
    { RETRO_DEVICE_ID_JOYPAD_RIGHT, 0x0400u },
    { RETRO_DEVICE_ID_JOYPAD_UP, 0x0800u },
    { RETRO_DEVICE_ID_JOYPAD_L, 0x1000u },
    { RETRO_DEVICE_ID_JOYPAD_B, 0x2000u },
    { RETRO_DEVICE_ID_JOYPAD_A, 0x4000u },
    { RETRO_DEVICE_ID_JOYPAD_R, 0x8000u }
};

static uint32_t expected_in0(uint16_t mask) {
    uint32_t v = 0xffffu;
    for (unsigned i = 0; i < sizeof(pad_gpio) / sizeof(pad_gpio[0]); ++i)
        if (mask & (1u << pad_gpio[i].id)) v &= ~pad_gpio[i].in0_bit;
    return v;
}

static int check_pad(int mode, int jit, uint16_t mask) {
    simulated_mask = mask;
    bitmask_queries = individual_queries = polled = presented = 0;
    observed_gpio = 0;
    retro_run();
    uint32_t want = expected_in0(mask) & 0xff00u;
    if (polled != 1 || presented != 1 || (observed_gpio & 0xff00u) != want) {
        fprintf(stderr, "FAIL: bitmask mode=%d jit=%d pad=%04x gpio=%08x want=%08x poll=%u video=%u\n",
                mode, jit, mask, observed_gpio & 0xff00u, want, polled, presented);
        return 0;
    }
    if (mode == 2 ? (bitmask_queries != 1 || individual_queries != 0)
                  : (bitmask_queries != 0 || individual_queries != 10)) {
        fprintf(stderr, "FAIL: bitmask mode=%d jit=%d pad=%04x mask_queries=%u id_queries=%u\n",
                mode, jit, mask, bitmask_queries, individual_queries);
        return 0;
    }
    /* START/SELECT are exposed through the second GPIO input register. */
    uint32_t in1 = gp32_debug_read32(emu, 0x15600030u);
    int start = (mask & (1u << RETRO_DEVICE_ID_JOYPAD_START)) != 0;
    int select = (mask & (1u << RETRO_DEVICE_ID_JOYPAD_SELECT)) != 0;
    if (((in1 & 0x40u) == 0) != start || ((in1 & 0x80u) == 0) != select) {
        fprintf(stderr, "FAIL: bitmask mode=%d jit=%d pad=%04x gpio_in1=%08x start=%d select=%d\n",
                mode, jit, mask, in1, start, select);
        return 0;
    }
    return 1;
}

int main(void) {
    const uint32_t program[] = {
        0xe59f000cu, /* LDR r0,=GPIO */
        0xe59f100cu, /* LDR r1,=RAM */
        0xe5902000u, /* loop: LDR r2,[r0] */
        0xe5812000u, /* STR r2,[r1] */
        0xeafffffcu, /* B loop */
        0x1560000cu, 0x0c000000u
    };
    uint8_t bios[sizeof(program)];
    for (unsigned i = 0; i < sizeof(program) / sizeof(program[0]); ++i)
        gp32_st32le(bios + i * 4u, program[i]);
    for (int jit = 0; jit <= 1; ++jit) {
        retro_init();
        retro_set_input_poll(poll_input);
        retro_set_input_state(input_state);
        retro_set_video_refresh(present);
        emu = gp32_create(NULL);
        if (!emu || gp32_load_bios_data(emu, bios, sizeof(bios)) != GP32_OK) return 2;
        gp32_set_jit(emu, jit);
        const unsigned sequence[] = {0, 1, 0, 1, 0};
        for (unsigned frame = 0; frame < sizeof(sequence) / sizeof(sequence[0]); ++frame) {
            pressed = sequence[frame]; polled = presented = 0;
            observed_gpio = 0;
            retro_run();
            if (polled != 1 || presented != 1 ||
                ((observed_gpio & 0x4000u) == 0) != (pressed != 0)) {
                fprintf(stderr, "FAIL: input jit=%d frame=%u press=%u gpio=%08x poll=%u video=%u\n",
                        jit, frame, pressed, observed_gpio, polled, presented);
                retro_deinit(); return 1;
            }
        }
        retro_deinit();
    }
    /* State-load lifecycle: a pad mask captured inside a serialized state is
     * host input, not game input. The first retro_run after retro_unserialize
     * must apply the frontend's live pad, so a stored press can never leak
     * into gameplay after a load. */
    for (int jit = 0; jit <= 1; ++jit) {
        retro_init();
        retro_set_input_poll(poll_input);
        retro_set_input_state(input_state);
        retro_set_video_refresh(present);
        emu = gp32_create(NULL);
        if (!emu || gp32_load_bios_data(emu, bios, sizeof(bios)) != GP32_OK) return 2;
        gp32_set_jit(emu, jit);
        pressed = 1; polled = presented = 0; observed_gpio = 0;
        retro_run();
        if ((observed_gpio & 0x4000u) != 0) {
            fprintf(stderr, "FAIL: input lifecycle jit=%d pre-save press missed gpio=%08x\n",
                    jit, observed_gpio);
            retro_deinit(); return 1;
        }
        size_t st_size = retro_serialize_size();
        uint8_t *st = st_size ? (uint8_t *)malloc(st_size) : NULL;
        if (!st || !retro_serialize(st, st_size)) {
            fprintf(stderr, "FAIL: input lifecycle jit=%d serialize failed size=%zu\n", jit, st_size);
            free(st); retro_deinit(); return 1;
        }
        pressed = 0;
        retro_run();
        if (!retro_unserialize(st, st_size)) {
            fprintf(stderr, "FAIL: input lifecycle jit=%d unserialize failed\n", jit);
            free(st); retro_deinit(); return 1;
        }
        free(st);
        pressed = 0; polled = presented = 0; observed_gpio = 0;
        retro_run();
        if (polled != 1 || presented != 1 || (observed_gpio & 0x4000u) == 0) {
            fprintf(stderr, "FAIL: input lifecycle jit=%d stored press leaked post-load gpio=%08x poll=%u video=%u\n",
                    jit, observed_gpio, polled, presented);
            retro_deinit(); return 1;
        }
        pressed = 1; polled = presented = 0; observed_gpio = 0;
        retro_run();
        if (polled != 1 || presented != 1 || (observed_gpio & 0x4000u) != 0) {
            fprintf(stderr, "FAIL: input lifecycle jit=%d post-load press missed gpio=%08x poll=%u video=%u\n",
                    jit, observed_gpio, polled, presented);
            retro_deinit(); return 1;
        }
        retro_deinit();
    }
    /* A frontend that negotiates RETRO_ENVIRONMENT_GET_INPUT_BITMASKS is polled
     * with one mask query per retro_run; one that answers "no", or does not
     * know the command, keeps the individual per-button queries. Both must
     * produce identical GPIO, and re-negotiation must not leak a previous
     * mode's fast path into the next session. */
    static const uint16_t pads[] = {
        0x0000u,
        (uint16_t)(1u << RETRO_DEVICE_ID_JOYPAD_A),
        (uint16_t)((1u << RETRO_DEVICE_ID_JOYPAD_L) | (1u << RETRO_DEVICE_ID_JOYPAD_R)),
        (uint16_t)((1u << RETRO_DEVICE_ID_JOYPAD_A) | (1u << RETRO_DEVICE_ID_JOYPAD_B) |
                   (1u << RETRO_DEVICE_ID_JOYPAD_L) | (1u << RETRO_DEVICE_ID_JOYPAD_R) |
                   (1u << RETRO_DEVICE_ID_JOYPAD_START) | (1u << RETRO_DEVICE_ID_JOYPAD_SELECT) |
                   (1u << RETRO_DEVICE_ID_JOYPAD_UP) | (1u << RETRO_DEVICE_ID_JOYPAD_DOWN) |
                   (1u << RETRO_DEVICE_ID_JOYPAD_LEFT) | (1u << RETRO_DEVICE_ID_JOYPAD_RIGHT)),
        /* Unmapped ids, including the sign-extended bit 15, must be ignored. */
        (uint16_t)((1u << RETRO_DEVICE_ID_JOYPAD_X) | (1u << RETRO_DEVICE_ID_JOYPAD_L2) |
                   (1u << RETRO_DEVICE_ID_JOYPAD_R2) | (1u << RETRO_DEVICE_ID_JOYPAD_L3) |
                   (1u << RETRO_DEVICE_ID_JOYPAD_R3))
    };
    static const int modes[] = { 2, 0, 1 }; /* supported first, then re-negotiation */
    for (unsigned mi = 0; mi < sizeof(modes) / sizeof(modes[0]); ++mi) {
        for (int jit = 0; jit <= 1; ++jit) {
            frontend_bitmask_mode = modes[mi];
            retro_set_environment(bitmask_env);
            retro_init();
            retro_set_input_poll(poll_input);
            retro_set_input_state(bitmask_input_state);
            retro_set_video_refresh(present);
            emu = gp32_create(NULL);
            if (!emu || gp32_load_bios_data(emu, bios, sizeof(bios)) != GP32_OK) return 2;
            gp32_set_jit(emu, jit);
            for (unsigned p = 0; p < sizeof(pads) / sizeof(pads[0]); ++p) {
                if (!check_pad(modes[mi], jit, pads[p])) { retro_deinit(); return 1; }
            }
            retro_deinit();
        }
    }
    puts("PASS: input press/release observed by ARM GPIO program before same-run video callback (jit off/on)");
    puts("PASS: joypad bitmask negotiation (1 mask query when supported, 10 per-button queries otherwise) keeps GPIO identical");
    return 0;
}
