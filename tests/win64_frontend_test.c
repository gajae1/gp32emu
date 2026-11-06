/* Real Win64 frontend lifecycle, without starting audio/video devices. */
#define WinMain unused_frontend_entry
#ifndef WIN64_FRONTEND_SOURCE
#define WIN64_FRONTEND_SOURCE "../src/win64/gp32_win64_main.c"
#endif
#include WIN64_FRONTEND_SOURCE
#undef WinMain
#include "smartmedia.h"
#include <direct.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static int write_fixture(const char *path, size_t size, int value) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    for (size_t i = 0; i < size; ++i) {
        if (fputc(value, f) == EOF) { fclose(f); return 0; }
    }
    return fclose(f) == 0;
}

static int marker(const char *path) {
    char error[256];
    smc_t *s = smc_create();
    if (!s) return -1;
    int v = -1;
    if (smc_load_file(s, path, error, sizeof(error))) {
        smc_command_w(s, 0x00);
        smc_address_w(s, 7); smc_address_w(s, 3); smc_address_w(s, 0);
        v = smc_data_r(s);
    }
    smc_destroy(s);
    return v;
}

/* Exercise the real dialog resources and message handlers without user input. */
static int dialog_case, dialog_seen, dialog_ok;
static BOOL CALLBACK drive_dialog(HWND dlg, LPARAM unused) {
    (void)unused;
    if (dialog_case < 2 && GetDlgItem(dlg, IDC_KEY_FIRST)) {
        dialog_seen = 1;
        ShowWindow(dlg, SW_HIDE);
        SendMessageA(dlg, WM_COMMAND, IDC_KEY_FIRST + 4, 0);
        HWND button = GetDlgItem(dlg, IDC_KEY_FIRST + 4);
        SendMessageA(button, WM_KEYDOWN, dialog_case == 0 ? 'W' : 'E', 0);
        SendMessageA(dlg, WM_COMMAND, dialog_case == 0 ? IDOK : IDCANCEL, 0);
        dialog_ok = 1;
        return FALSE;
    }
    HWND list = GetDlgItem(dlg, IDC_GAME_LIST);
    if (dialog_case == 2 && list) {
        dialog_seen = 1;
        ShowWindow(dlg, SW_HIDE);
        LRESULT index = SendMessageA(list, LB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)"first.smc");
        LRESULT save = SendMessageA(list, LB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)"first.smc.gp32.sav");
        if (index != LB_ERR && save == LB_ERR) {
            SendMessageA(list, LB_SETCURSEL, index, 0);
            dialog_ok = 1;
            SendMessageA(dlg, WM_COMMAND, IDOK, 0);
        } else SendMessageA(dlg, WM_COMMAND, IDCANCEL, 0);
        return FALSE;
    }
    return TRUE;
}

static VOID CALLBACK dialog_timer(HWND window, UINT msg, UINT_PTR id, DWORD time) {
    (void)window; (void)msg; (void)id; (void)time;
    EnumThreadWindows(GetCurrentThreadId(), drive_dialog, 0);
}

/* No windows, devices, files or sleeps: exercise the real frontend's elapsed
 * conversion plus its existing 250 ms cap. Literal expectations are independent
 * of qpc_elapsed_us; the long-gap case overflowed before the cap. */
