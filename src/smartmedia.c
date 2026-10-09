/*
 * SmartMedia/NAND model ported to C11 from MAME's smartmed/nandflash devices.
 * Original files: src/devices/machine/smartmed.cpp, nandflash.cpp
 * License: BSD-3-Clause, copyright Raphael Nabet.
 */
#include "smartmedia.h"
#include "zip.h"
#include "save_atomic.h"
#include <stdatomic.h>
#include <time.h>
#ifndef GP32EMU_ENABLE_THREADS
#define GP32EMU_ENABLE_THREADS 0
#endif
#if defined(_WIN32)
#include <windows.h>
#elif GP32EMU_ENABLE_THREADS
#include <pthread.h>
#endif

typedef struct smc_save_job {
    uint8_t *image;
    size_t size;
    char path[SAVE_ATOMIC_MAX_PATH], error[256];
    uint64_t epoch;
    atomic_bool done;
    int ok, delta;
#if GP32EMU_ENABLE_THREADS
#if defined(_WIN32)
    HANDLE thread;
#else
    pthread_t thread;
#endif
#endif
} smc_save_job_t;

/* v0014 savestate delta.
 *
 * A state's SmartMedia section usually carries only the NAND pages that differ
 * from the image the frontend passed, not the whole 17-34 MB card. That image
 * is stable for the life of the content file: the libretro core persists guest
 * writes to <save dir>/<rom>.gp32.sav and every later session mounts that file
 * over the frontend content, so the base a state was saved against is still
 * available when the state is loaded in a later session of the same game.
 *
 * The entry region is padded to a per-session budget, so one mounted card
 * serializes to one size while the game runs. The budget holds what the mounted
 * card already differs by plus room for the guest's own writes; a card the
 * guest rewrites past that budget falls back to carrying the whole image (the
 * pre-v0014 payload), which keeps every state loadable and never exceeds the
 * size a frontend latched before this change. */
#define SMC_STATE_DELTA_FLAG 0x2u        /* smc_state_image_t::dirty bit */
#define SMC_STATE_DELTA_TAG 0x31444d53u  /* 'S','M','D','1' */
#define SMC_STATE_DELTA_MIN_ENTRIES 2048u
#define SMC_STATE_DELTA_HEADROOM 1024u
#define SMC_STATE_MAX_IMAGE ((size_t)128u * 1024u * 1024u)
#define SMC_STATE_MAX_PAGE_TOTAL 2112u

/* Delta body, written after smc_state_image_t when SMC_STATE_DELTA_FLAG is set:
 * this head, the differing pages in ascending page order, zero padding to the
 * entry budget, then the page register. */
typedef struct smc_state_delta_head {
    uint32_t tag;
    uint32_t capacity;    /* entry slots the zero padding fills to */
    uint32_t entry_bytes; /* 4 + page_total_size */
    uint32_t count;       /* entries present */
    uint8_t base_digest[16];
    uint64_t base_size;
    uint64_t base_num_pages;
} smc_state_delta_head_t;

typedef enum sm_mode {
    SM_M_INIT,
    SM_M_READ,
    SM_M_PROGRAM,
    SM_M_ERASE,
    SM_M_READSTATUS,
    SM_M_READID,
    SM_M_30,
    SM_M_RANDOM_DATA_INPUT,
    SM_M_RANDOM_DATA_OUTPUT
} sm_mode_t;

typedef enum sm_pointer_mode {
    SM_PM_A,
    SM_PM_B,
    SM_PM_C
} sm_pointer_mode_t;

struct smc {
    uint8_t *data;
    size_t data_size;
    uint8_t header[1024];
    size_t header_size;
    int dirty;
    uint32_t page_data_size;
    uint32_t page_total_size;
    uint32_t num_pages;
    uint32_t log2_pages_per_block;
    uint32_t col_address_cycles;
    uint32_t row_address_cycles;
    uint32_t sequential_row_read;
    uint8_t id[5];
    uint32_t id_len;
    uint8_t *page_reg;
    uint8_t data_uid[256 + 16];
    int data_uid_present;
    sm_mode_t mode;
    sm_pointer_mode_t pointer_mode;
    uint32_t page_addr;
    uint32_t byte_addr;
    uint32_t addr_load_ptr;
    uint8_t status;
    uint8_t accumulated_status;
    bool mode_3065;
    uint32_t program_byte_count;
    /* v0014 state base: the NAND payload of the image the frontend passed and
     * a 128-bit digest of that image, plus an exact map of the pages that
     * currently differ from it. */
    uint8_t *base;
    size_t base_size;
    uint32_t base_page_total_size;
    uint32_t base_num_pages;
    uint8_t base_digest[16];
    uint8_t *page_diff;
    uint32_t diff_pages;
    uint32_t state_entries;
    /* Host persistence state is deliberately absent from savestates. */
    uint64_t write_epoch, observed_epoch, quiet_since, last_attempt;
    int autosave_started, autosave_error, persist_dirty;
    smc_save_job_t *save_job;
};

static unsigned log2_u32(uint32_t v) {
    unsigned n = 0;
    while ((UINT32_C(1) << n) < v) n++;
    return n;
}

smc_t *smc_create(void) {
    smc_t *s = (smc_t *)calloc(1, sizeof(*s));
    if (s) smc_reset(s);
    return s;
}

void smc_destroy(smc_t *s) {
    if (!s) return;
    smc_autosave_wait(s, NULL, 0);
    free(s->data);
    free(s->page_reg);
    free(s->base);
    free(s->page_diff);
    free(s);
}

static int smc_detect_small_geometry(uint8_t id1, uint8_t id2, uint32_t *page_data, uint32_t *page_total, uint32_t *pages, uint32_t *log2_ppb) {
    if (id1 == 0xec) {
        switch (id2) {
        case 0xa4: *page_data=0x100; *page_total=0x108; *pages=0x00800; *log2_ppb=0; return 1;
        case 0x6e: *page_data=0x100; *page_total=0x108; *pages=0x01000; *log2_ppb=0; return 1;
        case 0xea: *page_data=0x100; *page_total=0x108; *pages=0x02000; *log2_ppb=4; return 1;
        case 0xe3: *page_data=0x200; *page_total=0x210; *pages=0x02000; *log2_ppb=4; return 1;
        case 0xe6: *page_data=0x200; *page_total=0x210; *pages=0x04000; *log2_ppb=4; return 1;
        case 0x73: *page_data=0x200; *page_total=0x210; *pages=0x08000; *log2_ppb=5; return 1;
        case 0x75: *page_data=0x200; *page_total=0x210; *pages=0x10000; *log2_ppb=5; return 1;
        case 0x76: *page_data=0x200; *page_total=0x210; *pages=0x20000; *log2_ppb=5; return 1;
        case 0x79: *page_data=0x200; *page_total=0x210; *pages=0x40000; *log2_ppb=5; return 1;
        }
    } else if (id1 == 0x98) {
        switch (id2) {
        case 0x73: *page_data=0x200; *page_total=0x210; *pages=0x08000; *log2_ppb=5; return 1;
        case 0x75: *page_data=0x200; *page_total=0x210; *pages=0x10000; *log2_ppb=5; return 1;
        }
    }
    return 0;
}

