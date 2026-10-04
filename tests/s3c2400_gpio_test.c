/* Real GPIO transactions against a synthetic NAND image. Reads of the data
 * latch must not advance NAND; each read-control activation must do so once. */
#include "s3c2400.h"
#include "gp32emu/gp32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GPIO 0x15600000u
static s3c2400_t *soc;
static arm_bus_t bus;
static unsigned width;
static int failures;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s (%u-bit)\n", m, width); ++failures; } } while (0)

static void write_port(unsigned off, uint32_t value) {
    if (width == 16) bus.write16(bus.user, GPIO + off, (uint16_t)value);
    else bus.write32(bus.user, GPIO + off, value);
}
static void send_byte(unsigned latch, uint8_t value) {
    write_port(0x08, 1);              /* output direction */
    write_port(0x30, latch | 8);      /* write inactive */
    bus.write8(bus.user, GPIO + 0x0c, value);
    write_port(0x30, latch);          /* one NAND write */
    write_port(0x30, latch | 8);
}
static void address(unsigned col, unsigned page) {
    send_byte(0x10, (uint8_t)col);
    send_byte(0x10, (uint8_t)page);
    send_byte(0x10, (uint8_t)(page >> 8));
}
static void begin_read(void) {
    write_port(0x30, 8);              /* neither command nor address latch */
    write_port(0x08, 0);              /* input direction */
}
static uint8_t read_byte(void) {
    write_port(0x24, 0x40);          /* selected, read active */
    uint32_t first = bus.read32(bus.user, GPIO + 0x0c);
    CHECK(bus.read32_io(bus.user, GPIO + 0x0c) == first,
          "specialized and ordinary GPIO reads must agree without advancing NAND");
    CHECK(bus.read8(bus.user, GPIO + 0x0c) == (uint8_t)first,
          "byte read must observe the same data latch");
    write_port(0x24, 0x140);         /* read inactive */
    return (uint8_t)first;
}

/* Host-button input is one mask write per frame while games poll
 * GPBDAT/GPEDAT per read. Both the ordinary and specialized IO read paths
 * must observe identical active-low bits, and a state save/load must restore
 * the mask that was live at save time. Expected bits mirror the GP32 pad
 * wiring, computed independently of the implementation. */
static uint32_t want_bdat(uint32_t mask) {
    static const struct { uint32_t pad, bit; } map[] = {
        { GP32_BUTTON_LEFT, 0x0100u }, { GP32_BUTTON_DOWN, 0x0200u },
        { GP32_BUTTON_RIGHT, 0x0400u }, { GP32_BUTTON_UP, 0x0800u },
        { GP32_BUTTON_L, 0x1000u }, { GP32_BUTTON_B, 0x2000u },
        { GP32_BUTTON_A, 0x4000u }, { GP32_BUTTON_R, 0x8000u }
    };
    uint32_t v = 0xff00u;
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); ++i)
        if (mask & map[i].pad) v &= ~map[i].bit;
    return v;
}
static uint32_t want_edat(uint32_t mask) {
    uint32_t v = 0xc0u;
    if (mask & GP32_BUTTON_START)  v &= ~0x40u;
    if (mask & GP32_BUTTON_SELECT) v &= ~0x80u;
    return v;
}
static void check_button_mask(uint32_t mask) {
    char msg[96];
    s3c2400_set_buttons(soc, mask);
    snprintf(msg, sizeof(msg), "GPBDAT buttons mask %08lx ordinary vs fastIO vs expected",
             (unsigned long)mask);
    CHECK((bus.read32(bus.user, GPIO + 0x0c) & 0xff00u) == want_bdat(mask) &&
          (bus.read32_io(bus.user, GPIO + 0x0c) & 0xff00u) == want_bdat(mask), msg);
    snprintf(msg, sizeof(msg), "GPEDAT buttons mask %08lx ordinary vs fastIO vs expected",
             (unsigned long)mask);
    CHECK((bus.read32(bus.user, GPIO + 0x30) & 0xc0u) == want_edat(mask) &&
          (bus.read32_io(bus.user, GPIO + 0x30) & 0xc0u) == want_edat(mask), msg);
    CHECK(bus.read8(bus.user, GPIO + 0x0d) == (uint8_t)(want_bdat(mask) >> 8),
          "GPBDAT high byte must match on byte reads");
}

