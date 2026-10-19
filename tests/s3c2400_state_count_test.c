/* The size path must count current wire bytes without allocating the large
 * SoC image. Compare it with an actual writer across audio/card changes. */
#include <stdlib.h>
static unsigned count_allocations;
static void *counted_malloc(size_t bytes) {
    ++count_allocations;
    return malloc(bytes);
}
#define malloc counted_malloc
#include "../src/s3c2400.c"
#undef malloc

static unsigned failures, cases;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr, "FAIL: %s (%d)\n", msg, __LINE__); ++failures; } } while (0)

static size_t check_size(s3c2400_t *s) {
    ++cases;
    state_io_t count = state_io_counter();
    count_allocations = 0;
    CHECK(s3c2400_state_save_io(s, &count), "count current state");
    CHECK(count_allocations == 0u, "count does not allocate a temporary SoC image");
    uint8_t *bytes = malloc(count.pos + 32u);
    CHECK(bytes != NULL, "allocate output");
    if (!bytes) return 0;
    memset(bytes, 0xa5, count.pos + 32u);
    state_io_t writer = state_io_writer(bytes, count.pos);
    CHECK(s3c2400_state_save_io(s, &writer) && writer.pos == count.pos,
          "actual writer consumes exactly the counted size");
    for (size_t i = count.pos; i < count.pos + 32u; ++i)
        CHECK(bytes[i] == 0xa5u, "exact writer leaves the guard untouched");
    free(bytes);
    state_io_t overflow = state_io_counter();
    overflow.pos = SIZE_MAX - 15u;
    CHECK(!s3c2400_state_save_io(s, &overflow) && overflow.failed,
          "count overflow fails without wrapping");
    return count.pos;
}

static void program_byte(smc_t *smc, uint32_t page) {
    smc_command_w(smc, 0x00u); smc_command_w(smc, 0x80u);
    smc_address_w(smc, 0u);
    smc_address_w(smc, (uint8_t)page);
    smc_address_w(smc, (uint8_t)(page >> 8));
    smc_data_w(smc, 0u); smc_command_w(smc, 0x10u);
}

static void check_fixture(size_t ram_size, uint32_t pages) {
    s3c2400_t *s = s3c2400_create(ram_size);
    CHECK(s != NULL, "create SoC");
    if (!s) return;
    size_t empty = check_size(s);
    for (unsigned span = 0; span < 4u; ++span) {
        for (unsigned i = 0; i < 17u; ++i)
            s3c2400_audio_append_s16_stereo(s, (int16_t)i, (int16_t)-i,
                                          span & 1u ? 22050u : 44100u);
        CHECK(s3c2400_audio_consume(s, 3u), "consume part of an audio span");
        CHECK(check_size(s) > empty, "unread PCM and rate spans enlarge the wire image");
    }
    s3c2400_audio_clear(s);
    CHECK(check_size(s) == empty, "draining audio restores its original wire size");
    size_t image_size = (size_t)pages * 528u;
    uint8_t *image = malloc(image_size);
    CHECK(image != NULL, "allocate synthetic card");
    if (!image) { s3c2400_destroy(s); return; }
    memset(image, 0x5a, image_size);
    char err[128] = {0};
    CHECK(s3c2400_load_smartmedia_buffer(s, image, image_size, err, sizeof(err)), "mount base card");
    size_t delta_size = check_size(s);
    program_byte(s->smc, pages - 1u);
    CHECK(check_size(s) == delta_size, "last-page write fits the fixed delta budget");
    for (uint32_t page = 0; page < 2047u; ++page) program_byte(s->smc, page);
    CHECK(check_size(s) == delta_size, "completely populated entry budget keeps its wire size");
    program_byte(s->smc, 2047u);
    CHECK(check_size(s) > delta_size, "exceeding the budget uses the current full image size");
    CHECK(s3c2400_set_smartmedia_state_base(s, image, image_size, err, sizeof(err)), "replan current page differences");
    CHECK(check_size(s) > delta_size, "a larger entry budget is counted afresh");
    free(image);
    s3c2400_destroy(s);
}

int main(void) {
    check_fixture(64u * 1024u, 8192u);
    check_fixture(8u * 1024u * 1024u, 8195u);
    check_fixture(16u * 1024u * 1024u, 8192u);
    uint8_t guard = 0xa5u;
    state_io_t writer = state_io_writer(&guard, 1u);
    CHECK(!state_io_count_bytes(&writer, 1u) && writer.failed && !writer.pos && guard == 0xa5u,
          "count-only operation cannot silently skip a real write");
    printf("s3c2400_state_count cases=%u failures=%u\n", cases, failures);
    return failures ? 1 : 0;
}