static int smc_set_geometry_from_size(smc_t *s, size_t size, char *err, size_t err_len) {
    /* MAME GP32 software list uses raw 528-byte pages when geometry is supplied by the softlist. */
    if ((size % 528u) == 0) {
        s->page_data_size = 512;
        s->page_total_size = 528;
        s->num_pages = (uint32_t)(size / 528u);
        s->col_address_cycles = 1;
        s->row_address_cycles = (s->num_pages > 0x10000u) ? 3u : 2u;
        s->log2_pages_per_block = (s->num_pages <= 16384u) ? 4u : 5u;
        s->sequential_row_read = 1;
        s->id_len = 2;
        s->id[0] = 0xec;
        if (s->num_pages <= 8192u) s->id[1] = 0xe3;
        else if (s->num_pages <= 16384u) s->id[1] = 0xe6;
        else if (s->num_pages <= 32768u) s->id[1] = 0x73;
        else if (s->num_pages <= 65536u) s->id[1] = 0x75;
        else if (s->num_pages <= 131072u) s->id[1] = 0x76;
        else s->id[1] = 0x79;
        return 1;
    }
    if ((size % 2112u) == 0) {
        s->page_data_size = 2048;
        s->page_total_size = 2112;
        s->num_pages = (uint32_t)(size / 2112u);
        s->col_address_cycles = 2;
        s->row_address_cycles = (s->num_pages > 0x10000u) ? 3u : 2u;
        s->log2_pages_per_block = log2_u32(64);
        s->sequential_row_read = 0;
        s->id_len = 4;
        s->id[0] = 0xec; s->id[1] = 0xf1; s->id[2] = 0x00; s->id[3] = 0x15;
        return 1;
    }
    if (err && err_len) snprintf(err, err_len, "unsupported SmartMedia size %zu; expected MAME format-2 or raw 528-/2112-byte pages", size);
    return 0;
}

/* ---- v0014 state base, page-difference map and mount support ---- */

static uint64_t smc_rotl64(uint64_t v, unsigned n) {
    return (v << n) | (v >> (64u - n));
}

/* Two independent 64-bit word hashes bind a state to the exact frontend image
 * it was saved against; the pair is stored in the delta body. */
static void smc_image_digest(const uint8_t *data, size_t size, uint8_t out[16]) {
    uint64_t h1 = UINT64_C(0xcbf29ce484222325);
    uint64_t h2 = UINT64_C(0x9e3779b97f4a7c15);
    size_t i = 0;
    for (; i + 8u <= size; i += 8u) {
        uint64_t w = 0;
        memcpy(&w, data + i, 8u);
        h1 = (h1 ^ w) * UINT64_C(0x100000001b3);
        h2 = smc_rotl64(h2, 27u) ^ (w + UINT64_C(0x165667b19e3779f9)) * UINT64_C(0xc2b2ae3d27d4eb4f);
    }
    for (; i < size; ++i) {
        h1 = (h1 ^ data[i]) * UINT64_C(0x100000001b3);
        h2 = smc_rotl64(h2, 27u) ^ (uint64_t)data[i] * UINT64_C(0x100000001b3);
    }
    for (unsigned k = 0; k < 8u; ++k) {
        out[k] = (uint8_t)(h1 >> (k * 8u));
        out[8u + k] = (uint8_t)(h2 >> (k * 8u));
    }
}

static void smc_base_release(smc_t *s) {
    free(s->base);
    free(s->page_diff);
    s->base = NULL;
    s->page_diff = NULL;
    s->base_size = 0;
    s->base_page_total_size = 0;
    s->base_num_pages = 0;
    s->diff_pages = 0;
    s->state_entries = 0;
    memset(s->base_digest, 0, sizeof(s->base_digest));
}

/* NAND payload shape of a frontend SmartMedia image. Mirrors the mount rules:
 * the MAME header form keeps 1024 bytes in front of the pages, the raw form is
 * pages only and carries its geometry in the size. */
static int smc_image_payload(const uint8_t *image, size_t image_size,
                             size_t *payload_off, uint32_t *page_total_size, uint32_t *num_pages) {
    if (image_size == 0) return 0;
    if (image_size > 1024u) {
        uint32_t pd = 0, pt = 0, np = 0, ppb = 0;
        if (smc_detect_small_geometry(image[0], image[1], &pd, &pt, &np, &ppb) &&
            image_size - 1024u == (size_t)pt * np) {
            *payload_off = 1024u;
            *page_total_size = pt;
            *num_pages = np;
            return 1;
        }
    }
    if ((image_size % 528u) == 0) {
        *page_total_size = 528u;
        *num_pages = (uint32_t)(image_size / 528u);
    } else if ((image_size % 2112u) == 0) {
        *page_total_size = 2112u;
        *num_pages = (uint32_t)(image_size / 2112u);
    } else {
        return 0;
    }
    *payload_off = 0;
    return 1;
}

/* The base describes this card exactly: same image size, page size and page
 * count. Any other shape means the state cannot express this card. */
static int smc_base_geometry_ok(const smc_t *s) {
    return s->base != NULL && s->page_total_size != 0 && s->num_pages != 0 &&
           s->base_size == s->data_size &&
           s->base_page_total_size == s->page_total_size &&
           s->base_num_pages == s->num_pages;
}

static int smc_page_differs(const smc_t *s, uint32_t page) {
    return memcmp(s->data + (size_t)page * s->page_total_size,
                  s->base + (size_t)page * s->page_total_size, s->page_total_size) != 0;
}

static void smc_diff_bit(smc_t *s, uint32_t page, int differ) {
    if (!s->page_diff || page >= s->base_num_pages) return;
    uint8_t mask = (uint8_t)(1u << (page & 7u));
    uint8_t *byte = &s->page_diff[page >> 3];
    int was = (*byte & mask) != 0;
    if (differ == was) return;
    if (differ) {
        *byte |= mask;
        s->diff_pages++;
    } else {
        *byte &= (uint8_t)~mask;
        s->diff_pages--;
    }
}

/* Exact refresh of the pages an erase or a program actually touched. */
static void smc_diff_refresh(smc_t *s, uint32_t first, uint32_t count) {
    if (!smc_base_geometry_ok(s)) return;
    for (uint32_t i = 0; i < count && first + i < s->num_pages; ++i)
        smc_diff_bit(s, first + i, smc_page_differs(s, first + i));
}

static void smc_diff_rebuild(smc_t *s) {
    if (s->page_diff && s->base_num_pages)
        memset(s->page_diff, 0, ((size_t)s->base_num_pages + 7u) / 8u);
    s->diff_pages = 0;
    if (!smc_base_geometry_ok(s)) return;
    for (uint32_t page = 0; page < s->num_pages; ++page) {
        if (smc_page_differs(s, page)) {
            s->page_diff[page >> 3] |= (uint8_t)(1u << (page & 7u));
            s->diff_pages++;
        }
    }
}

/* Session entry budget: what the mounted card already differs by, plus room for
 * the guest's own writes, never below the fixed minimum. A card that stays
 * close to the frontend image therefore keeps one serialized size in every
 * session, and a card an older full-image state replaced wholesale keeps the
 * full-image form. */
static void smc_state_plan(smc_t *s) {
    if (!smc_base_geometry_ok(s)) {
        s->state_entries = 0;
        return;
    }
    uint64_t budget = (uint64_t)s->diff_pages + SMC_STATE_DELTA_HEADROOM;
    if (budget < SMC_STATE_DELTA_MIN_ENTRIES) budget = SMC_STATE_DELTA_MIN_ENTRIES;
    if (budget > s->num_pages) budget = s->num_pages;
    s->state_entries = (uint32_t)budget;
}

/* Delta form decision, shared by the serializer and the size query. The entry
 * region must stay smaller than the image it replaces and must hold every page
 * that differs; otherwise the section carries the whole image, which loads in
 * every session. */
static int smc_state_delta_usable(const smc_t *s) {
    if (s->state_entries == 0 || !smc_base_geometry_ok(s)) return 0;
    if ((uint64_t)s->diff_pages > s->state_entries) return 0;
    uint64_t region = sizeof(smc_state_delta_head_t) +
                      (uint64_t)s->state_entries * (4u + (uint64_t)s->page_total_size);
    return region < (uint64_t)s->data_size;
}

/* A mount result: everything a load computes before the live device is touched. */
static int smc_base_adopt(smc_t *s, const uint8_t *image, size_t size, int identical, char *err, size_t err_len);

