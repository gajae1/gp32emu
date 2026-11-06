/* Synthetic FAT/card images exercise the public direct loader. No game,
 * firmware or publisher bytes are used. The directory index is quarantined
 * on free so stale diagnostic reads fail deterministically, without UB. */
#include "smc_direct.h"

static void *index_allocation;
static size_t index_bytes;
static int index_retired, fail_exec_copy, copy_allocations, injected;

static void *fixture_malloc(size_t size) {
    /* A 64-byte executable is read from FAT, copied into the file list, then
     * copied for the executable loader. Fail only that last owned copy. */
    if (fail_exec_copy && size == 64u && ++copy_allocations == 3) {
        injected = 1;
        return NULL;
    }
    return malloc(size);
}

static void *fixture_realloc(void *ptr, size_t size) {
    void *next = realloc(ptr, size);
    if (next) { index_allocation = next; index_bytes = size; }
    return next;
}

static void fixture_free(void *ptr) {
    if (ptr && ptr == index_allocation) {
        /* The real allocation is released after the public call returns.
         * Cleared retired storage must never supply a diagnostic filename. */
        memset(ptr, 0, index_bytes);
        index_retired = 1;
    } else {
        free(ptr);
    }
}

#define malloc fixture_malloc
#define realloc fixture_realloc
#define free fixture_free
#include "../src/smc_direct.c"
#undef free
#undef realloc
#undef malloc

static int failures;
static const char *current_case;
#define CHECK(c, msg) do { if (!(c)) { \
    fprintf(stderr, "FAIL[%s]: %s\n", current_case, msg); ++failures; \
} } while (0)

enum { FAT_BYTES = 64u * 512u, EXEC_SECTOR = 16u, EXEC_BYTES = 64u };

static void release_index(void) {
    CHECK(!index_allocation || index_retired, "directory index ownership is released");
    free(index_allocation);
    index_allocation = NULL;
    index_bytes = 0;
    index_retired = fail_exec_copy = copy_allocations = injected = 0;
}

static void make_executable(uint8_t out[EXEC_BYTES]) {
    memset(out, 0, EXEC_BYTES);
    gp32_st32le(out, 0xea000006u);
    gp32_st32le(out + 4u, 0x0c000000u);
    for (unsigned off = 8u; off <= 24u; off += 4u)
        gp32_st32le(out + off, 0x0c000040u);
    gp32_st32le(out + 32u, 0xeafffffeu);
    for (unsigned i = 36u; i < EXEC_BYTES; ++i) out[i] = (uint8_t)(i * 7u);
}

/* One FAT12 volume: boot/FAT/root at sectors 0/1/2, data at 3.
 * Cluster 15 is sector 16, across a 16-page NAND block boundary. */
static void make_fat(uint8_t image[FAT_BYTES], const char ext[3], int empty) {
    memset(image, 0, FAT_BYTES);
    image[0] = 0xebu; image[1] = 0x3cu; image[2] = 0x90u;
    gp32_st16le(image + 11u, 512u);
    image[13] = 1u;
    gp32_st16le(image + 14u, 1u);
    image[16] = 1u;
    gp32_st16le(image + 17u, 16u);
    gp32_st16le(image + 19u, 64u);
    image[21] = 0xf8u;
    gp32_st16le(image + 22u, 1u);
    memcpy(image + 54u, "FAT12   ", 8u);
    image[510] = 0x55u; image[511] = 0xaau;
    uint8_t *fat = image + 512u;
    fat[0] = 0xf8u; fat[1] = fat[2] = 0xffu;
    fat[22] = 0xf0u; fat[23] = 0xffu; /* FAT12 cluster 15 -> EOC */
    uint8_t *entry = image + 1024u;
    memcpy(entry, "TEST    ", 8u);
    memcpy(entry + 8u, ext, 3u);
    entry[11] = 0x20u;
    gp32_st16le(entry + 26u, 15u);
    gp32_st32le(entry + 28u, EXEC_BYTES);
    make_executable(image + EXEC_SECTOR * 512u);
    if (empty) {
        entry += 32u;
        memcpy(entry, "EMPTY   DAT", 11u);
        entry[11] = 0x20u; /* zero length; no allocated cluster */
    }
}

static void check_loaded(const uint8_t *image, size_t size, int empty) {
    smc_direct_package_t pkg = {0};
    char err[256] = {0};
    uint8_t expected[EXEC_BYTES];
    make_executable(expected);
    int loaded = smc_direct_load_buffer(image, size, "synthetic card", &pkg, err, sizeof(err));
    CHECK(loaded, "public loader accepts the complete synthetic card");
    if (!loaded) fprintf(stderr, "  loader: %s\n", err);
    if (loaded) {
        CHECK(pkg.image.payload_size == EXEC_BYTES &&
              !memcmp(pkg.image.payload, expected, EXEC_BYTES), "executable bytes survive card extraction");
        CHECK(pkg.asset_count == (empty ? 2u : 1u), "all files retain their directory entries");
        CHECK(!strcmp(pkg.executable_path, "TEST.GXB"), "selected executable retains its name");
        if (empty && pkg.asset_count == 2u)
            CHECK(!strcmp(pkg.assets[1].path, "EMPTY.DAT") && pkg.assets[1].size == 0u &&
                  pkg.assets[1].first_cluster == 0u, "empty asset needs no FAT cluster");
    }
    smc_direct_package_free(&pkg);
    release_index();
}

