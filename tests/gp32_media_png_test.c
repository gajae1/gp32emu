/* Screenshots are PNG files. Decoding the written file with miniz's inflate
 * (separate from its PNG writer) must give a valid 320x240 8-bit RGB PNG with
 * correct chunk CRCs, holding exactly the pixels gp32_media_frame_to_bgra_320x240
 * produces for both the plain and the raw (rotated) LCD layout, in R, G, B
 * order. A path that cannot be opened reports an error and creates no file. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "media/gp32_media.h"

typedef unsigned long mz_ulong;
mz_ulong mz_crc32(mz_ulong crc, const unsigned char *ptr, size_t buf_len);
void *tinfl_decompress_mem_to_heap(const void *src, size_t src_len, size_t *out_len, int flags);
void mz_free(void *p);
#define TINFL_FLAG_PARSE_ZLIB_HEADER 1

#define W GP32_MEDIA_LCD_W
#define H GP32_MEDIA_LCD_H

static int failures;
#define CHECK(cond, what) do { if (!(cond)) { fprintf(stderr, "FAIL %s: %s\n", label, what); ++failures; } } while (0)

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = n > 0 ? malloc((size_t)n) : NULL;
    if (p && fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); p = NULL; }
    fclose(f);
    *len = p ? (size_t)n : 0;
    return p;
}

/* Returns the W*H*3 RGB pixels of the PNG, or NULL. */
static uint8_t *decode_png(const char *label, const uint8_t *png, size_t len) {
    static const uint8_t sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    if (len < 8 || memcmp(png, sig, 8)) { CHECK(0, "PNG signature"); return NULL; }
    size_t pos = 8, idat_len = 0;
    uint8_t *idat = NULL;
    int ihdr_ok = 0, crc_ok = 1, iend = 0;
    while (!iend && pos + 12 <= len) {
        uint32_t n = be32(png + pos);
        const uint8_t *type = png + pos + 4;
        if (n > len - pos - 12) break;
        crc_ok &= mz_crc32(0, type, (size_t)n + 4) == be32(type + 4 + n);
        if (!memcmp(type, "IHDR", 4)) {
            const uint8_t *d = type + 4;
            ihdr_ok = n == 13 && be32(d) == W && be32(d + 4) == H && d[8] == 8 && d[9] == 2 && !d[10] && !d[11] && !d[12];
        } else if (!memcmp(type, "IDAT", 4)) {
            uint8_t *grown = realloc(idat, idat_len + n);
            if (!grown) break;
            idat = grown;
            memcpy(idat + idat_len, type + 4, n);
            idat_len += n;
        } else if (!memcmp(type, "IEND", 4)) {
            iend = 1;
        }
        pos += 12 + (size_t)n;
    }
    CHECK(ihdr_ok, "IHDR is 320x240, 8-bit RGB, not interlaced");
    CHECK(crc_ok, "every chunk CRC matches");
    CHECK(iend && pos == len, "IEND closes the file");
    size_t raw_len = 0;
    uint8_t *raw = idat ? tinfl_decompress_mem_to_heap(idat, idat_len, &raw_len, TINFL_FLAG_PARSE_ZLIB_HEADER) : NULL;
    free(idat);
    const size_t row = 1 + (size_t)W * 3;
    if (!raw || raw_len != row * H) { CHECK(0, "image data inflates to 240 rows"); mz_free(raw); return NULL; }
    uint8_t *rgb = malloc((size_t)W * H * 3);
    int unfiltered = 1;
    for (size_t y = 0; rgb && y < H; ++y) {
        unfiltered &= raw[y * row] == 0;
        memcpy(rgb + y * W * 3, raw + y * row + 1, (size_t)W * 3);
    }
    mz_free(raw);
    CHECK(unfiltered, "rows use filter type 0, as this decoder expects");
    return rgb;
}

static uint8_t *check_frame(const char *label, const char *dir, const gp32_framebuffer_desc_t *fb) {
    char path[1024], err[256] = { 0 };
    snprintf(path, sizeof(path), "%s/%s.png", dir, label);
    CHECK(gp32_media_write_png_320x240(path, fb, err, sizeof(err)), err);
    size_t len = 0;
    uint8_t *png = read_file(path, &len);
    remove(path);
    if (!png) { CHECK(0, "PNG file readable"); return NULL; }
    uint8_t *rgb = decode_png(label, png, len);
    free(png);
    uint32_t *want = malloc((size_t)W * H * sizeof(uint32_t));
    if (!rgb || !want || !gp32_media_frame_to_bgra_320x240(fb, want)) { free(want); return rgb; }
    size_t diff = 0;
    for (size_t i = 0; i < (size_t)W * H; ++i)
        diff += rgb[i * 3] != (uint8_t)(want[i] >> 16) || rgb[i * 3 + 1] != (uint8_t)(want[i] >> 8) || rgb[i * 3 + 2] != (uint8_t)want[i];
    free(want);
    CHECK(diff == 0, "decoded pixels equal the converted frame");
    return rgb;
}

