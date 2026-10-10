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

static_assert(sizeof(void *) == 8, "The Windows frontend supports 64-bit targets only.");

#define GP32_LCD_W 320u
#define GP32_LCD_H 240u
#define GP32_DEFAULT_CLOCK_HZ 66000000u
#define IDI_GP32EMU 101
#define GP32_STATUS_MS 3000u
#define GP32_CURSOR_IDLE_FRAMES 90u
#define GP32_BASE_TITLE "GP32emu"
#define GP32_APP_VERSION "1.0.0"
#define GP32_RECENT_MAX 8
#define GP32_STATE_SLOTS 9
#define GP32_FAST_FORWARD 4u

#define IDM_FILE_OPEN_GAME       1002
#define IDM_FILE_SAVE_STATE      1005
#define IDM_FILE_LOAD_STATE      1006
#define IDM_FILE_SCREENSHOT      1007
#define IDM_FILE_RECORD_MKV      1008
#define IDM_FILE_STOP_RECORDING  1009
#define IDM_FILE_EXIT            1010
#define IDM_FILE_LIBRARY         1011
#define IDM_FILE_QUICK_SAVE      1012
#define IDM_FILE_QUICK_LOAD      1013
#define IDM_FILE_RECENT_CLEAR    1014
#define IDM_FILE_RECENT_FIRST    1020 /* GP32_RECENT_MAX entries */
#define IDM_STATE_SLOT_FIRST     1040 /* GP32_STATE_SLOTS entries */
#define IDM_EMU_RUN              1101
#define IDM_EMU_RESET            1102
#define IDM_EMU_JIT              1103
#define IDM_EMU_FAST_FORWARD     1104
#define IDM_VIDEO_D3D11          1201
#define IDM_VIDEO_GDI            1202
#define IDM_VIDEO_FULLSCREEN     1204
#define IDM_VIDEO_LCD_PERSISTENCE 1206
#define IDM_VIDEO_FRAME_INTERP    1207
#define IDM_VIDEO_SIZE_FIRST     1210 /* 1x..4x */
#define IDM_VIDEO_KEEP_ASPECT    1220 /* KEEP_ASPECT..STRETCH form one radio group */
#define IDM_VIDEO_INTEGER        1221
#define IDM_VIDEO_STRETCH        1222
#define IDM_AUDIO_WAVEOUT        1301
#define IDM_AUDIO_WASAPI_SHARED  1302
#define IDM_AUDIO_WASAPI_EXCL    1303
#define IDM_CONFIG_SET_BIOS     1401
#define IDM_CONFIG_CLEAR_BIOS   1402
#define IDM_CONFIG_BOOT_BIOS    1403
#define IDM_CONFIG_USE_HLE      1404
#define IDM_CONFIG_KEYBOARD     1405
#define IDM_HELP_CONTROLS       1501
#define IDM_HELP_ABOUT          1502

typedef struct app_state {
    HINSTANCE inst;
    HWND hwnd;
    HMENU menu;
    HMENU recent_menu;
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
    char recent[GP32_RECENT_MAX][MAX_PATH];
    char fps_text[32];
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
    int state_slot;   /* 1..GP32_STATE_SLOTS */
    int fast_key;     /* Tab held */
    int fast_toggle;  /* Emulation > Fast Forward */
    int fast_active;  /* frames run at GP32_FAST_FORWARD speed, sound muted */
    int status_active;
    char status_msg[256];
    DWORD status_tick_ms;
    unsigned cursor_idle_frames;
    int display_awake;
    LARGE_INTEGER qpf;
    LARGE_INTEGER last_qpc;
    uint64_t accum_units;
} app_state_t;

static app_state_t *g_app;

static void app_update_title(app_state_t *a);
static void update_menu_checks(app_state_t *a);

static const char *base_name(const char *p) {
    const char *b = p;
    if (!p) return "";
    for (; *p; ++p) if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

/* The program the machine runs, or NULL for a BIOS-only boot. */
static const char *app_game_file(const app_state_t *a) {
    return a->fpk[0] ? a->fpk : a->fxe[0] ? a->fxe : a->smc[0] ? a->smc : NULL;
}

/* Status messages surface as a short-lived suffix on the window title, so the
   frontend needs no extra UI surface. */
static void app_set_status(app_state_t *a, const char *msg) {
    if (!a) return;
    if (!msg || !msg[0]) {
        a->status_msg[0] = 0;
        a->status_active = 0;
        app_update_title(a);
        return;
    }
    snprintf(a->status_msg, sizeof(a->status_msg), "%s", msg);
    a->status_tick_ms = GetTickCount();
    a->status_active = 1;
    app_update_title(a);
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
    a->state_slot = GetPrivateProfileIntA("Emulation", "StateSlot", 1, a->config_path);
    if (a->state_slot < 1 || a->state_slot > GP32_STATE_SLOTS) a->state_slot = 1;
    for (int i = 0; i < GP32_RECENT_MAX; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "Game%d", i + 1);
        GetPrivateProfileStringA("Recent", key, "", a->recent[i], MAX_PATH, a->config_path);
    }
    char audio[32];
    GetPrivateProfileStringA("Audio", "Backend", "waveout", audio, (DWORD)sizeof(audio), a->config_path);
    if (!strcmp(audio, "waveout")) a->audio_mode = GP32_WIN64_AUDIO_WAVEOUT;
    else if (!strcmp(audio, "wasapi_shared")) a->audio_mode = GP32_WIN64_AUDIO_WASAPI_SHARED;
    else if (!strcmp(audio, "wasapi_exclusive")) a->audio_mode = GP32_WIN64_AUDIO_WASAPI_EXCLUSIVE;
    else a->audio_mode = GP32_WIN64_AUDIO_WAVEOUT;
    a->preferences.audio_mode = a->audio_mode == GP32_WIN64_AUDIO_WAVEOUT ? GP32_WIN64_AUDIO_WAVEOUT_ID
        : a->audio_mode == GP32_WIN64_AUDIO_WASAPI_EXCLUSIVE ? GP32_WIN64_AUDIO_WASAPI_EXCLUSIVE_ID
        : GP32_WIN64_AUDIO_WASAPI_SHARED_ID;
    snprintf(a->preferences.video_backend, sizeof(a->preferences.video_backend), "%s", a->video_backend);
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
    char slot[8];
    snprintf(slot, sizeof(slot), "%d", a->state_slot >= 1 && a->state_slot <= GP32_STATE_SLOTS ? a->state_slot : 1);
    WritePrivateProfileStringA("Emulation", "StateSlot", slot, a->config_path);
    const char *audio = a->audio_mode == GP32_WIN64_AUDIO_WAVEOUT ? "waveout" : (a->audio_mode == GP32_WIN64_AUDIO_WASAPI_EXCLUSIVE ? "wasapi_exclusive" : "wasapi_shared");
    WritePrivateProfileStringA("Audio", "Backend", audio, a->config_path);
}

/* Title: GP32emu - game - one of status message, Paused, Fast forward or
   the emulated frame rate. */
static void app_update_title(app_state_t *a) {
    if (!a || !a->hwnd) return;
    const char *file = app_game_file(a);
    const char *extra = a->status_active && a->status_msg[0] ? a->status_msg
        : !a->emu ? ""
        : !a->running ? GP32_TR("Paused", "일시정지")
        : a->fast_active ? GP32_TR("Fast forward", "빨리 감기")
        : a->fps_text;
    char game[MAX_PATH + 4] = "", title[MAX_PATH + 300];
    if (file || a->emu) snprintf(game, sizeof(game), " - %s", file ? base_name(file) : "BIOS");
    if (extra[0]) snprintf(title, sizeof(title), "%s%s - %s", GP32_BASE_TITLE, game, extra);
    else snprintf(title, sizeof(title), "%s%s", GP32_BASE_TITLE, game);
    SetWindowTextA(a->hwnd, title);
}

