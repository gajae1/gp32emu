#ifndef GP32EMU_GP32_CODEC_H
#define GP32EMU_GP32_CODEC_H

#include <stdint.h>

/* GP32 audio codec register model: NXP UDA1330A-class L3 stereo DAC (SSOP16
 * board part; UDA1340M/1341TS/1344TS decode the same byte stream). Register
 * and steady-state model only: the de-emphasis filter and soft-mute ramp
 * are not modelled, and no host mixer or host volume is touched.
 *
 * L3 transport on GPEDAT (0x15600030): GPE9 = L3CLOCK, GPE10 = L3MODE,
 * GPE11 = L3DATA. L3MODE LOW + 8 clocks = address byte (0x14 data, 0x16
 * status); L3MODE HIGH + 8 clocks = data byte. Bytes are LSB first and are
 * sampled on the L3CLOCK rising edge. MODE rising latches the last eight
 * address bits; complete data bytes latch immediately. Partial bytes are
 * discarded on mode changes.
 *
 * Programming: data byte bits 7:6 select the register (00 = VC5..VC0 volume,
 * 10 = "100 DE1 DE0 MT 0 0" de-emphasis + mute). The firmware writes
 * volume(v & 0x3F) and then the constant 0x80 (no de-emphasis, MT = 0) after
 * every volume change.
 *
 * The zeroed default state is unity gain: VC 0 is 0 dB (65536 Q16) and MT = 0,
 * so callers that never observe an L3 write keep unity gain.
 *
 * Source: NXP UDA1330ATS "Low-cost stereo filter DAC", product specification
 * 2001 Feb 02 (L3 protocol p.8/9, register tables p.11),
 * https://www.nxp.com/docs/en/data-sheet/UDA1330ATS.pdf ; volume ladder per
 * NXP UDA1341TS Table 18 (2002 May 16),
 * https://www.nxp.com/docs/en/data-sheet/UDA1341TS.pdf */

/* GPEDAT (0x15600030) bit masks for the bit-banged L3 bus. */
#define GP32_CODEC_GPE_CLOCK (UINT32_C(1) << 9)
#define GP32_CODEC_GPE_MODE  (UINT32_C(1) << 10)
#define GP32_CODEC_GPE_DATA  (UINT32_C(1) << 11)

/* L3 address bytes: bits 7:2 = device address 000101, bits 1:0 = transfer type
 * (00 = data, 10 = status). */
#define GP32_CODEC_ADDR_DATA   0x14u
#define GP32_CODEC_ADDR_STATUS 0x16u

typedef struct gp32_codec {
    uint8_t address; /* last complete L3 address byte (0x14/0x16) */
    uint8_t shift;   /* partial byte under construction, LSB first */
    uint8_t bits;    /* data count 0..7; address-window count saturates at 8 */
    uint8_t volume;  /* VC5..VC0 volume code (0..63); 62/63 = -infinity */
    uint8_t control; /* last control byte "100 DE1 DE0 MT 0 0"; MT = bit 2 */
    uint8_t status;  /* last status byte written via address 0x16 */
} gp32_codec_t;

/* Reset to the zero state (VC 0 / unity gain, no mute). */
void gp32_codec_reset(gp32_codec_t *codec);
/* Feed one GPEDAT value transition; GPE9..GPE11 only. */
void gp32_codec_gpio(gp32_codec_t *codec, uint32_t old_gpio, uint32_t new_gpio);
/* Apply one complete L3 transfer (address byte plus data byte). The latched
 * address field belongs to the GPIO byte stream; this entry point leaves it
 * unchanged and only updates the register selected by the address argument. */
void gp32_codec_data(gp32_codec_t *codec, uint8_t address, uint8_t data);
/* Steady-state linear gain in Q16 (unity 65536, mute 0). */
uint32_t gp32_codec_gain_q16(const gp32_codec_t *codec);

#endif /* GP32EMU_GP32_CODEC_H */