typedef struct smc_mount_image {
    uint8_t *data;
    uint8_t *page_reg;
    size_t data_size;
    uint8_t header[1024];
    size_t header_size;
    uint32_t page_data_size;
    uint32_t page_total_size;
    uint32_t num_pages;
    uint32_t log2_pages_per_block;
    uint32_t col_address_cycles;
    uint32_t row_address_cycles;
    uint32_t sequential_row_read;
    uint8_t id[5];
    uint32_t id_len;
    uint8_t data_uid[256 + 16];
    int data_uid_present;
} smc_mount_image_t;

static void smc_mount_image_free(smc_mount_image_t *img) {
    free(img->data);
    free(img->page_reg);
    memset(img, 0, sizeof(*img));
}

/* Parse a SmartMedia image exactly the way a mount does, into a fresh card
 * image that is idle (reset command state) and ready to be installed. */
static int smc_parse_image(const uint8_t *src, size_t len, smc_mount_image_t *out, char *err, size_t err_len) {
    memset(out, 0, sizeof(*out));
    size_t payload_off = 0;
    uint32_t pd = 0, pt = 0, np = 0, ppb = 0;
    if (len > 1024u && smc_detect_small_geometry(src[0], src[1], &pd, &pt, &np, &ppb) &&
        (len - 1024u) == (size_t)pt * np) {
        payload_off = 1024u;
        memcpy(out->header, src, 1024u);
        out->header_size = 1024u;
        out->page_data_size = pd;
        out->page_total_size = pt;
        out->num_pages = np;
        out->log2_pages_per_block = ppb;
        out->col_address_cycles = 1;
        out->row_address_cycles = (np > 0x10000u) ? 3u : 2u;
        out->sequential_row_read = 1;
        out->id_len = 3;
        out->id[0] = src[0];
        out->id[1] = src[1];
        out->id[2] = src[2];
        for (int i = 0; i < 8; ++i) {
            memcpy(out->data_uid + i * 32, src + 256, 16);
            for (int j = 0; j < 16; ++j) out->data_uid[i * 32 + 16 + j] = (uint8_t)(src[256 + j] ^ 0xffu);
        }
        memcpy(out->data_uid + 256, src + 272, 16);
        out->data_uid_present = 1;
    } else {
        smc_t shape;
        memset(&shape, 0, sizeof(shape));
        if (!smc_set_geometry_from_size(&shape, len, err, err_len)) return 0;
        out->page_data_size = shape.page_data_size;
        out->page_total_size = shape.page_total_size;
        out->num_pages = shape.num_pages;
        out->log2_pages_per_block = shape.log2_pages_per_block;
        out->col_address_cycles = shape.col_address_cycles;
        out->row_address_cycles = shape.row_address_cycles;
        out->sequential_row_read = shape.sequential_row_read;
        memcpy(out->id, shape.id, sizeof(out->id));
        out->id_len = shape.id_len;
    }
    out->data_size = len - payload_off;
    out->data = (uint8_t *)malloc(out->data_size ? out->data_size : 1u);
    out->page_reg = (uint8_t *)malloc(out->page_total_size);
    if (!out->data || !out->page_reg) {
        smc_mount_image_free(out);
        return 0;
    }
    memcpy(out->data, src + payload_off, out->data_size);
    /* A freshly mounted card starts with an idle command interface. */
    memset(out->page_reg, 0xff, out->page_total_size);
    return 1;
}

static void smc_apply_idle_command_state(smc_t *s) {
    s->mode = SM_M_INIT;
    s->pointer_mode = SM_PM_A;
    s->page_addr = 0;
    s->byte_addr = 0;
    s->addr_load_ptr = 0;
    s->status = 0xc0; /* ready, not protected */
    s->accumulated_status = 0;
    s->mode_3065 = false;
    s->program_byte_count = 0;
}

/* Install a parsed card image. The state base survives when it still describes
 * the new card shape; its exact page map is rebuilt. */
static void smc_install_image(smc_t *s, smc_mount_image_t *img, int keep_base) {
    ++s->write_epoch;
    uint8_t *base = s->base;
    size_t base_size = s->base_size;
    uint32_t base_page_total = s->base_page_total_size;
    uint32_t base_num_pages = s->base_num_pages;
    uint8_t digest[16];
    uint8_t *map = s->page_diff;
    uint32_t plan = s->state_entries;
    memcpy(digest, s->base_digest, sizeof(digest));
    s->base = NULL;
    s->page_diff = NULL;
    s->base_size = 0;
    s->base_page_total_size = 0;
    s->base_num_pages = 0;
    s->diff_pages = 0;
    s->state_entries = 0;
    free(s->data);
    free(s->page_reg);
    s->data = img->data;
    s->data_size = img->data_size;
    memcpy(s->header, img->header, sizeof(s->header));
    s->header_size = img->header_size;
    s->dirty = 0;
    s->persist_dirty = 0;
    s->page_data_size = img->page_data_size;
    s->page_total_size = img->page_total_size;
    s->num_pages = img->num_pages;
    s->log2_pages_per_block = img->log2_pages_per_block;
    s->col_address_cycles = img->col_address_cycles;
    s->row_address_cycles = img->row_address_cycles;
    s->sequential_row_read = img->sequential_row_read;
    memcpy(s->id, img->id, sizeof(s->id));
    s->id_len = img->id_len;
    s->page_reg = img->page_reg;
    memcpy(s->data_uid, img->data_uid, sizeof(s->data_uid));
    s->data_uid_present = img->data_uid_present;
    img->data = NULL;
    img->page_reg = NULL;
    smc_apply_idle_command_state(s);
    s->base = base;
    s->base_size = base_size;
    s->base_page_total_size = base_page_total;
    s->base_num_pages = base_num_pages;
    memcpy(s->base_digest, digest, sizeof(digest));
    s->page_diff = map;
    if (!keep_base || !smc_base_geometry_ok(s)) {
        smc_base_release(s);
        return;
    }
    smc_diff_rebuild(s);
    s->state_entries = plan;
    smc_state_plan(s);
}

int smc_load_buffer(smc_t *s, const uint8_t *src, size_t len, char *err, size_t err_len) {
    if (!s || !src || len == 0) return 0;
    smc_mount_image_t img;
    if (!smc_parse_image(src, len, &img, err, err_len)) {
        /* A rejected buffer mount clears the card, as every build before v0014
         * did: an invalid image must not leave a stale card for the guest. */
        smc_autosave_wait(s, NULL, 0);
        free(s->data);
        free(s->page_reg);
        free(s->base);
        free(s->page_diff);
        memset(s, 0, sizeof(*s));
        return 0;
    }
    smc_install_image(s, &img, 0);
    smc_mount_image_free(&img);
    /* The image just mounted is the state base: the frontend passes it again in
     * every later session of this game. */
    return smc_base_adopt(s, src, len, 1, err, err_len);
}

int smc_load_buffer_over_base(smc_t *s, const uint8_t *src, size_t len, char *err, size_t err_len) {
    if (!s || !src || len == 0) return 0;
    smc_mount_image_t img;
    if (!smc_parse_image(src, len, &img, err, err_len)) return 0;
    smc_install_image(s, &img, 1);
    smc_mount_image_free(&img);
    return 1;
}

/* Read a SmartMedia image from a plain file or from the first .smc member of a
 * zip archive. */
