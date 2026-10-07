#ifndef GP32EMU_S3C2400_H
#define GP32EMU_S3C2400_H

#include "common.h"
#include "smartmedia.h"
#include "arm920t.h"

typedef struct s3c2400 s3c2400_t;

typedef void (*s3c2400_log_fn)(void *user, const char *line);

s3c2400_t *s3c2400_create(size_t ram_size);
void s3c2400_destroy(s3c2400_t *soc);
void s3c2400_reset(s3c2400_t *soc);
arm_bus_t s3c2400_get_bus(s3c2400_t *soc);
/* CPU-facing live GPIO readback: the four polled words of the SmartMedia bit
 * banging window (GPBCON 0x15600008, GPBDAT 0x1560000c, GPCDAT 0x15600024,
 * GPEDAT 0x15600030) as stable, immutable SoC-owned descriptors. Each mirror
 * always equals the ordinary GPIO read, at every CPU-resume boundary and after
 * every store; the storage stays valid until s3c2400_destroy. Returns NULL
 * with *count == 0 when unavailable. */
const arm_live_read32_t *s3c2400_live_read32(const s3c2400_t *soc, size_t *count);

int s3c2400_load_bios(s3c2400_t *soc, const char *path, char *err, size_t err_len);
int s3c2400_load_bios_buffer(s3c2400_t *soc, const uint8_t *data, size_t size, char *err, size_t err_len);
/* Fixed RAM table the retail firmware's IRQ dispatcher indexes by INTOFFSET.
 * The firmware's "install IRQ handler" service (SWI 9) writes it and its
 * dispatcher (ROM 0x8c) calls the entry; direct mode publishes the same
 * contract so SDK software that installs handlers keeps working. */
#define S3C2400_HLE_ISR_TABLE_ADDR 0x0c7ac000u
void s3c2400_install_hle_bios(s3c2400_t *soc);
int s3c2400_load_smartmedia(s3c2400_t *soc, const char *path, char *err, size_t err_len);
int s3c2400_load_smartmedia_buffer(s3c2400_t *soc, const uint8_t *data, size_t size, char *err, size_t err_len);
/* Mount a persisted card image over the state base the previous mount set (see
 * smartmedia.h), or set that base without mounting anything. */
int s3c2400_load_smartmedia_over_base(s3c2400_t *soc, const char *path, char *err, size_t err_len);
int s3c2400_load_smartmedia_buffer_over_base(s3c2400_t *soc, const uint8_t *data, size_t size, char *err, size_t err_len);
int s3c2400_set_smartmedia_state_base(s3c2400_t *soc, const uint8_t *data, size_t size, char *err, size_t err_len);
int s3c2400_set_smartmedia_state_base_file(s3c2400_t *soc, const char *path, char *err, size_t err_len);
int s3c2400_save_smartmedia(s3c2400_t *soc, const char *path, char *err, size_t err_len);
int s3c2400_load_ram_image(s3c2400_t *soc, uint32_t addr, const uint8_t *data, size_t size, char *err, size_t err_len);
size_t s3c2400_ram_size(const s3c2400_t *soc);
const uint8_t *s3c2400_ram_data(const s3c2400_t *soc);
void s3c2400_set_buttons(s3c2400_t *soc, uint32_t button_mask);
void s3c2400_set_irq_sink(s3c2400_t *soc, arm920t_t *cpu);
void s3c2400_set_log(s3c2400_t *soc, s3c2400_log_fn fn, void *user);
/* Direct firmware services share the IIC EEPROM, including its saved state.
 * Raw chip addresses wrap at 8 KiB; firmware API bounds belong to the caller. */
uint8_t s3c2400_eeprom_read8(const s3c2400_t *soc, uint32_t addr);
void s3c2400_eeprom_write8(s3c2400_t *soc, uint32_t addr, uint8_t value);