int main(void) {
    uint8_t image[528u * 128u];
    memset(image, 0xff, sizeof(image));
    image[3u * 528u + 8u] = 0x96;
    for (width = 16; width <= 32; width += 16) {
        char error[128] = {0};
        soc = s3c2400_create(0);
        if (!soc || !s3c2400_load_smartmedia_buffer(soc, image, sizeof(image), error, sizeof(error))) return 2;
        bus = s3c2400_get_bus(soc);
        write_port(0x24, 0x140);
        write_port(0x30, 8);
        CHECK(!(bus.read32(bus.user, GPIO + 0x30) & 4), "loaded card must be present");
        send_byte(0x20, 0x90);       /* READ ID */
        send_byte(0x10, 0);
        begin_read();
        CHECK(read_byte() == 0xec && read_byte() == 0xe6, "NAND manufacturer/device ID");
        send_byte(0x20, 0x80);       /* PROGRAM */
        address(7, 3);
        send_byte(0, 0x42);
        send_byte(0x20, 0x10);       /* confirm */
        send_byte(0x20, 0x00);       /* READ */
        address(7, 3);
        begin_read();
        CHECK(read_byte() == 0x42, "programmed byte must survive a GPIO command/address roundtrip");
        CHECK(read_byte() == 0x96, "next read must advance exactly one NAND byte");
        write_port(0x24, 0x1c0);    /* deselect */
        CHECK(bus.read32(bus.user, GPIO + 0x24) & 0x80, "deselect must reset chip signals");
        s3c2400_destroy(soc);
    }
    width = 32;
    soc = s3c2400_create(0);
    if (!soc) return 2;
    bus = s3c2400_get_bus(soc);
    static const uint32_t masks[] = {
        0u, GP32_BUTTON_A,
        GP32_BUTTON_LEFT | GP32_BUTTON_UP | GP32_BUTTON_L,
        GP32_BUTTON_RIGHT | GP32_BUTTON_DOWN | GP32_BUTTON_B | GP32_BUTTON_R,
        GP32_BUTTON_START | GP32_BUTTON_SELECT,
        GP32_BUTTON_A | GP32_BUTTON_B | GP32_BUTTON_L | GP32_BUTTON_R |
            GP32_BUTTON_START | GP32_BUTTON_SELECT | GP32_BUTTON_UP |
            GP32_BUTTON_DOWN | GP32_BUTTON_LEFT | GP32_BUTTON_RIGHT
    };
    for (unsigned i = 0; i < sizeof(masks) / sizeof(masks[0]); ++i)
        check_button_mask(masks[i]);
    /* Host-owned input survives reset and must keep feeding both ports. */
    s3c2400_reset(soc);
    CHECK((bus.read32_io(bus.user, GPIO + 0x0c) & 0xff00u) == want_bdat(masks[5]) &&
          (bus.read32_io(bus.user, GPIO + 0x30) & 0xc0u) == want_edat(masks[5]),
          "pressed buttons must still read back after reset");
    /* Save with everything pressed, release, restore: the reads must
     * reproduce the save-time mask, then live input must work again. */
    FILE *state = tmpfile();
    CHECK(state != NULL, "state scratch file");
    if (state) {
        CHECK(s3c2400_state_save(soc, state), "state save");
        s3c2400_set_buttons(soc, 0u);
        CHECK((bus.read32(bus.user, GPIO + 0x0c) & 0xff00u) == 0xff00u &&
              (bus.read32(bus.user, GPIO + 0x30) & 0xc0u) == 0xc0u,
              "release must clear every button bit");
        rewind(state);
        CHECK(s3c2400_state_load(soc, state), "state load");
        fclose(state);
        CHECK((bus.read32(bus.user, GPIO + 0x0c) & 0xff00u) == want_bdat(masks[5]) &&
              (bus.read32_io(bus.user, GPIO + 0x0c) & 0xff00u) == want_bdat(masks[5]) &&
              (bus.read32(bus.user, GPIO + 0x30) & 0xc0u) == want_edat(masks[5]) &&
              (bus.read32_io(bus.user, GPIO + 0x30) & 0xc0u) == want_edat(masks[5]),
              "restored state must replay the save-time button mask on both read paths");
        check_button_mask(GP32_BUTTON_SELECT);
    }
    s3c2400_destroy(soc);
    if (failures) return 1;
    puts("PASS: GPIO NAND ID, program/read, latched reads, halfword/word control writes, button parity+state");
    return 0;
}
