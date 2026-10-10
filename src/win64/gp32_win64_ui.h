#ifndef GP32_WIN64_UI_H
#define GP32_WIN64_UI_H

/* Shared with the Windows resource compiler. */
#define IDD_KEYBOARD 201
#define IDD_LIBRARY 202
#define IDC_KEY_FIRST 2000
#define IDC_KEY_DEFAULTS 2010
#define IDC_KEY_HINT 2011
#define IDC_KEY_LABEL_FIRST 2020
#define IDC_GAME_FOLDER 2100
#define IDC_GAME_BROWSE 2101
#define IDC_GAME_LIST 2102
#define IDC_GAME_REFRESH 2103
#define IDC_GAME_HINT 2104

#ifndef RC_INVOKED
#include <windows.h>
#include <stddef.h>
#include <stdint.h>

#define GP32_KEY_COUNT 10
/* Korean UI when the Windows display language is Korean (or [UI]
   Language=ko in GP32emu.ini), English otherwise. Strings are UTF-8; the
   manifest makes UTF-8 the process code page for the A-suffixed APIs. */
extern int gp32_win64_korean;
#define GP32_TR(en, ko) (gp32_win64_korean ? (ko) : (en))
void gp32_win64_ui_init_language(const char *ini);
/* Playback selections the dialogs can change; the main file persists them
   with the rest of the configuration. */
#define GP32_WIN64_AUDIO_WAVEOUT_ID 0
#define GP32_WIN64_AUDIO_WASAPI_SHARED_ID 1
#define GP32_WIN64_AUDIO_WASAPI_EXCLUSIVE_ID 2
typedef struct gp32_win64_preferences {
    UINT keys[GP32_KEY_COUNT];
    char game_folder[MAX_PATH];
    int audio_mode;
    char video_backend[16];
} gp32_win64_preferences_t;

void gp32_win64_preferences_load(gp32_win64_preferences_t *p, const char *ini);
void gp32_win64_preferences_save(const gp32_win64_preferences_t *p, const char *ini);
uint32_t gp32_win64_key_button(const gp32_win64_preferences_t *p, WPARAM vk);
void gp32_win64_key_name(UINT key, char *out, size_t size);
int gp32_win64_is_card_save(const char *path);
int gp32_win64_is_game(const char *path);
int gp32_win64_keyboard_dialog(HWND owner, HINSTANCE inst, gp32_win64_preferences_t *p);
int gp32_win64_library_dialog(HWND owner, HINSTANCE inst, gp32_win64_preferences_t *p,
                              char path[MAX_PATH]);
#endif
#endif
