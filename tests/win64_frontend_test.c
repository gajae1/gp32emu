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

int main(int argc, char **argv) {
    const char *root = argc > 1 ? argv[1] : "win64_frontend_tmp";
    CHECK(strlen(root) < MAX_PATH - 50u);
    _mkdir(root);
    app_state_t a = {0};
    a.no_audio = 1;
    char original[MAX_PATH], second[MAX_PATH], saved[MAX_PATH], changed[MAX_PATH], dump[MAX_PATH];
    snprintf(a.bios, sizeof(a.bios), "%s/bios.bin", root);
    snprintf(original, sizeof(original), "%s/first.smc", root);
    snprintf(second, sizeof(second), "%s/second.smc", root);
    snprintf(saved, sizeof(saved), "%s.gp32.smc", original);
    snprintf(changed, sizeof(changed), "%s/changed.smc", root);
    snprintf(dump, sizeof(dump), "%s/dump.smc", root);
    remove(saved);
    CHECK(write_fixture(a.bios, 64, 0x5a));
    CHECK(write_fixture(original, 528u * 128u, 0xff));
    CHECK(write_fixture(second, 528u * 128u, 0xff));
    snprintf(a.smc, sizeof(a.smc), "%s", original);
    snprintf(a.state_path, sizeof(a.state_path), "%s/first.gp32st", root);
    CHECK(app_create_machine(&a));
    CHECK(gp32_save_state(a.emu, a.state_path) == GP32_OK);
    app_destroy_machine(&a);
    CHECK(marker(saved) == 0xff);
    char error[256];
    smc_t *s = smc_create();
    CHECK(s && smc_load_file(s, saved, error, sizeof(error)));
    smc_command_w(s, 0x80);
    smc_address_w(s, 7); smc_address_w(s, 3); smc_address_w(s, 0);
    smc_data_w(s, 0x42); smc_command_w(s, 0x10);
    CHECK(smc_save_file(s, changed, error, sizeof(error)));
    smc_destroy(s);
    CHECK(app_create_machine(&a));
    CHECK(gp32_load_smartmedia(a.emu, changed) == GP32_OK);
    /* The next selection must not redirect the old machine's pending save. */
    snprintf(a.smc, sizeof(a.smc), "%s", second);
    CHECK(app_create_machine(&a));
    CHECK(marker(saved) == 0x42 && marker(original) == 0xff);
    snprintf(a.smc, sizeof(a.smc), "%s", original);
    CHECK(app_create_machine(&a));
    CHECK(gp32_save_smartmedia(a.emu, dump) == GP32_OK && marker(dump) == 0x42);
    CHECK(gp32_load_state(a.emu, a.state_path) == GP32_OK);
    CHECK(gp32_save_smartmedia(a.emu, dump) == GP32_OK && marker(dump) == 0xff);
    app_destroy_machine(&a);
    CHECK(write_fixture(saved, 3, 0x11));
    CHECK(!app_create_machine(&a) && !a.emu && !a.running);
    FILE *bad = fopen(saved, "rb"); CHECK(bad && fgetc(bad) == 0x11); fclose(bad);

    HWND window = CreateWindowExA(0, "STATIC", "input probe", 0, 0, 0, 1, 1,
                                  NULL, NULL, GetModuleHandleA(NULL), NULL);
    CHECK(window);
    SetWindowLongPtrA(window, GWLP_USERDATA, (LONG_PTR)&a);
    wndproc(window, WM_KEYDOWN, VK_LEFT, 0);
    CHECK(a.keyboard_buttons == GP32_BUTTON_LEFT);
    wndproc(window, WM_ENTERMENULOOP, 0, 0);
    CHECK(!a.keyboard_buttons);
    wndproc(window, WM_KEYDOWN, VK_RIGHT, 0);
    CHECK(a.keyboard_buttons == GP32_BUTTON_RIGHT);
    wndproc(window, WM_KILLFOCUS, 0, 0);
    CHECK(!a.keyboard_buttons);
    DestroyWindow(window);
    puts("PASS: standalone media persistence, replacement, corrupt save and menu keys");
    return 0;
}
