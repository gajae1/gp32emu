#include "gp32_win64_ui.h"
#include "gp32emu/gp32.h"
#include <shlobj.h>
#include <stdio.h>
#include <string.h>

static const UINT default_keys[GP32_KEY_COUNT] = {
    VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, 'Z', 'X', 'A', 'S', VK_RETURN, VK_SHIFT
};
static const char *const key_names[GP32_KEY_COUNT] = {
    "Up", "Down", "Left", "Right", "A", "B", "L", "R", "Start", "Select"
};
static const char *const key_labels_ko[GP32_KEY_COUNT] = {
    "위", "아래", "왼쪽", "오른쪽", "A", "B", "L", "R", "Start", "Select"
};
static const uint32_t button_bits[GP32_KEY_COUNT] = {
    GP32_BUTTON_UP, GP32_BUTTON_DOWN, GP32_BUTTON_LEFT, GP32_BUTTON_RIGHT,
    GP32_BUTTON_A, GP32_BUTTON_B, GP32_BUTTON_L, GP32_BUTTON_R,
    GP32_BUTTON_START, GP32_BUTTON_SELECT
};

int gp32_win64_korean;

void gp32_win64_ui_init_language(const char *ini) {
    char lang[8] = "auto";
    if (ini && ini[0]) GetPrivateProfileStringA("UI", "Language", "auto", lang, (DWORD)sizeof(lang), ini);
    if (!_stricmp(lang, "ko")) gp32_win64_korean = 1;
    else if (!_stricmp(lang, "en")) gp32_win64_korean = 0;
    else gp32_win64_korean = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_KOREAN;
}

static UINT normalize_key(UINT key) {
    if (key == VK_LSHIFT || key == VK_RSHIFT) return VK_SHIFT;
    if (key == VK_LCONTROL || key == VK_RCONTROL) return VK_CONTROL;
    return key;
}

static int valid_key(UINT key) {
    /* Esc, Tab, Alt, Windows and F5-F12 are emulator or window commands. */
    return key >= VK_BACK && key < 256 && key != VK_ESCAPE && key != VK_TAB && key != VK_MENU &&
        key != VK_LMENU && key != VK_RMENU && key != VK_LWIN && key != VK_RWIN &&
        !(key >= VK_F5 && key <= VK_F12);
}

void gp32_win64_preferences_load(gp32_win64_preferences_t *p, const char *ini) {
    memcpy(p->keys, default_keys, sizeof(p->keys));
    p->game_folder[0] = 0;
    p->audio_mode = GP32_WIN64_AUDIO_WAVEOUT_ID;
    strcpy(p->video_backend, "d3d11");
    if (!ini || !ini[0]) return;
    int valid = 1;
    for (int i = 0; i < GP32_KEY_COUNT; ++i) {
        p->keys[i] = normalize_key(GetPrivateProfileIntA("Keyboard", key_names[i], default_keys[i], ini));
        if (!valid_key(p->keys[i])) valid = 0;
        for (int j = 0; j < i; ++j) if (p->keys[i] == p->keys[j]) valid = 0;
    }
    /* A malformed partial map must not leave any button unreachable. */
    if (!valid) memcpy(p->keys, default_keys, sizeof(p->keys));
    GetPrivateProfileStringA("Paths", "GameFolder", "", p->game_folder, MAX_PATH, ini);
    char audio[32];
    GetPrivateProfileStringA("Audio", "Backend", "waveout", audio, (DWORD)sizeof(audio), ini);
    p->audio_mode = !strcmp(audio, "wasapi_shared") ? GP32_WIN64_AUDIO_WASAPI_SHARED_ID
        : !strcmp(audio, "wasapi_exclusive") ? GP32_WIN64_AUDIO_WASAPI_EXCLUSIVE_ID
        : GP32_WIN64_AUDIO_WAVEOUT_ID;
    GetPrivateProfileStringA("Video", "Backend", "d3d11", p->video_backend, (DWORD)sizeof(p->video_backend), ini);
}

void gp32_win64_preferences_save(const gp32_win64_preferences_t *p, const char *ini) {
    WritePrivateProfileStringA("Paths", "GameFolder", p->game_folder, ini);
    const char *audio = p->audio_mode == GP32_WIN64_AUDIO_WASAPI_SHARED_ID ? "wasapi_shared"
        : p->audio_mode == GP32_WIN64_AUDIO_WASAPI_EXCLUSIVE_ID ? "wasapi_exclusive" : "waveout";
    WritePrivateProfileStringA("Audio", "Backend", audio, ini);
    WritePrivateProfileStringA("Video", "Backend", p->video_backend[0] ? p->video_backend : "d3d11", ini);
    for (int i = 0; i < GP32_KEY_COUNT; ++i) {
        char value[16];
        snprintf(value, sizeof(value), "%u", p->keys[i]);
        WritePrivateProfileStringA("Keyboard", key_names[i], value, ini);
    }
}

