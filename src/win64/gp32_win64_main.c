#include "gp32emu/gp32.h"
#include "gp32emu/platform.h"
#include "gp32_win64_audio.h"
#include "gp32_win64_sdl_input.h"
#include "gp32_win64_video.h"
#include "gp32_win64_ui.h"
#include "media/gp32_media.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GP32_LCD_W 320u
#define GP32_LCD_H 240u
#define GP32_DEFAULT_CLOCK_HZ 66000000u
#define IDI_GP32EMU 101

#define IDM_FILE_OPEN_BIOS       1001
#define IDM_FILE_OPEN_SMC        1002
#define IDM_FILE_OPEN_FXE        1003
#define IDM_FILE_OPEN_FPK        1004
#define IDM_FILE_SAVE_STATE      1005
#define IDM_FILE_LOAD_STATE      1006
#define IDM_FILE_SCREENSHOT      1007
#define IDM_FILE_RECORD_MKV      1008
#define IDM_FILE_STOP_RECORDING  1009
#define IDM_FILE_EXIT            1010
#define IDM_EMU_RUN              1101
#define IDM_EMU_RESET            1102
#define IDM_EMU_JIT              1103
#define IDM_VIDEO_D3D11          1201
#define IDM_VIDEO_GDI            1202
#define IDM_VIDEO_INTEGER        1203
#define IDM_VIDEO_FULLSCREEN     1204
#define IDM_VIDEO_KEEP_ASPECT    1205
#define IDM_VIDEO_LCD_PERSISTENCE 1206
#define IDM_VIDEO_FRAME_INTERP    1207
#define IDM_AUDIO_WAVEOUT        1301
#define IDM_AUDIO_WASAPI_SHARED  1302
#define IDM_AUDIO_WASAPI_EXCL    1303
#define IDM_CONFIG_SET_BIOS     1401
#define IDM_CONFIG_CLEAR_BIOS   1402
#define IDM_CONFIG_BOOT_BIOS    1403
#define IDM_CONFIG_USE_HLE      1404
#define IDM_CONFIG_KEYBOARD     1405
#define IDM_FILE_LIBRARY        1011

typedef struct app_state {
    HINSTANCE inst;
    HWND hwnd;
    HMENU menu;
    gp32_t *emu;
    gp32_win64_video_t *video;
    gp32_win64_audio_t *audio;
    gp32_win64_sdl_input_t *sdl_input;
    gp32_media_recorder_t *recorder;
    char bios[MAX_PATH];
    char smc[MAX_PATH];
    char smc_save[MAX_PATH]; /* Belongs to the loaded machine, not the next selection. */
    char smc_legacy[MAX_PATH];
    char fxe[MAX_PATH];
    char fpk[MAX_PATH];
    char state_path[MAX_PATH];
    char screenshot_path[MAX_PATH];
    char record_path[MAX_PATH];
    char config_path[MAX_PATH];
    char video_backend[16];
    gp32_win64_preferences_t preferences;
    gp32_win64_audio_mode_t audio_mode;
    WINDOWPLACEMENT windowed_placement;
    LONG_PTR windowed_style;
    LONG_PTR windowed_exstyle;
    uint32_t keyboard_buttons;
    uint32_t buttons;
    uint64_t frame_index;
    uint64_t fps_tick_ms;
    uint32_t render_frames;
    uint32_t emu_frames;
    int running;
    int jit;
    int integer_scaling;
    int keep_aspect;
    int lcd_persistence;
    int frame_interpolation;
    int use_hle;
    int recording;
    int fullscreen;
    int quit;
    int no_audio;
    LARGE_INTEGER qpf;
    LARGE_INTEGER last_qpc;
    uint64_t accum_units;
} app_state_t;

static app_state_t *g_app;

static void app_set_status(app_state_t *a, const char *msg) {
    (void)a;
    (void)msg;
}

static void app_show_status_error(app_state_t *a, const char *msg) {
    app_set_status(a, msg ? msg : "Error");
    fprintf(stderr, "GP32emu: %s\n", msg ? msg : "Error");
    if (a && a->hwnd) MessageBoxA(IsWindow(a->hwnd) ? a->hwnd : NULL,
                                 msg ? msg : "Error", "GP32emu", MB_OK | MB_ICONERROR);
}

static void app_make_config_path(app_state_t *a) {
    if (!a) return;
    DWORD n = GetModuleFileNameA(NULL, a->config_path, (DWORD)sizeof(a->config_path));
    if (n == 0 || n >= sizeof(a->config_path)) {
        snprintf(a->config_path, sizeof(a->config_path), "GP32emu.ini");
        return;
    }
    char *slash = strrchr(a->config_path, '\\');
    char *slash2 = strrchr(a->config_path, '/');
    if (slash2 && (!slash || slash2 > slash)) slash = slash2;
    if (slash) slash[1] = 0;
    else a->config_path[0] = 0;
    strncat(a->config_path, "GP32emu.ini", sizeof(a->config_path) - strlen(a->config_path) - 1u);
}

static void app_load_config(app_state_t *a) {
    if (!a) return;
    app_make_config_path(a);
    gp32_win64_preferences_load(&a->preferences, a->config_path);
    GetPrivateProfileStringA("Paths", "BIOS", "", a->bios, (DWORD)sizeof(a->bios), a->config_path);
    GetPrivateProfileStringA("Video", "Backend", a->video_backend[0] ? a->video_backend : "d3d11", a->video_backend, (DWORD)sizeof(a->video_backend), a->config_path);
    a->integer_scaling = GetPrivateProfileIntA("Video", "IntegerScaling", a->integer_scaling, a->config_path) ? 1 : 0;
    a->keep_aspect = GetPrivateProfileIntA("Video", "KeepAspect", a->keep_aspect, a->config_path) ? 1 : 0;
    a->lcd_persistence = GetPrivateProfileIntA("Video", "LCDPersistence", a->lcd_persistence, a->config_path) ? 1 : 0;
    a->frame_interpolation = GetPrivateProfileIntA("Video", "FrameInterpolation", a->frame_interpolation, a->config_path) ? 1 : 0;
    if (a->keep_aspect) a->integer_scaling = 0;
    a->jit = GetPrivateProfileIntA("Emulation", "JIT", a->jit, a->config_path) ? 1 : 0;
    a->use_hle = GetPrivateProfileIntA("Emulation", "UseHLE", a->bios[0] ? 0 : 1, a->config_path) ? 1 : 0;
    if (a->bios[0]) a->use_hle = 0;
    else a->use_hle = 1;
    char audio[32];
    GetPrivateProfileStringA("Audio", "Backend", "waveout", audio, (DWORD)sizeof(audio), a->config_path);
    if (!strcmp(audio, "waveout")) a->audio_mode = GP32_WIN64_AUDIO_WAVEOUT;
    else if (!strcmp(audio, "wasapi_shared")) a->audio_mode = GP32_WIN64_AUDIO_WASAPI_SHARED;
    else if (!strcmp(audio, "wasapi_exclusive")) a->audio_mode = GP32_WIN64_AUDIO_WASAPI_EXCLUSIVE;
    else a->audio_mode = GP32_WIN64_AUDIO_WAVEOUT;
}

