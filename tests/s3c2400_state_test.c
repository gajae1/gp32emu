/* Corrupt state indices must be rejected before committing the live SoC.
 * Include the wire-image definition but link all other real subsystems. */
#include "../src/s3c2400.c"
#include <limits.h>
#if defined(__APPLE__)
#include <pthread.h>
#endif

static int failures;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr, "FAIL: %s (%d)\n", msg, __LINE__); ++failures; } } while (0)

static int run_state_tests(void) {
    s3c2400_t *s = s3c2400_create(1024u * 1024u);
    CHECK(s != NULL, "create state fixture");
    if (!s) return 1;
    arm_bus_t bus = s3c2400_get_bus(s);
    arm920t_t *cpu = arm920t_create(&bus);
    CHECK(cpu != NULL, "create IRQ state sink");
    if (!cpu) { s3c2400_destroy(s); return 1; }
    s3c2400_set_irq_sink(s, cpu);
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

    /* Older IRQ arbitration ORed new selections into INTPND. A real old
     * two-timer state contains 0xc00 with INTOFFSET=10; simply holding that
     * bitmap now leaves a stale offset after the first acknowledgement.
     * Preserve the recorded selection and leave the other SRCPND request
     * queued. An inconsistent offset must select a present bit safely. */
    const uint32_t legacy_irqs[][2] = {
        {0xc00u, 10u}, {0xc00u, 11u}, {0xc00u, 32u}, {0xc00u, 31u},
        {0x800u, 11u}, {0x800u, UINT32_MAX}
    };
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(legacy_irqs); ++i) {
        memcpy(input, saved, size);
        s3c2400_state_image_t *image = (s3c2400_state_image_t *)input;
        image->irq[0] = 0xc00u;
        image->irq[4] = legacy_irqs[i][0];
        image->irq[2] = ~0xc00u;
        image->irq[5] = legacy_irqs[i][1];
        const uint32_t selected = legacy_irqs[i][1] == 11u || legacy_irqs[i][0] == 0x800u ? 11u : 10u;
        const uint32_t queued = selected == 10u ? 11u : 10u;
        io = state_io_reader(input, size);
        CHECK(s3c2400_state_load_io(s, &io, 1, 1, 1, 1, 1), "legacy IRQ bitmap accepted");
        CHECK(s3c2400_read32(s, 0x14400010u) == (1u << selected) &&
              s3c2400_read32(s, 0x14400014u) == selected,
              "legacy state restores one IRQ with a matching offset");
        s3c2400_write32(s, 0x14400000u, 1u << selected);
        s3c2400_write32(s, 0x14400010u, 1u << selected);
        CHECK(s3c2400_read32(s, 0x14400010u) == (1u << queued) &&
              s3c2400_read32(s, 0x14400014u) == queued,
              "legacy queued IRQ remains serviceable after the selected IRQ");
        s3c2400_write32(s, 0x14400000u, 1u << queued);
        s3c2400_write32(s, 0x14400010u, 1u << queued);
        CHECK(s3c2400_read32(s, 0x14400000u) == 0u &&
              s3c2400_read32(s, 0x14400010u) == 0u &&
              s3c2400_read32(s, 0x14400014u) == 0u,
              "both restored IRQs can be fully acknowledged");
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
    free(saved); free(input); free(after);
    arm920t_destroy(cpu); s3c2400_destroy(s);
    if (failures) return 1;
    puts("PASS: state index validation, legacy IRQ normalization and IIC transaction continuation");
    return 0;
}

#if defined(__APPLE__)
static void *run_state_worker(void *unused) {
    (void)unused;
    return (void *)(uintptr_t)run_state_tests();
}
#endif

int main(void) {
#if defined(__APPLE__)
    /* iOS runs persistence on a dispatch worker with a small stack. The wire
     * image includes BIOS and pixels and must not live on that stack. */
    pthread_attr_t attr;
    pthread_t worker;
    if (pthread_attr_init(&attr)) return 1;
    int err = pthread_attr_setstacksize(&attr, 512u * 1024u);
    if (!err) err = pthread_create(&worker, &attr, run_state_worker, NULL);
    pthread_attr_destroy(&attr);
    if (err) return 1;
    void *result = NULL;
    if (pthread_join(worker, &result)) return 1;
    return (int)(uintptr_t)result;
#else
    return run_state_tests();
#endif
}