uint32_t gp32_win64_key_button(const gp32_win64_preferences_t *p, WPARAM vk) {
    UINT key = normalize_key((UINT)vk);
    for (int i = 0; i < GP32_KEY_COUNT; ++i)
        if (p->keys[i] == key) return button_bits[i];
    return 0;
}

void gp32_win64_key_name(UINT key, char *out, size_t size) {
    LONG scan = (LONG)(MapVirtualKeyA(key, MAPVK_VK_TO_VSC) << 16);
    if ((key >= VK_PRIOR && key <= VK_DOWN) || key == VK_INSERT || key == VK_DELETE || key == VK_DIVIDE)
        scan |= 1L << 24;
    if (!GetKeyNameTextA(scan, out, (int)size)) snprintf(out, size, "Key %u", key);
}

int gp32_win64_is_card_save(const char *path) {
    size_t n = strlen(path);
    return n >= 9 && (!_stricmp(path + n - 9, ".gp32.smc") || !_stricmp(path + n - 9, ".gp32.sav"));
}

int gp32_win64_is_game(const char *path) {
    const char *ext = strrchr(path, '.');
    return ext && !gp32_win64_is_card_save(path) &&
        (!_stricmp(ext, ".smc") || !_stricmp(ext, ".fxe") || !_stricmp(ext, ".fpk"));
}

typedef struct keyboard_dialog {
    gp32_win64_preferences_t pending;
    gp32_win64_preferences_t *target;
    WNDPROC button_proc;
    int capturing;
} keyboard_dialog_t;

static const char *keyboard_hint(void) {
    return GP32_TR("Click a control, then press a key. Duplicate keys are swapped.",
                   "바꿀 버튼을 누른 뒤 원하는 키를 누르세요. 이미 쓰던 키는 서로 바뀝니다.");
}

static void key_labels(HWND dlg, keyboard_dialog_t *d) {
    for (int i = 0; i < GP32_KEY_COUNT; ++i) {
        char name[64];
        gp32_win64_key_name(d->pending.keys[i], name, sizeof(name));
        SetDlgItemTextA(dlg, IDC_KEY_FIRST + i, name);
    }
}

static LRESULT CALLBACK capture_key(HWND button, UINT msg, WPARAM wp, LPARAM lp) {
    HWND dlg = GetParent(button);
    keyboard_dialog_t *d = (keyboard_dialog_t *)GetWindowLongPtrA(dlg, DWLP_USER);
    if (d && d->capturing == GetDlgCtrlID(button) - IDC_KEY_FIRST) {
        if (msg == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
        if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
            if (lp & (1L << 30)) return 0;
            UINT key = normalize_key((UINT)wp);
            if (key != VK_ESCAPE) {
                if (!valid_key(key)) {
                    SetDlgItemTextA(dlg, IDC_KEY_HINT, GP32_TR("That key is reserved for emulator commands. Choose another key.",
                                                               "에뮬레이터 단축키로 쓰는 키입니다. 다른 키를 누르세요."));
                    return 0;
                }
                /* Swap duplicate assignments, keeping every control usable. */
                for (int i = 0; i < GP32_KEY_COUNT; ++i)
                    if (i != d->capturing && d->pending.keys[i] == key)
                        d->pending.keys[i] = d->pending.keys[d->capturing];
                d->pending.keys[d->capturing] = key;
            }
            d->capturing = -1;
            key_labels(dlg, d);
            SetDlgItemTextA(dlg, IDC_KEY_HINT, keyboard_hint());
            return 0;
        }
    }
    return CallWindowProcA(d->button_proc, button, msg, wp, lp);
}