static void app_save_config(app_state_t *a) {
    if (!a || !a->config_path[0]) return;
    gp32_win64_preferences_save(&a->preferences, a->config_path);
    WritePrivateProfileStringA("Paths", "BIOS", a->bios[0] ? a->bios : NULL, a->config_path);
    WritePrivateProfileStringA("Video", "Backend", a->video_backend[0] ? a->video_backend : "d3d11", a->config_path);
    WritePrivateProfileStringA("Video", "IntegerScaling", a->integer_scaling ? "1" : "0", a->config_path);
    WritePrivateProfileStringA("Video", "KeepAspect", a->keep_aspect ? "1" : "0", a->config_path);
    WritePrivateProfileStringA("Video", "LCDPersistence", a->lcd_persistence ? "1" : "0", a->config_path);
    WritePrivateProfileStringA("Video", "FrameInterpolation", a->frame_interpolation ? "1" : "0", a->config_path);
    WritePrivateProfileStringA("Emulation", "JIT", a->jit ? "1" : "0", a->config_path);
    WritePrivateProfileStringA("Emulation", "UseHLE", (!a->bios[0] && a->use_hle) ? "1" : "0", a->config_path);
    const char *audio = a->audio_mode == GP32_WIN64_AUDIO_WAVEOUT ? "waveout" : (a->audio_mode == GP32_WIN64_AUDIO_WASAPI_EXCLUSIVE ? "wasapi_exclusive" : "wasapi_shared");
    WritePrivateProfileStringA("Audio", "Backend", audio, a->config_path);
}

