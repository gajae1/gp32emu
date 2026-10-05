#ifndef GP32EMU_SMC_DIRECT_H
#define GP32EMU_SMC_DIRECT_H

#include "common.h"
#include "fxe.h"
#include "fpk.h"

typedef struct smc_direct_package {
    fxe_image_t image;
    fpk_asset_t *assets;
    size_t asset_count;
    char title[64];
    char executable_path[260];
} smc_direct_package_t;

/* Which launcher can start the card's executable. The retail BIOS launcher
 * boots a card from its top-level GAME\ directory only: measured freeware
 * cards whose executable sits in another folder (the GPMM\ layout) end in the
 * firmware's own card-scan error path, so they need the host-side direct
 * loader even when a BIOS image is available. */
typedef enum smc_card_launch_layout {
    SMC_CARD_LAYOUT_NONE = 0,        /* no launchable executable (or unreadable image) */
    SMC_CARD_LAYOUT_BIOS_GAME = 1,   /* executable inside the top-level GAME\ directory */
    SMC_CARD_LAYOUT_DIRECT_ONLY = 2  /* executable only outside GAME\ (freeware layout) */
} smc_card_launch_layout_t;

int smc_direct_load_file(const char *path, smc_direct_package_t *pkg, char *err, size_t err_len);
int smc_direct_load_buffer(const uint8_t *data, size_t size, const char *label, smc_direct_package_t *pkg, char *err, size_t err_len);
void smc_direct_package_free(smc_direct_package_t *pkg);

/* Classify a card image from its FAT layout without loading a package.
 * exe_out, when given, receives the executable the named launcher would start
 * (the top-level GAME\ executable for SMC_CARD_LAYOUT_BIOS_GAME, otherwise the
 * file the direct loader picks). Any read or parse failure returns
 * SMC_CARD_LAYOUT_NONE with a reason in err, which tells the caller to keep its
 * own boot default instead of switching paths. */
smc_card_launch_layout_t smc_direct_classify_file(const char *path, char *exe_out, size_t exe_out_len, char *err, size_t err_len);
smc_card_launch_layout_t smc_direct_classify_buffer(const uint8_t *data, size_t size, const char *label, char *exe_out, size_t exe_out_len, char *err, size_t err_len);

#endif /* GP32EMU_SMC_DIRECT_H */
