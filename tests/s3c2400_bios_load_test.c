/* A short host read must not partly replace live firmware. The fault injector
 * performs a real partial fread, then leaves the original loader to reject it. */
#include <stdio.h>
static int inject_short_read;
static size_t bios_fread(void *ptr, size_t size, size_t count, FILE *stream) {
    if (inject_short_read && count > 1u) return fread(ptr, size, count - 1u, stream);
    return fread(ptr, size, count, stream);
}
#define fread bios_fread
#include "../src/s3c2400.c"
#undef fread

static unsigned cases, failures;
static void check(int condition, const char *message) {
    ++cases;
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    s3c2400_t *s = s3c2400_create(0x800000u);
    uint8_t *expected = malloc(BIOS_SIZE);
    if (!s || !expected) return 2;
    uint8_t first[64], replacement[64];
    for (unsigned i = 0; i < 64u; ++i) { first[i] = (uint8_t)(0x30u + i); replacement[i] = (uint8_t)i; }
    check(s3c2400_load_bios_buffer(s, first, sizeof(first), NULL, 0u), "initial BIOS buffer");
    memcpy(expected, s->bios, BIOS_SIZE);
    FILE *f = fopen(argv[1], "wb");
    if (!f) return 2;
    check(fwrite(replacement, 1u, sizeof(replacement), f) == sizeof(replacement), "write BIOS fixture");
    check(fclose(f) == 0, "close BIOS fixture");
    inject_short_read = 1;
    check(!s3c2400_load_bios(s, argv[1], NULL, 0u), "injected short read is rejected");
    inject_short_read = 0;
    check(!memcmp(s->bios, expected, BIOS_SIZE), "failed read preserves every live BIOS byte");
    check(s3c2400_load_bios(s, argv[1], NULL, 0u), "complete file load succeeds");
    memset(expected, 0xff, BIOS_SIZE);
    memcpy(expected, replacement, sizeof(replacement));
    check(!memcmp(s->bios, expected, BIOS_SIZE), "file payload and erased tail match");
    /* The internal buffer API can receive a slice of its own BIOS mapping. */
    memcpy(first, replacement + 8u, 24u);
    check(s3c2400_load_bios_buffer(s, s->bios + 8u, 24u, NULL, 0u), "overlapping source is accepted");
    memset(expected, 0xff, BIOS_SIZE);
    memcpy(expected, first, 24u);
    check(!memcmp(s->bios, expected, BIOS_SIZE), "overlapping source is copied before tail erase");
    f = fopen(argv[1], "wb");
    if (!f) return 2;
    check(fclose(f) == 0, "write empty file");
    check(s3c2400_load_bios(s, argv[1], NULL, 0u), "existing empty-file behavior remains valid");
    memset(expected, 0xff, BIOS_SIZE);
    check(!memcmp(s->bios, expected, BIOS_SIZE), "empty file erases whole BIOS");
    (void)remove(argv[1]);
    free(expected);
    s3c2400_destroy(s);
    printf("s3c2400_bios_load_test: %u checks, %u failures\n", cases, failures);
    return failures != 0u;
}