static void check_empty_file(void) {
    current_case = "unallocated empty file";
    uint8_t image[FAT_BYTES], before[FAT_BYTES];
    make_fat(image, "GXB", 1);
    memcpy(before, image, sizeof(image));
    check_loaded(image, sizeof(image), 1);
    CHECK(!memcmp(image, before, sizeof(image)), "input card remains immutable");
    /* A nonempty file with the same invalid chain must still be rejected. */
    gp32_st32le(image + 1024u + 32u + 28u, 1u);
    smc_direct_package_t pkg = {0};
    char err[256] = {0};
    CHECK(!smc_direct_load_buffer(image, sizeof(image), "bad chain", &pkg, err, sizeof(err)),
          "nonempty file with cluster zero remains invalid");
    smc_direct_package_free(&pkg);
    release_index();
}

static void check_error_ownership(const char ext[3], int oom) {
    current_case = oom ? "executable copy allocation failure" : "missing GXC companion";
    uint8_t image[FAT_BYTES], before[FAT_BYTES];
    make_fat(image, ext, 0);
    memcpy(before, image, sizeof(image));
    fail_exec_copy = oom;
    smc_direct_package_t pkg = {0};
    char err[256] = {0}, name[16];
    snprintf(name, sizeof(name), "TEST.%.3s", ext);
    int loaded = smc_direct_load_buffer(image, sizeof(image), "error fixture", &pkg, err, sizeof(err));
    CHECK(!loaded, "public loader reports failure");
    CHECK(!oom || injected, "allocation failure reaches executable-copy ownership boundary");
    CHECK(strstr(err, name) != NULL, "failure diagnostic retains the owned filename until formatting finishes");
    CHECK(!pkg.image.payload && !pkg.assets && !pkg.asset_count, "failed load publishes no owned package data");
    CHECK(!memcmp(image, before, sizeof(image)), "failed load preserves original bytes");
    smc_direct_package_free(&pkg);
    release_index();
}

/* Swap physical blocks 0 and 1. Their spare-area logical addresses, not
 * physical order, must reconstruct the FAT volume and executable. */
static void check_nand_geometry(unsigned pages, unsigned block_pages, uint8_t id, int header) {
    char label[80];
    snprintf(label, sizeof(label), "%u-page block / %u-page card / %s", block_pages, pages,
             header ? "format-2" : "raw");
    current_case = label;
    size_t prefix = header ? 1024u : 0u;
    size_t size = prefix + (size_t)pages * 528u;
    uint8_t *image = malloc(size);
    uint8_t fat[FAT_BYTES];
    CHECK(image != NULL, "allocate synthetic NAND card");
    if (!image) return;
    memset(image, 0xff, size);
    if (header) { image[0] = 0xecu; image[1] = id; }
    make_fat(fat, "GXB", 0);
    for (unsigned logical = 0; logical < 64u / block_pages; ++logical) {
        unsigned physical = logical < 2u ? logical ^ 1u : logical;
        uint16_t lba = (uint16_t)(0x1000u | (logical << 1u));
        /* SmartMedia block-address parity is even, in bit zero. */
        unsigned parity = 0;
        for (unsigned bit = 1u; bit < 16u; ++bit) parity ^= (lba >> bit) & 1u;
        lba |= (uint16_t)parity;
        for (unsigned page = 0; page < block_pages; ++page) {
            uint8_t *dst = image + prefix + ((size_t)physical * block_pages + page) * 528u;
            memcpy(dst, fat + ((size_t)logical * block_pages + page) * 512u, 512u);
            dst[518] = dst[523] = (uint8_t)(lba >> 8u);
            dst[519] = dst[524] = (uint8_t)lba;
        }
    }
    check_loaded(image, size, 0);
    free(image);
}

int main(void) {
    current_case = "plain FAT control";
    uint8_t image[FAT_BYTES];
    make_fat(image, "GXB", 0);
    check_loaded(image, sizeof(image), 0);
    check_empty_file();
    check_error_ownership("GXE", 0);
    check_error_ownership("GXB", 1);
    check_error_ownership("FXE", 1);
    for (int header = 0; header <= 1; ++header) {
        check_nand_geometry(8192u, 16u, 0xe3u, header);
        check_nand_geometry(16384u, 16u, 0xe6u, header);
        check_nand_geometry(32768u, 32u, 0x73u, header);
    }
    printf("Direct SmartMedia loading: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