static int smc_read_file_image(const char *path, uint8_t **out, size_t *out_len, char *err, size_t err_len) {
    uint8_t *buf = NULL;
    size_t len = 0;
    if (gp32_zip_path_maybe(path)) {
        char entry_name[260] = {0};
        static const char * const exts[] = { ".smc" };
        if (!gp32_zip_read_first_matching(path, exts, GP32_ARRAY_COUNT(exts), &buf, &len, entry_name, sizeof(entry_name), err, err_len)) return 0;
        GP32_UNUSED(entry_name);
    } else {
        FILE *f = fopen(path, "rb");
        if (!f) {
            if (err && err_len) snprintf(err, err_len, "open %s: %s", path, strerror(errno));
            return 0;
        }
        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
        long n = ftell(f);
        if (n <= 0) { fclose(f); return 0; }
        rewind(f);
        buf = (uint8_t *)malloc((size_t)n);
        if (!buf) { fclose(f); return 0; }
        if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
            if (err && err_len) snprintf(err, err_len, "read %s failed", path);
            free(buf);
            fclose(f);
            return 0;
        }
        fclose(f);
        len = (size_t)n;
    }
    *out = buf;
    *out_len = len;
    return 1;
}

int smc_load_file(smc_t *s, const char *path, char *err, size_t err_len) {
    if (!s || !path) return 0;
    uint8_t *buf = NULL;
    size_t len = 0;
    if (!smc_read_file_image(path, &buf, &len, err, err_len)) return 0;
    int ok = smc_load_buffer(s, buf, len, err, err_len);
    free(buf);
    return ok;
}

int smc_load_file_over_base(smc_t *s, const char *path, char *err, size_t err_len) {
    if (!s || !path) return 0;
    uint8_t *buf = NULL;
    size_t len = 0;
    if (!smc_read_file_image(path, &buf, &len, err, err_len)) return 0;
    int ok = smc_load_buffer_over_base(s, buf, len, err, err_len);
    free(buf);
    return ok;
}

/* Adopt an image as the state base. The "identical" flag says the device
 * already holds exactly this image, which skips a whole-card comparison that
 * would find no difference at all (the common case: the frontend content is
 * the card the session mounted). */
static int smc_base_adopt(smc_t *s, const uint8_t *image, size_t size, int identical, char *err, size_t err_len) {
    if (!s || !image || size == 0) return 0;
    size_t payload_off = 0;
    uint32_t page_total = 0, num_pages = 0;
    if (!smc_image_payload(image, size, &payload_off, &page_total, &num_pages)) {
        if (err && err_len)
            snprintf(err, err_len, "unsupported state base image size %zu; expected MAME format-2 or raw 528-/2112-byte pages", size);
        return 0;
    }
    size_t payload = size - payload_off;
    size_t map_bytes = ((size_t)num_pages + 7u) / 8u;
    uint8_t *copy = (uint8_t *)malloc(payload ? payload : 1u);
    uint8_t *map = (uint8_t *)calloc(map_bytes ? map_bytes : 1u, 1u);
    if (!copy || !map) {
        free(copy);
        free(map);
        return 0;
    }
    memcpy(copy, image + payload_off, payload);
    free(s->base);
    free(s->page_diff);
    s->base = copy;
    s->base_size = payload;
    s->base_page_total_size = page_total;
    s->base_num_pages = num_pages;
    s->page_diff = map;
    s->diff_pages = 0;
    s->state_entries = 0;
    smc_image_digest(image, size, s->base_digest);
    if (!smc_base_geometry_ok(s)) return 1;
    if (identical && s->page_diff) {
        memset(s->page_diff, 0, ((size_t)s->base_num_pages + 7u) / 8u);
        s->diff_pages = 0;
        smc_state_plan(s);
    } else {
        smc_diff_rebuild(s);
        smc_state_plan(s);
    }
    return 1;
}

int smc_set_state_base(smc_t *s, const uint8_t *image, size_t size, char *err, size_t err_len) {
    return smc_base_adopt(s, image, size, 0, err, err_len);
}

int smc_set_state_base_file(smc_t *s, const char *path, char *err, size_t err_len) {
    if (!s || !path) return 0;
    uint8_t *buf = NULL;
    size_t len = 0;
    if (!smc_read_file_image(path, &buf, &len, err, err_len)) return 0;
    int ok = smc_set_state_base(s, buf, len, err, err_len);
    free(buf);
    return ok;
}

int smc_save_file(smc_t *s, const char *path, char *err, size_t err_len) {
    if (!s || !path || !s->data) return 0;
    save_atomic_t stage;
    if (!save_atomic_begin(&stage, path, err, err_len)) return 0;
    if (s->header_size) {
        if (fwrite(s->header, 1, s->header_size, stage.file) != s->header_size) {
            save_atomic_abort(&stage);
            if (err && err_len) snprintf(err, err_len, "write %s header failed", path);
            return 0;
        }
    }
    if (fwrite(s->data, 1, s->data_size, stage.file) != s->data_size) {
        save_atomic_abort(&stage);
        if (err && err_len) snprintf(err, err_len, "write %s payload failed", path);
        return 0;
    }
    if (!save_atomic_commit(&stage, path, err, err_len)) return 0;
    s->dirty = 0;
    s->persist_dirty = 0;
    s->autosave_error = 0;
    return 1;
}

int smc_is_dirty(const smc_t *s) { return s ? s->persist_dirty : 0; }

/* Persistent saves are not machine snapshots: an explicit little-endian
 * 64-byte header binds sorted page records to the immutable card image.
 * Header: magic[8], page bytes/u32, pages/u32, records/u32, card header/u32,
 * base payload bytes/u64, base digest[16], body digest[16]. The body contains
 * the card header followed by {page index/u32, complete NAND page} records.
 * Both the data and spare area of each page are preserved. */
#define SMC_SAVE_HEAD 64u
static const uint8_t smc_save_magic[8] = {'G','P','3','2','S','A','V','1'};

uint8_t *smc_copy_image(const smc_t *s, size_t *size) {
    if (!s || !s->data || !size) return NULL;
    *size = s->header_size + s->data_size;
    uint8_t *image = malloc(*size);
    if (image) {
        memcpy(image, s->header, s->header_size);
        memcpy(image + s->header_size, s->data, s->data_size);
    }
    return image;
}

static smc_save_job_t *smc_prepare_save(smc_t *s, const char *path) {
    if (!s || !path || !s->data || strlen(path) >= SAVE_ATOMIC_MAX_PATH) return NULL;
    smc_save_job_t *job = calloc(1, sizeof(*job));
    if (!job) return NULL;
    atomic_init(&job->done, false);
    strcpy(job->path, path);
    job->epoch = s->write_epoch;
    if (!smc_base_geometry_ok(s)) {
        /* Full legacy states may contain a card with no matching original. */
        job->image = smc_copy_image(s, &job->size);
        if (job->image) return job;
        free(job); return NULL;
    }
    size_t body_size = s->header_size + (size_t)s->diff_pages * (4u + s->page_total_size);
    job->size = SMC_SAVE_HEAD + body_size;
    job->image = calloc(1, job->size);
    if (!job->image) { free(job); return NULL; }
    uint8_t *head = job->image, *body = head + SMC_SAVE_HEAD;
    memcpy(body, s->header, s->header_size);
    size_t pos = s->header_size;
    for (uint32_t first = 0; first < s->num_pages; first += 8u) {
        unsigned mask = s->page_diff[first >> 3];
        for (uint32_t page = first; mask && page < s->num_pages; ++page, mask >>= 1) {
            if (!(mask & 1u)) continue;
            if (body_size - pos < 4u + s->page_total_size) goto bad;
            gp32_st32le(body + pos, page);
            memcpy(body + pos + 4u, s->data + (size_t)page * s->page_total_size, s->page_total_size);
            pos += 4u + s->page_total_size;
        }
    }
    if (pos != body_size) goto bad;
    memcpy(head, smc_save_magic, sizeof(smc_save_magic));
    gp32_st32le(head + 8, s->page_total_size);
    gp32_st32le(head + 12, s->num_pages);
    gp32_st32le(head + 16, s->diff_pages);
    gp32_st32le(head + 20, (uint32_t)s->header_size);
    gp32_st32le(head + 24, (uint32_t)s->base_size);
    memcpy(head + 32, s->base_digest, 16);
    job->delta = 1;
    return job;
bad:
    free(job->image); free(job); return NULL;
}

