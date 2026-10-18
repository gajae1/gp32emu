/* The copied bus certificate must follow live RAM replacement, including a
 * standalone SoC state load which grows the allocation. */
#include "s3c2400.h"
#include <inttypes.h>
#include <stdio.h>

static unsigned cases, failures;
static void check(int ok, const char *message) {
    ++cases;
    if (!ok) { fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

static void check_direct_spans(const arm_bus_t *bus, size_t ram_size) {
    const uint32_t ram_base = 0x0c000000u, bios_size = 0x00080000u;
    const struct { uint32_t start; size_t bytes; int read, write; } spans[] = {
        {0u, 1u, 1, 0}, {0u, bios_size, 1, 0},
        {16u, bios_size - 16u, 1, 0}, {bios_size - 1u, 1u, 1, 0},
        {bios_size - 1u, 2u, 0, 0}, {bios_size, 0u, 1, 0},
        {bios_size, 1u, 0, 0}, {ram_base - 1u, 1u, 0, 0},
        {ram_base, ram_size, 1, 1}, {ram_base + 16u, ram_size - 16u, 1, 1},
        {ram_base + (uint32_t)ram_size - 1u, 1u, 1, 1},
        {ram_base + (uint32_t)ram_size - 1u, 2u, 0, 0},
        {ram_base + (uint32_t)ram_size, 0u, 1, 1},
        {ram_base + (uint32_t)ram_size, 1u, 0, 0},
        {16u, SIZE_MAX, 0, 0}, {16u, SIZE_MAX - 15u, 0, 0},
        {ram_base + 16u, SIZE_MAX, 0, 0},
        {ram_base + 16u, SIZE_MAX - 15u, 0, 0}
    };
    uint8_t *bios = bus->fastmem(bus->user, 0u, bios_size, 0);
    uint8_t *ram = bus->fastmem(bus->user, ram_base, ram_size, 1);
    check(bios && ram, "whole physical windows are available");
    if (!bios || !ram) return;
    for (unsigned i = 0; i < sizeof(spans) / sizeof(spans[0]); ++i) {
        for (int write = 0; write < 2; ++write) {
            int valid = write ? spans[i].write : spans[i].read;
            uint8_t *expected = !valid ? NULL : spans[i].start >= ram_base ?
                ram + (spans[i].start - ram_base) : bios + spans[i].start;
            uint8_t *got = bus->fastmem(bus->user, spans[i].start, spans[i].bytes, write);
            check(got == expected, "complete direct span and overflow-safe bounds");
        }
    }
}

static void check_ranges(const arm_bus_t *bus, size_t ram_size) {
    const uint32_t ram_base = 0x0c000000u;
    const struct { uint32_t start; size_t bytes; int read, write; } ranges[] = {
        {0u, 1u, 0, 1}, {0x0007ffffu, 2u, 0, 1}, {0x00080000u, 4u, 1, 1},
        {ram_base - 1u, 1u, 1, 1}, {ram_base - 1u, 2u, 0, 0},
        {ram_base, 4u, 0, 0}, {ram_base + (uint32_t)ram_size - 1u, 2u, 0, 0},
        {ram_base + (uint32_t)ram_size, 4u, 1, 1},
        {0x14000000u, 0x02000000u, 1, 1}, {UINT32_MAX, 1u, 1, 1},
        {UINT32_MAX, 2u, 0, 0}, {0u, 0u, 0, 0}
    };
    check(bus->fastmem_excludes != NULL, "SoC supplies a range certificate");
    if (!bus->fastmem_excludes) return;
    for (unsigned i = 0; i < sizeof(ranges) / sizeof(ranges[0]); ++i)
        for (int write = 0; write < 2; ++write)
            check(!!bus->fastmem_excludes(bus->user, ranges[i].start, ranges[i].bytes, write) ==
                  (write ? ranges[i].write : ranges[i].read), "complete interval exclusion");
    check(!!bus->fastmem_excludes(bus->user, 0x0c800000u, 0xf3800000u, 0) ==
          (ram_size <= 0x00800000u), "A64 open-bus range follows live RAM size");
}

int main(void) {
    s3c2400_t *small = s3c2400_create(0x00800000u), *large = s3c2400_create(0x01000000u);
    FILE *small_state = tmpfile(), *large_state = tmpfile();
    if (!small || !large || !small_state || !large_state) return 2;
    arm_bus_t copied = s3c2400_get_bus(small);
    check_ranges(&copied, 0x00800000u);
    check_direct_spans(&copied, 0x00800000u);
    check(s3c2400_state_save(small, small_state), "save small RAM state");
    check(s3c2400_state_save(large, large_state), "save large RAM state");
    rewind(large_state);
    check(s3c2400_state_load(small, large_state), "grow RAM through standalone state load");
    check(s3c2400_ram_size(small) == 0x01000000u, "state load replaced RAM size");
    check_ranges(&copied, 0x01000000u);
    check_direct_spans(&copied, 0x01000000u);
    check(copied.fastmem(copied.user, 0x0c800000u, 4u, 0) != NULL,
          "copied bus maps newly loaded extended RAM");
    rewind(small_state);
    check(s3c2400_state_load(small, small_state), "restore small RAM allocation");
    check_ranges(&copied, 0x00800000u);
    check_direct_spans(&copied, 0x00800000u);
    fclose(small_state); fclose(large_state);
    s3c2400_destroy(small); s3c2400_destroy(large);

    /* Creation accepts a size exceeding the GP32 default. Certifying the
     * device window must use the same live extent as its actual fastmem. */
    large = s3c2400_create(0x08001010u);
    if (!large) return 2;
    copied = s3c2400_get_bus(large);
    for (int write = 0; write < 2; ++write) {
        check(!copied.fastmem_excludes(copied.user, 0x14000000u, 0x02000000u, write),
              "extended RAM overlaps identity-I/O window");
        check(copied.fastmem(copied.user, 0x14001000u, 4u, write) != NULL,
              "overlapping word has the requested direct permission");
    }
    s3c2400_destroy(large);
    printf("s3c2400_fastmem_test: %u cases, %u failures\n", cases, failures);
    return failures != 0u;
}
