/* Reuse the emulator's SMC/FAT/FXE loaders for local executable analysis.
 * Prints JSON metadata; optionally writes the selected executable payload to
 * a new file. Asset names are metadata only, never host filesystem paths. */
#include "smc_direct.h"

static void json_string(const char *s) {
    putchar('"');
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { putchar('\\'); putchar(c); }
        else if (c < 32u || c >= 127u) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

/* Which launcher starts this card: "bios_game" for a commercial GAME\\ layout
 * the retail BIOS boots, "direct_only" for a card only the host-side direct
 * loader can start (freeware GPMM\\), "none" when neither finds an
 * executable. */
static const char *launcher_name(smc_card_launch_layout_t layout) {
    switch (layout) {
    case SMC_CARD_LAYOUT_BIOS_GAME: return "bios_game";
    case SMC_CARD_LAYOUT_DIRECT_ONLY: return "direct_only";
    default: return "none";
    }
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        fputs("usage: gp32_smc_inspect image.smc [new-payload.bin]\n", stderr);
        return 2;
    }
    char err[512] = {0};
    smc_direct_package_t pkg = {0};
    if (!smc_direct_load_file(argv[1], &pkg, err, sizeof(err))) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    if (argc == 3) {
        FILE *f = fopen(argv[2], "wbx"); /* Do not overwrite any existing file. */
        if (!f) {
            fprintf(stderr, "create payload: %s\n", strerror(errno));
            smc_direct_package_free(&pkg);
            return 1;
        }
        int ok = fwrite(pkg.image.payload, 1, pkg.image.payload_size, f) == pkg.image.payload_size;
        if (fclose(f) != 0) ok = 0;
        if (!ok) {
            fputs("incomplete payload write\n", stderr);
            smc_direct_package_free(&pkg);
            return 1;
        }
    }
    fputs("{\"executable\":", stdout); json_string(pkg.executable_path);
    {
        char launcher_exe[260] = {0}, layout_err[256] = {0};
        smc_card_launch_layout_t layout = smc_direct_classify_file(argv[1], launcher_exe, sizeof(launcher_exe), layout_err, sizeof(layout_err));
        fputs(",\"launcher\":", stdout); json_string(launcher_name(layout));
        fputs(",\"launcher_executable\":", stdout); json_string(launcher_exe);
    }
    printf(",\"load_addr\":%u,\"entry_addr\":%u,\"payload_size\":%zu,"
           "\"was_fxe\":%d,\"was_b2fxec\":%d,\"was_host_decrunched\":%d,\"assets\":[",
           pkg.image.load_addr, pkg.image.entry_addr, pkg.image.payload_size,
           pkg.image.was_fxe, pkg.image.was_b2fxec, pkg.image.was_host_decrunched);
    for (size_t i = 0; i < pkg.asset_count; ++i) {
        const fpk_asset_t *a = &pkg.assets[i];
        if (i) putchar(',');
        fputs("{\"path\":", stdout); json_string(a->path);
        printf(",\"size\":%zu,\"cluster\":%u,\"attr\":%u}", a->size, a->first_cluster, a->attr);
    }
    puts("]}");
    smc_direct_package_free(&pkg);
    return ferror(stdout) ? 1 : 0;
}