static int pacing_only(void) {
    static const struct {
        int64_t frequency, last, now;
        uint64_t want_us;
    } cases[] = {
        {10000000, 1000000, 1166667, 16666},
        {10000000, 0, 9, 0},
        {10000000, 0, 10, 1},
        {10000000, 0, 2499999, 249999},
        {10000000, 0, 2500000, 250000},
        {10000000, 0, 2500001, 250000},
        {3579545, 0, 894886, 249999},
        {3579545, 0, 894887, 250000},
        {1000000000, 0, 16666667, 16666},
        {10000000, 0, INT64_C(18446744073710), 250000},
        {1000000000, 0, INT64_C(18446744073710), 250000},
        {10000000, 100, 100, 0},
        {10000000, 100, 50, 0},
        {10000000, INT64_MAX - 200, INT64_MAX - 100, 10},
        {10000000, -10, 20, 3},
        {10000000, INT64_MIN, INT64_MAX, 250000},
        {1000000, 0, 16667, 16667}
    };
    unsigned failed = 0;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        app_state_t a = {0};
        LARGE_INTEGER now;
        a.qpf.QuadPart = cases[i].frequency;
        a.last_qpc.QuadPart = cases[i].last;
        now.QuadPart = cases[i].now;
        printf("pacing case %u: frequency=%lld last=%lld now=%lld\n", i,
               (long long)cases[i].frequency, (long long)cases[i].last,
               (long long)cases[i].now);
        fflush(stdout); /* Keep the failing fixture visible if UB traps. */
        uint64_t got = qpc_elapsed_us(&a, now);
        if (got > 250000u) got = 250000u; /* Same consumer cap as app_pump. */
        if (got != cases[i].want_us) {
            fprintf(stderr, "FAIL: pacing case %u: got=%llu expected=%llu us\n",
                    i, (unsigned long long)got, (unsigned long long)cases[i].want_us);
            ++failed;
        }
    }
    if (failed) return 1;
    puts("PASS: 17 device-free pacing cases; normal, backward and long-gap elapsed time");
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--pacing-only")) return pacing_only();
    const char *root = argc > 1 ? argv[1] : "win64_frontend_tmp";
    CHECK(strlen(root) < MAX_PATH - 50u);
    _mkdir(root);
    app_state_t a = {0};
    a.no_audio = 1;
    gp32_win64_preferences_load(&a.preferences, NULL);
    char original[MAX_PATH], second[MAX_PATH], saved[MAX_PATH], changed[MAX_PATH], dump[MAX_PATH];
    snprintf(a.bios, sizeof(a.bios), "%s/bios.bin", root);
    snprintf(original, sizeof(original), "%s/first.smc", root);
    snprintf(second, sizeof(second), "%s/second.smc", root);
    snprintf(saved, sizeof(saved), "%s.gp32.sav", original);
    char legacy[MAX_PATH];
    snprintf(legacy, sizeof(legacy), "%s.gp32.smc", original);
    remove(legacy);
    snprintf(changed, sizeof(changed), "%s/changed.smc", root);
    snprintf(dump, sizeof(dump), "%s/dump.smc", root);
    remove(saved);
    CHECK(write_fixture(a.bios, 64, 0x5a));
    CHECK(write_fixture(original, 528u * 128u, 0xff));
    CHECK(write_fixture(second, 528u * 128u, 0xff));
    snprintf(a.smc, sizeof(a.smc), "%s", original);
    snprintf(a.state_path, sizeof(a.state_path), "%s/first.gp32st", root);
    CHECK(app_create_machine(&a));
    gp32_t *loaded = a.emu;
    CHECK(!app_open_game(&a, saved, 0) && !strcmp(a.smc, original) && a.emu == loaded);
    snprintf(a.smc, sizeof(a.smc), "%s", saved);
    CHECK(!app_create_machine(&a) && a.emu == loaded);
    snprintf(a.smc, sizeof(a.smc), "%s", legacy);
    CHECK(!app_create_machine(&a) && a.emu == loaded);
    snprintf(a.smc, sizeof(a.smc), "%s", original);
    CHECK(gp32_save_state(a.emu, a.state_path) == GP32_OK);
    app_destroy_machine(&a);
    CHECK(GetFileAttributesA(saved) == INVALID_FILE_ATTRIBUTES); /* an unchanged card writes no save */
    CHECK(marker(original) == 0xff);
    char error[256];
    smc_t *s = smc_create();
    CHECK(s && smc_load_file(s, original, error, sizeof(error)));
    smc_command_w(s, 0x80);
    smc_address_w(s, 7); smc_address_w(s, 3); smc_address_w(s, 0);
    smc_data_w(s, 0x42); smc_command_w(s, 0x10);
    CHECK(smc_save_file(s, changed, error, sizeof(error)));
    smc_destroy(s);
    CHECK(app_create_machine(&a));
    CHECK(gp32_load_smartmedia_over_base(a.emu, changed) == GP32_OK);
    /* The next selection must not redirect the old machine's pending save. */
    snprintf(a.smc, sizeof(a.smc), "%s", second);
    CHECK(app_create_machine(&a));
    CHECK(marker(original) == 0xff && GetFileAttributesA(saved) != INVALID_FILE_ATTRIBUTES);
    snprintf(a.smc, sizeof(a.smc), "%s", original);
    CHECK(app_create_machine(&a));
    CHECK(gp32_save_smartmedia(a.emu, dump) == GP32_OK && marker(dump) == 0x42);
    CHECK(gp32_load_state(a.emu, a.state_path) == GP32_OK);
    CHECK(gp32_save_smartmedia(a.emu, dump) == GP32_OK && marker(dump) == 0xff);
    app_destroy_machine(&a);
    CHECK(remove(saved) == 0);
    CHECK(CopyFileA(changed, legacy, FALSE));
    CHECK(app_create_machine(&a));
    CHECK(!strcmp(a.smc_save, saved));
    CHECK(marker(original) == 0xff && GetFileAttributesA(legacy) != INVALID_FILE_ATTRIBUTES);
    CHECK(gp32_save_smartmedia(a.emu, dump) == GP32_OK && marker(dump) == 0x42);
    /* States and persistent page saves share the immutable original base. */
    CHECK(gp32_load_state(a.emu, a.state_path) == GP32_OK);
    CHECK(gp32_save_smartmedia(a.emu, dump) == GP32_OK && marker(dump) == 0xff);
    app_destroy_machine(&a);
    CHECK(GetFileAttributesA(saved) != INVALID_FILE_ATTRIBUTES);
    CHECK(marker(original) == 0xff);
    CHECK(GetFileAttributesA(legacy) == INVALID_FILE_ATTRIBUTES);
    CHECK(write_fixture(saved, 3, 0x11));
    CHECK(!app_create_machine(&a) && !a.emu && !a.running);
    FILE *bad = fopen(saved, "rb"); CHECK(bad && fgetc(bad) == 0x11); fclose(bad);

    HWND window = CreateWindowExA(0, "STATIC", "input probe", 0, 0, 0, 1, 1,
                                  NULL, NULL, GetModuleHandleA(NULL), NULL);
    CHECK(window);
    SetWindowLongPtrA(window, GWLP_USERDATA, (LONG_PTR)&a);
    wndproc(window, WM_KEYDOWN, VK_ESCAPE, 0);
    MSG pending;
    CHECK(!PeekMessageA(&pending, window, WM_CLOSE, WM_CLOSE, PM_REMOVE));
    wndproc(window, WM_KEYDOWN, VK_LEFT, 0);
    CHECK(a.keyboard_buttons == GP32_BUTTON_LEFT);
    wndproc(window, WM_ENTERMENULOOP, 0, 0);
    CHECK(!a.keyboard_buttons);
    wndproc(window, WM_KEYDOWN, VK_RIGHT, 0);
    CHECK(a.keyboard_buttons == GP32_BUTTON_RIGHT);
    wndproc(window, WM_KILLFOCUS, 0, 0);
    CHECK(!a.keyboard_buttons);
    a.preferences.keys[4] = 'Q';
    wndproc(window, WM_KEYDOWN, 'Z', 0);
    CHECK(!a.keyboard_buttons);
    wndproc(window, WM_KEYDOWN, 'Q', 0);
    CHECK(a.keyboard_buttons == GP32_BUTTON_A);
    wndproc(window, WM_KEYUP, 'Q', 0);
    CHECK(!a.keyboard_buttons);
    /* F-key dialogs ignore auto-repeat (bit 30) instead of stacking. */
    wndproc(window, WM_KEYDOWN, VK_F5, 1L << 30);
    CHECK(!a.keyboard_buttons);
    wndproc(window, WM_KEYDOWN, VK_F12, 1L << 30);
    CHECK(!a.keyboard_buttons);
    /* Status text is a short-lived title suffix owned by the frontend. */
    a.running = 1;
    app_set_status(&a, "State saved");
    CHECK(strstr(a.status_msg, "State saved") && a.status_active);
    SetWindowTextA(window, "GP32emu - probe");
    app_set_status(&a, "");
    CHECK(!a.status_active && !a.status_msg[0]);
    a.running = 0;
    char ini[MAX_PATH], relative_ini[MAX_PATH];
    snprintf(relative_ini, sizeof(relative_ini), "%s/preferences.ini", root);
    CHECK(GetFullPathNameA(relative_ini, MAX_PATH, ini, NULL) > 0);
    snprintf(a.preferences.game_folder, MAX_PATH, "%s", root);
    gp32_win64_preferences_save(&a.preferences, ini);
    gp32_win64_preferences_t restored;
    gp32_win64_preferences_load(&restored, ini);
    CHECK(restored.keys[4] == 'Q' && !strcmp(restored.game_folder, root));
    CHECK(restored.audio_mode == GP32_WIN64_AUDIO_WAVEOUT_ID && !strcmp(restored.video_backend, "d3d11"));
    a.preferences.audio_mode = GP32_WIN64_AUDIO_WASAPI_EXCLUSIVE_ID;
    snprintf(a.preferences.video_backend, sizeof(a.preferences.video_backend), "%s", "gdi");
    gp32_win64_preferences_save(&a.preferences, ini);
    gp32_win64_preferences_load(&restored, ini);
    CHECK(restored.audio_mode == GP32_WIN64_AUDIO_WASAPI_EXCLUSIVE_ID && !strcmp(restored.video_backend, "gdi"));
    a.preferences.audio_mode = GP32_WIN64_AUDIO_WAVEOUT_ID;
    snprintf(a.preferences.video_backend, sizeof(a.preferences.video_backend), "%s", "d3d11");
    gp32_win64_preferences_save(&a.preferences, ini);
    CHECK(gp32_win64_key_button(&restored, VK_RSHIFT) == GP32_BUTTON_SELECT);
    CHECK(gp32_win64_is_game("Title.SMC") && gp32_win64_is_game("Homebrew.FXE"));
    CHECK(!gp32_win64_is_game("Title.smc.gp32.smc.gp32.smc"));
    CHECK(!gp32_win64_is_game("Title.smc.gp32.sav"));
    CHECK(!gp32_win64_is_game("Title.zip"));
    UINT_PTR timer = SetTimer(NULL, 0, 10, dialog_timer);
    CHECK(timer);
    CHECK(gp32_win64_keyboard_dialog(window, GetModuleHandleA(NULL), &restored));
    CHECK(dialog_seen && dialog_ok && restored.keys[4] == 'W');
    dialog_case = 1; dialog_seen = dialog_ok = 0;
    CHECK(!gp32_win64_keyboard_dialog(window, GetModuleHandleA(NULL), &restored));
    CHECK(dialog_seen && dialog_ok && restored.keys[4] == 'W');
    dialog_case = 2; dialog_seen = dialog_ok = 0;
    char chosen[MAX_PATH];
    CHECK(gp32_win64_library_dialog(window, GetModuleHandleA(NULL), &restored, chosen));
    CHECK(dialog_seen && dialog_ok && !strcmp(base_name(chosen), "first.smc"));
    KillTimer(NULL, timer);
    remove(ini);
    DestroyWindow(window);
    puts("PASS: media persistence, legacy saves, nested-save rejection, keyboard settings and safe Esc");
    return 0;
}