static void app_rebuild_recent_menu(app_state_t *a) {
    if (!a->recent_menu) return;
    while (GetMenuItemCount(a->recent_menu) > 0) DeleteMenu(a->recent_menu, 0, MF_BYPOSITION);
    int shown = 0;
    for (int i = 0; i < GP32_RECENT_MAX; ++i) {
        if (!a->recent[i][0]) continue;
        /* "&1  name", with '&' doubled so file names show literally. */
        char label[MAX_PATH + 16];
        int n = snprintf(label, sizeof(label), "&%d  ", shown + 1);
        for (const char *s = base_name(a->recent[i]); *s && n < (int)sizeof(label) - 3; ++s) {
            if (*s == '&') label[n++] = '&';
            label[n++] = *s;
        }
        label[n] = 0;
        AppendMenuA(a->recent_menu, MF_STRING, IDM_FILE_RECENT_FIRST + i, label);
        shown++;
    }
    if (!shown) {
        AppendMenuA(a->recent_menu, MF_STRING | MF_GRAYED, 0, GP32_TR("No recent games", "최근 게임 없음"));
        return;
    }
    AppendMenuA(a->recent_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(a->recent_menu, MF_STRING, IDM_FILE_RECENT_CLEAR, GP32_TR("Clear List", "목록 지우기"));
}

static void app_save_recent(app_state_t *a) {
    if (a->config_path[0]) {
        for (int i = 0; i < GP32_RECENT_MAX; ++i) {
            char key[16];
            snprintf(key, sizeof(key), "Game%d", i + 1);
            WritePrivateProfileStringA("Recent", key, a->recent[i][0] ? a->recent[i] : NULL, a->config_path);
        }
    }
    app_rebuild_recent_menu(a);
}

/* Most recent first; reopening a listed game moves it to the top. */
static void app_add_recent(app_state_t *a, const char *path) {
    char full[MAX_PATH];
    DWORD n = GetFullPathNameA(path, MAX_PATH, full, NULL);
    if (!n || n >= MAX_PATH) snprintf(full, sizeof(full), "%s", path);
    int at = GP32_RECENT_MAX - 1;
    for (int i = 0; i < GP32_RECENT_MAX; ++i)
        if (!_stricmp(a->recent[i], full)) { at = i; break; }
    memmove(a->recent[1], a->recent[0], (size_t)at * MAX_PATH);
    snprintf(a->recent[0], MAX_PATH, "%s", full);
    app_save_recent(a);
}

static void app_forget_recent(app_state_t *a, int index) {
    memmove(a->recent[index], a->recent[index + 1], (size_t)(GP32_RECENT_MAX - 1 - index) * MAX_PATH);
    a->recent[GP32_RECENT_MAX - 1][0] = 0;
    app_save_recent(a);
}

/* <folder of GP32emu.ini>\\name, created on demand. */
static int app_folder(const app_state_t *a, const char *name, char out[MAX_PATH]) {
    const char *slash = strrchr(a->config_path, '\\'), *slash2 = strrchr(a->config_path, '/');
    if (slash2 && (!slash || slash2 > slash)) slash = slash2;
    int dir = slash ? (int)(slash - a->config_path + 1) : 0;
    if (snprintf(out, MAX_PATH, "%.*s%s", dir, a->config_path, name) >= MAX_PATH) return 0;
    return CreateDirectoryA(out, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static void app_status_tick(app_state_t *a) {
    if (!a || !a->status_active) return;
    if (GetTickCount() - a->status_tick_ms < GP32_STATUS_MS) return;
    a->status_active = 0;
    a->status_msg[0] = 0;
    app_update_title(a);
}

/* Keep the display awake while a running machine is visible; the flag guard
   keeps the power request off the per-frame path. */
static void app_update_execution_state(app_state_t *a) {
    if (!a) return;
    int active = a->running && !IsIconic(a->hwnd);
    if (active == a->display_awake) return;
    a->display_awake = active;
    if (active) SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED);
    else SetThreadExecutionState(ES_CONTINUOUS);
}

static void app_set_cursor_hidden(app_state_t *a, int hidden) {
    if (!a) return;
    static int cursor_hidden;
    if (hidden == cursor_hidden) return;
    ShowCursor(hidden ? FALSE : TRUE);
    cursor_hidden = hidden;
    a->cursor_idle_frames = 0;
}

static void app_destroy_machine(app_state_t *a) {
    if (!a) return;
    if (a->emu && a->smc_save[0]) {
        if (gp32_save_card_progress(a->emu, a->smc_save) != GP32_OK) {
            char error[512];
            snprintf(error, sizeof(error), GP32_TR("Saving the game failed: %s\nThe latest progress was not saved.",
                                                   "게임 저장에 실패했습니다: %s\n마지막 진행 상황이 저장되지 않았습니다."), gp32_get_error(a->emu));
            app_show_status_error(a, error);
        } else if (a->smc_legacy[0] && remove(a->smc_legacy) != 0) {
            app_show_status_error(a, GP32_TR("Progress was saved as .gp32.sav. The unused older .gp32.smc file could not be removed.",
                                             "진행 상황은 .gp32.sav로 저장했지만, 더 이상 쓰지 않는 예전 .gp32.smc 파일을 지우지 못했습니다."));
        }
    }
    if (a->emu) gp32_destroy(a->emu);
    a->emu = NULL;
    a->smc_save[0] = 0;
    a->smc_legacy[0] = 0;
    a->running = 0;
    a->frame_index = 0;
    a->accum_units = 1000000ull;
    if (a->hwnd) InvalidateRect(a->hwnd, NULL, TRUE);
    update_menu_checks(a);
}

static void app_destroy_audio(app_state_t *a) {
    if (a && a->audio) { gp32_win64_audio_destroy(a->audio); a->audio = NULL; }
}

static void app_stop_recording(app_state_t *a) {
    if (!a || !a->recorder) return;
    gp32_media_recorder_close(a->recorder);
    a->recorder = NULL;
    a->recording = 0;
    app_set_status(a, GP32_TR("Recording stopped", "녹화를 멈췄습니다"));
    update_menu_checks(a);
}

static void app_toggle_fullscreen(app_state_t *a) {
    if (!a || !a->hwnd) return;
    app_set_cursor_hidden(a, 0);
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
    app_set_cursor_hidden(a, 0);
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
    if (!a->audio) return;
    const char *error = gp32_win64_audio_error(a->audio);
    if (error[0]) app_set_status(a, error);
}

static int app_create_machine(app_state_t *a) {
    if (!a) return 0;
    if (gp32_win64_is_card_save(a->smc)) {
        app_show_status_error(a, GP32_TR("This file holds saved progress, not the game.\nOpen the original .smc game; its saved progress is loaded automatically.",
                                         "이 파일은 게임이 아니라 저장 데이터입니다.\n원본 .smc 게임을 여세요. 저장된 진행 상황은 자동으로 불러옵니다."));
        return 0;
    }
    if (!a->bios[0] && !a->smc[0] && !a->fxe[0] && !a->fpk[0]) {
        app_set_status(a, GP32_TR("No game selected. Use File > Open Game or Game Library.",
                                  "선택한 게임이 없습니다. 파일 > 게임 열기나 게임 목록을 사용하세요."));
        return 0;
    }
    app_destroy_machine(a);
    char save_path[MAX_PATH] = {0};
    char legacy_path[MAX_PATH] = {0};
    const char *media_path = a->smc;
    if (a->smc[0]) {
        int n = snprintf(save_path, sizeof(save_path), "%s.gp32.sav", a->smc);
        if (n < 0 || (size_t)n >= sizeof(save_path)) {
            app_show_status_error(a, GP32_TR("The game path is too long for its save file.", "게임 경로가 너무 길어 저장 파일을 만들 수 없습니다."));
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
                app_show_status_error(a, GP32_TR("Cannot access the existing save file.", "기존 저장 파일에 접근할 수 없습니다."));
                return 0;
            }
        }
        if (attr != INVALID_FILE_ATTRIBUTES) {
            if (attr & FILE_ATTRIBUTE_DIRECTORY) {
                app_show_status_error(a, GP32_TR("The save file path is a folder.", "저장 파일 위치가 폴더입니다."));
                return 0;
            }
            if (media_path != legacy_path) media_path = save_path;
        } else if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) {
            app_show_status_error(a, GP32_TR("Cannot access the existing save file.", "기존 저장 파일에 접근할 수 없습니다."));
            return 0;
        }
    }
    gp32_options_t opt;
    memset(&opt, 0, sizeof(opt));
    a->emu = gp32_create(&opt);
    if (!a->emu) { app_show_status_error(a, GP32_TR("Not enough memory to start the emulator.", "메모리가 부족해 에뮬레이터를 시작할 수 없습니다.")); return 0; }
    gp32_set_jit(a->emu, a->jit);
    gp32_status_t st = GP32_OK;
    const int have_program = a->smc[0] || a->fxe[0] || a->fpk[0];
    const int use_real_bios = a->bios[0] != 0;
    const int use_hle_boot = have_program && !use_real_bios;
    if (use_hle_boot && !a->use_hle) {
        app_show_status_error(a, GP32_TR("No BIOS is set, and starting games without a BIOS is turned off in Settings.",
                                         "BIOS가 지정되지 않았고, 설정에서 BIOS 없이 실행이 꺼져 있습니다."));
        app_destroy_machine(a);
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
        snprintf(buf, sizeof(buf), GP32_TR("Cannot start this game: %s", "게임을 시작할 수 없습니다: %s"), gp32_get_error(a->emu));
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
    if (have_program && !use_real_bios)
        app_set_status(a, GP32_TR("Started without a BIOS (HLE)", "BIOS 없이 시작했습니다 (HLE)"));
    else app_set_status(a, "");
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
        app_show_status_error(a, GP32_TR("This file holds saved progress, not the game.\nOpen the original .smc game; its saved progress is loaded automatically.",
                                         "이 파일은 게임이 아니라 저장 데이터입니다.\n원본 .smc 게임을 여세요. 저장된 진행 상황은 자동으로 불러옵니다."));
        return 0;
    }
    a->smc[0] = a->fxe[0] = a->fpk[0] = 0;
    char *target = kind == 1 ? a->fxe : kind == 2 ? a->fpk : a->smc;
    snprintf(target, MAX_PATH, "%s", path);
    if (!app_create_machine(a)) return 0;
    app_add_recent(a, path);
    return 1;
}

