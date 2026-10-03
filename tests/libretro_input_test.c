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
    puts("PASS: input press/release observed by ARM GPIO program before same-run video callback (jit off/on)");
    return 0;
}