/* No live emulator memory is accessed by this writer. */
static void smc_write_save(smc_save_job_t *job) {
    save_atomic_t stage;
    /* Hash the immutable snapshot here, off the emulation thread during
     * autosave. Legacy full-card images have no delta checksum field. */
    if (job->delta)
        smc_image_digest(job->image + SMC_SAVE_HEAD, job->size - SMC_SAVE_HEAD, job->image + 48);
    job->ok = save_atomic_begin(&stage, job->path, job->error, sizeof(job->error));
    if (job->ok) {
        job->ok = fwrite(job->image, 1, job->size, stage.file) == job->size;
        if (!job->ok) snprintf(job->error, sizeof(job->error), "writing card changes failed");
        if (job->ok) job->ok = save_atomic_sync(&stage, job->error, sizeof(job->error));
        if (job->ok) job->ok = save_atomic_commit(&stage, job->path, job->error, sizeof(job->error));
        else save_atomic_abort(&stage);
    }
    atomic_store_explicit(&job->done, true, memory_order_release);
}

#if GP32EMU_ENABLE_THREADS
#if defined(_WIN32)
static DWORD WINAPI smc_save_worker(LPVOID arg) { smc_write_save(arg); return 0; }
#else
static void *smc_save_worker(void *arg) { smc_write_save(arg); return NULL; }
#endif
#endif

int smc_autosave_wait(smc_t *s, char *err, size_t err_len) {
    if (!s || !s->save_job) return 1;
    smc_save_job_t *job = s->save_job;
#if GP32EMU_ENABLE_THREADS
#if defined(_WIN32)
    WaitForSingleObject(job->thread, INFINITE);
    CloseHandle(job->thread);
#else
    pthread_join(job->thread, NULL);
#endif
#endif
    int ok = job->ok;
    if (ok && s->write_epoch == job->epoch) s->persist_dirty = 0;
    if (!ok && err && err_len) snprintf(err, err_len, "%s", job->error);
    free(job->image); free(job); s->save_job = NULL;
    return ok;
}

int smc_save_changes(smc_t *s, const char *path, char *err, size_t err_len) {
    if (!s || !path || !s->data) return 0;
    /* Join before the final flush: an older background snapshot must never
     * replace a newer exit save. A failed worker is retried by this flush. */
    smc_autosave_wait(s, NULL, 0);
    smc_save_job_t *job = smc_prepare_save(s, path);
    if (!job) { if (err && err_len) snprintf(err, err_len, "cannot prepare card save"); return 0; }
    smc_write_save(job);
    int ok = job->ok;
    if (!ok && err && err_len) snprintf(err, err_len, "%s", job->error);
    free(job->image); free(job);
    if (ok) { s->dirty = 0; s->persist_dirty = 0; s->autosave_error = 0; }
    return ok;
}

uint64_t smc_host_time_ms(void) {
#if defined(_WIN32)
    return GetTickCount64();
#elif defined(GP32EMU_WASM)
    return (uint64_t)((double)clock() * 1000.0 / CLOCKS_PER_SEC);
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

int smc_autosave_poll(smc_t *s, const char *path, uint64_t now, char *err, size_t err_len) {
    if (!s || !s->data || !path || !path[0]) return 1;
    if (!s->autosave_started) {
        s->autosave_started = 1;
        s->last_attempt = s->quiet_since = now;
        s->observed_epoch = s->write_epoch;
    }
    if (s->save_job && atomic_load_explicit(&s->save_job->done, memory_order_acquire)) {
        if (!smc_autosave_wait(s, err, err_len)) goto failed;
        s->autosave_error = 0;
    }
    if (s->observed_epoch != s->write_epoch) {
        s->observed_epoch = s->write_epoch;
        s->quiet_since = now;
    }
    /* Coalesce a save burst and avoid checkpointing a partially written FAT.
     * At most one attempt per 10 host seconds, after 2 seconds without writes.
     * Persistent storage latency never runs on the emulator thread in threaded builds. */
    if (s->save_job || !s->persist_dirty || now - s->last_attempt < 10000u || now - s->quiet_since < 2000u) return 1;
    s->last_attempt = now;
    smc_save_job_t *job = smc_prepare_save(s, path);
    if (!job) {
        if (err && err_len) snprintf(err, err_len, "cannot prepare automatic card save");
        goto failed;
    }
#if GP32EMU_ENABLE_THREADS
#if defined(_WIN32)
    job->thread = CreateThread(NULL, 0, smc_save_worker, job, 0, NULL);
    int started = job->thread != NULL;
#else
    int started = pthread_create(&job->thread, NULL, smc_save_worker, job) == 0;
#endif
    if (!started) {
        free(job->image); free(job);
        if (err && err_len) snprintf(err, err_len, "cannot start automatic card save worker");
        goto failed;
    }
#else
    smc_write_save(job);
#endif
    s->save_job = job;
    return 1;
failed:
    if (s->autosave_error) return 1;
    s->autosave_error = 1;
    return 0;
}

int smc_load_changes(smc_t *s, const char *path, char *err, size_t err_len) {
    if (!s || !path) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) { if (err && err_len) snprintf(err, err_len, "cannot open card save: %s", path); return 0; }
    uint8_t head[SMC_SAVE_HEAD], digest[16];
    size_t got = fread(head, 1, sizeof(head), f);
    if (got < 8 || memcmp(head, smc_save_magic, 8)) {
        fclose(f);
        return smc_load_file_over_base(s, path, err, err_len);
    }
    uint8_t *body = NULL, *image = NULL;
    int ok = 0;
    if (got != sizeof(head) || !s->base) goto done;
    uint32_t page_bytes = gp32_ld32le(head + 8), pages = gp32_ld32le(head + 12);
    uint32_t count = gp32_ld32le(head + 16), header_bytes = gp32_ld32le(head + 20);
    uint32_t base_bytes = gp32_ld32le(head + 24);
    if (!pages || page_bytes != s->base_page_total_size || pages != s->base_num_pages ||
        count > pages || (header_bytes != 0 && header_bytes != sizeof(s->header)) ||
        base_bytes != s->base_size || gp32_ld32le(head + 28) != 0 ||
        base_bytes > SMC_STATE_MAX_IMAGE || (uint64_t)page_bytes * pages != base_bytes ||
        memcmp(head + 32, s->base_digest, 16)) goto done;
    size_t body_size = header_bytes + (size_t)count * (4u + page_bytes);
    if (fseek(f, 0, SEEK_END) != 0 || ftell(f) != (long)(SMC_SAVE_HEAD + body_size) ||
        fseek(f, SMC_SAVE_HEAD, SEEK_SET) != 0) goto done;
    body = malloc(body_size ? body_size : 1u);
    if (!body || fread(body, 1, body_size, f) != body_size) goto done;
    smc_image_digest(body, body_size, digest);
    if (memcmp(digest, head + 48, 16)) goto done;
    image = malloc((size_t)header_bytes + base_bytes);
    if (!image) goto done;
    memcpy(image, body, header_bytes);
    memcpy(image + header_bytes, s->base, base_bytes);
    uint32_t previous = 0;
    size_t pos = header_bytes;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t page = gp32_ld32le(body + pos);
        if (page >= pages || (i && page <= previous)) goto done;
        memcpy(image + header_bytes + (size_t)page * page_bytes, body + pos + 4u, page_bytes);
        pos += 4u + page_bytes;
        previous = page;
    }
    /* Parse/stage first: truncated, foreign or corrupt input leaves the live
     * card unchanged. Mounting rebuilds the same difference map as guest writes. */
    ok = smc_load_buffer_over_base(s, image, (size_t)header_bytes + base_bytes, err, err_len);
