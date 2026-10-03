#include "gp32emu/video_effects.h"

#include <stdlib.h>
#include <string.h>

static uint32_t blend_half(uint32_t a, uint32_t b) {
    /* Same result as blending each 8-bit channel separately:
       floor((a + b) / 2) == ((a & 0xfe) >> 1) + ((b & 0xfe) >> 1) + (a & b & 1)
       for every channel.  Every lane stays below 256, so lanes never carry into
       their neighbour, and the alpha byte is dropped exactly as before. */
    return ((a & 0x00fefefeu) >> 1) + ((b & 0x00fefefeu) >> 1) + (a & b & 0x00010101u);
}

static uint32_t blend_lcd_persistence(uint32_t cur, uint32_t old) {
    /* Mild GP32 FLU-style sample-and-hold/response persistence.  The current
       frame remains dominant so menus and pixel art stay legible, while the
       previous persisted output contributes a short motion trail.

       Per channel this is (3*cur + old + 2) >> 2.  Splitting both inputs into
       their top six and low two bits keeps every packed lane below 256, so the
       arithmetic below is exact for each channel and lanes never carry into
       each other, while the alpha byte is dropped exactly as before. */
    const uint32_t cur_hi = (cur >> 2) & 0x003f3f3fu;
    const uint32_t old_hi = (old >> 2) & 0x003f3f3fu;
    const uint32_t low = (cur & 0x00030303u) * 3u + (old & 0x00030303u) + 0x00020202u;
    return cur_hi * 3u + old_hi + ((low & 0x000c0c0cu) >> 2);
}

int gp32_video_effects_init(gp32_video_effects_t *fx) {
    if (!fx) return 0;
    memset(fx, 0, sizeof(*fx));
    fx->prev_raw = (uint32_t *)malloc((size_t)GP32_VIDEO_EFFECTS_PIXELS * sizeof(uint32_t));
    fx->prev_lcd = (uint32_t *)malloc((size_t)GP32_VIDEO_EFFECTS_PIXELS * sizeof(uint32_t));
    if (!fx->prev_raw || !fx->prev_lcd) {
        gp32_video_effects_shutdown(fx);
        return 0;
    }
    return 1;
}

void gp32_video_effects_shutdown(gp32_video_effects_t *fx) {
    if (!fx) return;
    free(fx->prev_raw);
    free(fx->prev_lcd);
    memset(fx, 0, sizeof(*fx));
}

void gp32_video_effects_reset(gp32_video_effects_t *fx) {
    if (!fx) return;
    fx->have_prev_raw = 0;
    fx->have_prev_lcd = 0;
}

void gp32_video_effects_set(gp32_video_effects_t *fx, int lcd_persistence, int frame_interpolation) {
    if (!fx) return;
    lcd_persistence = lcd_persistence != 0;
    frame_interpolation = frame_interpolation != 0;
    if (fx->lcd_persistence != lcd_persistence || fx->frame_interpolation != frame_interpolation) gp32_video_effects_reset(fx);
    fx->lcd_persistence = lcd_persistence;
    fx->frame_interpolation = frame_interpolation;
}

int gp32_video_effects_lcd_persistence(const gp32_video_effects_t *fx) { return fx ? fx->lcd_persistence : 0; }
int gp32_video_effects_frame_interpolation(const gp32_video_effects_t *fx) { return fx ? fx->frame_interpolation : 0; }
int gp32_video_effects_active(const gp32_video_effects_t *fx) { return fx && (fx->lcd_persistence || fx->frame_interpolation); }

int gp32_video_effects_process_320x240(gp32_video_effects_t *fx, const uint32_t *src_rgb, uint32_t *dst_rgb) {
    if (!fx || !src_rgb || !dst_rgb || !fx->prev_raw || !fx->prev_lcd) return 0;
    const int interp = fx->frame_interpolation;
    const int lcd = fx->lcd_persistence;
    if (!interp && !lcd) {
        if (dst_rgb != src_rgb) memcpy(dst_rgb, src_rgb, (size_t)GP32_VIDEO_EFFECTS_PIXELS * sizeof(uint32_t));
        gp32_video_effects_reset(fx);
        return 1;
    }
    if ((interp && !fx->have_prev_raw) || (lcd && !fx->have_prev_lcd)) {
        if (dst_rgb != src_rgb) memcpy(dst_rgb, src_rgb, (size_t)GP32_VIDEO_EFFECTS_PIXELS * sizeof(uint32_t));
        memcpy(fx->prev_raw, src_rgb, (size_t)GP32_VIDEO_EFFECTS_PIXELS * sizeof(uint32_t));
        memcpy(fx->prev_lcd, src_rgb, (size_t)GP32_VIDEO_EFFECTS_PIXELS * sizeof(uint32_t));
        fx->have_prev_raw = 1;
        fx->have_prev_lcd = 1;
        return 1;
    }
    /* Both effects are enabled and their histories are seeded, so the per-pixel
       flags of the original single loop are loop-invariant.  Split the frame
       into one branch-free loop per mode and touch only the history buffer the
       mode actually samples.  A history buffer is only read while its own
       effect is enabled, and gp32_video_effects_set() resets the histories
       whenever a flag changes, so skipping the stores of a buffer the current
       mode never samples stays invisible: the first frame after such a change
       is re-seeded before any blend reads history. */
    uint32_t *const prev_raw = fx->prev_raw;
    uint32_t *const prev_lcd = fx->prev_lcd;
    if (interp && lcd) {
        for (uint32_t i = 0; i < GP32_VIDEO_EFFECTS_PIXELS; ++i) {
            const uint32_t raw = src_rgb[i] & 0x00ffffffu;
            const uint32_t p = blend_lcd_persistence(blend_half(raw, prev_raw[i]), prev_lcd[i]);
            prev_raw[i] = raw;
            prev_lcd[i] = p;
            dst_rgb[i] = p;
        }
    } else if (interp) {
        for (uint32_t i = 0; i < GP32_VIDEO_EFFECTS_PIXELS; ++i) {
            const uint32_t raw = src_rgb[i] & 0x00ffffffu;
            const uint32_t p = blend_half(raw, prev_raw[i]);
            prev_raw[i] = raw;
            dst_rgb[i] = p;
        }
    } else {
        for (uint32_t i = 0; i < GP32_VIDEO_EFFECTS_PIXELS; ++i) {
            const uint32_t raw = src_rgb[i] & 0x00ffffffu;
            const uint32_t p = blend_lcd_persistence(raw, prev_lcd[i]);
            prev_lcd[i] = p;
            dst_rgb[i] = p;
        }
    }
    fx->have_prev_raw = 1;
    fx->have_prev_lcd = 1;
    return 1;
}
