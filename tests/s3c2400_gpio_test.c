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
static const arm_live_read32_t *live_reads;
static size_t live_read_count;
static uint32_t live_pa[2];
static const volatile uint32_t *live_word[2];
static unsigned width;
static int failures;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s (%u-bit)\n", m, width); ++failures; } } while (0)

/* The CPU loads the advertised live words instead of issuing GPIO reads, so
 * each mirror must equal the ordinary and specialized reads at every point.
 * Descriptor fields are snapshotted at registration, like the CPU does. */
static void live_begin(void) {
    live_read_count = 0;
    live_reads = s3c2400_live_read32(soc, &live_read_count);
    CHECK(live_reads != NULL && live_read_count == 2, "two live readback descriptors");
    if (!live_reads || live_read_count != 2) { live_reads = NULL; live_read_count = 0; return; }
    CHECK(live_reads[0].pa == GPIO + 0x0cu && live_reads[1].pa == GPIO + 0x30u,
          "live descriptors must cover GPBDAT 0x1560000c and GPEDAT 0x15600030");
    CHECK(live_reads[0].word != NULL && live_reads[1].word != NULL,
          "live descriptor words must point at SoC-owned storage");
    for (unsigned i = 0; i < live_read_count; ++i) {
        live_pa[i] = live_reads[i].pa;
        live_word[i] = live_reads[i].word;
    }
}
static void check_live_descriptors(void) {
    if (live_read_count != 2) return;
    size_t count = 0;
    const arm_live_read32_t *again = s3c2400_live_read32(soc, &count);
    CHECK(again == live_reads && count == live_read_count,
          "the live descriptor list must stay at the same SoC-owned address");
    CHECK(again[0].pa == live_pa[0] && again[0].word == live_word[0] &&
          again[1].pa == live_pa[1] && again[1].word == live_word[1],
          "live descriptors must keep their registered addresses and words");
}
static void check_live_parity(const char *where) {
    for (size_t i = 0; i < live_read_count; ++i) {
        uint32_t pa = live_pa[i];
        uint32_t ordinary = bus.read32(bus.user, pa);
        uint32_t specialized = bus.read32_io(bus.user, pa);
        uint32_t mirror = *live_word[i];
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "%s: live word %08lx must equal ordinary %08lx and fastIO %08lx at %08lx",
                 where, (unsigned long)mirror, (unsigned long)ordinary,
                 (unsigned long)specialized, (unsigned long)pa);
        CHECK(mirror == ordinary && ordinary == specialized, msg);
    }
}

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
    check_live_parity("NAND data latch read");
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
/* Expected live mirror bits, computed from the pad wiring like want_*. */
static void check_live_buttons(uint32_t mask, const char *where) {
    if (live_read_count != 2) return;
    char msg[128];
    snprintf(msg, sizeof(msg), "%s: live GPBDAT word must carry the active-low pad bits", where);
    CHECK((*live_word[0] & 0xff00u) == want_bdat(mask), msg);
    snprintf(msg, sizeof(msg), "%s: live GPEDAT word must carry the active-low start/select bits", where);
    CHECK((*live_word[1] & 0xc0u) == want_edat(mask), msg);
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
    check_live_parity("button mask change");
    check_live_buttons(mask, "button mask change");
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
        live_begin();
        check_live_descriptors();
        check_live_parity("card load");
        CHECK(live_read_count == 2 && (*live_word[1] & 4u) == 0,
              "live GPEDAT mirror must report the loaded card as present");
        write_port(0x24, 0x140);
        write_port(0x30, 8);
        CHECK(!(bus.read32(bus.user, GPIO + 0x30) & 4), "loaded card must be present");
        check_live_parity("SmartMedia control setup");
        send_byte(0x20, 0x90);       /* READ ID */
        send_byte(0x10, 0);
        begin_read();
        /* This small raw fixture uses the 4 MiB geometry's ID, not 8 MiB. */
        CHECK(read_byte() == 0xec && read_byte() == 0xe3, "NAND manufacturer/device ID");
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
        check_live_parity("chip-off deselect");
        CHECK(live_read_count == 2 && (*live_word[0] & 0xffu) == 0,
              "chip-off must clear the live data latch mirror");
        CHECK(live_read_count == 2 && (*live_word[1] & 4u) == 0,
              "the loaded card must still be present in the live mirror");
        /* Every GPIO write width must refresh both live words; the CPU loads
         * them instead of issuing the ordinary reads. */
        bus.write8(bus.user, GPIO + 0x08, 1);
        check_live_parity("GPIO byte write");
        bus.write16(bus.user, GPIO + 0x08, 0);
        check_live_parity("GPIO halfword write");
        bus.write32(bus.user, GPIO + 0x24, 0x140); /* chip selected again */
        check_live_parity("GPDDAT word write");
        bus.write8(bus.user, GPIO + 0x0c, 0x5a);
        check_live_parity("GPBDAT byte write");
        bus.write16(bus.user, GPIO + 0x0e, 0x1234);
        check_live_parity("GPBDAT high halfword write");
        bus.write32(bus.user, GPIO + 0x0c, 0x87654321u);
        check_live_parity("GPBDAT word write");
        bus.write8(bus.user, GPIO + 0x30, 0x28);
        check_live_parity("GPEDAT byte write");
        CHECK(live_read_count == 2 && (*live_word[1] & 0x20u) != 0,
              "live GPEDAT mirror must follow latch writes");
        bus.write16(bus.user, GPIO + 0x30, 0x18);
        check_live_parity("GPEDAT halfword write");
        check_live_descriptors();
        /* A failing wrapper refreshes too: a geometry-rejected raw image
         * clears the mounted card before smc_load_buffer returns 0. */
        error[0] = '\0';
        CHECK(!s3c2400_load_smartmedia(soc, "no-such-smartmedia-file.smc", error, sizeof(error)),
              "missing card file must fail");
        check_live_parity("failed card file load");
        CHECK(live_read_count == 2 && (*live_word[1] & 4u) == 0,
              "failed file open must leave the card present");
        error[0] = '\0';
        CHECK(!s3c2400_load_smartmedia_buffer(soc, image, 1024u, error, sizeof(error)),
              "geometry-rejected card buffer must fail");
        check_live_parity("failed card buffer load");
        CHECK(live_read_count == 2 && (*live_word[1] & 4u) != 0,
              "live GPEDAT mirror must expose the removed card");
        CHECK(bus.read32(bus.user, GPIO + 0x30) & 4u, "ordinary read must agree the card is gone");
        s3c2400_destroy(soc);
    }
    width = 32;
    soc = s3c2400_create(0);
    if (!soc) return 2;
    bus = s3c2400_get_bus(soc);
    live_begin();
    check_live_descriptors();
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
    /* Dirty the latched GPIO state so reset must refresh the live mirrors too. */
    write_port(0x24, 0x140);         /* chip selected so latch writes survive */
    write_port(0x30, 0x18);
    check_live_parity("pre-reset latch write");
    CHECK(live_read_count == 2 && (*live_word[1] & 0x18u) == 0x18u,
          "live GPEDAT mirror must follow pre-reset latch writes");
    /* Host-owned input survives reset and must keep feeding both ports. */
    s3c2400_reset(soc);
    check_live_parity("reset");
    check_live_descriptors();
    CHECK(live_read_count == 2 && (*live_word[1] & 0x1cu) == 0x0cu,
          "reset must clear the live GPEDAT latch mirrors");
    CHECK((bus.read32_io(bus.user, GPIO + 0x0c) & 0xff00u) == want_bdat(masks[5]) &&
          (bus.read32_io(bus.user, GPIO + 0x30) & 0xc0u) == want_edat(masks[5]) &&
          live_read_count == 2 &&
          (*live_word[0] & 0xff00u) == want_bdat(masks[5]) &&
          (*live_word[1] & 0xc0u) == want_edat(masks[5]) &&
          (*live_word[1] & 0x1cu) == 0x0cu,
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
        check_live_parity("button release");
        CHECK(live_read_count == 2 && (*live_word[0] & 0xff00u) == 0xff00u &&
              (*live_word[1] & 0xc0u) == 0xc0u,
              "release must clear every live button bit");
        /* Dirty latched control lines and card presence after the save so the
         * restore must recompute both mirrors from the loaded image alone. */
        char load_error[128] = {0};
        CHECK(s3c2400_load_smartmedia_buffer(soc, image, sizeof(image), load_error, sizeof(load_error)),
              "card load before state restore");
        write_port(0x24, 0x140);     /* chip selected so the latch survives */
        write_port(0x30, 0x18);      /* address latch raised */
        check_live_parity("dirty before state restore");
        CHECK(live_read_count == 2 && (*live_word[1] & 0x1cu) == 0x18u,
              "dirty latches and card presence must show in the live GPEDAT word");
        rewind(state);
        CHECK(s3c2400_state_load(soc, state), "state load");
        fclose(state);
        CHECK((bus.read32(bus.user, GPIO + 0x0c) & 0xff00u) == want_bdat(masks[5]) &&
              (bus.read32_io(bus.user, GPIO + 0x0c) & 0xff00u) == want_bdat(masks[5]) &&
              (bus.read32(bus.user, GPIO + 0x30) & 0xc0u) == want_edat(masks[5]) &&
              (bus.read32_io(bus.user, GPIO + 0x30) & 0xc0u) == want_edat(masks[5]) &&
              live_read_count == 2 &&
              (*live_word[0] & 0xff00u) == want_bdat(masks[5]) &&
              (*live_word[1] & 0xc0u) == want_edat(masks[5]) &&
              (*live_word[1] & 0x1cu) == 0x0cu,
              "restored state must replay the save-time button mask on both read paths");
        check_live_parity("state load");
        check_live_descriptors();
        check_button_mask(GP32_BUTTON_SELECT);
    }
    s3c2400_destroy(soc);
    if (failures) return 1;
    puts("PASS: GPIO NAND ID, program/read, latched reads, halfword/word control writes, button parity+state, live readback mirrors");
    return 0;
}