static int app_open_path(app_state_t *a, const char *path) {
    const char *ext = strrchr(path, '.');
    if (!gp32_win64_is_game(path) && !gp32_win64_is_card_save(path)) {
        app_show_status_error(a, GP32_TR("Unsupported file. Choose an SMC, FXE or FPK game.",
                                         "지원하지 않는 파일입니다. SMC, FXE, FPK 게임을 고르세요."));
        return 0;
    }
    return app_open_game(a, path, (ext && !_stricmp(ext, ".fxe")) ? 1 : (ext && !_stricmp(ext, ".fpk")) ? 2 : 0);
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
    if (!select_save_file(a->hwnd, GP32_TR("Save State As", "다른 이름으로 상태 저장"), "GP32 state (*.gp32st)\0*.gp32st\0All files\0*.*\0", "gp32st", path, sizeof(path))) return;
    gp32_status_t st = gp32_save_state(a->emu, path);
    if (st == GP32_OK) { snprintf(a->state_path, sizeof(a->state_path), "%s", path); app_set_status(a, GP32_TR("State saved", "상태를 저장했습니다")); }
    else app_set_status(a, gp32_get_error(a->emu));
}

static void app_after_state_load(app_state_t *a) {
    gp32_clear_audio(a->emu);
    app_create_audio(a);
    a->accum_units = 1000000ull;
}

static void app_load_state_dialog(app_state_t *a) {
    if (!a || !a->emu) return;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s", a->state_path[0] ? a->state_path : "gp32_state.gp32st");
    if (!select_open_file(a->hwnd, GP32_TR("Load State From File", "파일에서 상태 불러오기"), "GP32 state (*.gp32st)\0*.gp32st\0All files\0*.*\0", path, sizeof(path))) return;
    gp32_status_t st = gp32_load_state(a->emu, path);
    if (st == GP32_OK) {
        snprintf(a->state_path, sizeof(a->state_path), "%s", path);
        app_after_state_load(a);
        app_set_status(a, GP32_TR("State loaded", "상태를 불러왔습니다"));
    } else app_set_status(a, gp32_get_error(a->emu));
}

/* Quick states: <exe folder>\\states\\<game file>.state<slot>.gp32st, so every
   game keeps its own slots and the files survive moving the game folder. */
static int app_slot_path(app_state_t *a, int slot, char out[MAX_PATH]) {
    const char *game = app_game_file(a);
    char dir[MAX_PATH];
    if (!app_folder(a, "states", dir)) return 0;
    return snprintf(out, MAX_PATH, "%s\\%s.state%d.gp32st", dir, game ? base_name(game) : "BIOS", slot) < MAX_PATH;
}

static void app_quick_save(app_state_t *a) {
    if (!a || !a->emu) return;
    char path[MAX_PATH], msg[96];
    if (!app_slot_path(a, a->state_slot, path)) {
        app_set_status(a, GP32_TR("Cannot create the states folder", "states 폴더를 만들 수 없습니다"));
        return;
    }
    if (gp32_save_state(a->emu, path) != GP32_OK) { app_set_status(a, gp32_get_error(a->emu)); return; }
    snprintf(msg, sizeof(msg), GP32_TR("Saved state to slot %d", "슬롯 %d에 상태를 저장했습니다"), a->state_slot);
    app_set_status(a, msg);
}

static void app_quick_load(app_state_t *a) {
    if (!a || !a->emu) return;
    char path[MAX_PATH], msg[96];
    if (!app_slot_path(a, a->state_slot, path) || GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        snprintf(msg, sizeof(msg), GP32_TR("Slot %d is empty", "슬롯 %d이(가) 비어 있습니다"), a->state_slot);
        app_set_status(a, msg);
        return;
    }
    if (gp32_load_state(a->emu, path) != GP32_OK) { app_set_status(a, gp32_get_error(a->emu)); return; }
    app_after_state_load(a);
    snprintf(msg, sizeof(msg), GP32_TR("Loaded state from slot %d", "슬롯 %d에서 상태를 불러왔습니다"), a->state_slot);
    app_set_status(a, msg);
}

static void app_select_slot(app_state_t *a, int slot) {
    char path[MAX_PATH], msg[96];
    a->state_slot = slot;
    app_save_config(a);
    int used = a->emu && app_slot_path(a, slot, path) && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
    snprintf(msg, sizeof(msg), used ? GP32_TR("State slot %d", "상태 슬롯 %d")
                                    : GP32_TR("State slot %d (empty)", "상태 슬롯 %d (비어 있음)"), slot);
    app_set_status(a, msg);
    update_menu_checks(a);
}

/* <exe folder>\\<folder>\\<game> YYYYMMDD-HHMMSS.<ext> */
static int app_media_path(app_state_t *a, const char *folder, const char *ext, char out[MAX_PATH]) {
    char dir[MAX_PATH], name[MAX_PATH];
    const char *game = app_game_file(a);
    if (!app_folder(a, folder, dir)) return 0;
    snprintf(name, sizeof(name), "%s", game ? base_name(game) : "GP32");
    char *dot = strrchr(name, '.');
    if (dot && dot != name) *dot = 0;
    SYSTEMTIME t;
    GetLocalTime(&t);
    return snprintf(out, MAX_PATH, "%s\\%s %04u%02u%02u-%02u%02u%02u.%s", dir, name, t.wYear, t.wMonth, t.wDay,
                    t.wHour, t.wMinute, t.wSecond, ext) < MAX_PATH;
}

/* F12 saves straight to the screenshots folder; no dialog interrupts play. */
static void app_screenshot_dialog(app_state_t *a) {
    if (!a || !a->emu) return;
    char path[MAX_PATH];
    if (!app_media_path(a, "screenshots", "bmp", path)) {
        app_show_status_error(a, GP32_TR("Cannot create the screenshots folder.", "screenshots 폴더를 만들 수 없습니다."));
        return;
    }
    gp32_framebuffer_desc_t fb;
    char err[256] = {0};
    if (gp32_get_framebuffer(a->emu, &fb) == GP32_OK && gp32_media_write_bmp_320x240(path, &fb, err, sizeof(err))) {
        char msg[MAX_PATH + 64];
        snprintf(a->screenshot_path, sizeof(a->screenshot_path), "%s", path);
        snprintf(msg, sizeof(msg), GP32_TR("Screenshot saved: screenshots\\%s", "스크린샷 저장: screenshots\\%s"), base_name(path));
        app_set_status(a, msg);
    } else {
        app_show_status_error(a, err[0] ? err : GP32_TR("Screenshot failed", "스크린샷을 저장하지 못했습니다"));
    }
}

