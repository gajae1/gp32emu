/* Corrupt state indices must be rejected before committing the live SoC.
 * Include the wire-image definition but link all other real subsystems. */
#include "../src/s3c2400.c"
#include <limits.h>

static int failures;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr, "FAIL: %s (%d)\n", msg, __LINE__); ++failures; } } while (0)

int main(void) {
    s3c2400_t *s = s3c2400_create(1024u * 1024u);
    CHECK(s != NULL, "create state fixture");
    if (!s) return 1;
    state_io_t count = state_io_counter();
    CHECK(s3c2400_state_save_io(s, &count), "count state bytes");
    size_t size = count.pos;
    uint8_t *saved = malloc(size), *input = malloc(size), *after = malloc(size);
    CHECK(saved && input && after, "allocate state buffers");
    if (!saved || !input || !after) goto end;
    state_io_t io = state_io_writer(saved, size);
    CHECK(s3c2400_state_save_io(s, &io), "capture live state");

    for (unsigned bad = 0; bad < 2u; ++bad) {
        memcpy(input, saved, size);
        s3c2400_state_image_t *image = (s3c2400_state_image_t *)input;
        if (bad) image->iic_data_index = -1;
        else image->iis_fifo_index = 2u;
        io = state_io_reader(input, size);
        CHECK(!s3c2400_state_load_io(s, &io, 1, 1, 1, 1, 1),
              "invalid peripheral index rejected");
        io = state_io_writer(after, size);
        CHECK(s3c2400_state_save_io(s, &io) && io.pos == size &&
              memcmp(saved, after, size) == 0,
              "rejected image leaves complete live state unchanged");
        /* Restore the fixture even on a failing baseline. */
        io = state_io_reader(saved, size);
        CHECK(s3c2400_state_load_io(s, &io, 1, 1, 1, 1, 1), "restore valid state");
    }

    /* Once the address bytes have been sent, old states may carry an
     * arbitrarily large byte count. They must continue the same transaction
     * without a signed increment wrapping into the address phase. */
    const int indices[] = {4, 123, INT_MAX};
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(indices); ++i) {
        memcpy(input, saved, size);
        s3c2400_state_image_t *image = (s3c2400_state_image_t *)input;
        image->iic_data_index = indices[i];
        image->iic_data[0] = 0xa0u;
        image->iic_address = 0x34u;
        image->iic[0] = 0x10u; /* pending byte awaiting acknowledgment */
        image->iic[1] = 0xe0u; /* master transmit, transfer active */
        image->iic[3] = 0x5au;
        io = state_io_reader(input, size);
        CHECK(s3c2400_state_load_io(s, &io, 1, 1, 1, 1, 1),
              "legacy IIC data-phase count accepted");
        s3c2400_write32(s, 0x15400000u, 0u);
        CHECK(s3c2400_eeprom_read8(s, 0x34u) == 0x5au,
              "restored IIC transaction writes the next byte at its saved address");
    }
end:
    free(saved); free(input); free(after); s3c2400_destroy(s);
    if (failures) return 1;
    puts("PASS: state index validation and IIC transaction continuation");
    return 0;
}
