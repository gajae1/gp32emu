/* Real GPIO transactions against a synthetic NAND image. Reads of the data
 * latch must not advance NAND; each read-control activation must do so once. */
#include "s3c2400.h"
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
    if (failures) return 1;
    puts("PASS: GPIO NAND ID, program/read, latched reads, halfword/word control writes");
    return 0;
}