static void app_start_recording(app_state_t *a) {
    if (!a || a->recorder || !a->emu) return;
    char path[MAX_PATH];
    if (!app_media_path(a, "recordings", "mkv", path)) {
        app_show_status_error(a, GP32_TR("Cannot create the recordings folder.", "recordings 폴더를 만들 수 없습니다."));
        return;
    }
    char err[256] = {0};
    a->recorder = gp32_media_recorder_open(path, 44100u, err, sizeof(err));
    if (!a->recorder) { app_show_status_error(a, err[0] ? err : GP32_TR("Cannot start recording", "녹화를 시작할 수 없습니다")); return; }
    snprintf(a->record_path, sizeof(a->record_path), "%s", path);
    a->recording = 1;
    char msg[MAX_PATH + 64];
    snprintf(msg, sizeof(msg), GP32_TR("Recording: recordings\\%s", "녹화 중: recordings\\%s"), base_name(path));
    app_set_status(a, msg);
    update_menu_checks(a);
}

static void update_menu_checks(app_state_t *a) {
    if (!a || !a->menu) return;
    /* Commands that act on a running machine are greyed until one exists. */
    static const UINT need_machine[] = {
        IDM_FILE_QUICK_SAVE, IDM_FILE_QUICK_LOAD, IDM_FILE_SAVE_STATE, IDM_FILE_LOAD_STATE,
        IDM_FILE_SCREENSHOT, IDM_EMU_RUN, IDM_EMU_RESET, IDM_EMU_FAST_FORWARD
    };
    for (size_t i = 0; i < sizeof(need_machine) / sizeof(need_machine[0]); ++i)
        EnableMenuItem(a->menu, need_machine[i], MF_BYCOMMAND | (a->emu ? MF_ENABLED : MF_GRAYED));
    CheckMenuItem(a->menu, IDM_EMU_RUN, MF_BYCOMMAND | (a->emu && !a->running ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(a->menu, IDM_EMU_FAST_FORWARD, MF_BYCOMMAND | (a->fast_toggle ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(a->menu, IDM_EMU_JIT, MF_BYCOMMAND | (a->jit ? MF_CHECKED : MF_UNCHECKED));
    EnableMenuItem(a->menu, IDM_FILE_RECORD_MKV, MF_BYCOMMAND | (a->emu && !a->recording ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(a->menu, IDM_FILE_STOP_RECORDING, MF_BYCOMMAND | (a->recording ? MF_ENABLED : MF_GRAYED));
    int slot = a->state_slot >= 1 && a->state_slot <= GP32_STATE_SLOTS ? a->state_slot : 1;
    CheckMenuRadioItem(a->menu, IDM_STATE_SLOT_FIRST, IDM_STATE_SLOT_FIRST + GP32_STATE_SLOTS - 1,
                       IDM_STATE_SLOT_FIRST + slot - 1, MF_BYCOMMAND);
    CheckMenuItem(a->menu, IDM_CONFIG_USE_HLE, MF_BYCOMMAND | ((!a->bios[0] && a->use_hle) ? MF_CHECKED : MF_UNCHECKED));
    EnableMenuItem(a->menu, IDM_CONFIG_USE_HLE, MF_BYCOMMAND | (a->bios[0] ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(a->menu, IDM_CONFIG_CLEAR_BIOS, MF_BYCOMMAND | (a->bios[0] ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(a->menu, IDM_CONFIG_BOOT_BIOS, MF_BYCOMMAND | (a->bios[0] ? MF_ENABLED : MF_GRAYED));
    CheckMenuItem(a->menu, IDM_VIDEO_FULLSCREEN, MF_BYCOMMAND | (a->fullscreen ? MF_CHECKED : MF_UNCHECKED));
    for (UINT i = 0; i < 4u; ++i)
        EnableMenuItem(a->menu, IDM_VIDEO_SIZE_FIRST + i, MF_BYCOMMAND | (a->fullscreen ? MF_GRAYED : MF_ENABLED));
    CheckMenuRadioItem(a->menu, IDM_VIDEO_KEEP_ASPECT, IDM_VIDEO_STRETCH,
                       a->integer_scaling ? IDM_VIDEO_INTEGER : a->keep_aspect ? IDM_VIDEO_KEEP_ASPECT : IDM_VIDEO_STRETCH,
                       MF_BYCOMMAND);
    CheckMenuItem(a->menu, IDM_VIDEO_LCD_PERSISTENCE, MF_BYCOMMAND | (a->lcd_persistence ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(a->menu, IDM_VIDEO_FRAME_INTERP, MF_BYCOMMAND | (a->frame_interpolation ? MF_CHECKED : MF_UNCHECKED));
    /* Show the backend that actually opened, not the requested one, so a
       silent D3D11 or WASAPI fallback is visible in the menu. */
    int gdi = !strcmp(a->video_backend, "gdi");
    if (a->video) gdi = strcmp(gp32_win64_video_active_backend(a->video), "d3d11") != 0;
    CheckMenuRadioItem(a->menu, IDM_VIDEO_D3D11, IDM_VIDEO_GDI, gdi ? IDM_VIDEO_GDI : IDM_VIDEO_D3D11, MF_BYCOMMAND);
    UINT audio_ui = a->audio_mode == GP32_WIN64_AUDIO_WAVEOUT ? IDM_AUDIO_WAVEOUT
        : (a->audio_mode == GP32_WIN64_AUDIO_WASAPI_EXCLUSIVE ? IDM_AUDIO_WASAPI_EXCL : IDM_AUDIO_WASAPI_SHARED);
    if (a->audio) {
        const char *backend = gp32_win64_audio_backend(a->audio);
        if (strncmp(backend, "waveOut", 7) == 0) audio_ui = IDM_AUDIO_WAVEOUT;
        else if (strncmp(backend, "WASAPI exclusive", 16) == 0) audio_ui = IDM_AUDIO_WASAPI_EXCL;
        else if (strncmp(backend, "WASAPI shared", 13) == 0) audio_ui = IDM_AUDIO_WASAPI_SHARED;
    }
    CheckMenuRadioItem(a->menu, IDM_AUDIO_WAVEOUT, IDM_AUDIO_WASAPI_EXCL, audio_ui, MF_BYCOMMAND);
}

static HMENU create_menu(app_state_t *a) {
    HMENU menu = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuA(file, MF_STRING, IDM_FILE_LIBRARY, GP32_TR("&Game Library...", "게임 목록(&G)..."));
    AppendMenuA(file, MF_STRING, IDM_FILE_OPEN_GAME, GP32_TR("&Open Game...", "게임 열기(&O)..."));
    a->recent_menu = CreatePopupMenu();
    AppendMenuA(file, MF_POPUP, (UINT_PTR)a->recent_menu, GP32_TR("&Recent Games", "최근 게임(&R)"));
    AppendMenuA(file, MF_SEPARATOR, 0, NULL);
    AppendMenuA(file, MF_STRING, IDM_FILE_QUICK_SAVE, GP32_TR("&Save State\tF5", "상태 저장(&S)\tF5"));
    AppendMenuA(file, MF_STRING, IDM_FILE_QUICK_LOAD, GP32_TR("&Load State\tF8", "상태 불러오기(&L)\tF8"));
    HMENU slots = CreatePopupMenu();
    for (int i = 0; i < GP32_STATE_SLOTS; ++i) {
        char label[32];
        snprintf(label, sizeof(label), GP32_TR("Slot &%d", "슬롯 &%d"), i + 1);
        AppendMenuA(slots, MF_STRING, IDM_STATE_SLOT_FIRST + i, label);
    }
    AppendMenuA(file, MF_POPUP, (UINT_PTR)slots, GP32_TR("State Slo&t\tF6/F7", "상태 슬롯(&T)\tF6/F7"));
    AppendMenuA(file, MF_STRING, IDM_FILE_SAVE_STATE, GP32_TR("Save State &As...", "다른 이름으로 상태 저장(&A)..."));
    AppendMenuA(file, MF_STRING, IDM_FILE_LOAD_STATE, GP32_TR("Load State from &File...", "파일에서 상태 불러오기(&F)..."));
    AppendMenuA(file, MF_SEPARATOR, 0, NULL);
    AppendMenuA(file, MF_STRING, IDM_FILE_SCREENSHOT, GP32_TR("Take S&creenshot\tF12", "스크린샷(&C)\tF12"));
    AppendMenuA(file, MF_STRING, IDM_FILE_RECORD_MKV, GP32_TR("Start Recording (MKV)", "녹화 시작 (MKV)"));
    AppendMenuA(file, MF_STRING | MF_GRAYED, IDM_FILE_STOP_RECORDING, GP32_TR("Stop Recording", "녹화 중지"));
    AppendMenuA(file, MF_SEPARATOR, 0, NULL);
    AppendMenuA(file, MF_STRING, IDM_FILE_EXIT, GP32_TR("E&xit", "종료(&X)"));
    AppendMenuA(menu, MF_POPUP, (UINT_PTR)file, GP32_TR("&File", "파일(&F)"));

    HMENU emu = CreatePopupMenu();
    AppendMenuA(emu, MF_STRING, IDM_EMU_RUN, GP32_TR("&Pause\tF9", "일시정지(&P)\tF9"));
    AppendMenuA(emu, MF_STRING, IDM_EMU_RESET, GP32_TR("&Reset", "리셋(&R)"));
    AppendMenuA(emu, MF_STRING, IDM_EMU_FAST_FORWARD, GP32_TR("&Fast Forward\tHold Tab", "빨리 감기(&F)\tTab 누르기"));
    AppendMenuA(emu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(emu, MF_STRING, IDM_EMU_JIT, GP32_TR("Use &JIT Recompiler (faster)", "JIT 사용(&J) (빠름)"));
    AppendMenuA(menu, MF_POPUP, (UINT_PTR)emu, GP32_TR("&Emulation", "에뮬레이션(&E)"));

    HMENU video = CreatePopupMenu();
    HMENU size = CreatePopupMenu();
    for (UINT i = 0; i < 4u; ++i) {
        char label[32];
        snprintf(label, sizeof(label), "&%ux  (%ux%u)", i + 1u, GP32_LCD_W * (i + 1u), GP32_LCD_H * (i + 1u));
        AppendMenuA(size, MF_STRING, IDM_VIDEO_SIZE_FIRST + i, label);
    }
    AppendMenuA(video, MF_POPUP, (UINT_PTR)size, GP32_TR("Window &Size", "창 크기(&S)"));
    AppendMenuA(video, MF_STRING, IDM_VIDEO_FULLSCREEN, GP32_TR("&Fullscreen\tF11 / Alt+Enter", "전체 화면(&F)\tF11 / Alt+Enter"));
    AppendMenuA(video, MF_SEPARATOR, 0, NULL);
    AppendMenuA(video, MF_STRING, IDM_VIDEO_KEEP_ASPECT, GP32_TR("Keep 4:3 &Aspect", "4:3 비율 유지(&A)"));
    AppendMenuA(video, MF_STRING, IDM_VIDEO_INTEGER, GP32_TR("&Integer Scaling (sharpest)", "정수배 확대(&I) (가장 선명)"));
    AppendMenuA(video, MF_STRING, IDM_VIDEO_STRETCH, GP32_TR("Stretch to &Window", "창에 맞게 늘이기(&W)"));
    AppendMenuA(video, MF_SEPARATOR, 0, NULL);
    AppendMenuA(video, MF_STRING, IDM_VIDEO_LCD_PERSISTENCE, GP32_TR("&LCD Ghosting (like the original screen)", "LCD 잔상 효과(&L) (실기 화면처럼)"));
    AppendMenuA(video, MF_STRING, IDM_VIDEO_FRAME_INTERP, GP32_TR("Frame &Blending", "프레임 블렌딩(&B)"));
    AppendMenuA(video, MF_SEPARATOR, 0, NULL);
    HMENU renderer = CreatePopupMenu();
    AppendMenuA(renderer, MF_STRING, IDM_VIDEO_D3D11, "Direct3D 11");
    AppendMenuA(renderer, MF_STRING, IDM_VIDEO_GDI, GP32_TR("GDI (compatibility)", "GDI (호환 모드)"));
    AppendMenuA(video, MF_POPUP, (UINT_PTR)renderer, GP32_TR("&Renderer", "렌더러(&R)"));
    AppendMenuA(menu, MF_POPUP, (UINT_PTR)video, GP32_TR("&Video", "비디오(&V)"));

    HMENU audio = CreatePopupMenu();
    AppendMenuA(audio, MF_STRING, IDM_AUDIO_WAVEOUT, GP32_TR("waveOut (default)", "waveOut (기본)"));
    AppendMenuA(audio, MF_STRING, IDM_AUDIO_WASAPI_SHARED, GP32_TR("WASAPI Shared", "WASAPI 공유"));
    AppendMenuA(audio, MF_STRING, IDM_AUDIO_WASAPI_EXCL, GP32_TR("WASAPI Exclusive", "WASAPI 독점"));
    AppendMenuA(menu, MF_POPUP, (UINT_PTR)audio, GP32_TR("&Audio", "오디오(&A)"));

    HMENU config = CreatePopupMenu();
    AppendMenuA(config, MF_STRING, IDM_CONFIG_KEYBOARD, GP32_TR("&Keyboard Controls...", "키보드 설정(&K)..."));
    AppendMenuA(config, MF_SEPARATOR, 0, NULL);
    AppendMenuA(config, MF_STRING, IDM_CONFIG_SET_BIOS, GP32_TR("Set &BIOS...", "BIOS 지정(&B)..."));
    AppendMenuA(config, MF_STRING, IDM_CONFIG_CLEAR_BIOS, GP32_TR("&Clear BIOS", "BIOS 지정 해제(&C)"));
    AppendMenuA(config, MF_STRING, IDM_CONFIG_BOOT_BIOS, GP32_TR("Start BIOS &Menu", "BIOS 메뉴 실행(&M)"));
    AppendMenuA(config, MF_STRING, IDM_CONFIG_USE_HLE, GP32_TR("Allow Games &without a BIOS (HLE)", "BIOS 없이 게임 실행 허용(&W) (HLE)"));
    AppendMenuA(menu, MF_POPUP, (UINT_PTR)config, GP32_TR("&Settings", "설정(&S)"));

    HMENU help = CreatePopupMenu();
    AppendMenuA(help, MF_STRING, IDM_HELP_CONTROLS, GP32_TR("&Controls and Shortcuts", "조작법과 단축키(&C)"));
    AppendMenuA(help, MF_STRING, IDM_HELP_ABOUT, GP32_TR("&About GP32emu", "GP32emu 정보(&A)"));
    AppendMenuA(menu, MF_POPUP, (UINT_PTR)help, GP32_TR("&Help", "도움말(&H)"));
    return menu;
}

/* Resize the client area to scale x 320x240. A second pass corrects for the
   menu bar wrapping to another line at the new width. */
static void app_set_window_scale(app_state_t *a, unsigned scale) {
    if (!a->hwnd || a->fullscreen) return;
    ShowWindow(a->hwnd, SW_RESTORE);
    for (int pass = 0; pass < 2; ++pass) {
        RECT win, client;
        GetWindowRect(a->hwnd, &win);
        GetClientRect(a->hwnd, &client);
        int w = (int)(win.right - win.left) + (int)(GP32_LCD_W * scale) - (int)(client.right - client.left);
        int h = (int)(win.bottom - win.top) + (int)(GP32_LCD_H * scale) - (int)(client.bottom - client.top);
        SetWindowPos(a->hwnd, NULL, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

static void app_apply_scaling(app_state_t *a) {
    app_save_config(a);
    if (a->video) {
        gp32_win64_video_set_keep_aspect(a->video, a->keep_aspect);
        gp32_win64_video_set_integer_scaling(a->video, a->integer_scaling);
    }
    update_menu_checks(a);
}

static void app_show_controls(app_state_t *a) {
    char k[GP32_KEY_COUNT][48], text[2048];
    for (int i = 0; i < GP32_KEY_COUNT; ++i) gp32_win64_key_name(a->preferences.keys[i], k[i], sizeof(k[i]));
    snprintf(text, sizeof(text), GP32_TR(
        "Keyboard (change in Settings > Keyboard Controls)\n"
        "Up = %s, Down = %s, Left = %s, Right = %s\n"
        "A = %s, B = %s, L = %s, R = %s, Start = %s, Select = %s\n\n"
        "Gamepad\n"
        "D-pad or left stick: directions\n"
        "Bottom face button: A, right face button: B\n"
        "Shoulder buttons or triggers: L and R\n"
        "Start: Start, Back / View / Share: Select\n\n"
        "Shortcuts\n"
        "F5 save state, F8 load state, F6 / F7 previous / next slot\n"
        "F9 pause, hold Tab to fast forward\n"
        "F11 or Alt+Enter fullscreen, Esc leaves fullscreen\n"
        "F12 screenshot (saved in the screenshots folder)\n\n"
        "Drop a game file on the window to play it.",
        "키보드 (설정 > 키보드 설정에서 변경)\n"
        "위 = %s, 아래 = %s, 왼쪽 = %s, 오른쪽 = %s\n"
        "A = %s, B = %s, L = %s, R = %s, Start = %s, Select = %s\n\n"
        "게임패드\n"
        "방향 패드나 왼쪽 스틱: 방향\n"
        "아래쪽 버튼: A, 오른쪽 버튼: B\n"
        "숄더 버튼이나 트리거: L, R\n"
        "Start: Start, Back / View / Share: Select\n\n"
        "단축키\n"
        "F5 상태 저장, F8 상태 불러오기, F6 / F7 이전 / 다음 슬롯\n"
        "F9 일시정지, Tab을 누르고 있으면 빨리 감기\n"
        "F11 또는 Alt+Enter 전체 화면, Esc로 전체 화면 해제\n"
        "F12 스크린샷 (screenshots 폴더에 저장)\n\n"
        "게임 파일을 창에 끌어다 놓으면 바로 실행합니다."),
        k[0], k[1], k[2], k[3], k[4], k[5], k[6], k[7], k[8], k[9]);
    MessageBoxA(a->hwnd, text, GP32_TR("Controls and Shortcuts", "조작법과 단축키"), MB_OK | MB_ICONINFORMATION);
}

static void app_show_about(app_state_t *a) {
    char text[1024];
    const char *pad = gp32_win64_sdl_input_status(a->sdl_input);
    if (!strcmp(pad, "no controller")) pad = GP32_TR("none", "없음");
    else if (!strcmp(pad, "disabled")) pad = GP32_TR("off", "꺼짐");
    snprintf(text, sizeof(text), GP32_TR(
        "GP32emu %s\nGame Park GP32 emulator\n\n"
        "Video: %s\nAudio: %s\nController: %s\nCPU: %s\nBIOS: %s\n\n"
        "Settings, states, screenshots and recordings are kept in the folder of gp32emu_win64.exe.",
        "GP32emu %s\nGame Park GP32 에뮬레이터\n\n"
        "비디오: %s\n오디오: %s\n컨트롤러: %s\nCPU: %s\nBIOS: %s\n\n"
        "설정, 상태 저장, 스크린샷, 녹화 파일은 gp32emu_win64.exe가 있는 폴더에 저장됩니다."),
        GP32_APP_VERSION,
        a->video ? gp32_win64_video_active_backend(a->video) : "-",
        a->audio ? gp32_win64_audio_backend(a->audio) : GP32_TR("off", "꺼짐"),
        pad,
        a->jit ? "JIT" : GP32_TR("interpreter", "인터프리터"),
        a->bios[0] ? base_name(a->bios) : GP32_TR("not set (HLE)", "지정 안 함 (HLE)"));
    MessageBoxA(a->hwnd, text, GP32_TR("About GP32emu", "GP32emu 정보"), MB_OK | MB_ICONINFORMATION);
}

static void app_command(app_state_t *a, UINT id) {
    if (!a) return;
    char path[MAX_PATH];
    if (id >= IDM_FILE_RECENT_FIRST && id < IDM_FILE_RECENT_FIRST + GP32_RECENT_MAX) {
        int i = (int)(id - IDM_FILE_RECENT_FIRST);
        snprintf(path, sizeof(path), "%s", a->recent[i]);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
            app_forget_recent(a, i);
            app_show_status_error(a, GP32_TR("This game file no longer exists, so it was removed from the list.",
                                             "게임 파일이 없어져 목록에서 지웠습니다."));
        } else app_open_path(a, path);
        return;
    }
    if (id >= IDM_STATE_SLOT_FIRST && id < IDM_STATE_SLOT_FIRST + GP32_STATE_SLOTS) {
        app_select_slot(a, (int)(id - IDM_STATE_SLOT_FIRST) + 1);
        return;
    }
    if (id >= IDM_VIDEO_SIZE_FIRST && id < IDM_VIDEO_SIZE_FIRST + 4u) {
        app_set_window_scale(a, id - IDM_VIDEO_SIZE_FIRST + 1u);
        return;
    }
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
        if (selected) app_open_path(a, path);
        break;
    }
    case IDM_FILE_OPEN_GAME:
        if (select_open_file(a->hwnd, GP32_TR("Open Game", "게임 열기"),
                             GP32_TR("GP32 games (*.smc;*.fxe;*.fpk)\0*.smc;*.fxe;*.fpk\0All files\0*.*\0",
                                     "GP32 게임 (*.smc;*.fxe;*.fpk)\0*.smc;*.fxe;*.fpk\0모든 파일\0*.*\0"),
                             path, sizeof(path)))
            app_open_path(a, path);
        break;
    case IDM_FILE_RECENT_CLEAR:
        memset(a->recent, 0, sizeof(a->recent));
        app_save_recent(a);
        break;
    case IDM_FILE_QUICK_SAVE: app_quick_save(a); break;
    case IDM_FILE_QUICK_LOAD: app_quick_load(a); break;
    case IDM_FILE_SAVE_STATE: app_save_state_dialog(a); break;
    case IDM_FILE_LOAD_STATE: app_load_state_dialog(a); break;
    case IDM_FILE_SCREENSHOT: app_screenshot_dialog(a); break;
    case IDM_FILE_RECORD_MKV: app_start_recording(a); break;
    case IDM_FILE_STOP_RECORDING: app_stop_recording(a); break;
    case IDM_FILE_EXIT: PostMessageA(a->hwnd, WM_CLOSE, 0, 0); break;
    case IDM_EMU_RUN:
        if (!a->emu) app_create_machine(a);
        else a->running = !a->running;
        update_menu_checks(a);
        app_update_title(a);
        break;
    case IDM_EMU_RESET:
        if (!a->emu) app_create_machine(a);
        else app_reset_machine(a);
        break;
    case IDM_EMU_FAST_FORWARD:
        a->fast_toggle = !a->fast_toggle;
        update_menu_checks(a);
        break;
    case IDM_EMU_JIT: {
        int new_jit = !a->jit;
        if (a->emu && MessageBoxA(a->hwnd, GP32_TR("Changing this restarts the game. Progress since the last in-game save or saved state is lost. Continue?",
                                                   "이 설정을 바꾸면 게임이 다시 시작됩니다. 마지막 게임 내 저장이나 상태 저장 이후의 진행은 사라집니다. 계속할까요?"),
                                  "GP32emu", MB_YESNO | MB_ICONWARNING) != IDYES) { update_menu_checks(a); break; }
        a->jit = new_jit;
        app_save_config(a);
        if (a->emu) app_create_machine(a);
        update_menu_checks(a);
        break;
    }
    case IDM_VIDEO_D3D11: strcpy(a->video_backend, "d3d11"); app_save_config(a); app_recreate_video(a); update_menu_checks(a); break;
    case IDM_VIDEO_GDI: strcpy(a->video_backend, "gdi"); app_save_config(a); app_recreate_video(a); update_menu_checks(a); break;
    case IDM_VIDEO_FULLSCREEN: app_toggle_fullscreen(a); break;
    case IDM_VIDEO_KEEP_ASPECT: a->keep_aspect = 1; a->integer_scaling = 0; app_apply_scaling(a); break;
    case IDM_VIDEO_INTEGER: a->integer_scaling = 1; a->keep_aspect = 0; app_apply_scaling(a); break;
    case IDM_VIDEO_STRETCH: a->integer_scaling = 0; a->keep_aspect = 0; app_apply_scaling(a); break;
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
        if (select_open_file(a->hwnd, GP32_TR("Choose GP32 BIOS", "GP32 BIOS 선택"),
                             GP32_TR("GP32 BIOS (*.bin;*.rom;*.bios;*.zip)\0*.bin;*.rom;*.bios;*.zip\0All files\0*.*\0",
                                     "GP32 BIOS (*.bin;*.rom;*.bios;*.zip)\0*.bin;*.rom;*.bios;*.zip\0모든 파일\0*.*\0"),
                             path, sizeof(path))) {
            snprintf(a->bios, sizeof(a->bios), "%s", path);
            a->use_hle = 0;
            app_save_config(a);
            update_menu_checks(a);
            app_set_status(a, a->emu ? GP32_TR("BIOS set. It is used from the next game start.", "BIOS를 지정했습니다. 다음 게임 실행부터 사용합니다.")
                                     : GP32_TR("BIOS set", "BIOS를 지정했습니다"));
        }
        break;
    case IDM_CONFIG_CLEAR_BIOS:
        a->bios[0] = 0;
        a->use_hle = 1;
        app_save_config(a);
        update_menu_checks(a);
        app_set_status(a, GP32_TR("BIOS cleared. Games start without a BIOS (HLE).", "BIOS 지정을 해제했습니다. 게임을 BIOS 없이 실행합니다 (HLE)."));
        break;
    case IDM_CONFIG_BOOT_BIOS:
        if (!a->bios[0]) break;
        a->smc[0] = a->fxe[0] = a->fpk[0] = 0;
        app_create_machine(a);
        break;
    case IDM_CONFIG_USE_HLE:
        if (!a->bios[0]) {
            a->use_hle = !a->use_hle;
            app_set_status(a, a->use_hle ? GP32_TR("Games may start without a BIOS (HLE)", "BIOS 없이 게임 실행을 허용했습니다 (HLE)")
                                         : GP32_TR("Games need a BIOS now. Set one in Settings.", "이제 BIOS가 있어야 게임을 실행합니다. 설정에서 BIOS를 지정하세요."));
        }
        app_save_config(a);
        update_menu_checks(a);
        break;
    case IDM_HELP_CONTROLS: app_show_controls(a); break;
    case IDM_HELP_ABOUT: app_show_about(a); break;
    default: break;
    }
}

/* Remember the windowed position and size for the next start. */
static void app_save_window(app_state_t *a) {
    if (!a->config_path[0] || !a->hwnd) return;
    WINDOWPLACEMENT wp;
    memset(&wp, 0, sizeof(wp));
    wp.length = sizeof(wp);
    if (a->fullscreen) wp = a->windowed_placement;
    else if (!GetWindowPlacement(a->hwnd, &wp)) return;
    if (wp.length != sizeof(wp)) return;
    int maximized = wp.showCmd == SW_SHOWMAXIMIZED ||
                    (wp.showCmd == SW_SHOWMINIMIZED && (wp.flags & WPF_RESTORETOMAXIMIZED));
    char value[96];
    snprintf(value, sizeof(value), "%ld %ld %ld %ld %d", wp.rcNormalPosition.left, wp.rcNormalPosition.top,
             wp.rcNormalPosition.right, wp.rcNormalPosition.bottom, maximized);
    WritePrivateProfileStringA("Window", "Placement", value, a->config_path);
}

static int app_restore_window(app_state_t *a) {
    char value[96];
    RECT r;
    int maximized = 0;
    GetPrivateProfileStringA("Window", "Placement", "", value, (DWORD)sizeof(value), a->config_path);
    if (sscanf(value, "%ld %ld %ld %ld %d", &r.left, &r.top, &r.right, &r.bottom, &maximized) != 5 ||
        r.right - r.left < 200 || r.bottom - r.top < 150 || !MonitorFromRect(&r, MONITOR_DEFAULTTONULL))
        return 0;
    WINDOWPLACEMENT wp;
    memset(&wp, 0, sizeof(wp));
    wp.length = sizeof(wp);
    GetWindowPlacement(a->hwnd, &wp);
    wp.rcNormalPosition = r;
    wp.flags = 0;
    wp.showCmd = maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    return SetWindowPlacement(a->hwnd, &wp) != 0;
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    app_state_t *a = (app_state_t *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTA *cs = (CREATESTRUCTA *)lparam;
        a = (app_state_t *)cs->lpCreateParams;
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)a);
        a->hwnd = hwnd;
        DragAcceptFiles(hwnd, TRUE);
        app_recreate_video(a);
        return 0;
    }
    case WM_SIZE:
        if (a) {
            unsigned w = LOWORD(lparam), h = HIWORD(lparam);
            a->cursor_idle_frames = 0;
            if (a->video) gp32_win64_video_resize(a->video, w ? w : 1u, h ? h : 1u);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (a) a->cursor_idle_frames = 0;
        return 0;
    case WM_PAINT:
        /* With no machine nothing presents; tell a first-time user what to do. */
        if (a && !a->emu) {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(170, 178, 190));
            HGDIOBJ old = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
            const char *hint = GP32_TR("Open a game with File > Open Game or File > Game Library,\nor drop an SMC, FXE or FPK file here.",
                                       "파일 > 게임 열기나 파일 > 게임 목록으로 게임을 고르거나,\nSMC, FXE, FPK 파일을 여기에 끌어다 놓으세요.");
            /* GDI converts A-strings with the font's charset, not the UTF-8
               process code page, so draw the wide form. */
            wchar_t wide[256];
            if (!MultiByteToWideChar(CP_UTF8, 0, hint, -1, wide, 256)) wide[0] = 0;
            RECT box = rc;
            DrawTextW(dc, wide, -1, &box, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
            int h = box.bottom - box.top;
            box.left = rc.left + 16; box.right = rc.right - 16;
            box.top = rc.top + (rc.bottom - rc.top - h) / 2; box.bottom = box.top + h;
            DrawTextW(dc, wide, -1, &box, DT_CENTER | DT_WORDBREAK);
            SelectObject(dc, old);
            EndPaint(hwnd, &ps);
            return 0;
        }
        return DefWindowProcA(hwnd, msg, wparam, lparam);
    case WM_KEYDOWN:
        if (a) {
            /* Auto-repeat (bit 30) must not repeat a command or stack dialogs. */
            int repeat = (lparam & (1L << 30)) != 0;
            switch (wparam) {
            case VK_ESCAPE: if (a->fullscreen) app_toggle_fullscreen(a); return 0;
            case VK_TAB: a->fast_key = 1; return 0;
            case VK_F5: if (!repeat) app_quick_save(a); return 0;
            case VK_F6: if (!repeat) app_select_slot(a, a->state_slot <= 1 ? GP32_STATE_SLOTS : a->state_slot - 1); return 0;
            case VK_F7: if (!repeat) app_select_slot(a, a->state_slot >= GP32_STATE_SLOTS ? 1 : a->state_slot + 1); return 0;
            case VK_F8: if (!repeat) app_quick_load(a); return 0;
            case VK_F9: if (!repeat && a->emu) app_command(a, IDM_EMU_RUN); return 0;
            case VK_F11: if (!repeat) app_toggle_fullscreen(a); return 0;
            case VK_F12: if (!repeat) app_screenshot_dialog(a); return 0;
            default: break;
            }
            a->keyboard_buttons |= gp32_win64_key_button(&a->preferences, wparam);
        }
        return 0;
    case WM_SYSKEYDOWN:
        if (a && wparam == VK_RETURN && (HIWORD(lparam) & KF_ALTDOWN)) { app_toggle_fullscreen(a); return 0; }
        return DefWindowProcA(hwnd, msg, wparam, lparam);
    case WM_KEYUP:
        if (a) {
            if (wparam == VK_TAB) a->fast_key = 0;
            a->keyboard_buttons &= ~gp32_win64_key_button(&a->preferences, wparam);
        }
        return 0;
    case WM_ENTERMENULOOP:
    case WM_KILLFOCUS:
        /* Modal menus and other windows can consume the matching key release. */
        if (a) { a->keyboard_buttons = 0; a->fast_key = 0; }
        return 0;
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wparam;
        wchar_t wide[MAX_PATH];
        char path[MAX_PATH];
        int ok = DragQueryFileW(drop, 0, wide, MAX_PATH) &&
                 WideCharToMultiByte(CP_UTF8, 0, wide, -1, path, (int)sizeof(path), NULL, NULL);
        DragFinish(drop);
        if (a && ok) {
            SetForegroundWindow(hwnd);
            app_open_path(a, path);
        }
        return 0;
    }
    case WM_COMMAND:
        app_command(a, LOWORD(wparam));
        return 0;
    case WM_CLOSE:
        if (a) app_save_window(a);
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
    /* Fast forward drops sound instead of queueing several seconds of it. */
    int quiet = a->fast_active;
    gp32_set_buttons(a->emu, a->buttons);
    gp32_set_host_pump(a->emu, !quiet && (a->audio || a->recorder) ? pump_frame_audio : NULL, &audio_ctx);
    gp32_status_t st = gp32_run_frame(a->emu);
    gp32_set_host_pump(a->emu, NULL, NULL);
    if (st == GP32_OK) {
        if (quiet) gp32_clear_audio(a->emu);
        else pump_frame_audio(a->emu, &audio_ctx);
    }
    /* Recorder cleanup updates menus; keep it outside the core's pump hook. */
    if (audio_ctx.recording_failed) {
        app_set_status(a, gp32_media_recorder_error(a->recorder));
        app_stop_recording(a);
    }
    if (st != GP32_OK) { app_set_status(a, gp32_get_error(a->emu)); a->running = 0; return; }
    a->frame_index++;
    a->emu_frames++;
    if (a->smc_save[0] && gp32_poll_card_progress(a->emu, a->smc_save) != GP32_OK)
        app_show_status_error(a, gp32_get_error(a->emu));
}

static void app_pump(app_state_t *a) {
    int quit = 0;
    static int was_running;
    uint32_t actions = 0;
    if (a->sdl_input) gp32_win64_sdl_input_poll(a->sdl_input, a->keyboard_buttons, &a->buttons, &actions, &quit);
    else a->buttons = a->keyboard_buttons;
    if (quit) PostMessageA(a->hwnd, WM_CLOSE, 0, 0);
    /* Frontend actions (F5/F8) belong to the message loop here; this frontend
       opens its own save/load dialogs, so the SDL action bits are unused. */
    (void)actions;

    int minimized = IsIconic(a->hwnd);
    if (a->running && !was_running) {
        a->accum_units = 1000000ull;
        // Refresh presentation and audio after a pause so a resume never plays
        // stale PCM or a stale frame.
        if (a->video && a->emu) {
            gp32_framebuffer_desc_t fb;
            if (gp32_get_framebuffer(a->emu, &fb) == GP32_OK) gp32_win64_video_present(a->video, &fb);
        }
        if (a->emu && a->audio) { gp32_clear_audio(a->emu); app_create_audio(a); }
    }
    if (!a->running && was_running) {
        // A pause drains the device with its own gap fade instead of a pop.
        gp32_clear_audio(a->emu);
        if (a->audio) gp32_win64_audio_reset(a->audio);
    }
    was_running = a->running;
    app_update_execution_state(a);
    /* Recording keeps real time so its audio and video stay in step. */
    int fast = a->running && a->emu && !minimized && !a->recorder && (a->fast_key || a->fast_toggle);
    if (fast != a->fast_active) {
        a->fast_active = fast;
        if (a->emu) gp32_clear_audio(a->emu);
        if (a->audio) gp32_win64_audio_reset(a->audio);
        a->accum_units = 1000000ull;
        app_update_title(a);
    }
    const unsigned speed = fast ? GP32_FAST_FORWARD : 1u;

    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    uint64_t elapsed_us = qpc_elapsed_us(a, now);
    a->last_qpc = now;
    if (minimized) {
        /* Keep the backlog empty while minimized so a restore resumes in
           real time instead of bursting a minute of frames. */
        a->accum_units = 1000000ull;
        elapsed_us = 0;
    } else if (elapsed_us > 250000u) {
        elapsed_us = 250000u;
    }
    int ran = 0;
    if (a->running && a->emu && !minimized) {
        a->accum_units += elapsed_us * 60ull * speed;
        unsigned steps = 0;
        while (a->accum_units >= 1000000ull && steps < 5u * speed) {
            run_one_frame(a);
            a->accum_units -= 1000000ull;
            steps++;
            ran = 1;
        }
        if (a->accum_units >= 1000000ull && steps >= 5u * speed) a->accum_units = 1000000ull;
    }
    if (ran && a->video && a->emu) {
        gp32_framebuffer_desc_t fb;
        if (gp32_get_framebuffer(a->emu, &fb) == GP32_OK) { gp32_win64_video_present(a->video, &fb); if (a->recorder && !gp32_media_recorder_add_frame(a->recorder, &fb, a->frame_index ? a->frame_index - 1u : 0u)) { app_set_status(a, gp32_media_recorder_error(a->recorder)); app_stop_recording(a); } a->render_frames++; }
    }
    if (a->audio) gp32_win64_audio_pump(a->audio);

    DWORD now_ms = GetTickCount();
    app_status_tick(a);
    if (a->fullscreen) {
        /* Hide the pointer once the mouse has been idle for a moment. */
        if (a->cursor_idle_frames < GP32_CURSOR_IDLE_FRAMES) {
            a->cursor_idle_frames++;
            if (a->cursor_idle_frames == GP32_CURSOR_IDLE_FRAMES) app_set_cursor_hidden(a, 1);
        }
    }
    if (!a->fps_tick_ms) a->fps_tick_ms = now_ms;
    if (now_ms - a->fps_tick_ms >= 1000u) {
        /* Backends and controller are listed in Help > About. */
        snprintf(a->fps_text, sizeof(a->fps_text), a->recording ? "REC  %u fps" : "%u fps", a->emu_frames);
        a->render_frames = a->emu_frames = 0;
        a->fps_tick_ms = now_ms;
        app_update_title(a);
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
    gp32_win64_ui_init_language(app.config_path);
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
    app.menu = create_menu(&app);
    app_rebuild_recent_menu(&app);
    update_menu_checks(&app);
    RECT wr = {0, 0, (LONG)(GP32_LCD_W * 2u), (LONG)(GP32_LCD_H * 2u)};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, TRUE);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, GP32_BASE_TITLE, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top, NULL, app.menu, inst, &app);
    if (!hwnd) return 1;
    app.hwnd = hwnd;
    app.sdl_input = gp32_win64_sdl_input_create(0);
    if (!app_restore_window(&app)) ShowWindow(hwnd, show);
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
        /* Ask once; "No" keeps starting games without a BIOS until one is set. */
        if (GetPrivateProfileIntA("Paths", "AskForBIOS", 1, app.config_path)) {
            int choose = MessageBoxA(hwnd, GP32_TR(
                "Most GP32 games need the original GP32 BIOS file (512 KB or smaller, for example gp32166m.bin).\n\n"
                "Choose your BIOS file now?\n\n"
                "No: start games without a BIOS. Some games do not start this way. You can set the BIOS later in Settings.",
                "대부분의 GP32 게임은 원본 GP32 BIOS 파일(512KB 이하, 예: gp32166m.bin)이 있어야 합니다.\n\n"
                "지금 BIOS 파일을 지정할까요?\n\n"
                "아니요: BIOS 없이 실행합니다. 일부 게임은 이 방식으로 시작하지 않습니다. BIOS는 나중에 설정에서 지정할 수 있습니다."),
                "GP32emu", MB_YESNO | MB_ICONQUESTION) == IDYES;
            if (choose) app_command(&app, IDM_CONFIG_SET_BIOS);
            else WritePrivateProfileStringA("Paths", "AskForBIOS", "0", app.config_path);
        }
    } else {
        app.use_hle = 0;
        update_menu_checks(&app);
    }
    if (app.smc[0] || app.fxe[0] || app.fpk[0]) {
        if (app_create_machine(&app)) app_add_recent(&app, app_game_file(&app));
    } else if (app.bios[0]) app_create_machine(&app);

    MSG msg;
    while (!app.quit) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        if (app.quit) break;
        app_pump(&app);
    }
    if (app.fullscreen) { app.fullscreen = 0; app_toggle_fullscreen(&app); }
    SetThreadExecutionState(ES_CONTINUOUS);
    app_stop_recording(&app);
    app_destroy_audio(&app);
    if (app.sdl_input) gp32_win64_sdl_input_destroy(app.sdl_input);
    if (app.video) gp32_win64_video_destroy(app.video);
    app_destroy_machine(&app);
    return 0;
}