done:
    fclose(f);
    free(body);
    free(image);
    if (!ok && err && err_len) snprintf(err, err_len, "invalid card save or original card mismatch: %s", path);
    return ok;
}

void smc_reset(smc_t *s) {
    if (!s) return;
    s->mode = SM_M_INIT;
    s->pointer_mode = SM_PM_A;
    s->page_addr = 0;
    s->byte_addr = 0;
    s->addr_load_ptr = 0;
    s->accumulated_status = 0;
    s->mode_3065 = false;
    s->program_byte_count = 0;
    s->status = 0xc0; /* ready, not protected */
    if (s->page_reg && s->page_total_size) memset(s->page_reg, 0xff, s->page_total_size);
}

int smc_is_present(const smc_t *s) { return s && s->data && s->num_pages != 0; }
int smc_is_protected(const smc_t *s) { return s ? ((s->status & 0x80) == 0) : 1; }
int smc_is_busy(const smc_t *s) { return s ? ((s->status & 0x40) == 0) : 0; }
size_t smc_image_size(const smc_t *s) { return s ? s->data_size : 0; }

void smc_command_w(smc_t *s, uint8_t data) {
    if (!smc_is_present(s)) return;
    switch (data) {
    case 0xff:
        s->mode = SM_M_INIT;
        s->pointer_mode = SM_PM_A;
        s->status = (uint8_t)((s->status & 0x80) | 0x40);
        s->accumulated_status = 0;
        s->mode_3065 = false;
        break;
    case 0x00:
        s->mode = SM_M_READ;
        s->pointer_mode = SM_PM_A;
        s->addr_load_ptr = 0;
        break;
    case 0x01:
        s->mode = SM_M_READ;
        s->pointer_mode = SM_PM_B;
        s->addr_load_ptr = 0;
        break;
    case 0x50:
        s->mode = SM_M_READ;
        s->pointer_mode = SM_PM_C;
        s->addr_load_ptr = 0;
        break;
    case 0x80:
        s->mode = SM_M_PROGRAM;
        s->addr_load_ptr = 0;
        s->program_byte_count = 0;
        if (s->page_reg) memset(s->page_reg, 0xff, s->page_total_size);
        break;
    case 0x10:
    case 0x15:
        if (s->mode == SM_M_PROGRAM || s->mode == SM_M_RANDOM_DATA_INPUT) {
            s->status = (uint8_t)((s->status & 0x80) | s->accumulated_status);
            if (s->page_addr < s->num_pages) {
                uint8_t *dst = &s->data[(size_t)s->page_addr * s->page_total_size];
                int changed = 0;
                for (uint32_t i = 0; i < s->page_total_size; ++i) {
                    uint8_t value = dst[i] & s->page_reg[i];
                    changed |= value != dst[i];
                    dst[i] = value;
                }
                if (changed) {
                    s->dirty = 1;
                    s->persist_dirty = 1;
                    ++s->write_epoch;
                    smc_diff_refresh(s, s->page_addr, 1u);
                }
            }
            s->status |= 0x40;
            s->accumulated_status = (data == 0x15) ? (uint8_t)(s->status & 0x1f) : 0;
            s->mode = SM_M_INIT;
        } else {
            s->mode = SM_M_INIT;
        }
        break;
    case 0x60:
        s->mode = SM_M_ERASE;
        s->page_addr = 0;
        s->addr_load_ptr = 0;
        break;
    case 0xd0:
        if (s->mode == SM_M_ERASE) {
            uint32_t first = s->page_addr & ~((UINT32_C(1) << s->log2_pages_per_block) - 1u);
            size_t off = (size_t)first * s->page_total_size;
            size_t len = ((size_t)1u << s->log2_pages_per_block) * s->page_total_size;
            if (off < s->data_size) {
                if (off + len > s->data_size) len = s->data_size - off;
                size_t i = 0;
                while (i < len && s->data[off + i] == 0xff) ++i;
                if (i < len) {
                    memset(s->data + off, 0xff, len);
                    s->dirty = 1;
                    s->persist_dirty = 1;
                    ++s->write_epoch;
                    smc_diff_refresh(s, first, (uint32_t)(len / s->page_total_size));
                }
            }
            s->status |= 0x40;
            s->mode = SM_M_INIT;
            if (s->pointer_mode == SM_PM_B) s->pointer_mode = SM_PM_A;
        } else {
            s->mode = SM_M_INIT;
        }
        break;
    case 0x70:
        s->mode = SM_M_READSTATUS;
        break;
    case 0x90:
        s->mode = SM_M_READID;
        s->addr_load_ptr = 0;
        break;
    case 0x30:
        if (s->col_address_cycles == 1) s->mode = SM_M_30;
        else if (s->mode == SM_M_READ && s->addr_load_ptr >= s->col_address_cycles + s->row_address_cycles) { }
        else s->mode = SM_M_INIT;
        break;
    case 0x65:
        if (s->mode == SM_M_30) s->mode_3065 = true;
        else s->mode = SM_M_INIT;
        break;
    case 0x05:
        if (s->mode == SM_M_READ || s->mode == SM_M_RANDOM_DATA_OUTPUT) {
            s->mode = SM_M_RANDOM_DATA_OUTPUT;
            s->addr_load_ptr = 0;
        } else s->mode = SM_M_INIT;
        break;
    case 0xe0:
        if (s->mode != SM_M_RANDOM_DATA_OUTPUT) s->mode = SM_M_INIT;
        break;
    case 0x85:
        if (s->mode == SM_M_PROGRAM || s->mode == SM_M_RANDOM_DATA_INPUT) {
            s->mode = SM_M_RANDOM_DATA_INPUT;
            s->addr_load_ptr = 0;
            s->program_byte_count = 0;
        } else s->mode = SM_M_INIT;
        break;
    default:
        s->mode = SM_M_INIT;
        break;
    }
}

void smc_address_w(smc_t *s, uint8_t data) {
    if (!smc_is_present(s)) return;
    switch (s->mode) {
    case SM_M_READ:
    case SM_M_PROGRAM:
        if (s->addr_load_ptr == 0) s->page_addr = 0;
        if ((s->addr_load_ptr == 0) && (s->col_address_cycles == 1)) {
            switch (s->pointer_mode) {
            case SM_PM_A: s->byte_addr = data; break;
            case SM_PM_B: s->byte_addr = (uint32_t)data + 256u; s->pointer_mode = SM_PM_A; break;
            case SM_PM_C: s->byte_addr = (uint32_t)(data & 0x0f) + (s->mode_3065 ? 256u : s->page_data_size); break;
            }
        } else if (s->addr_load_ptr < s->col_address_cycles) {
            s->byte_addr &= ~(UINT32_C(0xff) << (s->addr_load_ptr * 8u));
            s->byte_addr |= (uint32_t)data << (s->addr_load_ptr * 8u);
        } else if (s->addr_load_ptr < s->col_address_cycles + s->row_address_cycles) {
            uint32_t sh = (s->addr_load_ptr - s->col_address_cycles) * 8u;
            s->page_addr &= ~(UINT32_C(0xff) << sh);
            s->page_addr |= (uint32_t)data << sh;
        }
        s->addr_load_ptr++;
        break;
    case SM_M_ERASE:
        if (s->addr_load_ptr < s->row_address_cycles) {
            s->page_addr &= ~(UINT32_C(0xff) << (s->addr_load_ptr * 8u));
            s->page_addr |= (uint32_t)data << (s->addr_load_ptr * 8u);
        }
        s->addr_load_ptr++;
        break;
    case SM_M_RANDOM_DATA_INPUT:
    case SM_M_RANDOM_DATA_OUTPUT:
        if (s->addr_load_ptr < s->col_address_cycles) {
            s->byte_addr &= ~(UINT32_C(0xff) << (s->addr_load_ptr * 8u));
            s->byte_addr |= (uint32_t)data << (s->addr_load_ptr * 8u);
        }
        s->addr_load_ptr++;
        break;
    case SM_M_READID:
        if (s->addr_load_ptr == 0) s->byte_addr = data;
        s->addr_load_ptr++;
        break;
    default:
        break;
    }
}