const uint32_t *s3c2400_framebuffer(s3c2400_t *soc, uint32_t *w, uint32_t *h, uint32_t *stride, uint64_t *frames);
const int16_t *s3c2400_audio_samples(s3c2400_t *soc, uint64_t *frames, uint32_t *sample_rate_hz);
void s3c2400_audio_append_u8_mono(s3c2400_t *soc, const uint8_t *samples, uint32_t sample_count, uint32_t sample_rate_hz);
void s3c2400_audio_append_s16_stereo(s3c2400_t *soc, int16_t left, int16_t right, uint32_t sample_rate_hz);
/* HLE GpControlVolume: call only after settling audio for elapsed cycles. */
void s3c2400_audio_set_volume(s3c2400_t *soc, uint32_t volume);
int s3c2400_audio_consume(s3c2400_t *soc, uint64_t frames);
void s3c2400_audio_clear(s3c2400_t *soc);
/* Capture timed silence while IIS is stopped. Disabled for standalone SoC
 * diagnostics and direct-HLE software mixers; gp32 enables it for BIOS mode. */
void s3c2400_set_audio_idle(s3c2400_t *soc, int enabled);
void s3c2400_render_lcd(s3c2400_t *soc);
/* Frame period of the live TFT programming, as period_ns + period_frac/2^20 ns.
 * Returns 0 when no panel frame clock can be derived: ENVID off, STN mode, a
 * zero divider, or a period outside the 5..500 Hz sanity window. One pixel is
 * HCLK / (2 * (CLKVAL + 1)) and one frame is the programmed total number of
 * lines, which is the source the guest polls through LINECNT and VSTATUS. */
int s3c2400_lcd_frame_period(const s3c2400_t *soc, uint32_t *period_ns, uint32_t *period_frac);
void s3c2400_tick(s3c2400_t *soc, uint32_t cpu_cycles);
/* Execute the attached CPU, yielding at clock writes and IIS DMA IRQ deadlines. */
uint32_t s3c2400_run_cpu(s3c2400_t *soc, uint32_t cpu_cycles);
uint32_t s3c2400_debug_read32(s3c2400_t *soc, uint32_t addr);
uint32_t s3c2400_fclk_hz(const s3c2400_t *soc);
uint32_t s3c2400_hclk_hz(const s3c2400_t *soc);
uint32_t s3c2400_pclk_hz(const s3c2400_t *soc);
uint32_t s3c2400_run_clock_hz(const s3c2400_t *soc);
/* Host CPU-speed option (50..400 percent of the guest-programmed run clock).
 * Peripheral time is preserved. Call only between CPU runs. */
int s3c2400_set_cpu_speed_percent(s3c2400_t *soc, uint32_t percent);
uint32_t s3c2400_cpu_speed_percent(const s3c2400_t *soc);
int s3c2400_state_save(const s3c2400_t *soc, FILE *f);
int s3c2400_state_load(s3c2400_t *soc, FILE *f);
int s3c2400_state_save_io(const s3c2400_t *soc, state_io_t *io);
int s3c2400_state_load_io(s3c2400_t *soc, state_io_t *io, int has_audio_spans, int has_iis_phase, int has_lcd_phase, int has_idle_phase, int has_codec);
/* HLE continuation validates against incoming RAM/clock. Legacy guest-wait
 * migration also requires its saved stack to fit before SoC commit. Zero
 * parameters disable the corresponding check for the compatibility wrapper. */
int s3c2400_state_load_io_checked(s3c2400_t *soc, state_io_t *io, int has_audio_spans, int has_iis_phase, int has_lcd_phase, int has_idle_phase, int has_codec,
                                  uint32_t expected_ram_size, uint32_t expected_run_clock, uint32_t required_ram_size,
                                  smc_state_format_t card_format);

uint8_t s3c2400_read8(void *user, uint32_t addr);
uint16_t s3c2400_read16(void *user, uint32_t addr);
uint32_t s3c2400_read32(void *user, uint32_t addr);
void s3c2400_write8(void *user, uint32_t addr, uint8_t value);
void s3c2400_write16(void *user, uint32_t addr, uint16_t value);
void s3c2400_write32(void *user, uint32_t addr, uint32_t value);

#endif
