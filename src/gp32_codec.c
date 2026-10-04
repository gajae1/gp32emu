#include "gp32_codec.h"

/* Data-byte register select (address 0x14): bits 7:6 of the data byte. */
#define GP32_CODEC_SEL_VOLUME  0x00u
#define GP32_CODEC_SEL_CONTROL 0x80u
#define GP32_CODEC_VOLUME_MASK 0x3Fu

/* Control byte "100 DE1 DE0 MT 0 0": MT = bit 2 (1 = muting). DE1/DE0 are kept
 * verbatim in control; the de-emphasis filter itself is not modelled. */
#define GP32_CODEC_CONTROL_MT 0x04u

/* VC -> linear amplitude in Q16 (unity 65536), static integers only: VC 0/1 =
 * 0 dB, VC n (2..61) = 10^-((n-1)/20) rounded, VC 62/63 = -infinity. Mirrors
 * the datasheet ladder (0, 0, then -1 dB steps down to -60 dB at VC 61). */
static const uint32_t gp32_codec_gain_table[64] = {
     65536,  65536,  58409,  52057,  46396,  41350,  36854,  32846,
     29274,  26090,  23253,  20724,  18471,  16462,  14672,  13076,
     11654,  10387,   9257,   8250,   7353,   6554,   5841,   5206,
      4640,   4135,   3685,   3285,   2927,   2609,   2325,   2072,
      1847,   1646,   1467,   1308,   1165,   1039,    926,    825,
       735,    655,    584,    521,    464,    414,    369,    328,
       293,    261,    233,    207,    185,    165,    147,    131,
       117,    104,     93,     83,     74,     66,      0,      0,
};

void gp32_codec_reset(gp32_codec_t *codec) {
    if (!codec) return;
    *codec = (gp32_codec_t){0};
}

void gp32_codec_gpio(gp32_codec_t *codec, uint32_t old_gpio, uint32_t new_gpio) {
    const uint32_t old_clock = old_gpio & GP32_CODEC_GPE_CLOCK;
    const uint32_t new_clock = new_gpio & GP32_CODEC_GPE_CLOCK;
    const uint32_t old_mode = old_gpio & GP32_CODEC_GPE_MODE;
    const uint32_t new_mode = new_gpio & GP32_CODEC_GPE_MODE;
    if (!codec) return;

    if (old_mode != new_mode) {
        /* Address phase is a shift window, latched when MODE rises. Retail
           firmware primes CLOCK before transmitting the eight address bits;
           counting that prime as a byte bit would select 0x28 instead of 0x14. */
        if (new_mode && codec->bits == 8u) codec->address = codec->shift;
        codec->shift = 0;
        codec->bits = 0;
        return;
    }
    if (old_clock || !new_clock) return; /* sample on the L3CLOCK rising edge only */

    if (!new_mode) {
        codec->shift = (uint8_t)((codec->shift >> 1) |
            ((new_gpio & GP32_CODEC_GPE_DATA) ? 0x80u : 0u));
        if (codec->bits < 8u) ++codec->bits;
        return;
    }

    if (codec->bits >= 8u) {
        /* Out-of-range count (e.g. a foreign state file): the byte can never
         * complete, so start a fresh one. */
        codec->shift = 0;
        codec->bits = 0;
    }
    if (new_gpio & GP32_CODEC_GPE_DATA) {
        codec->shift = (uint8_t)(codec->shift | (uint8_t)(1u << codec->bits));
    }
    codec->bits++;
    if (codec->bits == 8u) {
        const uint8_t byte = codec->shift;
        codec->shift = 0;
        codec->bits = 0;
        gp32_codec_data(codec, codec->address, byte);
    }
}

void gp32_codec_data(gp32_codec_t *codec, uint8_t address, uint8_t data) {
    if (!codec) return;
    if (address == GP32_CODEC_ADDR_DATA) {
        switch (data & 0xC0u) {
        case GP32_CODEC_SEL_VOLUME:
            /* VC5..VC0; the firmware writes the low 6 bits of the volume. */
            codec->volume = (uint8_t)(data & GP32_CODEC_VOLUME_MASK);
            break;
        case GP32_CODEC_SEL_CONTROL:
            /* The firmware normalises with 0x80 (no de-emphasis, MT = 0)
             * after every volume write. */
            codec->control = data;
            break;
        default:
            /* 01 / 11 selector: not used by this model. */
            break;
        }
    } else if (address == GP32_CODEC_ADDR_STATUS) {
        /* Status register latch (reset, system clock, data format). */
        codec->status = data;
    }
    /* Unknown addresses are ignored. */
}

uint32_t gp32_codec_gain_q16(const gp32_codec_t *codec) {
    if (!codec) return 0;
    if (codec->control & GP32_CODEC_CONTROL_MT) return 0; /* MT = bit 2 */
    return gp32_codec_gain_table[codec->volume & GP32_CODEC_VOLUME_MASK];
}