static INT_PTR CALLBACK keyboard_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    keyboard_dialog_t *d = (keyboard_dialog_t *)GetWindowLongPtrA(dlg, DWLP_USER);
    if (msg == WM_INITDIALOG) {
        d = (keyboard_dialog_t *)lp;
        SetWindowLongPtrA(dlg, DWLP_USER, (LONG_PTR)d);
        for (int i = 0; i < GP32_KEY_COUNT; ++i)
            d->button_proc = (WNDPROC)SetWindowLongPtrA(GetDlgItem(dlg, IDC_KEY_FIRST + i), GWLP_WNDPROC, (LONG_PTR)capture_key);
        SetWindowTextA(dlg, GP32_TR("Keyboard Controls", "키보드 설정"));
        for (int i = 0; i < GP32_KEY_COUNT; ++i)
            SetDlgItemTextA(dlg, IDC_KEY_LABEL_FIRST + i, GP32_TR(key_names[i], key_labels_ko[i]));
        SetDlgItemTextA(dlg, IDC_KEY_HINT, keyboard_hint());
        SetDlgItemTextA(dlg, IDC_KEY_DEFAULTS, GP32_TR("Defaults", "기본값"));
        SetDlgItemTextA(dlg, IDOK, GP32_TR("OK", "확인"));
        SetDlgItemTextA(dlg, IDCANCEL, GP32_TR("Cancel", "취소"));
        key_labels(dlg, d);
        return TRUE;
    }
    if (msg == WM_COMMAND && d) {
        int id = LOWORD(wp);
        if (id >= IDC_KEY_FIRST && id < IDC_KEY_FIRST + GP32_KEY_COUNT) {
            key_labels(dlg, d);
            d->capturing = id - IDC_KEY_FIRST;
            SetDlgItemTextA(dlg, id, GP32_TR("Press a key...", "키를 누르세요..."));
            SetDlgItemTextA(dlg, IDC_KEY_HINT, GP32_TR("Esc cancels. F5-F12, Tab and Alt are emulator shortcuts and cannot be used.",
                                                       "Esc는 취소입니다. F5~F12, Tab, Alt는 에뮬레이터 단축키라 쓸 수 없습니다."));
            SetFocus(GetDlgItem(dlg, id));
        } else if (id == IDC_KEY_DEFAULTS) {
            d->capturing = -1;
            memcpy(d->pending.keys, default_keys, sizeof(default_keys));
            key_labels(dlg, d);
            SetDlgItemTextA(dlg, IDC_KEY_HINT, keyboard_hint());
        } else if (id == IDOK || id == IDCANCEL) {
            if (id == IDOK) *d->target = d->pending;
            EndDialog(dlg, id);
        }
        return TRUE;
    }
    return FALSE;
}

int gp32_win64_keyboard_dialog(HWND owner, HINSTANCE inst, gp32_win64_preferences_t *p) {
    keyboard_dialog_t d = { .pending = *p, .target = p, .capturing = -1 };
    return DialogBoxParamA(inst, MAKEINTRESOURCEA(IDD_KEYBOARD), owner, keyboard_proc, (LPARAM)&d) == IDOK;
}

typedef struct library_dialog {
    gp32_win64_preferences_t *prefs;
    char *selected;
} library_dialog_t;

static void library_refresh(HWND dlg, library_dialog_t *d) {
    HWND list = GetDlgItem(dlg, IDC_GAME_LIST);
    SendMessageA(list, LB_RESETCONTENT, 0, 0);
    EnableWindow(GetDlgItem(dlg, IDOK), FALSE);
    const char *folder = d->prefs->game_folder;
    SetDlgItemTextA(dlg, IDC_GAME_FOLDER, folder);
    if (!folder[0]) {
        SetDlgItemTextA(dlg, IDC_GAME_HINT, GP32_TR("Choose a folder containing SMC, FXE or FPK games.",
                                                    "SMC, FXE, FPK 게임이 들어 있는 폴더를 고르세요."));
        return;
    }
    char pattern[MAX_PATH];
    if (snprintf(pattern, sizeof(pattern), "%s\\*", folder) >= (int)sizeof(pattern)) {
        SetDlgItemTextA(dlg, IDC_GAME_HINT, GP32_TR("Folder path is too long.", "폴더 경로가 너무 깁니다."));
        return;
    }
    WIN32_FIND_DATAA entry;
    HANDLE find = FindFirstFileA(pattern, &entry);
    if (find == INVALID_HANDLE_VALUE) {
        SetDlgItemTextA(dlg, IDC_GAME_HINT, GP32_TR("Cannot read this folder. Check its location and permissions.",
                                                    "이 폴더를 읽을 수 없습니다. 위치와 권한을 확인하세요."));
        return;
    }
    int skipped = 0;
    do {
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!gp32_win64_is_game(entry.cFileName)) continue;
        if (strlen(folder) + 1 + strlen(entry.cFileName) >= MAX_PATH) { skipped++; continue; }
        if (SendMessageA(list, LB_ADDSTRING, 0, (LPARAM)entry.cFileName) < 0) { skipped++; break; }
    } while (FindNextFileA(find, &entry));
    DWORD error = GetLastError();
    FindClose(find);
    LRESULT count = SendMessageA(list, LB_GETCOUNT, 0, 0);
    if (count > 0) {
        SendMessageA(list, LB_SETCURSEL, 0, 0);
        EnableWindow(GetDlgItem(dlg, IDOK), TRUE);
    }
    char hint[192];
    if (skipped || error != ERROR_NO_MORE_FILES)
        snprintf(hint, sizeof(hint), "%s", GP32_TR("Some entries could not be listed (path length or folder access).",
                                                  "일부 파일은 표시하지 못했습니다 (경로 길이나 폴더 권한)."));
    else if (!count)
        snprintf(hint, sizeof(hint), "%s", GP32_TR("No games here. Subfolders, ZIPs and save files are not listed.",
                                                  "게임이 없습니다. 하위 폴더, ZIP, 저장 파일은 표시하지 않습니다."));
    else
        snprintf(hint, sizeof(hint), GP32_TR("%d games. Double-click to play.", "게임 %d개. 두 번 클릭하면 실행합니다."), (int)count);
    SetDlgItemTextA(dlg, IDC_GAME_HINT, hint);
}

