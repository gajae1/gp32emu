#ifndef GP32EMU_SMARTMEDIA_H
#define GP32EMU_SMARTMEDIA_H

#include "common.h"
#include "state_io.h"

typedef struct smc smc_t;

smc_t *smc_create(void);
void smc_destroy(smc_t *smc);
int smc_load_file(smc_t *smc, const char *path, char *err, size_t err_len);
int smc_load_buffer(smc_t *smc, const uint8_t *data, size_t size, char *err, size_t err_len);
int smc_save_file(smc_t *smc, const char *path, char *err, size_t err_len);
void smc_reset(smc_t *smc);
int smc_is_present(const smc_t *smc);
int smc_is_protected(const smc_t *smc);
int smc_is_busy(const smc_t *smc);
uint8_t smc_data_r(smc_t *smc);
void smc_command_w(smc_t *smc, uint8_t data);
void smc_address_w(smc_t *smc, uint8_t data);
void smc_data_w(smc_t *smc, uint8_t data);
size_t smc_image_size(const smc_t *smc);
int smc_is_dirty(const smc_t *smc);
int smc_save_changes(smc_t *smc, const char *path, char *err, size_t err_len);
int smc_load_changes(smc_t *smc, const char *path, char *err, size_t err_len);
uint8_t *smc_copy_image(const smc_t *smc, size_t *size);
int smc_state_save(const smc_t *smc, FILE *f);
int smc_state_load(smc_t *smc, FILE *f);

/* SmartMedia savestate section layouts.
 *
 * SMC_STATE_FORMAT_PRE_V14 is the layout every build up to savestate v0013
 * writes: the device's scalar state, the whole card image, the page register.
 * SMC_STATE_FORMAT_V14 (magic GP32STATEv0014) adds one more form: when the
 * live card differs from the frontend image in only a few pages, the section
 * carries just those pages instead of the whole 17-34 MB card. Its full form
 * is byte-identical to the older layout, so only the magic tells them apart
 * and an older full stream still loads. */
typedef enum smc_state_format {
    SMC_STATE_FORMAT_PRE_V14 = 0,
    SMC_STATE_FORMAT_V14 = 1
} smc_state_format_t;

int smc_state_save_io(const smc_t *smc, state_io_t *io);
int smc_state_load_io(smc_t *smc, state_io_t *io, smc_state_format_t format);

/* The image a v0014 delta section is expressed against.
 *
 * Immutable-content frontends keep their content as this base: every
 * later session of the same game passes that same content again, so a state
 * saved in one session still loads in a later one, even after the guest wrote
 * its save data to the persisted card. */
int smc_set_state_base(smc_t *smc, const uint8_t *image, size_t size, char *err, size_t err_len);
int smc_set_state_base_file(smc_t *smc, const char *path, char *err, size_t err_len);

/* Mount an image as the live card while keeping the base of the previous mount
 * (the persisted card supersedes the frontend content in a session, never the
 * state base). The base is dropped when the new card does not share its shape,
 * which makes this session's states carry the whole image again. */
int smc_load_buffer_over_base(smc_t *smc, const uint8_t *data, size_t size, char *err, size_t err_len);
int smc_load_file_over_base(smc_t *smc, const char *path, char *err, size_t err_len);

/* Staged section load: parse and validate the whole section against the
 * mounted card and the base, then commit it or drop it. A truncated or foreign
 * stream never touches the live card. */
typedef struct smc_state_stage smc_state_stage_t;
smc_state_stage_t *smc_state_stage_begin(smc_t *smc, state_io_t *io, smc_state_format_t format);
void smc_state_stage_commit(smc_t *smc, smc_state_stage_t *stage);
void smc_state_stage_destroy(smc_state_stage_t *stage);

#endif