uint8_t smc_data_r(smc_t *s) {
    if (!smc_is_present(s)) return 0xff;
    uint8_t reply = 0xff;
    switch (s->mode) {
    case SM_M_READ:
    case SM_M_RANDOM_DATA_OUTPUT:
        if (!s->mode_3065 && s->byte_addr < s->page_total_size && s->page_addr < s->num_pages) {
            reply = s->data[(size_t)s->page_addr * s->page_total_size + s->byte_addr];
        } else if (s->mode_3065 && s->data_uid_present) {
            uint32_t uid_addr = s->page_addr * s->page_total_size + s->byte_addr;
            if (uid_addr < (uint32_t)sizeof(s->data_uid)) reply = s->data_uid[uid_addr];
        }
        s->byte_addr++;
        if ((s->byte_addr == s->page_total_size) && s->sequential_row_read) {
            s->byte_addr = (s->pointer_mode != SM_PM_C) ? 0 : s->page_data_size;
            s->page_addr++;
            if (s->page_addr == s->num_pages) s->page_addr = 0;
        }
        break;
    case SM_M_READSTATUS:
        reply = (uint8_t)(s->status & 0xc1);
        break;
    case SM_M_READID:
        reply = (s->byte_addr < s->id_len) ? s->id[s->byte_addr] : 0;
        s->byte_addr++;
        break;
    default:
        break;
    }
    return reply;
}

void smc_data_w(smc_t *s, uint8_t data) {
    if (!smc_is_present(s)) return;
    switch (s->mode) {
    case SM_M_PROGRAM:
    case SM_M_RANDOM_DATA_INPUT:
        if (s->program_byte_count++ < s->page_total_size && s->page_reg) {
            if (s->byte_addr < s->page_total_size) s->page_reg[s->byte_addr] = data;
        }
        s->byte_addr++;
        if (s->byte_addr == s->page_total_size) s->byte_addr = (s->pointer_mode != SM_PM_C) ? 0 : s->page_data_size;
        break;
    default:
        break;
    }
}

typedef struct smc_state_image {
    size_t data_size;
    uint8_t header[1024];
    size_t header_size;
    int dirty;
    uint32_t page_data_size;
    uint32_t page_total_size;
    uint32_t num_pages;
    uint32_t log2_pages_per_block;
    uint32_t col_address_cycles;
    uint32_t row_address_cycles;
    uint32_t sequential_row_read;
    uint8_t id[5];
    uint32_t id_len;
    uint8_t data_uid[256 + 16];
    int data_uid_present;
    sm_mode_t mode;
    sm_pointer_mode_t pointer_mode;
    uint32_t page_addr;
    uint32_t byte_addr;
    uint32_t addr_load_ptr;
    uint8_t status;
    uint8_t accumulated_status;
    bool mode_3065;
    uint32_t program_byte_count;
} smc_state_image_t;

/* Zero padding of the delta entry region; one shared buffer keeps the write
 * loop bounded no matter how large the session budget is. */
static int smc_state_write_zeros(state_io_t *io, uint64_t bytes) {
    static const uint8_t zeros[256] = {0};
    while (bytes) {
        size_t chunk = bytes > sizeof(zeros) ? sizeof(zeros) : (size_t)bytes;
        if (!state_io_write(io, zeros, chunk)) return 0;
        bytes -= chunk;
    }
    return 1;
}

int smc_state_save_io(const smc_t *s, state_io_t *io) {
    if (!s || !io) return 0;
    smc_state_image_t st;
    memset(&st, 0, sizeof(st));
    st.data_size = s->data_size;
    memcpy(st.header, s->header, sizeof(st.header));
    st.header_size = s->header_size;
    st.dirty = s->dirty;
    st.page_data_size = s->page_data_size;
    st.page_total_size = s->page_total_size;
    st.num_pages = s->num_pages;
    st.log2_pages_per_block = s->log2_pages_per_block;
    st.col_address_cycles = s->col_address_cycles;
    st.row_address_cycles = s->row_address_cycles;
    st.sequential_row_read = s->sequential_row_read;
    memcpy(st.id, s->id, sizeof(st.id));
    st.id_len = s->id_len;
    memcpy(st.data_uid, s->data_uid, sizeof(st.data_uid));
    st.data_uid_present = s->data_uid_present;
    st.mode = s->mode;
    st.pointer_mode = s->pointer_mode;
    st.page_addr = s->page_addr;
    st.byte_addr = s->byte_addr;
    st.addr_load_ptr = s->addr_load_ptr;
    st.status = s->status;
    st.accumulated_status = s->accumulated_status;
    st.mode_3065 = s->mode_3065;
    st.program_byte_count = s->program_byte_count;
    /* The delta form only changes the section body; its scalar head keeps the
     * dirty bit the older layout stored and marks the form in another bit. */
    int delta = smc_state_delta_usable(s);
    if (delta) st.dirty |= (int)SMC_STATE_DELTA_FLAG;
    if (!state_io_write(io, &st, sizeof(st))) return 0;
    if (delta) {
        smc_state_delta_head_t head;
        memset(&head, 0, sizeof(head));
        head.tag = SMC_STATE_DELTA_TAG;
        head.capacity = s->state_entries;
        head.entry_bytes = 4u + s->page_total_size;
        head.count = s->diff_pages;
        memcpy(head.base_digest, s->base_digest, sizeof(head.base_digest));
        head.base_size = s->data_size;
        head.base_num_pages = s->num_pages;
        if (!state_io_write(io, &head, sizeof(head))) return 0;
        for (uint32_t page = 0; page < s->num_pages; ++page) {
            if (!(s->page_diff[page >> 3] & (uint8_t)(1u << (page & 7u)))) continue;
            if (!state_io_write(io, &page, sizeof(page))) return 0;
            if (!state_io_write(io, s->data + (size_t)page * s->page_total_size, s->page_total_size)) return 0;
        }
        /* Pad the entry region so one mounted card always serializes to the
         * same size, whatever the guest has written to it. */
        uint64_t pad = (uint64_t)(s->state_entries - s->diff_pages) * (4u + (uint64_t)s->page_total_size);
        if (!smc_state_write_zeros(io, pad)) return 0;
    } else if (!state_io_write(io, s->data, s->data_size)) {
        return 0;
    }
    return state_io_write(io, s->page_reg, s->page_total_size);
}

typedef struct smc_state_stage {
    smc_state_image_t st;
    int delta;
    uint32_t capacity;
    uint32_t count;
    uint8_t *data;    /* full form: the new live image */
    uint8_t *entries; /* delta form: count * (4 + page_total_size) */
    uint8_t *page_reg;
} smc_state_stage_t;

static void *smc_alloc_bytes(size_t bytes) {
    return malloc(bytes ? bytes : 1u);
}

/* Scalar fields every layout restores. The delta marker bit is stream syntax,
 * not device state: only the dirty bit is restored. */