static int CALLBACK browse_init(HWND hwnd, UINT msg, LPARAM lp, LPARAM data) {
    (void)lp;
    if (msg == BFFM_INITIALIZED && data && *(const char *)data)
        SendMessageA(hwnd, BFFM_SETSELECTIONA, TRUE, data);
    return 0;
}

static INT_PTR CALLBACK library_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    library_dialog_t *d = (library_dialog_t *)GetWindowLongPtrA(dlg, DWLP_USER);
    if (msg == WM_INITDIALOG) {
        d = (library_dialog_t *)lp;
        SetWindowLongPtrA(dlg, DWLP_USER, (LONG_PTR)d);
        SetWindowTextA(dlg, GP32_TR("Game Library", "게임 목록"));
        SetDlgItemTextA(dlg, IDC_GAME_BROWSE, GP32_TR("Choose folder...", "폴더 선택..."));
        SetDlgItemTextA(dlg, IDC_GAME_REFRESH, GP32_TR("Refresh", "새로 고침"));
        SetDlgItemTextA(dlg, IDOK, GP32_TR("Play", "실행"));
        SetDlgItemTextA(dlg, IDCANCEL, GP32_TR("Close", "닫기"));
        library_refresh(dlg, d);
        /* Start in the list so arrow keys and Enter pick a game at once. */
        SetFocus(GetDlgItem(dlg, IDC_GAME_LIST));
        return FALSE;
    }
    if (msg == WM_COMMAND && d) {
        int id = LOWORD(wp);
        if (id == IDC_GAME_BROWSE) {
            HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
            BROWSEINFOA info = {0};
            info.hwndOwner = dlg;
            info.lpszTitle = GP32_TR("Choose your GP32 game folder", "GP32 게임 폴더를 고르세요");
            info.ulFlags = BIF_RETURNONLYFSDIRS | (SUCCEEDED(hr) ? BIF_NEWDIALOGSTYLE : 0);
            info.lpfn = browse_init;
            info.lParam = (LPARAM)d->prefs->game_folder;
            PIDLIST_ABSOLUTE item = SHBrowseForFolderA(&info);
            char folder[MAX_PATH];
            if (item && SHGetPathFromIDListA(item, folder)) {
                snprintf(d->prefs->game_folder, MAX_PATH, "%s", folder);
                library_refresh(dlg, d);
            }
            CoTaskMemFree(item);
            if (SUCCEEDED(hr)) CoUninitialize();
        } else if (id == IDC_GAME_REFRESH) {
            library_refresh(dlg, d);
        } else if (id == IDCANCEL) {
            EndDialog(dlg, IDCANCEL);
        } else if (id == IDOK || (id == IDC_GAME_LIST && HIWORD(wp) == LBN_DBLCLK)) {
            HWND list = GetDlgItem(dlg, IDC_GAME_LIST);
            LRESULT sel = SendMessageA(list, LB_GETCURSEL, 0, 0);
            if (sel == LB_ERR) return TRUE;
            char name[MAX_PATH];
            LRESULT len = SendMessageA(list, LB_GETTEXTLEN, sel, 0);
            if (len < 0 || len >= MAX_PATH) return TRUE;
            SendMessageA(list, LB_GETTEXT, sel, (LPARAM)name);
            if (snprintf(d->selected, MAX_PATH, "%s\\%s", d->prefs->game_folder, name) >= MAX_PATH) return TRUE;
            EndDialog(dlg, IDOK);
        }
        return TRUE;
    }
    return FALSE;
}

int gp32_win64_library_dialog(HWND owner, HINSTANCE inst, gp32_win64_preferences_t *p,
                              char path[MAX_PATH]) {
    library_dialog_t d = { .prefs = p, .selected = path };
    path[0] = 0;
    return DialogBoxParamA(inst, MAKEINTRESOURCEA(IDD_LIBRARY), owner, library_proc, (LPARAM)&d) == IDOK;
}