static void check_portrait_pixels(void) {
    enum { N = W * H, MAX_STRIDE = H + 3 };
    _Alignas(16) static uint32_t source[W * MAX_STRIDE + 2], output[N + 2];
    static uint32_t want[N];
    const uint32_t strides[] = { H, MAX_STRIDE };
    /* Valid uint32_t alignment, deliberately not 16-byte vector alignment. */
    uint32_t *src = source + 1, *out = output + 1;
    for (size_t c = 0; c < sizeof(strides) / sizeof(strides[0]); ++c) {
        const char *label = c ? "portrait-padded-unaligned" : "portrait-tight-unaligned";
        uint32_t stride = strides[c];
        size_t span = (size_t)(W - 1) * stride + H;
        memset(source, 0xa5, sizeof(source));
        memset(output, 0xa5, sizeof(output));
        /* Each RGB value uniquely identifies a source coordinate. Scatter the
         * expected pixels: (column,row) -> (row,239-column), independently of
         * the converter's destination traversal and 4x4 tile layout. */
        for (uint32_t row = 0; row < W; ++row) {
            for (uint32_t col = 0; col < H; ++col) {
                uint32_t rgb = (col << 16) | row;
                src[(size_t)row * stride + col] = 0xff000000u | rgb;
                want[(size_t)(H - 1 - col) * W + row] = rgb;
            }
        }
        src[0] = 0; /* Include an already-zero alpha alongside nonzero alpha. */
        const gp32_framebuffer_desc_t fb = { src, H, W, stride, 0 };
        CHECK(gp32_media_frame_to_bgra_320x240(&fb, out), "portrait conversion succeeds");
        CHECK(!memcmp(out, want, sizeof(want)), "rotated coordinates and zero output alpha");
        CHECK(source[0] == 0xa5a5a5a5u && src[span] == 0xa5a5a5a5u, "source bounds unchanged");
        CHECK(output[0] == 0xa5a5a5a5u && out[N] == 0xa5a5a5a5u, "output bounds unchanged");
    }
}

static void check_portrait_overlap(void) {
    enum { N = W * H, WORDS = W * (H + 3) + 8, SRC = 4 };
    static uint32_t actual[WORDS], want[WORDS];
    static const struct { const char *label; uint32_t stride; int offset; } cases[] = {
        { "portrait-in-place", H, 0 },
        { "portrait-overlap-after", H + 3, 3 },
        { "portrait-overlap-before", H + 3, -3 },
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        const char *label = cases[c].label;
        size_t dst = (size_t)(SRC + cases[c].offset);
        uint32_t stride = cases[c].stride;
        for (size_t i = 0; i < WORDS; ++i)
            actual[i] = want[i] = 0xa5000000u | (uint32_t)(i + 1);
        /* ORIGINAL scalar order: advance one output pixel at a time, reading
         * the already-mutated backing buffer. A snapshot/temporary transpose
         * is intentionally NOT the overlap oracle. Unique RGB tags expose
         * every read affected by an earlier write, including across rows. */
        for (size_t i = 0; i < N; ++i) {
            size_t row = i % W, col = H - 1 - i / W;
            want[dst + i] = want[SRC + row * stride + col] & 0x00ffffffu;
        }
        const gp32_framebuffer_desc_t fb = { actual + SRC, H, W, stride, 0 };
        CHECK(gp32_media_frame_to_bgra_320x240(&fb, actual + dst), "overlap conversion succeeds");
        CHECK(!memcmp(actual, want, sizeof(want)), "original sequential pixels and untouched surrounding storage");
    }
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    const char *label = "setup";
    enum { STRIDE = W + 16 };
    static uint32_t plain[H * STRIDE], rotated[W * H];
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < STRIDE; ++x)
            plain[y * STRIDE + x] = 0xff000000u | (x * 5u & 255u) << 16 | (y * 3u & 255u) << 8 | ((x ^ y) & 255u);
    plain[0] = 0x00ff0000u; /* 0x00RRGGBB, as the D3D11 and GDI presenters read it */
    for (uint32_t i = 0; i < W * H; ++i) rotated[i] = (i * 2654435761u) & 0x00ffffffu;
    const gp32_framebuffer_desc_t plain_fb = { plain, W, H, STRIDE, 1 };
    const gp32_framebuffer_desc_t raw_fb = { rotated, H, W, H, 2 };   /* 240x320 raw LCD */

    uint8_t *rgb = check_frame("plain", dir, &plain_fb);
    label = "plain";
    CHECK(rgb && rgb[0] == 255 && rgb[1] == 0 && rgb[2] == 0, "red source pixel stays red");
    free(rgb);
    free(check_frame("rotated", dir, &raw_fb));

    check_portrait_pixels();
    check_portrait_overlap();

    label = "unwritable";
    char path[1024], err[256] = { 0 };
    snprintf(path, sizeof(path), "%s/no-such-folder/shot.png", dir);
    CHECK(!gp32_media_write_png_320x240(path, &plain_fb, err, sizeof(err)) && err[0], "open failure is reported");
    FILE *f = fopen(path, "rb");
    CHECK(!f, "no file is created");
    if (f) fclose(f);

    if (failures) return 1;
    puts("gp32_media_png: ok");
    return 0;
}