static void smc_apply_state_fields(smc_t *s, const smc_state_image_t *st) {
    memcpy(s->header, st->header, sizeof(s->header));
    s->header_size = st->header_size <= sizeof(s->header) ? st->header_size : 0u;
    s->dirty = st->dirty & 1;
    s->page_data_size = st->page_data_size;
    s->page_total_size = st->page_total_size;
    s->num_pages = st->num_pages;
    s->log2_pages_per_block = st->log2_pages_per_block;
    s->col_address_cycles = st->col_address_cycles;
    s->row_address_cycles = st->row_address_cycles;
    s->sequential_row_read = st->sequential_row_read;
    memcpy(s->id, st->id, sizeof(s->id));
    s->id_len = st->id_len <= sizeof(s->id) ? st->id_len : 0u;
    memcpy(s->data_uid, st->data_uid, sizeof(s->data_uid));
    s->data_uid_present = st->data_uid_present;
    s->mode = st->mode;
    s->pointer_mode = st->pointer_mode;
    s->page_addr = st->page_addr;
    s->byte_addr = st->byte_addr;
    s->addr_load_ptr = st->addr_load_ptr;
    s->status = st->status;
    s->accumulated_status = st->accumulated_status;
    s->mode_3065 = st->mode_3065;
    s->program_byte_count = st->program_byte_count;
}

smc_state_stage_t *smc_state_stage_begin(smc_t *s, state_io_t *io, smc_state_format_t format) {
    if (!s || !io) return NULL;
    smc_state_stage_t *stage = (smc_state_stage_t *)calloc(1, sizeof(*stage));
    if (!stage) return NULL;
    smc_state_image_t *st = &stage->st;
    if (!state_io_read(io, st, sizeof(*st))) goto fail;
    if (st->data_size > SMC_STATE_MAX_IMAGE || st->page_total_size > SMC_STATE_MAX_PAGE_TOTAL) goto fail;
    if ((st->page_total_size == 0) != (st->num_pages == 0)) goto fail;
    if (st->page_total_size ? st->data_size != (size_t)st->num_pages * st->page_total_size
                            : st->data_size != 0) goto fail;
    if (st->dirty & ~(int)(SMC_STATE_DELTA_FLAG | 1)) goto fail;
    stage->delta = format == SMC_STATE_FORMAT_V14 && (st->dirty & (int)SMC_STATE_DELTA_FLAG) != 0;
    if (stage->delta) {
        smc_state_delta_head_t head;
        if (!state_io_read(io, &head, sizeof(head))) goto fail;
        if (head.tag != SMC_STATE_DELTA_TAG || head.entry_bytes != 4u + st->page_total_size) goto fail;
        if (head.capacity == 0 || head.capacity > st->num_pages || head.count > head.capacity) goto fail;
        if (head.base_size != st->data_size || head.base_num_pages != st->num_pages) goto fail;
        /* The state carries only what differs from its base. A stream whose base
         * is not the image this session holds cannot be reconstructed, so it is
         * refused before the live card is touched. */
        if (!smc_base_geometry_ok(s) || s->data_size != st->data_size ||
            s->page_total_size != st->page_total_size || s->num_pages != st->num_pages ||
            memcmp(s->base_digest, head.base_digest, sizeof(head.base_digest)) != 0) goto fail;
        stage->capacity = head.capacity;
        stage->count = head.count;
        uint64_t bytes = (uint64_t)head.count * head.entry_bytes;
        stage->entries = (uint8_t *)smc_alloc_bytes((size_t)bytes);
        if (!stage->entries || !state_io_read(io, stage->entries, (size_t)bytes)) goto fail;
        uint32_t previous = 0;
        for (uint32_t i = 0; i < head.count; ++i) {
            uint32_t page = 0;
            memcpy(&page, stage->entries + (size_t)i * head.entry_bytes, sizeof(page));
            if (page >= st->num_pages || (i && page <= previous)) goto fail;
            previous = page;
        }
        if (!state_io_skip(io, (size_t)((uint64_t)(head.capacity - head.count) * head.entry_bytes))) goto fail;
    } else {
        stage->data = (uint8_t *)smc_alloc_bytes(st->data_size);
        if (!stage->data || !state_io_read(io, stage->data, st->data_size)) goto fail;
    }
    stage->page_reg = (uint8_t *)smc_alloc_bytes(st->page_total_size);
    if (!stage->page_reg || !state_io_read(io, stage->page_reg, st->page_total_size)) goto fail;
    return stage;
fail:
    smc_state_stage_destroy(stage);
    return NULL;
}

void smc_state_stage_destroy(smc_state_stage_t *stage) {
    if (!stage) return;
    free(stage->data);
    free(stage->entries);
    free(stage->page_reg);
    free(stage);
}

void smc_state_stage_commit(smc_t *s, smc_state_stage_t *stage) {
    if (!s || !stage) return;
    if (stage->delta) {
        /* The live card is the image this session mounted. Every page that
         * differs from the base and is not carried by the state returns to the
         * base image, then the state's own pages are applied, so the card ends
         * up exactly as it was when the state was saved. */
        for (uint32_t page = 0; page < s->num_pages; ++page) {
            if (s->page_diff && (s->page_diff[page >> 3] & (uint8_t)(1u << (page & 7u))))
                memcpy(s->data + (size_t)page * s->page_total_size,
                       s->base + (size_t)page * s->page_total_size, s->page_total_size);
        }
        if (s->page_diff && s->base_num_pages)
            memset(s->page_diff, 0, ((size_t)s->base_num_pages + 7u) / 8u);
        s->diff_pages = 0;
        smc_apply_state_fields(s, &stage->st);
        for (uint32_t i = 0; i < stage->count; ++i) {
            const uint8_t *entry = stage->entries + (size_t)i * (4u + s->page_total_size);
            uint32_t page = 0;
            memcpy(&page, entry, sizeof(page));
            memcpy(s->data + (size_t)page * s->page_total_size, entry + 4u, s->page_total_size);
            smc_diff_bit(s, page, smc_page_differs(s, page));
        }
        free(s->page_reg);
        s->page_reg = stage->page_reg;
        stage->page_reg = NULL;
    } else {
        smc_mount_image_t img;
        memset(&img, 0, sizeof(img));
        img.data = stage->data;
        img.page_reg = stage->page_reg;
        img.data_size = stage->st.data_size;
        memcpy(img.header, stage->st.header, sizeof(img.header));
        img.header_size = stage->st.header_size;
        img.page_data_size = stage->st.page_data_size;
        img.page_total_size = stage->st.page_total_size;
        img.num_pages = stage->st.num_pages;
        img.log2_pages_per_block = stage->st.log2_pages_per_block;
        img.col_address_cycles = stage->st.col_address_cycles;
        img.row_address_cycles = stage->st.row_address_cycles;
        img.sequential_row_read = stage->st.sequential_row_read;
        memcpy(img.id, stage->st.id, sizeof(img.id));
        img.id_len = stage->st.id_len;
        memcpy(img.data_uid, stage->st.data_uid, sizeof(img.data_uid));
        img.data_uid_present = stage->st.data_uid_present;
        /* A full or older stream replaces the card. The base survives when the
         * new card still has the shape it describes, so this session's own
         * states keep the delta form; otherwise they carry the whole image. */
        smc_install_image(s, &img, 1);
        smc_mount_image_free(&img);
        stage->data = NULL;
        stage->page_reg = NULL;
        smc_apply_state_fields(s, &stage->st);
    }
    /* Restoring a clean old slot can roll back an already-persisted card. */
    s->persist_dirty = s->data != NULL;
    ++s->write_epoch;
    smc_state_stage_destroy(stage);
}

int smc_state_load_io(smc_t *s, state_io_t *io, smc_state_format_t format) {
    if (!s || !io) return 0;
    smc_state_stage_t *stage = smc_state_stage_begin(s, io, format);
    if (!stage) return 0;
    smc_state_stage_commit(s, stage);
    return 1;
}

int smc_state_save(const smc_t *s, FILE *f) {
    state_io_t io = state_io_file(f);
    return smc_state_save_io(s, &io);
}

int smc_state_load(smc_t *s, FILE *f) {
    state_io_t io = state_io_file(f);
    return smc_state_load_io(s, &io, SMC_STATE_FORMAT_V14);
}