static const char *base_name(const char *p) {
    const char *b = p;
    if (!p) return "";
    for (; *p; ++p) if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

static void app_update_title(app_state_t *a) {
    if (!a || !a->hwnd) return;
    char title[512];
    const char *game = a->fpk[0] ? base_name(a->fpk) : (a->fxe[0] ? base_name(a->fxe) : (a->smc[0] ? base_name(a->smc) : "no game"));
    snprintf(title, sizeof(title), "GP32emu - %s", game);
    SetWindowTextA(a->hwnd, title);
}

static void app_destroy_machine(app_state_t *a) {
    if (!a) return;
    if (a->emu && a->smc_save[0]) {
        if (gp32_save_card_progress(a->emu, a->smc_save) != GP32_OK) {
            char error[512];
            snprintf(error, sizeof(error), "SmartMedia save failed: %s\nLatest progress was not saved.", gp32_get_error(a->emu));
            app_show_status_error(a, error);
        } else if (a->smc_legacy[0] && remove(a->smc_legacy) != 0) {
            app_show_status_error(a, "Progress was saved as .gp32.sav. The unused older .gp32.smc file could not be removed.");
        }
    }
    if (a->emu) gp32_destroy(a->emu);
    a->emu = NULL;
    a->smc_save[0] = 0;
    a->smc_legacy[0] = 0;
    a->running = 0;
    a->frame_index = 0;
    a->accum_units = 1000000ull;
}

static void app_destroy_audio(app_state_t *a) {
    if (a && a->audio) { gp32_win64_audio_destroy(a->audio); a->audio = NULL; }
}

static void update_menu_checks(app_state_t *a);

static void app_stop_recording(app_state_t *a) {
    if (!a || !a->recorder) return;
    gp32_media_recorder_close(a->recorder);
    a->recorder = NULL;
    a->recording = 0;
    app_set_status(a, "Recording stopped");
    update_menu_checks(a);
}

static void app_toggle_fullscreen(app_state_t *a) {
    if (!a || !a->hwnd) return;
    if (!a->fullscreen) {
        a->windowed_style = GetWindowLongPtrA(a->hwnd, GWL_STYLE);
        a->windowed_exstyle = GetWindowLongPtrA(a->hwnd, GWL_EXSTYLE);
        memset(&a->windowed_placement, 0, sizeof(a->windowed_placement));
        a->windowed_placement.length = sizeof(a->windowed_placement);
        GetWindowPlacement(a->hwnd, &a->windowed_placement);

        MONITORINFO mi;
        memset(&mi, 0, sizeof(mi));
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoA(MonitorFromWindow(a->hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return;

        SetMenu(a->hwnd, NULL);
        SetWindowLongPtrA(a->hwnd, GWL_STYLE, (a->windowed_style & ~(LONG_PTR)WS_OVERLAPPEDWINDOW) | (LONG_PTR)WS_POPUP);
        SetWindowLongPtrA(a->hwnd, GWL_EXSTYLE, a->windowed_exstyle & ~(LONG_PTR)(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME));
        SetWindowPos(a->hwnd, HWND_TOP,
                     mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        a->fullscreen = 1;
        if (a->video) gp32_win64_video_set_fullscreen(a->video, 1);
    } else {
        SetWindowLongPtrA(a->hwnd, GWL_STYLE, a->windowed_style ? a->windowed_style : (LONG_PTR)WS_OVERLAPPEDWINDOW);
        SetWindowLongPtrA(a->hwnd, GWL_EXSTYLE, a->windowed_exstyle);
        SetMenu(a->hwnd, a->menu);
        if (a->windowed_placement.length == sizeof(a->windowed_placement)) {
            SetWindowPlacement(a->hwnd, &a->windowed_placement);
        }
        SetWindowPos(a->hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        DrawMenuBar(a->hwnd);
        a->fullscreen = 0;
        if (a->video) gp32_win64_video_set_fullscreen(a->video, 0);
    }
    if (a->video) {
        RECT rc;
        GetClientRect(a->hwnd, &rc);
        gp32_win64_video_resize(a->video, (unsigned)(rc.right - rc.left), (unsigned)(rc.bottom - rc.top));
    }
    update_menu_checks(a);
}

static void app_create_audio(app_state_t *a) {
    if (!a || a->no_audio) return;
    app_destroy_audio(a);
    a->audio = gp32_win64_audio_create(a->audio_mode, 44100u);
    if (!a->audio) app_set_status(a, "Audio unavailable");
}

static int app_create_machine(app_state_t *a) {
    if (!a) return 0;
    if (gp32_win64_is_card_save(a->smc)) {
        app_show_status_error(a, "This is a SmartMedia save image, not the original game.\nOpen the original .smc file; its saved progress is loaded automatically.");
        return 0;
    }
    if (!a->bios[0] && !a->smc[0] && !a->fxe[0] && !a->fpk[0]) {
        app_set_status(a, "No BIOS or GP32 program selected. Use Config > Set BIOS path or File > Open SmartMedia/FXE/FPK.");
        return 0;
    }
    app_destroy_machine(a);
    char save_path[MAX_PATH] = {0};
    char legacy_path[MAX_PATH] = {0};
    const char *media_path = a->smc;
    if (a->smc[0]) {
        int n = snprintf(save_path, sizeof(save_path), "%s.gp32.sav", a->smc);
        if (n < 0 || (size_t)n >= sizeof(save_path)) {
            app_show_status_error(a, "SmartMedia path is too long for its save file.");
            return 0;
        }
        DWORD attr = GetFileAttributesA(save_path);
        if (attr == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND) {
            snprintf(legacy_path, sizeof(legacy_path), "%s.gp32.smc", a->smc);
            DWORD legacy_attr = GetFileAttributesA(legacy_path);
            if (legacy_attr != INVALID_FILE_ATTRIBUTES) {
                media_path = legacy_path;
                attr = legacy_attr;
            } else if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) {
                app_show_status_error(a, "Cannot access the existing SmartMedia save.");
                return 0;
            }
        }
        if (attr != INVALID_FILE_ATTRIBUTES) {
            if (attr & FILE_ATTRIBUTE_DIRECTORY) {
                app_show_status_error(a, "SmartMedia save path is a directory.");
                return 0;
            }
            if (media_path != legacy_path) media_path = save_path;
        } else if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) {
            app_show_status_error(a, "Cannot access the existing SmartMedia save.");
            return 0;
        }
    }
    gp32_options_t opt;
    memset(&opt, 0, sizeof(opt));
    a->emu = gp32_create(&opt);
    if (!a->emu) { app_show_status_error(a, "gp32_create failed"); return 0; }
    gp32_set_jit(a->emu, a->jit);
    gp32_status_t st = GP32_OK;
    const int have_program = a->smc[0] || a->fxe[0] || a->fpk[0];
    const int use_real_bios = a->bios[0] != 0;
    const int use_hle_boot = have_program && !use_real_bios;
    if (use_hle_boot && !a->use_hle) {
        app_show_status_error(a, "No BIOS path is configured and HLE BIOS fallback is disabled.");
        return 0;
    }
    if (use_real_bios) {
        st = gp32_load_bios(a->emu, a->bios);
        if (st == GP32_OK && a->smc[0]) {
            st = gp32_load_smartmedia(a->emu, a->smc);
            if (st == GP32_OK && media_path != a->smc) st = gp32_load_card_progress(a->emu, media_path);
        }
        if (st == GP32_OK) st = gp32_reset(a->emu);
    }
    if (st == GP32_OK && a->smc[0] && use_hle_boot) st = gp32_load_smartmedia_direct(a->emu, a->smc);
    if (st == GP32_OK && a->fxe[0]) st = gp32_load_fxe(a->emu, a->fxe);
    if (st == GP32_OK && a->fpk[0]) st = gp32_load_fpk(a->emu, a->fpk);
    if (st != GP32_OK) {
        char buf[512];
        snprintf(buf, sizeof(buf), "Load failed: %s", gp32_get_error(a->emu));
        app_show_status_error(a, buf);
        app_destroy_machine(a);
        return 0;
    }
    /* Direct HLE extracts assets but does not mount a writable card device. */
    snprintf(a->smc_save, sizeof(a->smc_save), "%s", use_real_bios ? save_path : "");
    if (use_real_bios && media_path == legacy_path) snprintf(a->smc_legacy, sizeof(a->smc_legacy), "%s", legacy_path);
    app_create_audio(a);
    app_update_title(a);
    a->running = 1;
    update_menu_checks(a);
    char msg[512];
    if (!a->smc[0] && !a->fxe[0] && !a->fpk[0] && a->bios[0]) snprintf(msg, sizeof(msg), "Booting BIOS: %s", base_name(a->bios));
    else snprintf(msg, sizeof(msg), "Loaded. Boot mode %s. F5/F8 save/load state, F12 screenshot.", use_real_bios ? "BIOS" : "HLE");
    app_set_status(a, msg);
    return 1;
}

static int app_reset_machine(app_state_t *a) {
    if (!a || !a->emu) return 0;
    gp32_status_t st = gp32_reset(a->emu);
    if (st != GP32_OK) { app_set_status(a, gp32_get_error(a->emu)); return 0; }
    gp32_clear_audio(a->emu);
    app_create_audio(a);
    a->accum_units = 1000000ull;
    return 1;
}

static int app_open_game(app_state_t *a, const char *path, int kind) {
    if (gp32_win64_is_card_save(path)) {
        app_show_status_error(a, "This file contains saved progress.\nSelect the original .smc game instead; its save is loaded automatically.");
        return 0;
    }
    a->smc[0] = a->fxe[0] = a->fpk[0] = 0;
    char *target = kind == 1 ? a->fxe : kind == 2 ? a->fpk : a->smc;
    snprintf(target, MAX_PATH, "%s", path);
    return app_create_machine(a);
}

static void app_recreate_video(app_state_t *a) {
    if (!a || !a->hwnd) return;
    if (a->video) gp32_win64_video_destroy(a->video);
    a->video = gp32_win64_video_create(a->hwnd, 2, a->integer_scaling, a->keep_aspect, a->lcd_persistence, a->frame_interpolation, a->video_backend);
    if (a->video) gp32_win64_video_set_fullscreen(a->video, a->fullscreen);
    RECT rc; GetClientRect(a->hwnd, &rc);
    gp32_win64_video_resize(a->video, (unsigned)(rc.right - rc.left), (unsigned)(rc.bottom - rc.top));
}

static int select_open_file(HWND owner, const char *title, const char *filter, char *out, DWORD out_size) {
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    out[0] = 0;
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrTitle = title;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = out;
    ofn.nMaxFile = out_size;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    return GetOpenFileNameA(&ofn) != 0;
}

static int select_save_file(HWND owner, const char *title, const char *filter, const char *def_ext, char *out, DWORD out_size) {
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrTitle = title;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = out;
    ofn.nMaxFile = out_size;
    ofn.lpstrDefExt = def_ext;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_EXPLORER;
    return GetSaveFileNameA(&ofn) != 0;
}

static void app_save_state_dialog(app_state_t *a) {
    if (!a || !a->emu) return;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s", a->state_path[0] ? a->state_path : "gp32_state.gp32st");
    if (!select_save_file(a->hwnd, "Save GP32 state", "GP32 state (*.gp32st)\0*.gp32st\0All files\0*.*\0", "gp32st", path, sizeof(path))) return;
    gp32_status_t st = gp32_save_state(a->emu, path);
    if (st == GP32_OK) { snprintf(a->state_path, sizeof(a->state_path), "%s", path); app_set_status(a, "State saved"); }
    else app_set_status(a, gp32_get_error(a->emu));
}

static void app_load_state_dialog(app_state_t *a) {
    if (!a || !a->emu) return;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s", a->state_path[0] ? a->state_path : "gp32_state.gp32st");
    if (!select_open_file(a->hwnd, "Load GP32 state", "GP32 state (*.gp32st)\0*.gp32st\0All files\0*.*\0", path, sizeof(path))) return;
    gp32_status_t st = gp32_load_state(a->emu, path);
    if (st == GP32_OK) {
        snprintf(a->state_path, sizeof(a->state_path), "%s", path);
        gp32_clear_audio(a->emu);
        app_create_audio(a);
        a->accum_units = 1000000ull;
        app_set_status(a, "State loaded");
    } else app_set_status(a, gp32_get_error(a->emu));
}

static void app_screenshot_dialog(app_state_t *a) {
    if (!a || !a->emu) return;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s", a->screenshot_path[0] ? a->screenshot_path : "gp32_screenshot.bmp");
    if (!select_save_file(a->hwnd, "Save GP32 screenshot", "Windows bitmap (*.bmp)\0*.bmp\0All files\0*.*\0", "bmp", path, sizeof(path))) return;
    gp32_framebuffer_desc_t fb;
    char err[256] = {0};
    if (gp32_get_framebuffer(a->emu, &fb) == GP32_OK && gp32_media_write_bmp_320x240(path, &fb, err, sizeof(err))) {
        snprintf(a->screenshot_path, sizeof(a->screenshot_path), "%s", path);
        app_set_status(a, "Screenshot saved");
    } else {
        app_show_status_error(a, err[0] ? err : "Screenshot failed");
    }
}

static void app_start_recording(app_state_t *a) {
    if (!a || a->recorder) return;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s", a->record_path[0] ? a->record_path : "gp32_recording.mkv");
    if (!select_save_file(a->hwnd, "Record ZMBV/PCM MKV", "Matroska video (*.mkv)\0*.mkv\0All files\0*.*\0", "mkv", path, sizeof(path))) return;
    char err[256] = {0};
    a->recorder = gp32_media_recorder_open(path, 44100u, err, sizeof(err));
    if (!a->recorder) { app_show_status_error(a, err[0] ? err : "Recording open failed"); return; }
    snprintf(a->record_path, sizeof(a->record_path), "%s", path);
    a->recording = 1;
    app_set_status(a, "Recording started: ZMBV video + PCM audio in MKV");
    update_menu_checks(a);
}

static void update_menu_checks(app_state_t *a) {
    if (!a || !a->menu) return;
    CheckMenuItem(a->menu, IDM_EMU_RUN, MF_BYCOMMAND | (a->running ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(a->menu, IDM_EMU_JIT, MF_BYCOMMAND | (a->jit ? MF_CHECKED : MF_UNCHECKED));
    EnableMenuItem(a->menu, IDM_FILE_RECORD_MKV, MF_BYCOMMAND | (a->recording ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(a->menu, IDM_FILE_STOP_RECORDING, MF_BYCOMMAND | (a->recording ? MF_ENABLED : MF_GRAYED));
    CheckMenuItem(a->menu, IDM_CONFIG_USE_HLE, MF_BYCOMMAND | ((!a->bios[0] && a->use_hle) ? MF_CHECKED : MF_UNCHECKED));
    EnableMenuItem(a->menu, IDM_CONFIG_USE_HLE, MF_BYCOMMAND | (a->bios[0] ? MF_GRAYED : MF_ENABLED));
    CheckMenuItem(a->menu, IDM_VIDEO_FULLSCREEN, MF_BYCOMMAND | (a->fullscreen ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(a->menu, IDM_VIDEO_KEEP_ASPECT, MF_BYCOMMAND | (a->keep_aspect ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(a->menu, IDM_VIDEO_INTEGER, MF_BYCOMMAND | (a->integer_scaling ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(a->menu, IDM_VIDEO_LCD_PERSISTENCE, MF_BYCOMMAND | (a->lcd_persistence ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(a->menu, IDM_VIDEO_FRAME_INTERP, MF_BYCOMMAND | (a->frame_interpolation ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuRadioItem(a->menu, IDM_VIDEO_D3D11, IDM_VIDEO_GDI, !strcmp(a->video_backend, "gdi") ? IDM_VIDEO_GDI : IDM_VIDEO_D3D11, MF_BYCOMMAND);
    CheckMenuRadioItem(a->menu, IDM_AUDIO_WAVEOUT, IDM_AUDIO_WASAPI_EXCL, a->audio_mode == GP32_WIN64_AUDIO_WAVEOUT ? IDM_AUDIO_WAVEOUT : (a->audio_mode == GP32_WIN64_AUDIO_WASAPI_EXCLUSIVE ? IDM_AUDIO_WASAPI_EXCL : IDM_AUDIO_WASAPI_SHARED), MF_BYCOMMAND);
}

static HMENU create_menu(void) {
    HMENU menu = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuA(file, MF_STRING, IDM_FILE_LIBRARY, "Game library...");
    AppendMenuA(file, MF_SEPARATOR, 0, NULL);
    AppendMenuA(file, MF_STRING, IDM_FILE_OPEN_BIOS, "Open BIOS...");
    AppendMenuA(file, MF_STRING, IDM_FILE_OPEN_SMC, "Open SmartMedia image...");
    AppendMenuA(file, MF_STRING, IDM_FILE_OPEN_FXE, "Open FXE...");
    AppendMenuA(file, MF_STRING, IDM_FILE_OPEN_FPK, "Open FPK...");
    AppendMenuA(file, MF_SEPARATOR, 0, NULL);
    AppendMenuA(file, MF_STRING, IDM_FILE_SAVE_STATE, "Save State...");
    AppendMenuA(file, MF_STRING, IDM_FILE_LOAD_STATE, "Load State...");
    AppendMenuA(file, MF_SEPARATOR, 0, NULL);
    AppendMenuA(file, MF_STRING, IDM_FILE_SCREENSHOT, "Take Screenshot...\tF12");
    AppendMenuA(file, MF_STRING, IDM_FILE_RECORD_MKV, "Start Recording ZMBV MKV...");
    AppendMenuA(file, MF_STRING | MF_GRAYED, IDM_FILE_STOP_RECORDING, "Stop Recording");
    AppendMenuA(file, MF_SEPARATOR, 0, NULL);
    AppendMenuA(file, MF_STRING, IDM_FILE_EXIT, "Exit");
    AppendMenuA(menu, MF_POPUP, (UINT_PTR)file, "File");

    HMENU emu = CreatePopupMenu();
    AppendMenuA(emu, MF_STRING, IDM_EMU_RUN, "Run/Pause");
    AppendMenuA(emu, MF_STRING, IDM_EMU_RESET, "Reset");
    AppendMenuA(emu, MF_STRING, IDM_EMU_JIT, "Enable x64 dynarec JIT");
    AppendMenuA(menu, MF_POPUP, (UINT_PTR)emu, "Emulation");

    HMENU video = CreatePopupMenu();
    AppendMenuA(video, MF_STRING, IDM_VIDEO_D3D11, "D3D11");
    AppendMenuA(video, MF_STRING, IDM_VIDEO_GDI, "GDI fallback");
    AppendMenuA(video, MF_SEPARATOR, 0, NULL);
    AppendMenuA(video, MF_STRING, IDM_VIDEO_FULLSCREEN, "Fullscreen\tF11 / Alt+Enter");
    AppendMenuA(video, MF_STRING, IDM_VIDEO_KEEP_ASPECT, "Keep aspect ratio, fill height");
    AppendMenuA(video, MF_STRING, IDM_VIDEO_INTEGER, "Integer scaling");
    AppendMenuA(video, MF_SEPARATOR, 0, NULL);
    AppendMenuA(video, MF_STRING, IDM_VIDEO_LCD_PERSISTENCE, "LCD persistence / FLU ghosting");
    AppendMenuA(video, MF_STRING, IDM_VIDEO_FRAME_INTERP, "Frame interpolation blend");
    AppendMenuA(menu, MF_POPUP, (UINT_PTR)video, "Video");

    HMENU audio = CreatePopupMenu();
    AppendMenuA(audio, MF_STRING, IDM_AUDIO_WAVEOUT, "waveOut");
    AppendMenuA(audio, MF_STRING, IDM_AUDIO_WASAPI_SHARED, "WASAPI shared");
    AppendMenuA(audio, MF_STRING, IDM_AUDIO_WASAPI_EXCL, "WASAPI exclusive");
    AppendMenuA(menu, MF_POPUP, (UINT_PTR)audio, "Audio");

    HMENU config = CreatePopupMenu();
    AppendMenuA(config, MF_STRING, IDM_CONFIG_KEYBOARD, "Keyboard controls...");
    AppendMenuA(config, MF_SEPARATOR, 0, NULL);
    AppendMenuA(config, MF_STRING, IDM_CONFIG_SET_BIOS, "Set BIOS path...");
    AppendMenuA(config, MF_STRING, IDM_CONFIG_CLEAR_BIOS, "Clear BIOS path");
    AppendMenuA(config, MF_SEPARATOR, 0, NULL);
    AppendMenuA(config, MF_STRING, IDM_CONFIG_BOOT_BIOS, "Boot BIOS now");
    AppendMenuA(config, MF_STRING, IDM_CONFIG_USE_HLE, "Use HLE BIOS fallback when no BIOS is configured");
    AppendMenuA(menu, MF_POPUP, (UINT_PTR)config, "Config");
    return menu;
}

static void app_command(app_state_t *a, UINT id) {
    if (!a) return;
    char path[MAX_PATH];
    switch (id) {
    case IDM_CONFIG_KEYBOARD:
        a->keyboard_buttons = 0;
        if (gp32_win64_keyboard_dialog(a->hwnd, a->inst, &a->preferences)) app_save_config(a);
        QueryPerformanceCounter(&a->last_qpc);
        break;
    case IDM_FILE_LIBRARY: {
        a->keyboard_buttons = 0;
        int selected = gp32_win64_library_dialog(a->hwnd, a->inst, &a->preferences, path);
        app_save_config(a);
        QueryPerformanceCounter(&a->last_qpc);
        if (selected) {
            const char *ext = strrchr(path, '.');
            app_open_game(a, path, !_stricmp(ext, ".fxe") ? 1 : !_stricmp(ext, ".fpk") ? 2 : 0);
        }
        break;
    }
    case IDM_FILE_OPEN_BIOS:
        if (select_open_file(a->hwnd, "Open GP32 BIOS", "GP32 BIOS (*.bin;*.rom;*.bios;*.zip)\0*.bin;*.rom;*.bios;*.zip\0All files\0*.*\0", path, sizeof(path))) {
            snprintf(a->bios, sizeof(a->bios), "%s", path);
            a->use_hle = 0;
            app_save_config(a);
            update_menu_checks(a);
            app_create_machine(a);
        }
        break;
    case IDM_FILE_OPEN_SMC:
        if (select_open_file(a->hwnd, "Open GP32 SmartMedia image", "SmartMedia (*.smc)\0*.smc\0All files\0*.*\0", path, sizeof(path))) app_open_game(a, path, 0);
        break;
    case IDM_FILE_OPEN_FXE:
        if (select_open_file(a->hwnd, "Open GP32 FXE", "GP32 executable (*.fxe)\0*.fxe\0All files\0*.*\0", path, sizeof(path))) app_open_game(a, path, 1);
        break;
    case IDM_FILE_OPEN_FPK:
        if (select_open_file(a->hwnd, "Open GP32 FPK", "GP32 package (*.fpk)\0*.fpk\0All files\0*.*\0", path, sizeof(path))) app_open_game(a, path, 2);
        break;
    case IDM_FILE_SAVE_STATE: app_save_state_dialog(a); break;
    case IDM_FILE_LOAD_STATE: app_load_state_dialog(a); break;
    case IDM_FILE_SCREENSHOT: app_screenshot_dialog(a); break;
    case IDM_FILE_RECORD_MKV: app_start_recording(a); break;
    case IDM_FILE_STOP_RECORDING: app_stop_recording(a); update_menu_checks(a); break;
    case IDM_FILE_EXIT: PostMessageA(a->hwnd, WM_CLOSE, 0, 0); break;
    case IDM_EMU_RUN:
        if (!a->emu) app_create_machine(a);
        else a->running = !a->running;
        update_menu_checks(a);
        break;
    case IDM_EMU_RESET:
        if (!a->emu) app_create_machine(a);
        else app_reset_machine(a);
        break;
    case IDM_EMU_JIT: {
        int new_jit = !a->jit;
        if (a->emu) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Changing CPU core to %s requires resetting the emulation core. Continue?", new_jit ? "x64 JIT" : "interpreter");
            if (MessageBoxA(a->hwnd, msg, "GP32emu", MB_YESNO | MB_ICONWARNING) != IDYES) { update_menu_checks(a); break; }
        }
        a->jit = new_jit;
        app_save_config(a);
        if (a->emu) app_create_machine(a);
        update_menu_checks(a);
        break;
    }
    case IDM_VIDEO_D3D11: strcpy(a->video_backend, "d3d11"); app_save_config(a); app_recreate_video(a); update_menu_checks(a); break;
    case IDM_VIDEO_GDI: strcpy(a->video_backend, "gdi"); app_save_config(a); app_recreate_video(a); update_menu_checks(a); break;
    case IDM_VIDEO_FULLSCREEN: app_toggle_fullscreen(a); break;
    case IDM_VIDEO_KEEP_ASPECT:
        a->keep_aspect = !a->keep_aspect;
        if (a->keep_aspect) a->integer_scaling = 0;
        app_save_config(a);
        if (a->video) { gp32_win64_video_set_keep_aspect(a->video, a->keep_aspect); gp32_win64_video_set_integer_scaling(a->video, a->integer_scaling); }
        update_menu_checks(a);
        break;
    case IDM_VIDEO_INTEGER:
        a->integer_scaling = !a->integer_scaling;
        if (a->integer_scaling) a->keep_aspect = 0;
        app_save_config(a);
        if (a->video) { gp32_win64_video_set_integer_scaling(a->video, a->integer_scaling); gp32_win64_video_set_keep_aspect(a->video, a->keep_aspect); }
        update_menu_checks(a);
        break;
    case IDM_VIDEO_LCD_PERSISTENCE:
        a->lcd_persistence = !a->lcd_persistence;
        app_save_config(a);
        if (a->video) gp32_win64_video_set_lcd_persistence(a->video, a->lcd_persistence);
        update_menu_checks(a);
        break;
    case IDM_VIDEO_FRAME_INTERP:
        a->frame_interpolation = !a->frame_interpolation;
        app_save_config(a);
        if (a->video) gp32_win64_video_set_frame_interpolation(a->video, a->frame_interpolation);
        update_menu_checks(a);
        break;
    case IDM_AUDIO_WAVEOUT: a->audio_mode = GP32_WIN64_AUDIO_WAVEOUT; app_save_config(a); app_create_audio(a); update_menu_checks(a); break;
    case IDM_AUDIO_WASAPI_SHARED: a->audio_mode = GP32_WIN64_AUDIO_WASAPI_SHARED; app_save_config(a); app_create_audio(a); update_menu_checks(a); break;
    case IDM_AUDIO_WASAPI_EXCL: a->audio_mode = GP32_WIN64_AUDIO_WASAPI_EXCLUSIVE; app_save_config(a); app_create_audio(a); update_menu_checks(a); break;
    case IDM_CONFIG_SET_BIOS:
        if (select_open_file(a->hwnd, "Set GP32 BIOS path", "GP32 BIOS (*.bin;*.rom;*.bios;*.zip)\0*.bin;*.rom;*.bios;*.zip\0All files\0*.*\0", path, sizeof(path))) {
            snprintf(a->bios, sizeof(a->bios), "%s", path);
            a->use_hle = 0;
            app_save_config(a);
            update_menu_checks(a);
            app_set_status(a, "BIOS path saved; HLE BIOS fallback disabled. Use Config > Boot BIOS now, or open a SmartMedia/FXE/FPK image.");
        }
        break;
    case IDM_CONFIG_CLEAR_BIOS:
        a->bios[0] = 0;
        a->use_hle = 1;
        app_save_config(a);
        update_menu_checks(a);
        app_set_status(a, "BIOS path cleared. HLE BIOS fallback will be used for game launches.");
        break;
    case IDM_CONFIG_BOOT_BIOS:
        a->smc[0] = a->fxe[0] = a->fpk[0] = 0;
        app_create_machine(a);
        break;
    case IDM_CONFIG_USE_HLE:
        if (a->bios[0]) {
            a->use_hle = 0;
            app_set_status(a, "HLE BIOS fallback is disabled while a BIOS path is configured.");
        } else {
            a->use_hle = !a->use_hle;
            app_set_status(a, a->use_hle ? "HLE BIOS fallback enabled for game launches" : "HLE BIOS fallback disabled; configure a BIOS before launching games");
        }
        app_save_config(a);
        update_menu_checks(a);
        break;
    default: break;
    }
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    app_state_t *a = (app_state_t *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTA *cs = (CREATESTRUCTA *)lparam;
        a = (app_state_t *)cs->lpCreateParams;
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)a);
        a->hwnd = hwnd;
        if (a->bios[0]) {
            char st[512];
            snprintf(st, sizeof(st), "Ready. BIOS path configured: %s", base_name(a->bios));
            app_set_status(a, st);
        }
        app_recreate_video(a);
        return 0;
    }
    case WM_SIZE:
        if (a) {
            unsigned w = LOWORD(lparam), h = HIWORD(lparam);
            if (a->video) gp32_win64_video_resize(a->video, w ? w : 1u, h ? h : 1u);
        }
        return 0;
    case WM_KEYDOWN:
        if (a) {
            if (wparam == VK_ESCAPE) { if (a->fullscreen) app_toggle_fullscreen(a); return 0; }
            if (wparam == VK_F11) { if (!(lparam & (1L << 30))) app_toggle_fullscreen(a); return 0; }
            if (wparam == VK_F5) { app_save_state_dialog(a); return 0; }
            if (wparam == VK_F8) { app_load_state_dialog(a); return 0; }
            if (wparam == VK_F12) { app_screenshot_dialog(a); return 0; }
            a->keyboard_buttons |= gp32_win64_key_button(&a->preferences, wparam);
        }
        return 0;
    case WM_SYSKEYDOWN:
        if (a && wparam == VK_RETURN && (HIWORD(lparam) & KF_ALTDOWN)) { app_toggle_fullscreen(a); return 0; }
        return DefWindowProcA(hwnd, msg, wparam, lparam);
    case WM_KEYUP:
        if (a) a->keyboard_buttons &= ~gp32_win64_key_button(&a->preferences, wparam);
        return 0;
    case WM_ENTERMENULOOP:
    case WM_KILLFOCUS:
        /* Modal menus and other windows can consume the matching key release. */
        if (a) a->keyboard_buttons = 0;
        return 0;
    case WM_COMMAND:
        app_command(a, LOWORD(wparam));
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (a) a->quit = 1;
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcA(hwnd, msg, wparam, lparam);
    }
}

static uint64_t qpc_elapsed_us(app_state_t *a, LARGE_INTEGER now) {
    int64_t diff = now.QuadPart - a->last_qpc.QuadPart;
    if (diff < 0) diff = 0;
    return (uint64_t)((diff * 1000000ll) / a->qpf.QuadPart);
}

typedef struct frame_audio_context {
    app_state_t *app;
    int recording_failed;
} frame_audio_context_t;

static void pump_frame_audio(gp32_t *g, void *user) {
    frame_audio_context_t *ctx = user;
    app_state_t *a = ctx->app;
    if (!a->audio && !a->recorder) return;
    gp32_audio_desc_t aud;
    while (gp32_get_audio(g, &aud) == GP32_OK && aud.frame_count > 0) {
        if (a->audio) gp32_win64_audio_submit(a->audio, &aud);
        if (a->recorder && !ctx->recording_failed &&
            !gp32_media_recorder_add_audio(a->recorder, &aud))
            ctx->recording_failed = 1;
        if (gp32_consume_audio(g, aud.frame_count) != GP32_OK) break;
    }
}

static void run_one_frame(app_state_t *a) {
    if (!a || !a->emu) return;
    frame_audio_context_t audio_ctx = { .app = a };
    gp32_set_buttons(a->emu, a->buttons);
    gp32_set_host_pump(a->emu, (a->audio || a->recorder) ? pump_frame_audio : NULL, &audio_ctx);
    gp32_status_t st = gp32_run_frame(a->emu);
    gp32_set_host_pump(a->emu, NULL, NULL);
    if (st == GP32_OK) pump_frame_audio(a->emu, &audio_ctx);
    /* Recorder cleanup updates menus; keep it outside the core's pump hook. */
    if (audio_ctx.recording_failed) {
        app_set_status(a, gp32_media_recorder_error(a->recorder));
        app_stop_recording(a);
    }
    if (st != GP32_OK) { app_set_status(a, gp32_get_error(a->emu)); a->running = 0; return; }
    a->frame_index++;
    a->emu_frames++;
}

static void app_pump(app_state_t *a) {
    int quit = 0;
    uint32_t actions = 0;
    if (a->sdl_input) gp32_win64_sdl_input_poll(a->sdl_input, a->keyboard_buttons, &a->buttons, &actions, &quit);
    else a->buttons = a->keyboard_buttons;
    if (quit) PostMessageA(a->hwnd, WM_CLOSE, 0, 0);
    if ((actions & GP32_FRONTEND_ACTION_SAVE_STATE) && a->emu) gp32_save_state(a->emu, a->state_path[0] ? a->state_path : "gp32_state.gp32st");
    if ((actions & GP32_FRONTEND_ACTION_LOAD_STATE) && a->emu) {
        if (gp32_load_state(a->emu, a->state_path[0] ? a->state_path : "gp32_state.gp32st") == GP32_OK) {
            gp32_clear_audio(a->emu);
            app_create_audio(a);
            a->accum_units = 1000000ull;
        }
    }

    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    uint64_t elapsed_us = qpc_elapsed_us(a, now);
    a->last_qpc = now;
    if (elapsed_us > 250000u) elapsed_us = 250000u;
    int ran = 0;
    if (a->running && a->emu) {
        a->accum_units += elapsed_us * 60ull;
        unsigned steps = 0;
        while (a->accum_units >= 1000000ull && steps < 5u) {
            run_one_frame(a);
            a->accum_units -= 1000000ull;
            steps++;
            ran = 1;
        }
        if (a->accum_units >= 1000000ull && steps >= 5u) a->accum_units = 1000000ull;
    }
    if (ran && a->video && a->emu) {
        gp32_framebuffer_desc_t fb;
        if (gp32_get_framebuffer(a->emu, &fb) == GP32_OK) { gp32_win64_video_present(a->video, &fb); if (a->recorder && !gp32_media_recorder_add_frame(a->recorder, &fb, a->frame_index ? a->frame_index - 1u : 0u)) { app_set_status(a, gp32_media_recorder_error(a->recorder)); app_stop_recording(a); } a->render_frames++; }
    }
    if (a->audio) gp32_win64_audio_pump(a->audio);

    DWORD now_ms = GetTickCount();
    if (!a->fps_tick_ms) a->fps_tick_ms = now_ms;
    if (now_ms - a->fps_tick_ms >= 1000u) {
        char st[512];
        const char *audio = a->audio ? gp32_win64_audio_backend(a->audio) : "off";
        const char *video = a->video ? gp32_win64_video_active_backend(a->video) : "none";
        snprintf(st, sizeof(st), "%u render / %u emu FPS | video %s | audio %s | input %s", a->render_frames, a->emu_frames, video, audio, gp32_win64_sdl_input_status(a->sdl_input));
        app_set_status(a, st);
        a->render_frames = a->emu_frames = 0;
        a->fps_tick_ms = now_ms;
    }
    Sleep(ran ? 0 : 1);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show) {
    (void)prev;
    app_state_t app;
    memset(&app, 0, sizeof(app));
    app.inst = inst;
    app.running = 0;
    app.jit = 1;
    app.integer_scaling = 0;
    app.keep_aspect = 1;
    app.audio_mode = GP32_WIN64_AUDIO_WAVEOUT;
    strcpy(app.video_backend, "d3d11");
    strcpy(app.state_path, "gp32_state.gp32st");
    strcpy(app.screenshot_path, "gp32_screenshot.bmp");
    strcpy(app.record_path, "gp32_recording.mkv");
    app_load_config(&app);
    g_app = &app;

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.lpszClassName = "GP32emuWin64";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(IDI_GP32EMU));
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    if (!RegisterClassA(&wc)) return 1;
    app.menu = create_menu();
    update_menu_checks(&app);
    RECT wr = {0, 0, (LONG)(GP32_LCD_W * 2u), (LONG)(GP32_LCD_H * 2u)};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, TRUE);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "GP32emu", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top, NULL, app.menu, inst, &app);
    if (!hwnd) return 1;
    app.hwnd = hwnd;
    app.sdl_input = gp32_win64_sdl_input_create(0);
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    QueryPerformanceFrequency(&app.qpf);
    QueryPerformanceCounter(&app.last_qpc);
    app.accum_units = 1000000ull;

    if (cmdline && cmdline[0]) {
        int argc = 0;
        LPWSTR *argvw = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; argvw && i < argc; ++i) {
            char arg[MAX_PATH];
            WideCharToMultiByte(CP_UTF8, 0, argvw[i], -1, arg, sizeof(arg), NULL, NULL);
            if (!strcmp(arg, "--bios") && i + 1 < argc) {
                WideCharToMultiByte(CP_UTF8, 0, argvw[++i], -1, app.bios, sizeof(app.bios), NULL, NULL);
                app.use_hle = 0;
                app_save_config(&app);
            } else if (!strcmp(arg, "--smc") && i + 1 < argc) {
                WideCharToMultiByte(CP_UTF8, 0, argvw[++i], -1, app.smc, sizeof(app.smc), NULL, NULL);
                app.fxe[0] = app.fpk[0] = 0;
            } else if (!strcmp(arg, "--fxe") && i + 1 < argc) {
                WideCharToMultiByte(CP_UTF8, 0, argvw[++i], -1, app.fxe, sizeof(app.fxe), NULL, NULL);
                app.smc[0] = app.fpk[0] = 0;
            } else if (!strcmp(arg, "--fpk") && i + 1 < argc) {
                WideCharToMultiByte(CP_UTF8, 0, argvw[++i], -1, app.fpk, sizeof(app.fpk), NULL, NULL);
                app.smc[0] = app.fxe[0] = 0;
            } else if (!strcmp(arg, "--no-audio")) {
                app.no_audio = 1;
            } else if (!strcmp(arg, "--no-jit")) {
                app.jit = 0;
            } else if (!strcmp(arg, "--jit")) {
                app.jit = 1;
            } else if (arg[0] != '-') {
                snprintf(app.smc, sizeof(app.smc), "%s", arg);
                app.fxe[0] = app.fpk[0] = 0;
            }
        }
        if (argvw) LocalFree(argvw);
    }
    if (!app.bios[0]) {
        app.use_hle = 1;
        update_menu_checks(&app);
        MessageBoxA(hwnd, "No BIOS path is configured. GP32emu will use the HLE BIOS fallback. HLE BIOS will fail to boot some games and only works with some commercial games and homebrew games.", "GP32emu HLE BIOS warning", MB_OK | MB_ICONWARNING);
    } else {
        app.use_hle = 0;
        update_menu_checks(&app);
    }
    if (app.bios[0] || app.smc[0] || app.fxe[0] || app.fpk[0]) app_create_machine(&app);

    MSG msg;
    while (!app.quit) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        if (app.quit) break;
        app_pump(&app);
    }
    app_stop_recording(&app);
    app_destroy_audio(&app);
    if (app.sdl_input) gp32_win64_sdl_input_destroy(app.sdl_input);
    if (app.video) gp32_win64_video_destroy(app.video);
    app_destroy_machine(&app);
    return 0;
}
