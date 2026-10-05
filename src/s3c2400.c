/*
 * Samsung S3C2400X GP32 SoC model. Register layout, GPIO/SmartMedia wiring,
 * LCD DMA palette format and memory map are derived from MAME gp32.cpp.
 * Original license: BSD-3-Clause, copyright Tim Schuerewegen.
 */
#include "s3c2400.h"
#include "gp32emu/gp32.h"
#include "zip.h"
#include "gp32_codec.h"
#include <stdatomic.h>
#include <assert.h>
#if defined(__aarch64__) && defined(__ARM_NEON) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#include <arm_neon.h>
#define GP32_LCD_NEON 1
#endif

#define BIOS_SIZE 0x80000u
#define RAM_BASE  0x0c000000u
#define RAM_DEFAULT_SIZE 0x00800000u
#define MPLLCON 1

#define INT_ADC       31
#define INT_RTC       30
#define INT_UTXD1     29
#define INT_UTXD0     28
#define INT_IIC       27
#define INT_USBH      26
#define INT_USBD      25
#define INT_URXD1     24
#define INT_URXD0     23
#define INT_SPI       22
#define INT_MMC       21
#define INT_DMA3      20
#define INT_DMA2      19
#define INT_DMA1      18
#define INT_DMA0      17
#define INT_TIMER4    14
#define INT_TIMER3    13
#define INT_TIMER2    12
#define INT_TIMER1    11
#define INT_TIMER0    10

#define BPPMODE_TFT_01 0x08u
#define BPPMODE_TFT_02 0x09u
#define BPPMODE_TFT_04 0x0au
#define BPPMODE_TFT_08 0x0bu
#define BPPMODE_TFT_16 0x0cu

typedef struct gp32_smc_lines {
    int add_latch;
    int chip;
    int cmd_latch;
    int do_read;
    int do_write;
    int read;
    int wp;
    int busy;
    uint8_t datarx;
    uint8_t datatx;
} gp32_smc_lines_t;

typedef struct lcd_state {
    uint32_t vramaddr_cur, vramaddr_max, offsize, pagewidth_cur, pagewidth_max;
    uint32_t bppmode, bswp, hwswp, hozval, lineval;
    uint32_t width, height;
} lcd_state_t;

typedef struct audio_boundary {
    uint64_t end_frame;
    uint32_t rate_hz;
    uint32_t reserved;
} audio_boundary_t;
static_assert(sizeof(audio_boundary_t) == 16u, "audio span wire record");

struct s3c2400 {
    uint8_t bios[BIOS_SIZE];
    uint8_t *ram;
    size_t ram_size;
    smc_t *smc;
    arm920t_t *cpu_irq_sink;
    uint32_t buttons;
    /* Active-low GPIO input bits derived from buttons on every assignment.
     * Host input changes once per frame while games poll GPBDAT/GPEDAT per
     * instruction, so each port read stays a single load instead of the
     * ten-branch mapping below. button_in0 feeds GPBDAT[15:8], button_in1
     * feeds GPEDAT[7:6]. Derived state; never serialized. */
    uint32_t button_in0, button_in1;
    uint32_t fb[320 * 240];
    uint32_t fb_w, fb_h;
    uint64_t frame_counter;
    uint32_t lcd_vpos;
    uint64_t lcd_line_accum;
    uint32_t lcd_cached_line;
    uint8_t lcd_line_valid;
    uint8_t lcd_timing_valid;
    uint64_t lcd_cached_frame_cycles, lcd_cached_line_cycles;
    uint32_t lcd_cached_visible, lcd_cached_total_lines;
    uint8_t eeprom[0x2000];
    uint8_t iic_data[4];
    int iic_data_index;
    uint16_t iic_address;

    uint32_t lcd_regs[0x400/4];
    uint16_t lcd_palette[0x400/2];
    uint32_t memcon[0x34/4];
    uint32_t usb_host[0x5c/4];
    uint32_t irq[0x18/4];
    uint32_t dma[0x7c/4];
    uint32_t clkpow[0x18/4];
    /* Transient CPU-run transaction; never part of the saved peripheral image. */
    uint32_t clkpow_before_run[0x18/4];
    uint8_t cpu_run_active, cpu_run_clock_written, cpu_lcd_deadline_set;
    /* STM issues at most 16 stores; the bus can split an unaligned word into
     * four byte lanes. Commit after ticking the elapsed prefix, in order. */
    struct { uint32_t addr, value, mask; } cpu_io_writes[16 * 4];
    unsigned cpu_io_write_count;
    uint32_t uart0[0x2c/4];
    uint32_t uart1[0x2c/4];
    uint32_t pwm[0x44/4];
    uint64_t pwm_accum[5];
    uint64_t pwm_dec_cycles[5];
    uint64_t pwm_period_cycles[5];
    uint8_t pwm_clock_dirty;
    uint32_t usb_dev[0xbc/4];
    uint32_t watchdog[0x0c/4];
    uint32_t iic[0x10/4];
    uint32_t iis[0x14/4];
    uint16_t iis_fifo[2];
    unsigned iis_fifo_index;
    uint64_t iis_accum; /* sample phase: CPU cycles * advertised sample rate */
    uint32_t iis_cached_run_hz; /* derived; not serialized */
    gp32_codec_t codec;
    uint32_t codec_gain_q16; /* derived from codec registers */
    int16_t *audio;
    uint64_t audio_frames;
    uint64_t audio_cap_frames;
    uint64_t audio_read_frames;
    audio_boundary_t *audio_boundaries;
    uint32_t audio_boundary_count, audio_boundary_head, audio_boundary_cap;
    uint32_t audio_sample_rate_hz;
    uint32_t iis_cached_rate_hz;
    uint64_t iis_cached_period_cycles;
    uint8_t iis_clock_dirty;
    uint32_t gpio[0x60/4];
    uint32_t rtc[0x4c/4];
    uint32_t adc[0x08/4];
    uint32_t spi[0x18/4];
    uint32_t mmc[0x40/4];
    lcd_state_t lcd;
    gp32_smc_lines_t smc_lines;
    s3c2400_log_fn log;
    void *log_user;
    /* Live GPIO readback mirrors and their CPU-facing descriptors (GPBDAT
     * 0x1560000c, GPEDAT 0x15600030). Appended last so every earlier field
     * keeps its offset. Derived state: never serialized, refreshed after
     * every mutation, storage stable until s3c2400_destroy. */
    volatile uint32_t live_gpbdat, live_gpedat;
    arm_live_read32_t live_read32[2];
    uint32_t lcd_hclk_remainder; /* fractional HCLK, denominator RUN clock */
    uint64_t audio_idle_phase; /* 44100-Hz silence fraction, denominator RUN */
    int audio_idle_enabled; /* host policy; direct HLE has its own PCM source */
    uint32_t cached_fclk_hz, cached_hclk_hz, cached_run_hz;
    /* Host option: guest instructions per emulated second, in percent of the
     * register-derived run clock. Peripheral clocks are unchanged. 0 = 100. */
    uint32_t cpu_speed_percent;
};

static uint8_t *s3c2400_fastmem(void *user, uint32_t addr, size_t bytes, int write);
static uint32_t s3c2400_read32_io(void *user, uint32_t addr);
static void s3c2400_write32_io(void *user, uint32_t addr, uint32_t value);
static void io_write32(s3c2400_t *s, uint32_t addr, uint32_t value, uint32_t mask);
static uint32_t lcd_current_line_count(s3c2400_t *s);
static uint32_t lcd_current_status(s3c2400_t *s);
static int lcd_is_tft(const uint32_t *regs);
static uint64_t lcd_panel_frame_cycles(s3c2400_t *s);
static uint32_t clk_fclk(const s3c2400_t *s, int reg);
static uint32_t clk_hclk(const s3c2400_t *s, int reg);
static uint32_t clk_run(const s3c2400_t *s, int reg);
static uint32_t clk_pclk(const s3c2400_t *s, int reg);
static void clock_refresh_values(s3c2400_t *s);
static void clock_write(s3c2400_t *s, uint32_t addr, uint32_t value, uint32_t mask);
static void check_irq(s3c2400_t *s);
static void color16_lut_build(void);

static int s3c2400_is_stable_read32(void *user, uint32_t addr) {
    const s3c2400_t *s = (const s3c2400_t *)user;
    /* DMA, LCD scan position and IRQs advance only after arm920t_run returns.
     * Both GPIO read paths only compose gpio[], cached buttons and SMC line/
     * presence/protection fields. NAND advances in gp32_smc_update on WRITES,
     * never a GPIO read. Inputs/card/state change outside the synchronous run;
     * DMA advances on tick. A write-free loop can therefore reuse all aligned
     * GPIO words for this run. CPU poll proofs reject peripheral stores.
     * Other MMIO is deliberately excluded: reads can acknowledge hardware. */
    return addr <= BIOS_SIZE - 4u ||
           (addr >= RAM_BASE && (uint64_t)(addr - RAM_BASE) + 4u <= s->ram_size) ||
           addr == 0x14a00000u ||
           (!(addr & 3u) && addr >= 0x15600000u && addr <= 0x15600058u);
}

static void slog(s3c2400_t *s, const char *fmt, ...) {
    if (!s || !s->log) return;
    char buf[256];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    s->log(s->log_user, buf);
}

/* Guest-visible GPBDAT/GPEDAT composition: the single source for ordinary
 * reads, the specialized read32_io path and the live readback mirrors. */
static uint32_t gp32_gpbdat_readback(const s3c2400_t *s) {
    return (s->gpio[0x0cu >> 2] & ~0xffffu) | s->smc_lines.datarx | (s->button_in0 & 0xff00u);
}
static uint32_t gp32_gpedat_readback(const s3c2400_t *s) {
    uint32_t data = s->gpio[0x30u >> 2] & ~0xfcu;
    if (s->smc_lines.cmd_latch) data |= 0x20u;
    if (s->smc_lines.add_latch) data |= 0x10u;
    if (!s->smc_lines.do_write) data |= 0x08u;
    if (!smc_is_present(s->smc)) data |= 0x04u;
    return data | (s->button_in1 & 0xc0u);
}

/* Keep both live words equal to an ordinary GPIO read. Must run after every
 * mutation of gpio[], smc_lines, cached buttons or card presence: the CPU
 * loads these words instead of issuing a bus read. */
static void live_read32_refresh(s3c2400_t *s) {
    s->live_gpbdat = gp32_gpbdat_readback(s);
    s->live_gpedat = gp32_gpedat_readback(s);
}

/* Fill the SoC-owned descriptor list once, before the first reset (which
 * populates both mirrors). Addresses stay fixed until s3c2400_destroy. */
static void live_read32_init(s3c2400_t *s) {
    s->live_read32[0].pa = 0x1560000cu;
    s->live_read32[0].word = &s->live_gpbdat;
    s->live_read32[1].pa = 0x15600030u;
    s->live_read32[1].word = &s->live_gpedat;
}

/* Recompute the guest-visible GPIO input bits from the host button mask.
 * Called wherever buttons is assigned: set_buttons, reset and state load.
 * Refreshes the live GPIO mirrors too, so all three callers stay fresh. */
static void buttons_refresh(s3c2400_t *s) {
    const uint32_t b = s->buttons;
    uint32_t v = 0xffffu;
    if (b & GP32_BUTTON_LEFT)  v &= ~0x0100u;
    if (b & GP32_BUTTON_DOWN)  v &= ~0x0200u;
    if (b & GP32_BUTTON_RIGHT) v &= ~0x0400u;
    if (b & GP32_BUTTON_UP)    v &= ~0x0800u;
    if (b & GP32_BUTTON_L)     v &= ~0x1000u;
    if (b & GP32_BUTTON_B)     v &= ~0x2000u;
    if (b & GP32_BUTTON_A)     v &= ~0x4000u;
    if (b & GP32_BUTTON_R)     v &= ~0x8000u;
    s->button_in0 = v;
    v = 0xffffu;
    if (b & GP32_BUTTON_START)  v &= ~0x0040u;
    if (b & GP32_BUTTON_SELECT) v &= ~0x0080u;
    s->button_in1 = v;
    live_read32_refresh(s);
}

s3c2400_t *s3c2400_create(size_t ram_size) {
    s3c2400_t *s = (s3c2400_t *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->ram_size = ram_size ? ram_size : RAM_DEFAULT_SIZE;
    s->ram = (uint8_t *)calloc(1, s->ram_size);
    s->smc = smc_create();
    if (!s->ram || !s->smc) { s3c2400_destroy(s); return NULL; }
    color16_lut_build();
    memset(s->bios, 0xff, sizeof(s->bios));
    memset(s->eeprom, 0xff, sizeof(s->eeprom));
    s->fb_w = 240; s->fb_h = 320;
    live_read32_init(s);
    s3c2400_reset(s);
    return s;
}

void s3c2400_destroy(s3c2400_t *s) {
    if (!s) return;
    smc_destroy(s->smc);
    free(s->ram);
    free(s->audio);
    free(s->audio_boundaries);
    free(s);
}

void s3c2400_set_irq_sink(s3c2400_t *s, arm920t_t *cpu) {
    if (s) { s->cpu_irq_sink = cpu; check_irq(s); }
}
void s3c2400_set_log(s3c2400_t *s, s3c2400_log_fn fn, void *user) { if (s) { s->log = fn; s->log_user = user; } }

void s3c2400_reset(s3c2400_t *s) {
    if (!s) return;
    memset(s->ram, 0, s->ram_size);
    memset(s->lcd_regs, 0, sizeof(s->lcd_regs));
    memset(s->lcd_palette, 0, sizeof(s->lcd_palette));
    memset(s->memcon, 0, sizeof(s->memcon));
    memset(s->usb_host, 0, sizeof(s->usb_host));
    memset(s->irq, 0, sizeof(s->irq));
    memset(s->dma, 0, sizeof(s->dma));
    memset(s->clkpow, 0, sizeof(s->clkpow));
    clock_refresh_values(s);
    s->cpu_run_active = s->cpu_run_clock_written = 0;
    s->cpu_lcd_deadline_set = 0;
    s->cpu_io_write_count = 0;
    memset(s->uart0, 0, sizeof(s->uart0));
    memset(s->uart1, 0, sizeof(s->uart1));
    memset(s->pwm, 0, sizeof(s->pwm));
    memset(s->pwm_accum, 0, sizeof(s->pwm_accum));
    memset(s->usb_dev, 0, sizeof(s->usb_dev));
    memset(s->watchdog, 0, sizeof(s->watchdog));
    memset(s->iic, 0, sizeof(s->iic));
    memset(s->iic_data, 0, sizeof(s->iic_data));
    s->iic_data_index = 0;
    s->iic_address = 0;
    memset(s->iis, 0, sizeof(s->iis));
    memset(s->iis_fifo, 0, sizeof(s->iis_fifo));
    gp32_codec_reset(&s->codec);
    s->codec_gain_q16 = gp32_codec_gain_q16(&s->codec);
    s->iis_fifo_index = 0;
    s->iis_accum = 0;
    s->audio_frames = s->audio_read_frames = 0;
    s->audio_boundary_head = s->audio_boundary_count = 0;
    s->audio_sample_rate_hz = 44100u;
    s->iis_cached_rate_hz = 44100u;
    s->iis_cached_period_cycles = 0;
    s->iis_clock_dirty = 1;
    s->pwm_clock_dirty = 1;
    memset(s->gpio, 0, sizeof(s->gpio));
    memset(s->rtc, 0, sizeof(s->rtc));
    memset(s->adc, 0, sizeof(s->adc));
    memset(s->spi, 0, sizeof(s->spi));
    memset(s->mmc, 0, sizeof(s->mmc));
    memset(&s->smc_lines, 0, sizeof(s->smc_lines));
    smc_reset(s->smc);
    s->fb_w = 240; s->fb_h = 320;
    memset(s->fb, 0, sizeof(s->fb));
    s->lcd_vpos = 0;
    s->lcd_line_accum = 0;
    s->lcd_hclk_remainder = 0;
    s->audio_idle_phase = 0;
    s->lcd_line_valid = 0;
    s->lcd_timing_valid = 0;
    /* buttons survives reset as host-owned input; keep the derived port
     * bits in lockstep (also covers the calloc'ed create path). */
    buttons_refresh(s);
    check_irq(s);
}

arm_bus_t s3c2400_get_bus(s3c2400_t *s) {
    arm_bus_t b;
    b.read8 = s3c2400_read8;
    b.read16 = s3c2400_read16;
    b.read32 = s3c2400_read32;
    b.read32_io = s3c2400_read32_io;
    b.write8 = s3c2400_write8;
    b.write16 = s3c2400_write16;
    b.write32 = s3c2400_write32;
    b.write32_io = s3c2400_write32_io;
    b.fastmem = s3c2400_fastmem;
    b.user = s;
    b.is_stable_read32 = s3c2400_is_stable_read32;
    return b;
}

const arm_live_read32_t *s3c2400_live_read32(const s3c2400_t *s, size_t *count) {
    if (count) *count = 0u;
    if (!s || !s->live_read32[0].word) return NULL;
    if (count) *count = GP32_ARRAY_COUNT(s->live_read32);
    return s->live_read32;
}

static int load_file_exact_or_less(uint8_t *dst, size_t cap, const char *path, char *err, size_t err_len) {
    if (gp32_zip_path_maybe(path)) {
        uint8_t *buf = NULL;
        size_t n = 0;
        char entry_name[260] = {0};
        static const char * const exts[] = { ".bin", ".rom", ".bios" };
        if (!gp32_zip_read_first_matching(path, exts, GP32_ARRAY_COUNT(exts), &buf, &n, entry_name, sizeof(entry_name), err, err_len)) return 0;
        if (n > cap) { if (err && err_len) snprintf(err, err_len, "%s:%s is too large", path, entry_name); free(buf); return 0; }
        memset(dst, 0xff, cap);
        memcpy(dst, buf, n);
        free(buf);
        return 1;
    }
    FILE *f = fopen(path, "rb");
    if (!f) { if (err && err_len) snprintf(err, err_len, "open %s: %s", path, strerror(errno)); return 0; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long n = ftell(f); rewind(f);
    if (n < 0 || (size_t)n > cap) { if (err && err_len) snprintf(err, err_len, "%s is too large", path); fclose(f); return 0; }
    memset(dst, 0xff, cap);
    if (fread(dst, 1, (size_t)n, f) != (size_t)n) { if (err && err_len) snprintf(err, err_len, "read %s failed", path); fclose(f); return 0; }
    fclose(f); return 1;
}
int s3c2400_load_bios(s3c2400_t *s, const char *path, char *err, size_t err_len) { return s && path && load_file_exact_or_less(s->bios, BIOS_SIZE, path, err, err_len); }
int s3c2400_load_bios_buffer(s3c2400_t *s, const uint8_t *data, size_t size, char *err, size_t err_len) {
    if (!s || !data || !size) { if (err && err_len) snprintf(err, err_len, "invalid BIOS buffer"); return 0; }
    if (size > BIOS_SIZE) { if (err && err_len) snprintf(err, err_len, "BIOS buffer is too large"); return 0; }
    memset(s->bios, 0xff, BIOS_SIZE);
    memcpy(s->bios, data, size);
    return 1;
}
void s3c2400_install_hle_bios(s3c2400_t *s) {
    if (!s) return;
    /* Direct-loaded FXE/GXB homebrew runs without the retail ROM, but legacy
       GPSDK/GPOS code still probes low firmware space.  An erased 0xff ROM
       turns null/firmware-probe reads into bogus callable addresses; the HLE
       firmware image uses zero-filled vectors/data so absent callbacks read as
       NULL while real BIOS boot remains controlled by s3c2400_load_bios(). */
    memset(s->bios, 0x00, BIOS_SIZE);
    /* The exception vectors are the one low-ROM region a direct-loaded guest
       executes instead of probing, and the retail ROM answers every one of them
       with firmware code: the reset/fault vectors branch into the reboot path
       (ROM 0x168), 0x14 self-loops, and the SWI vector dispatches to the
       firmware services (ROM 0xf0), returning PC for the selectors it does not
       implement.  Falling through zero words instead runs the guest through the
       erased ROM into unmapped memory, so publish the terminal equivalents: the
       entries the firmware would use to restart or fault stay parked
       (`b .`), and the SWI entry returns to the caller with the exception-return
       semantics the direct HLE declines to handle in C. */
    /* The retail ROM answers an IRQ by clearing the acknowledged source and
       calling the handler the firmware's own SWI 9 service stored in the fixed
       ISR table (ROM 0x8c, table address ROM 0x2a4); FIQ only clears and
       returns (ROM 0xd4). Direct mode has no firmware, so a guest that enables
       an interrupt and installs its handler through SWI 9 previously trapped on
       the parked vector and froze. Publish the ROM's own dispatchers at the top
       of the HLE image, byte for byte, so a direct-loaded guest sees exactly
       the interrupt path a real console runs; only the PC-relative table load
       needs its literal at the offset the copied instruction expects. */
    #define HLE_DISPATCH_IRQ_OFF 0x0007f000u
    #define HLE_DISPATCH_FIQ_OFF 0x0007f100u
    #define HLE_DISPATCH_LITERAL_OFF (HLE_DISPATCH_IRQ_OFF + 0x218u) /* 0xac's ldr r8,[pc,#0x1f0] */
    static const uint32_t direct_irq_dispatch[18] = {
        0xe24dd004u, /* sub sp, sp, #4 */
        0xe92d0380u, /* push {r7, r8, sb} */
        0xe3a08551u, /* mov r8, #0x14400000 */
        0xe598b014u, /* ldr sb, [r8, #0x14]        INTOFFSET */
        0xe3a07001u, /* mov r7, #1 */
        0xe1a07917u, /* lsl r7, r7, sb */
        0xe5887000u, /* str r7, [r8]               clear SRCPND */
        0xe5887010u, /* str r7, [r8, #0x10]        clear INTPND */
        0xe59f81f0u, /* ldr r8, [pc, #0x1f0]       ISR table base */
        0xe0888109u, /* add r8, r8, sb, lsl #2 */
        0xe5988000u, /* ldr r8, [r8]               handler */
        0xe3580000u, /* cmp r8, #0 */
        0x0a000001u, /* beq no-handler */
        0xe58d800cu, /* str r8, [sp, #0xc]         run the handler on pop */
        0xe8bd8380u, /* pop {r7, r8, sb, pc}       -> handler */
        0xe8bd0380u, /* pop {r7, r8, sb}           no handler: unwind */
        0xe28dd004u, /* add sp, sp, #4 */
        0xe25ef004u  /* subs pc, lr, #4 */
    };
    static const uint32_t direct_fiq_clear[7] = {
        0xe3a0a551u, /* mov sl, #0x14400000 */
        0xe59ac014u, /* ldr ip, [sl, #0x14] */
        0xe3a0b001u, /* mov fp, #1 */
        0xe1a0bc1bu, /* lsl fp, fp, ip */
        0xe58ab000u, /* str fp, [sl] */
        0xe58ab010u, /* str fp, [sl, #0x10] */
        0xe25ef004u  /* subs pc, lr, #4 */
    };
    for (unsigned i = 0; i < 18u; ++i) gp32_st32le(&s->bios[HLE_DISPATCH_IRQ_OFF + i * 4u], direct_irq_dispatch[i]);
    for (unsigned i = 0; i < 7u; ++i) gp32_st32le(&s->bios[HLE_DISPATCH_FIQ_OFF + i * 4u], direct_fiq_clear[i]);
    gp32_st32le(&s->bios[HLE_DISPATCH_LITERAL_OFF], S3C2400_HLE_ISR_TABLE_ADDR);
    uint32_t irq_vector = 0xea000000u | (((HLE_DISPATCH_IRQ_OFF - (0x18u + 8u)) >> 2) & 0x00ffffffu);
    uint32_t fiq_vector = 0xea000000u | (((HLE_DISPATCH_FIQ_OFF - (0x1cu + 8u)) >> 2) & 0x00ffffffu);
    static const uint32_t direct_vectors[8] = {
        0xeafffffeu, /* 0x00 reset: the ROM reboots the system; park in place */
        0xeafffffeu, /* 0x04 undefined instruction: ROM 0x04 branches to its fault/reboot path */
        0xe1b0f00eu, /* 0x08 SWI: movs pc, lr - return like an unimplemented ROM selector */
        0xeafffffeu, /* 0x0c prefetch abort: ROM 0x0c branches to its fault/reboot path */
        0xeafffffeu, /* 0x10 data abort: same fault/reboot path */
        0xeafffffeu, /* 0x14 reserved: the retail ROM self-loops at 0x14 */
        0,           /* 0x18 IRQ: firmware dispatcher copied above */
        0            /* 0x1c FIQ: firmware clear-and-return copied above */
    };
    for (unsigned i = 0; i < 8u; ++i) gp32_st32le(&s->bios[i * 4u], direct_vectors[i]);
    gp32_st32le(&s->bios[0x18u], irq_vector);
    gp32_st32le(&s->bios[0x1cu], fiq_vector);
}
int s3c2400_load_smartmedia(s3c2400_t *s, const char *path, char *err, size_t err_len) {
    if (!s) return 0;
    int ok = smc_load_file(s->smc, path, err, err_len);
    /* A failed load can still drop the mounted card; refresh either way. */
    live_read32_refresh(s);
    return ok;
}
int s3c2400_load_smartmedia_buffer(s3c2400_t *s, const uint8_t *data, size_t size, char *err, size_t err_len) {
    if (!s) return 0;
    int ok = smc_load_buffer(s->smc, data, size, err, err_len);
    live_read32_refresh(s);
    return ok;
}
int s3c2400_load_smartmedia_over_base(s3c2400_t *s, const char *path, char *err, size_t err_len) {
    if (!s) return 0;
    int ok = smc_load_file_over_base(s->smc, path, err, err_len);
    live_read32_refresh(s);
    return ok;
}
int s3c2400_load_smartmedia_buffer_over_base(s3c2400_t *s, const uint8_t *data, size_t size, char *err, size_t err_len) {
    if (!s) return 0;
    int ok = smc_load_buffer_over_base(s->smc, data, size, err, err_len);
    live_read32_refresh(s);
    return ok;
}
int s3c2400_set_smartmedia_state_base(s3c2400_t *s, const uint8_t *data, size_t size, char *err, size_t err_len) {
    return s && smc_set_state_base(s->smc, data, size, err, err_len);
}
int s3c2400_set_smartmedia_state_base_file(s3c2400_t *s, const char *path, char *err, size_t err_len) {
    return s && smc_set_state_base_file(s->smc, path, err, err_len);
}
int s3c2400_save_smartmedia(s3c2400_t *s, const char *path, char *err, size_t err_len) { return s && smc_save_file(s->smc, path, err, err_len); }

int s3c2400_load_ram_image(s3c2400_t *s, uint32_t addr, const uint8_t *data, size_t size, char *err, size_t err_len) {
    if (!s || !data) return 0;
    if (addr < RAM_BASE) { if (err && err_len) snprintf(err, err_len, "RAM image address below SDRAM"); return 0; }
    uint32_t off = addr - RAM_BASE;
    if ((size_t)off > s->ram_size || size > s->ram_size - (size_t)off) {
        if (err && err_len) snprintf(err, err_len, "RAM image too large for SDRAM");
        return 0;
    }
    memcpy(&s->ram[off], data, size);
    return 1;
}

size_t s3c2400_ram_size(const s3c2400_t *s) { return s ? s->ram_size : 0u; }

/* Identity-mapped SDRAM backing store. Callers must keep every byte they touch
 * inside s3c2400_ram_size(); a read of this array is exactly what
 * s3c2400_read32() returns for an address in RAM. */
const uint8_t *s3c2400_ram_data(const s3c2400_t *s) { return s ? s->ram : NULL; }

void s3c2400_set_buttons(s3c2400_t *s, uint32_t mask) { if (s) { s->buttons = mask; buttons_refresh(s); } }

static uint8_t expand6(uint32_t v) {
    v &= 0x3fu;
    return (uint8_t)((v << 2) | (v >> 4));
}

static uint32_t color_lcd5551(uint16_t data) {
    /* S3C2400 TFT palette entries and the GP32 16-bpp SDK color value use
       5:5:5:I: bits 15..11 red, 10..6 green, 5..1 blue, bit 0 common
       intensity.  The intensity bit is the shared LSB of each channel. */
    uint32_t i = data & 1u;
    uint8_t r = expand6((((uint32_t)data >> 11) & 0x1fu) << 1 | i);
    uint8_t g = expand6((((uint32_t)data >> 6) & 0x1fu) << 1 | i);
    uint8_t b = expand6((((uint32_t)data >> 1) & 0x1fu) << 1 | i);
    return 0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* 16-bpp scanout is direct color: each DMA halfword decodes to exactly one
 * RGBA value through the same 5:5:5:I rule used for palette entries, with no
 * dependence on any register or instance state.  Expanding that pure mapping
 * once into a shared table turns the frame loop into one load per pixel
 * instead of a per-pixel bit shuffle; the full 240x320 16-bpp frame measures
 * about 1.9x faster with byte-identical output. */
static uint32_t color16_lut[65536];
/* 0 = unbuilt, 1 = a thread is filling it, 2 = published.  The table is a pure
 * function of the pixel value, so every core shares one copy instead of paying
 * 256 KB per instance.  Concurrent s3c2400_create() calls would otherwise write
 * and read the table unsynchronized (a C data race); the CAS lets exactly one
 * thread build it while the others block on the release store, and the acquire
 * load in the fast path publishes the finished table to later creations. */
static atomic_int color16_lut_state;

static void color16_lut_build(void) {
    int expected = 0;
    if (atomic_load_explicit(&color16_lut_state, memory_order_acquire) == 2) return;
    if (atomic_compare_exchange_strong_explicit(&color16_lut_state, &expected, 1,
            memory_order_acq_rel, memory_order_acquire)) {
        for (uint32_t v = 0; v < 65536u; ++v) color16_lut[v] = color_lcd5551((uint16_t)v);
        atomic_store_explicit(&color16_lut_state, 2, memory_order_release);
    } else {
        while (atomic_load_explicit(&color16_lut_state, memory_order_acquire) != 2) {
            /* Another thread is publishing the table; wait for it. */
        }
    }
}

static uint8_t *ram_ptr(s3c2400_t *s, uint32_t addr, size_t bytes) {
    if (addr < RAM_BASE) return NULL;
    uint32_t off = addr - RAM_BASE;
    if ((size_t)off + bytes > s->ram_size) return NULL;
    return &s->ram[off];
}

static uint8_t *s3c2400_fastmem(void *user, uint32_t addr, size_t bytes, int write) {
    s3c2400_t *s = (s3c2400_t *)user;
    if (!s) return NULL;
    if (!write && (uint64_t)addr + bytes <= BIOS_SIZE) return &s->bios[addr];
    return ram_ptr(s, addr, bytes);
}

static uint32_t reg_array_read(uint32_t *a, size_t bytes, uint32_t off) {
    if ((size_t)off + 4 > bytes) return 0xffffffffu;
    return a[off >> 2];
}
static void reg_array_write(uint32_t *a, size_t bytes, uint32_t off, uint32_t value, uint32_t mask) {
    if ((size_t)off + 4 > bytes) return;
    uint32_t *r = &a[off >> 2];
    *r = (*r & ~mask) | (value & mask);
}

static uint32_t lcd_palette_read32(const s3c2400_t *s, uint32_t off) {
    if (!s || off >= 0x400u) return 0xffffffffu;
    uint32_t i = off >> 2;
    if (i >= 256u) return 0xffffffffu;
    return (uint32_t)s->lcd_palette[i];
}

static void lcd_palette_write32(s3c2400_t *s, uint32_t off, uint32_t value, uint32_t mask) {
    if (!s || off >= 0x400u) return;
    uint32_t i = off >> 2;
    if (i >= 256u) return;
    uint16_t cur = s->lcd_palette[i];
    if ((off & 2u) == 0u) {
        if (mask & 0x000000ffu) cur = (uint16_t)((cur & 0xff00u) | (value & 0x00ffu));
        if (mask & 0x0000ff00u) cur = (uint16_t)((cur & 0x00ffu) | (value & 0xff00u));
    } else {
        /* DATA[31:16] is invalid for S3C2400 palette entries. */
    }
    s->lcd_palette[i] = cur;
}

static void smc_lines_reset(gp32_smc_lines_t *m) { memset(m, 0, sizeof(*m)); }
static void gp32_smc_write(s3c2400_t *s, uint8_t data) {
    gp32_smc_lines_t *m = &s->smc_lines;
    if (m->chip && !m->read) {
        if (m->cmd_latch) smc_command_w(s->smc, data);
        else if (m->add_latch) smc_address_w(s->smc, data);
        else smc_data_w(s->smc, data);
    }
}
static uint8_t gp32_smc_read(s3c2400_t *s) { return smc_data_r(s->smc); }
static void gp32_smc_update(s3c2400_t *s) {
    gp32_smc_lines_t *m = &s->smc_lines;
    if (!m->chip) { smc_lines_reset(m); return; }
    if (m->do_write && !m->read) gp32_smc_write(s, m->datatx);
    else if (!m->do_write && m->do_read && m->read && !m->cmd_latch && !m->add_latch) m->datarx = gp32_smc_read(s);
}

/* The GPIO bit-bang driver rewrites the same latched lines millions of times
 * per frame, and gp32_smc_update is called on every one of those writes.  It
 * has an observable effect only when the card is selected and either a data
 * write is pending or a whole data-read window is open; that test is exact
 * (every other combination leaves the latched lines and the card untouched),
 * so callers may skip the call whenever it is false.  Unlike "the latched
 * bits did not change" this keeps the NAND read/write side effects, which
 * fire on every call that reaches them. */
static int gp32_smc_update_does_work(const gp32_smc_lines_t *m) {
    if (!m->chip) return 1; /* the call resets every latched line */
    if (m->do_write) return !m->read;
    return m->do_read && m->read && !m->cmd_latch && !m->add_latch;
}

static void check_irq(s3c2400_t *s) {
    if (!s->cpu_irq_sink) return;
    uint32_t pending = s->irq[0] & ~s->irq[2]; /* SRCPND masked by INTMSK */
    /* INTMOD selects the separate FIQ line. These sources bypass IRQ
     * arbitration and must not populate INTPND or INTOFFSET (manual ch.14). */
    arm920t_set_fiq(s->cpu_irq_sink, (pending & s->irq[1]) != 0u);
    pending &= ~s->irq[1];
    if (pending) {
#if defined(__GNUC__) || defined(__clang__)
        /* pending is nonzero: select the same lowest IRQ without scanning
         * every lower bit on each timer/DMA interrupt and acknowledgement. */
        unsigned n = (unsigned)__builtin_ctz(pending);
#else
        uint32_t t = pending, n = 0;
        while (!(t & 1u)) { n++; t >>= 1; }
#endif
        s->irq[4] |= (1u << n);
        s->irq[5] = n;
        arm920t_set_irq(s->cpu_irq_sink, 1);
    } else arm920t_set_irq(s->cpu_irq_sink, 0);
}
static void request_irq(s3c2400_t *s, unsigned n) {
    if (n >= 32) return;
    s->irq[0] |= (1u << n);
    check_irq(s);
}

static void iis_fifo_write16(s3c2400_t *s, uint16_t sample);
static uint32_t iis_frame_rate_hz(const s3c2400_t *s);
static uint64_t iis_period_cpu_cycles(const s3c2400_t *s);
static uint32_t iis_dma_transfers_per_frame(const s3c2400_t *s);

static void audio_append_stereo(s3c2400_t *s, int16_t left, int16_t right, uint32_t rate);
static int16_t codec_scale(int16_t sample, uint32_t gain) {
    return (int16_t)(((int32_t)sample * (int32_t)gain) / 65536);
}
void s3c2400_audio_set_volume(s3c2400_t *s, uint32_t volume) {
    if (!s) return;
    /* The firmware sends a volume byte followed by an unmute/control byte. */
    gp32_codec_data(&s->codec, GP32_CODEC_ADDR_DATA, (uint8_t)(volume & 63u));
    gp32_codec_data(&s->codec, GP32_CODEC_ADDR_DATA, 0x80u);
    s->codec_gain_q16 = gp32_codec_gain_q16(&s->codec);
}
static void iis_refresh_clock_cache(s3c2400_t *s);

/*
 * Reserve room for a whole IIS DMA batch in one growth decision.
 *
 * A whole-service transfer drains the channel's remaining transfer count (up
 * to 2^20 units, i.e. as many stereo frames) inside one
 * dma_iis_fast_trigger_count() call, so the per-sample append path re-tests
 * capacity, re-derives the audio_frames*2 index and repeats the
 * allocation-failure checks for every frame of that batch. Failed bulk
 * reservations fall back to per-sample growth, allowing a smaller allocation
 * to preserve part of the batch under memory pressure.
 */
static int audio_reserve_frames(s3c2400_t *s, uint64_t frames) {
    const uint64_t max_frames = SIZE_MAX / (2u * sizeof(int16_t));
    if (s->audio_frames > max_frames || frames > max_frames - s->audio_frames) return 0;
    uint64_t need = s->audio_frames + frames;
    if (need <= s->audio_cap_frames) return 1;
    if (s->audio_read_frames) {
        uint64_t read = s->audio_read_frames;
        s->audio_frames -= read;
        memmove(s->audio, s->audio + (size_t)read * 2u,
                (size_t)s->audio_frames * 2u * sizeof(*s->audio));
        for (uint32_t i = s->audio_boundary_head; i < s->audio_boundary_count; ++i)
            s->audio_boundaries[i].end_frame -= read;
        s->audio_read_frames = 0;
        need -= read;
        if (need <= s->audio_cap_frames) return 1;
    }
    uint64_t new_cap = s->audio_cap_frames ? s->audio_cap_frames : 65536u;
    while (new_cap < need) {
        if (new_cap > max_frames / 2u) {
            new_cap = need;
            break;
        }
        new_cap *= 2u;
    }
    if (new_cap > max_frames) return 0;
    int16_t *n = (int16_t *)realloc(s->audio, (size_t)new_cap * 2u * sizeof(int16_t));
    if (!n) return 0;
    s->audio = n;
    s->audio_cap_frames = new_cap;
    return 1;
}

/* Only rate transitions allocate metadata. Fixed-rate playback keeps its
 * contiguous PCM fast path. Call after reserving PCM, before appending it. */
static int audio_prepare_rate(s3c2400_t *s, uint32_t rate) {
    if (!rate) rate = 44100u;
    if (s->audio_frames != s->audio_read_frames && s->audio_sample_rate_hz != rate) {
        if (s->audio_boundary_count == s->audio_boundary_cap) {
            uint32_t live = s->audio_boundary_count - s->audio_boundary_head;
            if (s->audio_boundary_head) {
                memmove(s->audio_boundaries, s->audio_boundaries + s->audio_boundary_head,
                        (size_t)live * sizeof(*s->audio_boundaries));
                s->audio_boundary_count = live;
                s->audio_boundary_head = 0;
            } else {
                uint32_t cap = s->audio_boundary_cap ? s->audio_boundary_cap * 2u : 8u;
                if (cap <= s->audio_boundary_cap || cap > SIZE_MAX / sizeof(*s->audio_boundaries)) return 0;
                audio_boundary_t *next = realloc(s->audio_boundaries, (size_t)cap * sizeof(*next));
                if (!next) return 0;
                s->audio_boundaries = next;
                s->audio_boundary_cap = cap;
            }
        }
        s->audio_boundaries[s->audio_boundary_count++] =
            (audio_boundary_t){s->audio_frames, s->audio_sample_rate_hz, 0};
    }
    s->audio_sample_rate_hz = rate;
    return 1;
}

void s3c2400_set_audio_idle(s3c2400_t *s, int enabled) {
    if (s) s->audio_idle_enabled = enabled != 0;
}

/* Preserve the time of stopped hardware audio within, rather than only
 * between, frontend frames. A separate phase leaves the IIS restart timing
 * untouched. Silence uses a fixed source rate; active PCM keeps its own rate. */
static void audio_tick_idle(s3c2400_t *s, uint32_t cycles) {
    if (!s->audio_idle_enabled) return;
    uint32_t clock = s3c2400_run_clock_hz(s);
    uint64_t scaled = s->audio_idle_phase + (uint64_t)cycles * 44100u;
    uint64_t frames = scaled / clock;
    s->audio_idle_phase = scaled % clock;
    if (!frames || !audio_reserve_frames(s, frames) || !audio_prepare_rate(s, 44100u)) return;
    memset(s->audio + (size_t)s->audio_frames * 2u, 0, (size_t)frames * 2u * sizeof(*s->audio));
    s->audio_frames += frames;
}

static void dma_reload(s3c2400_t *s, int ch) {
    uint32_t *r = &s->dma[ch << 3];
    r[3] = (r[3] & ~0x000fffffu) | (r[2] & 0x000fffffu);
    r[4] = (r[4] & ~0x1fffffffu) | (r[0] & 0x1fffffffu);
    r[5] = (r[5] & ~0x1fffffffu) | (r[1] & 0x1fffffffu);
}

static uint32_t dma_iis_fast_trigger_count(s3c2400_t *s, uint32_t *r, uint32_t requests) {
    uint32_t tc = r[3] & 0x000fffffu;
    uint32_t src = r[4] & 0x1fffffffu;
    uint32_t dst = r[5] & 0x1fffffffu;
    unsigned dsz = GP32_BITS(r[2],21,20);
    int inc_src = GP32_BIT(r[0],29) == 0;
    int inc_dst = GP32_BIT(r[1],29) == 0;
    int service = GP32_BIT(r[2],26);
    if (!tc || !requests || inc_dst || dst != 0x15508010u || dsz == 0u) return 0;
    iis_refresh_clock_cache(s);
    /*
     * Units this call can move: single-service mode stops after the requests
     * handed in by the caller, whole-service mode hands the channel's whole
     * remaining transfer count to the first request.
     */
    uint32_t units = service ? tc : (requests < tc ? requests : tc);
    uint32_t completed = service ? 1u : units;
    /*
     * Batched PCM append.  One unit is one halfword for 16-bit transfers and a
     * whole stereo frame for 32-bit transfers, so the number of frames this
     * batch can produce is known up front and is committed with a single
     * capacity decision instead of one audio_append_stereo() call per frame.
     * The FIFO halfword waiting for its partner, the retained right halfword
     * and iis_fifo_index are carried in locals and published once, which
     * reproduces iis_fifo_write16() state for every batch length and starting
     * parity.  If the reservation fails, every frame falls back to
     * audio_append_stereo() itself, i.e. the unchanged per-sample path.
     */
    uint16_t pending = s->iis_fifo[0];
    uint16_t last_right = s->iis_fifo[1];
    unsigned idx = s->iis_fifo_index & 1u;
    uint32_t halfwords = (dsz == 1u) ? units : units * 2u;
    uint32_t frames = 0;
    uint32_t expected_frames = (halfwords + idx) >> 1;
    int direct = audio_reserve_frames(s, expected_frames) &&
                 (!expected_frames || audio_prepare_rate(s, s->iis_cached_rate_hz));
    uint64_t base_frames = s->audio_frames;
    int16_t *out = direct && s->audio ? s->audio + (size_t)base_frames * 2u : NULL;
    const uint32_t step = (dsz == 1u) ? 2u : 4u;
    /* Prove the whole increasing/fixed source span once. Partial RAM spans
     * and MMIO keep the per-unit checks and read side effects below. */
    const uint8_t *fp = ram_ptr(s, src, inc_src ? (size_t)units * step : (size_t)step);
    const size_t fstep = inc_src ? (size_t)step : 0u;
#define IIS_PCM_APPEND(left, right)                                    \
    do {                                                               \
        if (direct) {                                                  \
            out[0] = (int16_t)(left);                                  \
            out[1] = (int16_t)(right);                                 \
            out += 2;                                                  \
            frames++;                                                  \
        } else {                                                       \
            audio_append_stereo(s, (int16_t)(left), (int16_t)(right), s->iis_cached_rate_hz); \
        }                                                              \
        last_right = (uint16_t)(right);                                \
    } while (0)
    const uint16_t native_one = 1u;
    if (direct && fp && inc_src && !idx && dsz == 1u && units >= 2u &&
        *(const uint8_t *)&native_one == 1u) {
        /* Contiguous little-endian halfwords already have the host PCM
         * layout. Copy complete pairs and retain an odd final halfword in
         * the emulated FIFO, exactly as individual writes would do. */
        uint32_t paired = units & ~1u;
        memcpy(out, fp, (size_t)paired * 2u);
        frames = paired >> 1;
        idx = units & 1u;
        pending = gp32_ld16le(fp + (size_t)(units - (idx ? 1u : 2u)) * 2u);
        last_right = gp32_ld16le(fp + (size_t)(paired - 1u) * 2u);
        src += units * 2u;
    } else for (uint32_t i = 0; i < units; ++i) {
        const uint8_t *rp = fp ? fp + (size_t)i * fstep : ram_ptr(s, src, step);
        if (dsz == 1u) {
            uint16_t v = rp ? (uint16_t)(rp[0] | ((uint16_t)rp[1] << 8)) : s3c2400_read16(s, src);
            if (idx) {
                IIS_PCM_APPEND(pending, v);
                idx = 0;
            } else {
                pending = v;
                idx = 1;
            }
            if (inc_src) src += 2u;
        } else {
            uint32_t v = rp ? gp32_ld32le(rp) : s3c2400_read32(s, src);
            /* A word write to IISFIF pushes its upper halfword first, just
             * like the generic MMIO path. Preserve that order with a pending
             * halfword too; otherwise the fast path swaps stereo channels. */
            uint16_t first = (uint16_t)(v >> 16);
            uint16_t second = (uint16_t)v;
            if (idx) {
                IIS_PCM_APPEND(pending, first);
                pending = second;
            } else {
                IIS_PCM_APPEND(first, second);
                pending = first;
            }
            if (inc_src) src += 4u;
        }
    }
#undef IIS_PCM_APPEND
    if (direct) {
        s->audio_frames = base_frames + frames;
        /* Unity gain keeps the existing bulk copy path. Apply non-unity gain
           only to this newly queued span, never to previously queued audio. */
        if (s->codec_gain_q16 != 65536u) {
            for (uint64_t i = base_frames * 2u; i < s->audio_frames * 2u; ++i)
                s->audio[i] = codec_scale(s->audio[i], s->codec_gain_q16);
        }
        if (frames) s->audio_sample_rate_hz = s->iis_cached_rate_hz;
    }
    s->iis_fifo[0] = pending;
    s->iis_fifo[1] = last_right;
    s->iis_fifo_index = idx;
    tc -= units;
    r[4] = (r[4] & ~0x1fffffffu) | src;
    r[5] = (r[5] & ~0x1fffffffu) | dst;
    r[3] = (r[3] & ~0x000fffffu) | tc;
    if (!tc) {
        if (GP32_BIT(r[2],22)) r[6] &= ~(1u << 1);
        else dma_reload(s, 2);
        if (GP32_BIT(r[2],28)) request_irq(s, INT_DMA2);
    }
    return completed;
}

static int dma_iis_fast_trigger(s3c2400_t *s, uint32_t *r) {
    return dma_iis_fast_trigger_count(s, r, 1u) != 0u;
}

static uint32_t dma_request_iis_fast_count(s3c2400_t *s, uint32_t requests) {
    uint32_t *r = &s->dma[2 << 3];
    if (!((r[6] & 2u) && GP32_BIT(r[2], 23) && GP32_BITS(r[2], 25, 24) == 0u)) return 0;
    return dma_iis_fast_trigger_count(s, r, requests);
}

static void dma_trigger(s3c2400_t *s, int ch) {
    uint32_t *r = &s->dma[ch << 3];
    if (ch == 2 && dma_iis_fast_trigger(s, r)) return;
    uint32_t tc = r[3] & 0x000fffffu;
    uint32_t src = r[4] & 0x1fffffffu;
    uint32_t dst = r[5] & 0x1fffffffu;
    unsigned dsz = GP32_BITS(r[2],21,20);
    int inc_src = GP32_BIT(r[0],29) == 0;
    int inc_dst = GP32_BIT(r[1],29) == 0;
    int service = GP32_BIT(r[2],26);
    while (tc) {
        tc--;
        if (dsz == 0) { uint8_t v = s3c2400_read8(s, src); s3c2400_write8(s, dst, v); }
        else if (dsz == 1) { uint16_t v = s3c2400_read16(s, src); s3c2400_write16(s, dst, v); }
        else { uint32_t v = s3c2400_read32(s, src); s3c2400_write32(s, dst, v); }
        if (inc_src) src += 1u << dsz;
        if (inc_dst) dst += 1u << dsz;
        if (!service) break;
    }
    r[4] = (r[4] & ~0x1fffffffu) | src;
    r[5] = (r[5] & ~0x1fffffffu) | dst;
    r[3] = (r[3] & ~0x000fffffu) | tc;
    if (!tc) {
        if (GP32_BIT(r[2],22)) r[6] &= ~(1u << 1); /* reload bit set means no reload in MAME driver */
        else dma_reload(s, ch);
        if (GP32_BIT(r[2],28)) request_irq(s, INT_DMA0 + (unsigned)ch);
    }
}

static void dma_start(s3c2400_t *s, int ch) {
    uint32_t *r = &s->dma[ch << 3];
    dma_reload(s, ch);
    if (!GP32_BIT(r[2], 23)) dma_trigger(s, ch); /* software request only */
}


static void pwm_refresh_clock_cache(s3c2400_t *s) {
    static const uint32_t mux_table[4] = { 2u, 4u, 8u, 16u };
    static const unsigned prescaler_shift[5] = { 0, 0, 8, 8, 8 };
    static const unsigned mux_shift[5] = { 0, 4, 8, 12, 16 };
    if (!s || !s->pwm_clock_dirty) return;
    uint32_t runclk = clk_run(s, MPLLCON);
    uint32_t pclk = clk_pclk(s, MPLLCON);
    if (!runclk) runclk = 40000000u;
    if (!pclk) pclk = runclk;
    for (unsigned t = 0; t < 5u; ++t) {
        uint32_t prescaler = GP32_BITS(s->pwm[0], prescaler_shift[t] + 7u, prescaler_shift[t]);
        uint32_t mux = GP32_BITS(s->pwm[1], mux_shift[t] + 3u, mux_shift[t]);
        uint32_t div = mux < 4u ? mux_table[mux] : mux_table[3];
        uint32_t cnt = s->pwm[3u + t * 3u] & 0xffffu;
        if (cnt == 0u) cnt = 0x10000u;
        /* TCMPB sets the PWM output transition, not the counter reload/IRQ
         * period (S3C2400 manual ch.10). Keep the existing count convention. */
        uint32_t interval = cnt + 1u;
        uint64_t base_num = (uint64_t)runclk * (uint64_t)(prescaler + 1u) * (uint64_t)div;
        uint64_t dec_cycles = (base_num + (uint64_t)pclk - 1u) / (uint64_t)pclk;
        uint64_t period = (base_num * (uint64_t)interval) / (uint64_t)pclk;
        s->pwm_dec_cycles[t] = dec_cycles ? dec_cycles : 1u;
        s->pwm_period_cycles[t] = period ? period : 1u;
    }
    s->pwm_clock_dirty = 0;
}

static uint32_t pwm_current_count(s3c2400_t *s, unsigned t) {
    static const unsigned start_mask[5] = { 0x000001u, 0x000100u, 0x001000u, 0x010000u, 0x100000u };
    if (!s || t >= 5u) return 0u;
    uint32_t cnt = s->pwm[3u + t * 3u] & 0xffffu;
    if (cnt == 0u) cnt = 0x10000u;
    if (!(s->pwm[2] & start_mask[t])) return cnt;
    pwm_refresh_clock_cache(s);
    uint64_t dec_cycles = s->pwm_dec_cycles[t] ? s->pwm_dec_cycles[t] : 1u;
    uint64_t elapsed = s->pwm_accum[t] / dec_cycles;
    if (elapsed >= cnt) return 0u;
    return (uint32_t)(cnt - elapsed);
}

static uint32_t io_read32(s3c2400_t *s, uint32_t addr) {
    uint32_t off;
    if (addr >= 0x14000000u && addr <= 0x1400003bu) return reg_array_read(s->memcon, sizeof(s->memcon), addr - 0x14000000u);
    if (addr >= 0x14200000u && addr <= 0x1420005bu) return reg_array_read(s->usb_host, sizeof(s->usb_host), addr - 0x14200000u);
    if (addr >= 0x14400000u && addr <= 0x14400017u) return reg_array_read(s->irq, sizeof(s->irq), addr - 0x14400000u);
    if (addr >= 0x14600000u && addr <= 0x1460007bu) return reg_array_read(s->dma, sizeof(s->dma), addr - 0x14600000u);
    if (addr >= 0x14800000u && addr <= 0x14800017u) return reg_array_read(s->clkpow, sizeof(s->clkpow), addr - 0x14800000u);
    if (addr >= 0x14a00000u && addr <= 0x14a003ffu) {
        off = addr - 0x14a00000u;
        uint32_t data = reg_array_read(s->lcd_regs, sizeof(s->lcd_regs), off);
        if (off == 0) {
            uint32_t linecnt = (s->lcd_regs[0] & 1u) ? lcd_current_line_count(s) : 0u;
            data = (data & ~0xfffc0000u) | ((linecnt & 0x3ffu) << 18);
        }
        if (off == 0x10u) data = (data & ~(15u << 17)) | lcd_current_status(s);
        return data;
    }
    if (addr >= 0x14a00400u && addr <= 0x14a007ffu) return lcd_palette_read32(s, addr - 0x14a00400u);
    if (addr >= 0x15000000u && addr <= 0x1500002bu) {
        off = addr - 0x15000000u;
        uint32_t data = reg_array_read(s->uart0, sizeof(s->uart0), off);
        if (off == 0x10u) data = (data & ~0x06u) | 0x06u;
        return data;
    }
    if (addr >= 0x15004000u && addr <= 0x1500402bu) {
        off = addr - 0x15004000u;
        uint32_t data = reg_array_read(s->uart1, sizeof(s->uart1), off);
        if (off == 0x10u) data = (data & ~0x06u) | 0x06u;
        return data;
    }
    if (addr >= 0x15100000u && addr <= 0x15100043u) {
        off = addr - 0x15100000u;
        if (off == 0x14u) return pwm_current_count(s, 0u);
        if (off == 0x20u) return pwm_current_count(s, 1u);
        if (off == 0x2cu) return pwm_current_count(s, 2u);
        if (off == 0x38u) return pwm_current_count(s, 3u);
        if (off == 0x40u) return pwm_current_count(s, 4u);
        return reg_array_read(s->pwm, sizeof(s->pwm), off);
    }
    if (addr >= 0x15200140u && addr <= 0x152001fbu) return reg_array_read(s->usb_dev, sizeof(s->usb_dev), addr - 0x15200140u);
    if (addr >= 0x15300000u && addr <= 0x1530000bu) return reg_array_read(s->watchdog, sizeof(s->watchdog), addr - 0x15300000u);
    if (addr >= 0x15400000u && addr <= 0x1540000fu) { uint32_t data = reg_array_read(s->iic, sizeof(s->iic), addr - 0x15400000u); if ((addr & 0xff) == 0x04) data &= ~0xfu; return data; }
    if (addr >= 0x15508000u && addr <= 0x15508013u) return reg_array_read(s->iis, sizeof(s->iis), addr - 0x15508000u);
    if (addr >= 0x15600000u && addr <= 0x1560005bu) {
        off = addr - 0x15600000u;
        uint32_t data = reg_array_read(s->gpio, sizeof(s->gpio), off);
        switch (off) {
        case 0x08: data = (data & ~1u) | (!s->smc_lines.read ? 1u : 0u); break;
        case 0x0c: data = gp32_gpbdat_readback(s); break;
        case 0x24:
            data &= ~0x3c0u;
            if (!s->smc_lines.busy) data |= 0x200u;
            if (!s->smc_lines.do_read) data |= 0x100u;
            if (!s->smc_lines.chip) data |= 0x080u;
            if (!smc_is_protected(s->smc)) data |= 0x040u;
            break;
        case 0x30: data = gp32_gpedat_readback(s); break;
        }
        return data;
    }
    if (addr >= 0x15700040u && addr <= 0x1570008bu) return reg_array_read(s->rtc, sizeof(s->rtc), addr - 0x15700040u);
    if (addr >= 0x15800000u && addr <= 0x15800007u) return reg_array_read(s->adc, sizeof(s->adc), addr - 0x15800000u);
    if (addr >= 0x15900000u && addr <= 0x15900017u) return reg_array_read(s->spi, sizeof(s->spi), addr - 0x15900000u);
    if (addr >= 0x15a00000u && addr <= 0x15a0003fu) return reg_array_read(s->mmc, sizeof(s->mmc), addr - 0x15a00000u);
    return 0xffffffffu;
}

static uint32_t s3c2400_read32_io(void *user, uint32_t addr) {
    s3c2400_t *s = (s3c2400_t *)user;
    switch (addr) {
    case 0x14a00000u: {
        uint32_t data = s->lcd_regs[0];
        uint32_t linecnt = (data & 1u) ? lcd_current_line_count(s) : 0u;
        return (data & ~0xfffc0000u) | ((linecnt & 0x3ffu) << 18);
    }
    case 0x15100014u: return pwm_current_count(s, 0u);
    case 0x15100020u: return pwm_current_count(s, 1u);
    case 0x1510002cu: return pwm_current_count(s, 2u);
    case 0x15100038u: return pwm_current_count(s, 3u);
    case 0x15100040u: return pwm_current_count(s, 4u);
    case 0x15400004u: return s->iic[1] & ~0x0fu;
    case 0x15600008u: return (s->gpio[0x08u >> 2] & ~1u) | (!s->smc_lines.read ? 1u : 0u);
    case 0x1560000cu: return s->live_gpbdat;
    case 0x15600024u: {
        uint32_t data = s->gpio[0x24u >> 2] & ~0x3c0u;
        if (!s->smc_lines.busy) data |= 0x200u;
        if (!s->smc_lines.do_read) data |= 0x100u;
        if (!s->smc_lines.chip) data |= 0x080u;
        if (!smc_is_protected(s->smc)) data |= 0x040u;
        return data;
    }
    case 0x15600030u: return s->live_gpedat;
    default:
        return io_read32(s, addr);
    }
}

/* Identity-IO word store: the JIT proves the address is 4-byte aligned and
 * inside the device window before calling this, so the general entry's RAM
 * probe (RAM lives at 0x0c000000, never in 0x14000000..0x16000000) and its
 * width-alignment re-check are both already known. Everything else matches
 * s3c2400_write32 for a full-word store. */
static void s3c2400_write32_io(void *user, uint32_t addr, uint32_t value) {
    io_write32((s3c2400_t *)user, addr, value, 0xffffffffu);
}

uint8_t s3c2400_eeprom_read8(const s3c2400_t *s, uint32_t addr) {
    return s->eeprom[addr & 0x1fffu];
}

void s3c2400_eeprom_write8(s3c2400_t *s, uint32_t addr, uint8_t value) {
    s->eeprom[addr & 0x1fffu] = value;
}

static void iic_step(s3c2400_t *s) {
    unsigned mode_selection = GP32_BITS(s->iic[1], 7, 6);
    switch (mode_selection) {
    case 2: /* master receive */
        if (s->iic_data_index == 0) {
            /* first byte is the device address already in IICDS */
        } else {
            uint8_t data = s3c2400_eeprom_read8(s, s->iic_address);
            s->iic[3] = (s->iic[3] & ~0xffu) | data;
            s->iic_address = (uint16_t)((s->iic_address + 1u) & 0x1fffu);
        }
        s->iic_data_index++;
        break;
    case 3: { /* master transmit */
        uint8_t data = (uint8_t)(s->iic[3] & 0xffu);
        if (s->iic_data_index < 4) s->iic_data[s->iic_data_index] = data;
        s->iic_data_index++;
        if (s->iic_data_index == 3) s->iic_address = (uint16_t)(((uint16_t)s->iic_data[1] << 8) | s->iic_data[2]);
        else if (s->iic_data_index >= 4 && s->iic_data[0] == 0xa0) {
            s3c2400_eeprom_write8(s, s->iic_address, data);
            s->iic_address = (uint16_t)((s->iic_address + 1u) & 0x1fffu);
        }
        break;
    }
    default:
        break;
    }
    s->iic[0] |= 0x10u; /* interrupt pending flag */
    if (s->iic[0] & 0x20u) request_irq(s, INT_IIC);
}

static void iic_start(s3c2400_t *s) {
    s->iic_data_index = 0;
    iic_step(s);
}

static void defer_io_write(s3c2400_t *s, uint32_t addr, uint32_t value, uint32_t mask) {
    assert(s->cpu_io_write_count < GP32_ARRAY_COUNT(s->cpu_io_writes));
    unsigned i = s->cpu_io_write_count++;
    s->cpu_io_writes[i].addr = addr;
    s->cpu_io_writes[i].value = value;
    s->cpu_io_writes[i].mask = mask;
    arm920t_stop_run(s->cpu_irq_sink);
}

static void io_write32(s3c2400_t *s, uint32_t addr, uint32_t value, uint32_t mask) {
    uint32_t off;
    /* GPIO dominates SmartMedia bit-banging; preserve every signal update
     * while avoiding the unrelated peripheral range checks on each edge. */
    if (addr >= 0x15600000u && addr <= 0x1560005bu) {
        /* Every GPIO caller funnels aligned word offsets through here, so the
         * register word is addressed directly; the range check above already
         * keeps off inside gpio[]. */
        off = addr - 0x15600000u;
        uint32_t *reg = &s->gpio[off >> 2];
        /* Only GPEDAT carries the codec pins: take the pin snapshot, the
         * deferred-write split and the codec update on that one register so
         * ordinary SmartMedia GPBDAT/GPC/GPE writes skip all codec work. */
        if (off == 0x30u) {
            uint32_t old_gpe = *reg;
            int codec_output = (s->gpio[0x2cu >> 2] & 0xfc0000u) == 0x540000u;
            if (s->cpu_run_active && codec_output &&
                ((old_gpe ^ value) & mask & 0xe00u)) {
                defer_io_write(s, addr, value, mask);
                return;
            }
            *reg = (old_gpe & ~mask) | (value & mask);
            s->smc_lines.cmd_latch=((*reg&0x20u)!=0);
            s->smc_lines.add_latch=((*reg&0x10u)!=0);
            s->smc_lines.do_write=((*reg&0x08u)==0);
            if (gp32_smc_update_does_work(&s->smc_lines)) gp32_smc_update(s);
            if (codec_output && ((old_gpe ^ *reg) & 0xe00u)) {
                gp32_codec_gpio(&s->codec, old_gpe, *reg);
                s->codec_gain_q16 = gp32_codec_gain_q16(&s->codec);
            }
        } else {
            *reg = (*reg & ~mask) | (value & mask);
            switch(off){
            case 0x08: s->smc_lines.read = ((*reg & 1u) == 0); if (gp32_smc_update_does_work(&s->smc_lines)) gp32_smc_update(s); break;
            case 0x0c: s->smc_lines.datatx = (uint8_t)(*reg & 0xffu); break;
            case 0x24: s->smc_lines.do_read=((*reg&0x100u)==0); s->smc_lines.chip=((*reg&0x80u)==0); s->smc_lines.wp=((*reg&0x40u)==0); if (gp32_smc_update_does_work(&s->smc_lines)) gp32_smc_update(s); break;
            }
        }
        /* Every GPIO width/offset funnels here: register, NAND and latch
         * effects are already applied, so refresh both live words last. */
        live_read32_refresh(s);
        return;
    }
    if (s->cpu_run_active && ((addr >= 0x14600000u && addr <= 0x1460007bu) ||
                              (addr >= 0x14a00000u && addr <= 0x14a0000fu) ||
                              (addr >= 0x15100000u && addr <= 0x15100043u) ||
                              (addr >= 0x15508000u && addr <= 0x15508013u))) {
        /* A register change near the end of a CPU batch must not replace the
         * DMA source, timer, sample rate or LCD timing of its elapsed time.
         * Finish the current instruction, tick the old peripheral state, then
         * apply its stores. Reads in SWP/LDM precede any deferred store. */
        defer_io_write(s, addr, value, mask);
        return;
    }
    if (addr >= 0x14000000u && addr <= 0x1400003bu) { reg_array_write(s->memcon,sizeof(s->memcon),addr-0x14000000u,value,mask); return; }
    if (addr >= 0x14200000u && addr <= 0x1420005bu) { reg_array_write(s->usb_host,sizeof(s->usb_host),addr-0x14200000u,value,mask); return; }
    if (addr >= 0x14400000u && addr <= 0x14400017u) {
        off = addr - 0x14400000u; uint32_t old = reg_array_read(s->irq, sizeof(s->irq), off); reg_array_write(s->irq,sizeof(s->irq),off,value,mask);
        if (off == 0x00) s->irq[0] = old & ~value;
        else if (off == 0x10) s->irq[4] = old & ~value;
        check_irq(s);
        return;
    }
    if (addr >= 0x14600000u && addr <= 0x1460007bu) {
        off=addr-0x14600000u; uint32_t old=reg_array_read(s->dma,sizeof(s->dma),off); reg_array_write(s->dma,sizeof(s->dma),off,value,mask);
        /* DCON[22] is the reload-off option. It must not disable an
         * already-running channel when software writes DCON: hardware turns
         * the request off only when the current transfer count reaches zero.
         * Several BIOS-driven games rewrite DCON with reload-off while the
         * final audio DMA block is still draining. Clearing DMASKTRIG here
         * drops the terminal-count DMA interrupt and can stop streamed music. */
        if (off==0x18u||off==0x38u||off==0x58u||off==0x78u) {
            int ch = (int)(off >> 5);
            uint32_t *dr = &s->dma[ch << 3];
            uint32_t now = dr[6];
            if ((mask & 4u) && (now & 4u)) {
                /* DMASKTRIG[2] STOP forces the channel off after the current
                 * atomic transfer. DMA transfers are modeled atomically here,
                 * so complete the stop immediately and clear the current
                 * transfer registers as the hardware documents. */
                dr[3] = 0;
                dr[4] = 0;
                dr[5] = 0;
                dr[6] &= ~2u;
            } else if (((old ^ now) & 2u) && (now & 2u)) {
                if (!GP32_BIT(dr[2], 23)) dr[6] &= ~1u; /* SW_TRIG is self-clearing when accepted. */
                dma_start(s, ch);
            }
        }
        return;
    }
    if (addr >= 0x14800000u && addr <= 0x14800017u) {
        clock_write(s, addr, value, mask);
        return;
    }
    if (addr >= 0x14a00000u && addr <= 0x14a003ffu) {
        off = addr - 0x14a00000u;
        int was_tft = lcd_is_tft(s->lcd_regs);
        s->lcd_line_valid = 0;
        s->lcd_timing_valid = 0;
        uint32_t old = reg_array_read(s->lcd_regs, sizeof(s->lcd_regs), off);
        if (off == 0x10u) mask &= ~(15u << 17); /* status fields are read-only */
        reg_array_write(s->lcd_regs, sizeof(s->lcd_regs), off, value, mask);
        uint32_t now = reg_array_read(s->lcd_regs, sizeof(s->lcd_regs), off);
        if (off <= 0x0cu && (old != now)) {
            /* Mode changes/enable establish a new scan. Live timing writes
             * retain elapsed HCLK position, bounded by the new geometry. */
            if (was_tft != lcd_is_tft(s->lcd_regs) ||
                (off == 0u && !(old & 1u) && (now & 1u))) {
                s->lcd_line_accum = 0;
                s->lcd_hclk_remainder = 0;
            } else {
                s->lcd_line_accum %= lcd_panel_frame_cycles(s);
            }
        }
        if (off == 0u) s3c2400_render_lcd(s);
        return;
    }
    if (addr >= 0x14a00400u && addr <= 0x14a007ffu) { lcd_palette_write32(s, addr - 0x14a00400u, value, mask); return; }
    if (addr >= 0x15000000u && addr <= 0x1500002bu) { reg_array_write(s->uart0,sizeof(s->uart0),addr-0x15000000u,value,mask); return; }
    if (addr >= 0x15004000u && addr <= 0x1500402bu) { reg_array_write(s->uart1,sizeof(s->uart1),addr-0x15004000u,value,mask); return; }
    if (addr >= 0x15100000u && addr <= 0x15100043u) { reg_array_write(s->pwm,sizeof(s->pwm),addr-0x15100000u,value,mask); s->pwm_clock_dirty = 1; return; }
    if (addr >= 0x15200140u && addr <= 0x152001fbu) { reg_array_write(s->usb_dev,sizeof(s->usb_dev),addr-0x15200140u,value,mask); return; }
    if (addr >= 0x15300000u && addr <= 0x1530000bu) { reg_array_write(s->watchdog,sizeof(s->watchdog),addr-0x15300000u,value,mask); return; }
    if (addr >= 0x15400000u && addr <= 0x1540000fu) {
        off = addr - 0x15400000u;
        uint32_t old = reg_array_read(s->iic, sizeof(s->iic), off);
        reg_array_write(s->iic,sizeof(s->iic),off,value,mask);
        uint32_t now = reg_array_read(s->iic, sizeof(s->iic), off);
        if (off == 0x00u) {
            /* IICCON bit 4 is the interrupt-pending/ack bit.  Only advance
             * the EEPROM transaction when that bit is actually acknowledged.
             * Byte/halfword writes to other lanes used to look like bit 4 was
             * clear and could trigger millions of bogus iic_step() calls in
             * BIOS-driven games such as Astonishia Story R. */
            if ((mask & 0x10u) && (old & 0x10u) && !(now & 0x10u) && (s->iic[1] & 0x20u)) iic_step(s);
        } else if (off == 0x04u) {
            if (mask & 0x20u) {
                if (!(old & 0x20u) && (now & 0x20u)) iic_start(s);
                else if (!(now & 0x20u)) s->iic_data_index = 0;
                else if ((old ^ now) & 0xc0u) {
                    /* Repeated START switches transmit to receive while the
                     * bus is still busy. The pending IICCON acknowledgement
                     * clocks the new device address, not the first data byte.
                     * Retaining the transmit index skips that address phase
                     * and makes the BIOS discard EEPROM[offset] as a dummy. */
                    s->iic_data_index = 0;
                }
            }
        }
        return;
    }
    if (addr >= 0x15508000u && addr <= 0x15508013u) {
        off = addr - 0x15508000u;
        uint32_t old_rate = (off == 0x04u || off == 0x08u) ? iis_frame_rate_hz(s) : 0u;
        uint32_t old = reg_array_read(s->iis, sizeof(s->iis), off);
        reg_array_write(s->iis, sizeof(s->iis), off, value, mask);
        if (off == 0x00u && ((old ^ s->iis[0]) & 1u)) s->iis_accum = 0;
        if (off == 0x0cu && (old & ~s->iis[3] & 0x200u)) {
            /* Disabling TX FIFO flushes the pending half-frame. GP32 SDKs
             * use this when stopping PCM, before starting a fresh stream.
             * Already delivered stereo frames belong to the host queue. */
            s->iis_fifo_index = 0;
            memset(s->iis_fifo, 0, sizeof(s->iis_fifo));
        }
        if (off == 0x04u || off == 0x08u) {
            /* Retain elapsed CPU-cycle progress on live divider writes, as
             * before, but express it in the new rate's numerator units. */
            uint32_t rate = iis_frame_rate_hz(s);
            s->iis_accum = (s->iis_accum / old_rate) * rate +
                           (s->iis_accum % old_rate) * rate / old_rate;
            s->iis_clock_dirty = 1;
        }
        if (off == 0x10u) {
            if (mask & 0xffff0000u) iis_fifo_write16(s, (uint16_t)(value >> 16));
            if (mask & 0x0000ffffu) iis_fifo_write16(s, (uint16_t)value);
        }
        return;
    }
    if (addr >= 0x15700040u && addr <= 0x1570008bu) { reg_array_write(s->rtc,sizeof(s->rtc),addr-0x15700040u,value,mask); return; }
    if (addr >= 0x15800000u && addr <= 0x15800007u) { reg_array_write(s->adc,sizeof(s->adc),addr-0x15800000u,value,mask); return; }
    if (addr >= 0x15900000u && addr <= 0x15900017u) { reg_array_write(s->spi,sizeof(s->spi),addr-0x15900000u,value,mask); return; }
    if (addr >= 0x15a00000u && addr <= 0x15a0003fu) { reg_array_write(s->mmc,sizeof(s->mmc),addr-0x15a00000u,value,mask); return; }
    slog(s, "unmapped write32 %08" PRIx32 "=%08" PRIx32, addr, value);
}

uint8_t s3c2400_read8(void *user, uint32_t addr) {
    s3c2400_t *s=(s3c2400_t*)user;
    if (addr < BIOS_SIZE) return s->bios[addr];
    uint8_t *p=ram_ptr(s,addr,1); if(p) return *p;
    uint32_t a=addr&~3u, v=io_read32(s,a); return (uint8_t)(v >> ((addr&3u)*8u));
}
uint16_t s3c2400_read16(void *user, uint32_t addr) { uint16_t v=(uint16_t)s3c2400_read8(user,addr); v|=(uint16_t)s3c2400_read8(user,addr+1u)<<8; return v; }
uint32_t s3c2400_read32(void *user, uint32_t addr) {
    s3c2400_t *s=(s3c2400_t*)user;
    if (addr < BIOS_SIZE - 3u) return gp32_ld32le(&s->bios[addr]);
    uint8_t *p=ram_ptr(s,addr,4); if(p) return gp32_ld32le(p);
    if ((addr & 3u)==0) {
        if (addr == 0x14a00000u) { uint32_t data = s->lcd_regs[0]; uint32_t linecnt = (data & 1u) ? lcd_current_line_count(s) : 0u; return (data & ~0xfffc0000u) | ((linecnt & 0x3ffu) << 18); }
        return io_read32(s,addr);
    }
    return (uint32_t)s3c2400_read8(user,addr) | ((uint32_t)s3c2400_read8(user,addr+1)<<8) | ((uint32_t)s3c2400_read8(user,addr+2)<<16) | ((uint32_t)s3c2400_read8(user,addr+3)<<24);
}
void s3c2400_write8(void *user, uint32_t addr, uint8_t value) {
    s3c2400_t *s=(s3c2400_t*)user;
    uint8_t *p=ram_ptr(s,addr,1); if(p){*p=value;return;}
    uint32_t lane=(addr&3u)*8u, mask=0xffu<<lane, v=(uint32_t)value<<lane; io_write32(s,addr&~3u,v,mask);
}
void s3c2400_write16(void *user, uint32_t addr, uint16_t value) {
    s3c2400_t *s = (s3c2400_t *)user;
    uint8_t *p = ram_ptr(s, addr, 2);
    if (p) { p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8); return; }
    if ((addr & 1u) == 0u) {
        uint32_t lane = (addr & 3u) * 8u;
        uint32_t mask = 0xffffu << lane;
        uint32_t v = (uint32_t)value << lane;
        io_write32(s, addr & ~3u, v, mask);
        return;
    }
    s3c2400_write8(user, addr, (uint8_t)value);
    s3c2400_write8(user, addr + 1u, (uint8_t)(value >> 8));
}
void s3c2400_write32(void *user, uint32_t addr, uint32_t value) {
    s3c2400_t *s=(s3c2400_t*)user; uint8_t *p=ram_ptr(s,addr,4); if(p){gp32_st32le(p,value);return;} if((addr&3u)==0) io_write32(s,addr,value,0xffffffffu); else { s3c2400_write8(user,addr,(uint8_t)value); s3c2400_write8(user,addr+1,(uint8_t)(value>>8)); s3c2400_write8(user,addr+2,(uint8_t)(value>>16)); s3c2400_write8(user,addr+3,(uint8_t)(value>>24)); } }

static void lcd_dma_reload(s3c2400_t *s) {
    s->lcd.vramaddr_cur = s->lcd_regs[5] << 1;
    s->lcd.vramaddr_max = ((s->lcd_regs[5] & 0xffe00000u) | s->lcd_regs[6]) << 1;
    s->lcd.offsize = GP32_BITS(s->lcd_regs[7],21,11);
    s->lcd.pagewidth_cur = 0;
    s->lcd.pagewidth_max = GP32_BITS(s->lcd_regs[7],10,0);
}
static void lcd_dma_init(s3c2400_t *s) {
    lcd_dma_reload(s);
    s->lcd.bppmode = GP32_BITS(s->lcd_regs[0],4,1);
    s->lcd.bswp = GP32_BIT(s->lcd_regs[4],1);
    s->lcd.hwswp = GP32_BIT(s->lcd_regs[4],0);
    s->lcd.lineval = GP32_BITS(s->lcd_regs[1],23,14);
    s->lcd.hozval = GP32_BITS(s->lcd_regs[2],18,8);
    s->lcd.width = s->lcd.hozval + 1u;
    s->lcd.height = s->lcd.lineval + 1u;
    if (s->lcd.width > 240) s->lcd.width = 240;
    if (s->lcd.height > 320) s->lcd.height = 320;
}
static uint32_t lcd_dma_read(s3c2400_t *s) {
    uint8_t data[4] = {0,0,0,0};
    uint32_t cur = s->lcd.vramaddr_cur;
    uint32_t pwcur = s->lcd.pagewidth_cur;
    uint32_t pwmax = s->lcd.pagewidth_max;
    /* Fast path: the whole 32-bit DMA word lies contiguously inside RAM and
     * does not straddle a pagewidth line wrap, so one translation replaces
     * the two per-halfword ram_ptr() lookups.  Reaching pagewidth_max after
     * the second halfword still adds OFFSIZE, matching the per-halfword
     * update order exactly. */
    if ((pwmax == 0u || pwcur + 2u <= pwmax) &&
        cur >= RAM_BASE && (uint64_t)(cur - RAM_BASE) + 4u <= s->ram_size) {
        memcpy(data, &s->ram[cur - RAM_BASE], 4);
        cur += 4u;
        if (pwmax && pwcur + 2u >= pwmax) { cur += s->lcd.offsize << 1; pwcur = 0; }
        else pwcur += 2u;
        s->lcd.vramaddr_cur = cur;
        s->lcd.pagewidth_cur = pwcur;
    } else {
        for(int i=0;i<2;i++){
            uint8_t *v=ram_ptr(s,s->lcd.vramaddr_cur,2);
            if(v){data[i*2]=v[0];data[i*2+1]=v[1];}
            s->lcd.vramaddr_cur+=2; s->lcd.pagewidth_cur++;
            if(s->lcd.pagewidth_cur>=s->lcd.pagewidth_max && s->lcd.pagewidth_max){s->lcd.vramaddr_cur += s->lcd.offsize<<1; s->lcd.pagewidth_cur=0;}
        }
    }
    if(!s->lcd.hwswp) return !s->lcd.bswp ? ((uint32_t)data[3]<<24)|((uint32_t)data[2]<<16)|((uint32_t)data[1]<<8)|data[0] : ((uint32_t)data[0]<<24)|((uint32_t)data[1]<<16)|((uint32_t)data[2]<<8)|data[3];
    return !s->lcd.bswp ? ((uint32_t)data[1]<<24)|((uint32_t)data[0]<<16)|((uint32_t)data[3]<<8)|data[2] : ((uint32_t)data[2]<<24)|((uint32_t)data[3]<<16)|((uint32_t)data[0]<<8)|data[1];
}
static uint32_t pal(s3c2400_t *s, uint32_t i){
    /* Indexed scanout resolves each palette entry through the same pure
     * 5:5:5:I mapping as direct 16-bpp pixels, so the shared table serves
     * both.  The stored halfword stays the decode input, so no palette state
     * is cached here and palette writes need no invalidation. */
    return color16_lut[s->lcd_palette[i & 0xffu]];
}

/* 16-bpp scanout: each DMA word carries exactly two pixels.  When both fit
 * inside the current row the x<w crop and wrap tests collapse to one pointer
 * advance; the per-pixel path is kept for the width-1 tail of odd widths.
 * Row tracking removes the per-pixel y*240 multiply. */
static void lcd_render16(s3c2400_t *s, uint32_t w, uint32_t h) {
    uint32_t x = 0, y = 0;
    uint32_t *row = s->fb;
    while (s->lcd.vramaddr_cur < s->lcd.vramaddr_max && y < h) {
        uint32_t d = lcd_dma_read(s);
        if (x + 2u <= w) {
            row[x]     = color16_lut[(uint16_t)(d >> 16)];
            row[x + 1] = color16_lut[(uint16_t)d];
            x += 2u;
            if (x >= w) { x = 0; if (++y >= h) break; row += 240u; }
        } else {
            uint32_t color = color16_lut[(uint16_t)(d >> 16)];
            if (x < w) row[x] = color;
            if (++x >= w) { x = 0; if (++y >= h) break; row += 240u; }
            color = color16_lut[(uint16_t)d];
            if (x < w) row[x] = color;
            if (++x >= w) { x = 0; if (++y >= h) break; row += 240u; }
        }
    }
}

/* Indexed (1/2/4/8-bpp) scanout. Expand only the reachable palette entries
 * once per call; synchronous rendering cannot observe mid-frame writes.
 * Keep the empty-run return before constructing the temporary table. */
static void lcd_render_indexed(s3c2400_t *s, uint32_t w, uint32_t h, int pixels, int bits) {
    uint32_t x = 0, y = 0;
    uint32_t *row = s->fb;
    const unsigned shift = 32u - (unsigned)bits;
    const uint32_t mask = (1u << (unsigned)bits) - 1u;
    if (h == 0u || s->lcd.vramaddr_cur >= s->lcd.vramaddr_max) return;
    uint32_t pal_lut[256];
    for (uint32_t k = 0; k <= mask; ++k) pal_lut[k] = pal(s, k);
    while (s->lcd.vramaddr_cur < s->lcd.vramaddr_max && y < h) {
        uint32_t d = lcd_dma_read(s);
        for (int i = 0; i < pixels && y < h; i++) {
            uint32_t color = pal_lut[(d >> shift) & mask];
            d <<= bits;
            if (x < w) row[x] = color;
            if (++x >= w) { x = 0; y++; if (y < h) row += 240u; }
        }
    }
}

/* The four (!HWSWP, !BSWP) byte orders lcd_dma_read() can assemble, applied
 * to one raw little-endian word load. */
static uint32_t lcd_word_permute(uint32_t raw, uint32_t mode) {
    if (mode & 2u) raw = (raw << 16) | (raw >> 16); /* HWSWP */
    if (mode & 1u) raw = (raw >> 24) | ((raw >> 8) & 0x0000ff00u) |
                         ((raw << 8) & 0x00ff0000u) | (raw << 24); /* BSWP */
    return raw;
}

/* One contiguous-run word decoded to its two 16-bpp pixel halfwords in
 * consumption order: *a is the first pixel, *b the second.  This is exactly
 * the (HWSWP<<1)|BSWP assembly lcd_dma_read() performs, decomposed so the
 * swap decisions fold once per frame instead of once per word: BSWP swaps
 * the byte order of the whole word (which also exchanges the halfwords), and
 * HWSWP then selects which half is consumed first.  mode 0/2 reduce to two
 * halfword extracts, modes 1/3 add a single 32-bit byteswap. */
static inline void lcd_pair16(uint32_t d, uint32_t mode, uint32_t *a, uint32_t *b) {
    uint32_t v = d;
    if (mode & 1u) v = (v >> 24) | ((v >> 8) & 0x0000ff00u) |
                       ((v << 8) & 0x00ff0000u) | (v << 24); /* BSWP */
    if (mode & 2u) { *a = v & 0xffffu; *b = v >> 16; }       /* HWSWP */
    else           { *a = v >> 16;     *b = v & 0xffffu; }
}

/* Convert one bounded row span. AArch64 expands eight direct-color pixels
 * at a time without random accesses to the 256 KiB color table. Byte loads
 * accept unaligned guest addresses; the scalar tail never overreads a row. */
static inline void lcd_row16(const uint8_t *src, uint32_t *dst,
                             uint32_t words, uint32_t mode) {
#ifdef GP32_LCD_NEON
    while (words >= 4u) {
        uint8x16_t raw = vld1q_u8(src);
        if (!(mode & 2u)) raw = vreinterpretq_u8_u16(vrev32q_u16(vreinterpretq_u16_u8(raw)));
        if (mode & 1u) raw = vrev32q_u8(raw);
        const uint16x8_t v = vreinterpretq_u16_u8(raw);
        const uint16x8_t intensity = vshlq_n_u16(vandq_u16(v, vdupq_n_u16(1u)), 2);
        const uint16x8_t mask_hi = vdupq_n_u16(0xf8u);
        const uint16x8_t mask_lo = vdupq_n_u16(3u);
        const uint16x8_t r = vorrq_u16(intensity, vorrq_u16(
            vandq_u16(vshrq_n_u16(v, 8), mask_hi), vandq_u16(vshrq_n_u16(v, 14), mask_lo)));
        const uint16x8_t g = vorrq_u16(intensity, vorrq_u16(
            vandq_u16(vshrq_n_u16(v, 3), mask_hi), vandq_u16(vshrq_n_u16(v, 9), mask_lo)));
        const uint16x8_t b = vorrq_u16(intensity, vorrq_u16(
            vandq_u16(vshlq_n_u16(v, 2), mask_hi), vandq_u16(vshrq_n_u16(v, 4), mask_lo)));
        const uint8x8x4_t rgba = {{vmovn_u16(b), vmovn_u16(g), vmovn_u16(r), vdup_n_u8(0xffu)}};
        vst4_u8((uint8_t *)dst, rgba);
        src += 16; dst += 8; words -= 4u;
    }
#endif
    while (words--) {
        uint32_t a, b;
        lcd_pair16(gp32_ld32le(src), mode, &a, &b);
        dst[0] = color16_lut[a]; dst[1] = color16_lut[b];
        src += 4; dst += 2;
    }
}

/* An indexed byte is one whole pixel. Specialize the four byte orders
 * once per row span instead of shifting a variable-width pixel loop.
 * AArch64 stores the four gathered palette words as one 16-byte block, which
 * the compiler emits as a single STP pair instead of four 32-bit stores; every
 * pixel keeps its original address and value.  Other targets keep the
 * per-pixel stores their store ports already handle optimally. */
static inline void lcd_row8(const uint8_t *src, uint32_t *dst, uint32_t words,
                            const uint32_t *palette, uint32_t mode) {
#if defined(__aarch64__)
#define LCD8_ROW(A, B, C, D)                         \
    do {                                             \
        while (words--) {                            \
            uint32_t px[4];                          \
            px[0] = palette[src[A]];                 \
            px[1] = palette[src[B]];                 \
            px[2] = palette[src[C]];                 \
            px[3] = palette[src[D]];                 \
            memcpy(dst, px, sizeof(px));             \
            src += 4; dst += 4;                      \
        }                                            \
    } while (0)
#else
#define LCD8_ROW(A, B, C, D)                         \
    do {                                             \
        while (words--) {                            \
            dst[0] = palette[src[A]];                 \
            dst[1] = palette[src[B]];                 \
            dst[2] = palette[src[C]];                 \
            dst[3] = palette[src[D]];                 \
            src += 4; dst += 4;                       \
        }                                            \
    } while (0)
#endif
    switch (mode) {
    case 0u: LCD8_ROW(3, 2, 1, 0); break;
    case 1u: LCD8_ROW(0, 1, 2, 3); break;
    case 2u: LCD8_ROW(1, 0, 3, 2); break;
    default: LCD8_ROW(2, 3, 0, 1); break;
    }
#undef LCD8_ROW
}

/* Whole-run contiguous scanout.  With OFFSIZE == 0 no read is ever displaced:
 * halfword N of the run sits at start + 2*N no matter where the page counter
 * wraps, so the words the frame consumes are one straight RAM block.  The
 * consumed word count is min(words that fill the frame, words until VRAMADDR
 * reaches VRAMADDR_MAX), the same count the per-word loop stops at, and the
 * page counter ends at (pagewidth_cur + 2*words) modulo pagewidth_max, which
 * is exactly what the halfword walk leaves because with OFFSIZE == 0 a wrap
 * only resets the counter.  Returns 0 - leaving lcd_dma_read() and the two
 * per-word renderers untouched, including their zero fill for unbacked
 * halfwords - whenever a row could end inside a word (width not a multiple of
 * the pixels per word), an OFFSIZE gap exists, the run leaves RAM, or the
 * scanout consumes no word at all. */
static int lcd_render_contiguous(s3c2400_t *s, uint32_t w, uint32_t h, uint32_t ppw, uint32_t bits) {
    const uint32_t cur0 = s->lcd.vramaddr_cur;
    const uint32_t pwcur0 = s->lcd.pagewidth_cur;
    const uint32_t pwmax = s->lcd.pagewidth_max;
    if (s->lcd.offsize != 0u || w % ppw != 0u) return 0;
    const uint64_t frame_words = (uint64_t)w * (uint64_t)h / (uint64_t)ppw;
    const uint64_t range_words = s->lcd.vramaddr_max > cur0
        ? (((uint64_t)s->lcd.vramaddr_max - (uint64_t)cur0) + 3u) / 4u : 0u;
    const uint64_t run = frame_words < range_words ? frame_words : range_words;
    if (run == 0u) return 0;
    if (cur0 < RAM_BASE || (uint64_t)(cur0 - RAM_BASE) + 4u * run > (uint64_t)s->ram_size) return 0;
    /* A full aperture is overwritten below. Short or partial scanouts still
     * need black pixels outside the written region. Decide after acceptance
     * so the range/stride guards stay in one place. */
    if (w != 240u || h != 320u || run != frame_words)
        memset(s->fb, 0, sizeof(s->fb));
    const uint8_t *src = &s->ram[cur0 - RAM_BASE];
    const uint32_t words = (uint32_t)run;
    const uint32_t wprow = w / ppw;
    const uint32_t mode = (s->lcd.hwswp ? 2u : 0u) | (s->lcd.bswp ? 1u : 0u);
    uint32_t i = 0u, y = 0u;
    uint32_t *row = s->fb;
    if (bits == 16u) {
        /* The swap mode is fixed for the whole run, so the word loop is
         * specialized per mode: the two per-word lcd_word_permute() branch
         * tests disappear, the no-swap modes become pure half extraction and
         * the swap modes use one 32-bit byteswap per pixel pair. */
        switch (mode) {
#define LCD16_SCANOUT(MODE)                                                  \
            do {                                                             \
                while (y < h && i + wprow <= words) {                        \
                    lcd_row16(src + (size_t)i * 4u, row, wprow, MODE);         \
                    i += wprow; ++y; row += 240u;                            \
                }                                                            \
                /* Fewer words than one row are left, so this row can no     \
                 * longer wrap. */                                           \
                lcd_row16(src + (size_t)i * 4u, row, words - i, MODE);         \
            } while (0)
        case 0u: LCD16_SCANOUT(0u); break;
        case 1u: LCD16_SCANOUT(1u); break;
        case 2u: LCD16_SCANOUT(2u); break;
        default: LCD16_SCANOUT(3u); break;
#undef LCD16_SCANOUT
        }
    } else {
        const unsigned shift = 32u - bits;
        const uint32_t mask = (1u << bits) - 1u;
        /* Expand only the colors addressable at this bit depth; every pixel
         * index below is masked, so the remaining entries are never read. */
        uint32_t pal_lut[256];
        for (uint32_t k = 0; k <= mask; ++k) pal_lut[k] = color16_lut[s->lcd_palette[k]];
        if (bits == 8u) {
            while (y < h && i + wprow <= words) {
                lcd_row8(src + (size_t)i * 4u, row, wprow, pal_lut, mode);
                i += wprow; ++y; row += 240u;
            }
            lcd_row8(src + (size_t)i * 4u, row, words - i, pal_lut, mode);
        } else {
            while (y < h && i + wprow <= words) {
                const uint8_t *p = src + (size_t)i * 4u;
                uint32_t *dst = row;
                for (uint32_t k = 0; k < wprow; ++k, p += 4, dst += ppw) {
                    uint32_t d = gp32_ld32le(p);
                    d = lcd_word_permute(d, mode);
                    for (uint32_t q = 0; q < ppw; ++q) { dst[q] = pal_lut[(d >> shift) & mask]; d <<= bits; }
                }
                i += wprow; ++y; row += 240u;
            }
            uint32_t *dst = row;
            while (i < words) {
                uint32_t d = gp32_ld32le(src + (size_t)i * 4u);
                d = lcd_word_permute(d, mode);
                ++i;
                for (uint32_t q = 0; q < ppw; ++q) { dst[q] = pal_lut[(d >> shift) & mask]; d <<= bits; }
                dst += ppw;
            }
        }
    }
    s->lcd.vramaddr_cur = (uint32_t)((uint64_t)cur0 + 4u * run);
    const uint64_t pw_after = (uint64_t)pwcur0 + 2u * run;
    s->lcd.pagewidth_cur = pwmax ? (uint32_t)(pw_after % (uint64_t)pwmax) : (uint32_t)pw_after;
    return 1;
}

void s3c2400_render_lcd(s3c2400_t *s) {
    if (!s || !(s->lcd_regs[0] & 1u)) return;
    lcd_dma_init(s);
    uint32_t w=s->lcd.width?s->lcd.width:240, h=s->lcd.height?s->lcd.height:320;
    /* Decode the pixel format once per frame instead of once per DMA word.
     * An unsupported bppmode still consumes one DMA word and returns before
     * the frame tail, exactly like the original per-word switch, whenever the
     * scanout loop would have run at least once. */
    int pixels = 0, bits = 0;
    switch(s->lcd.bppmode){case BPPMODE_TFT_01:pixels=32;bits=1;break;case BPPMODE_TFT_02:pixels=16;bits=2;break;case BPPMODE_TFT_04:pixels=8;bits=4;break;case BPPMODE_TFT_08:pixels=4;bits=8;break;case BPPMODE_TFT_16:pixels=2;bits=16;break;default:break;}
    if (!pixels) {
        memset(s->fb,0,sizeof(s->fb));
        if (s->lcd.vramaddr_cur < s->lcd.vramaddr_max && h != 0u) { (void)lcd_dma_read(s); return; }
    }
    else if (!lcd_render_contiguous(s, w, h, bits == 16 ? 2u : (uint32_t)pixels, (uint32_t)bits)) {
        memset(s->fb,0,sizeof(s->fb));
        if (bits == 16) lcd_render16(s, w, h);
        else lcd_render_indexed(s, w, h, pixels, bits);
    }
    /*
     * GP32 panel aperture: the BIOS programs a 240x320 portrait DMA surface
     * and the handheld mounts it as a 320x240 landscape panel.  The first raw
     * DMA scanline is a panel-settle/prefetch line and is not part of the
     * visible glass area.  The official BIOS leaves that line stale while
     * fading the boot logo to black; exposing it after rotation produces a
     * spurious one-pixel white line at the left edge.  Present the visible
     * aperture by replacing that hidden raw scanline with the first displayed
     * one.  This is a panel-timing/aperture rule, not a BIOS-title filter.
     */
    if (w == 240u && h == 320u) {
        memcpy(&s->fb[0], &s->fb[240u], 240u * sizeof(s->fb[0]));
    }
    s->fb_w = w; s->fb_h = h; s->frame_counter++;
}

static uint32_t pll_frequency(uint32_t data) {
    uint32_t mdiv = GP32_BITS(data, 19, 12);
    uint32_t pdiv = GP32_BITS(data, 9, 4);
    uint32_t sdiv = GP32_BITS(data, 1, 0);
    uint32_t den = (pdiv + 2u) << sdiv;
    if (!den) return 12000000u;
    return (uint32_t)(((uint64_t)(mdiv + 8u) * 12000000ull) / den);
}

static uint32_t clk_fclk(const s3c2400_t *s, int reg) {
    return pll_frequency(s->clkpow[reg]);
}

static uint32_t clk_hclk(const s3c2400_t *s, int reg) {
    uint32_t f = clk_fclk(s, reg);
    switch (s->clkpow[5] & 3u) {
    case 0: return f;
    case 1: return f;
    case 2: return f / 2u;
    default: return f / 2u;
    }
}

static uint32_t run_clock_values(uint32_t f, uint32_t h) {
    /* The ARM core presently accounts one emulator cycle per decoded/executed
     * instruction group, not per ARM920T FCLK pipeline cycle. For ordinary
     * undivided clocks this maps to the firmware clock directly. In the common
     * GP32 high-PLL / half-HCLK mode, real code is bus/wait-state limited while
     * this interpreter/JIT does not yet charge those stalls per instruction, so
     * use an effective bus-side throughput for frontend pacing and for all
     * peripheral-period-to-core-cycle conversions. This is clock-topology based,
     * not title-specific. */
    if (!h) return f;
    if (f >= 100000000u && h <= f / 2u && h >= 60000000u) {
        uint64_t scaled = ((uint64_t)h * 8u + 5u) / 11u;
        return scaled ? (uint32_t)scaled : h;
    }
    return h ? h : f;
}

static uint32_t clk_run(const s3c2400_t *s, int reg) {
    uint32_t run = run_clock_values(clk_fclk(s, reg), clk_hclk(s, reg));
    uint32_t percent = s->cpu_speed_percent;
    /* Scaling only the instruction clock keeps PCLK/HCLK-derived IIS rates,
     * PWM periods and TFT timing at their real-time lengths: every peripheral
     * converts its own period to CPU cycles through this value. */
    if (percent && percent != 100u) run = (uint32_t)(((uint64_t)run * percent + 50u) / 100u);
    return run;
}

static uint32_t clk_pclk(const s3c2400_t *s, int reg) {
    uint32_t f = clk_fclk(s, reg);
    switch (s->clkpow[5] & 3u) {
    case 0: return f;
    case 1: return f / 2u;
    case 2: return f / 2u;
    default: return f / 4u;
    }
}

/* Derived host data, rebuilt on every register replacement (including the
 * temporary old-clock view used to settle a CPU slice). Never serialized. */
static void clock_refresh_values(s3c2400_t *s) {
    s->cached_fclk_hz = clk_fclk(s, MPLLCON);
    s->cached_hclk_hz = clk_hclk(s, MPLLCON);
    s->cached_run_hz = clk_run(s, MPLLCON);
}

uint32_t s3c2400_fclk_hz(const s3c2400_t *s) {
    uint32_t f = s ? s->cached_fclk_hz : 0u;
    return f ? f : 66000000u;
}

uint32_t s3c2400_hclk_hz(const s3c2400_t *s) {
    uint32_t h = s ? s->cached_hclk_hz : 0u;
    return h ? h : 66000000u;
}

uint32_t s3c2400_run_clock_hz(const s3c2400_t *s) {
    uint32_t h = s ? s->cached_run_hz : 0u;
    return h ? h : 66000000u;
}

uint32_t s3c2400_cpu_speed_percent(const s3c2400_t *s) {
    return s && s->cpu_speed_percent ? s->cpu_speed_percent : 100u;
}

static void audio_append_stereo(s3c2400_t *s, int16_t left, int16_t right, uint32_t rate) {
    if (!s) return;
    if (s->audio_frames >= s->audio_cap_frames) {
        if (!audio_reserve_frames(s, 1u)) return;
    }
    if (!audio_prepare_rate(s, rate)) return;
    if (s->codec_gain_q16 != 65536u) {
        left = codec_scale(left, s->codec_gain_q16);
        right = codec_scale(right, s->codec_gain_q16);
    }
    s->audio[s->audio_frames * 2u + 0u] = left;
    s->audio[s->audio_frames * 2u + 1u] = right;
    s->audio_frames++;
}

void s3c2400_audio_append_u8_mono(s3c2400_t *s, const uint8_t *samples, uint32_t sample_count, uint32_t sample_rate_hz) {
    if (!s || !samples || !sample_count) return;
    uint32_t rate = sample_rate_hz ? sample_rate_hz : 11025u;
    for (uint32_t i = 0; i < sample_count; ++i) {
        int16_t v = (int16_t)(((int)samples[i] - 128) * 256);
        audio_append_stereo(s, v, v, rate);
    }
}

void s3c2400_audio_append_s16_stereo(s3c2400_t *s, int16_t left, int16_t right, uint32_t sample_rate_hz) {
    if (!s) return;
    audio_append_stereo(s, left, right, sample_rate_hz ? sample_rate_hz : 11025u);
}


static uint32_t lcd_visible_lines(const s3c2400_t *s) {
    if (!s) return 320u;
    uint32_t lineval = GP32_BITS(s->lcd_regs[1], 23, 14);
    uint32_t visible = lineval + 1u;
    if (visible == 0u || visible > 1024u) visible = 320u;
    return visible;
}

static int lcd_is_tft(const uint32_t *regs) {
    /* CLKVAL=0 is outside the documented TFT range. Keep the legacy fallback
     * for STN and the synthetic direct-HLE framebuffer configuration. */
    return GP32_BITS(regs[0], 6, 5) == 3u && GP32_BITS(regs[0], 17, 8) >= 1u;
}

static uint64_t lcd_tft_line_period(const uint32_t *r) {
    uint32_t total = GP32_BITS(r[3], 7, 0) + 1u + GP32_BITS(r[2], 25, 19) + 1u +
                     GP32_BITS(r[2], 18, 8) + 1u + GP32_BITS(r[2], 7, 0) + 1u;
    return (uint64_t)total * 2u * (GP32_BITS(r[0], 17, 8) + 1u);
}

static uint32_t lcd_tft_total_lines(const uint32_t *r) {
    return GP32_BITS(r[1], 5, 0) + 1u + GP32_BITS(r[1], 31, 24) + 1u +
           GP32_BITS(r[1], 23, 14) + 1u + GP32_BITS(r[1], 13, 6) + 1u;
}

static void lcd_refresh_timing_cache(s3c2400_t *s) {
    if (s->lcd_timing_valid) return;
    s->lcd_cached_visible = lcd_visible_lines(s);
    if (lcd_is_tft(s->lcd_regs)) {
        s->lcd_cached_total_lines = lcd_tft_total_lines(s->lcd_regs);
        s->lcd_cached_line_cycles = lcd_tft_line_period(s->lcd_regs);
        s->lcd_cached_frame_cycles = s->lcd_cached_line_cycles * s->lcd_cached_total_lines;
    } else {
        uint32_t runclk = s3c2400_run_clock_hz(s);
        s->lcd_cached_frame_cycles = ((uint64_t)runclk + 30u) / 60u;
        if (!s->lcd_cached_frame_cycles) s->lcd_cached_frame_cycles = 1u;
        s->lcd_cached_total_lines = s->lcd_cached_visible + s->lcd_cached_visible / 16u + 8u;
        s->lcd_cached_line_cycles = s->lcd_cached_frame_cycles / s->lcd_cached_total_lines;
        if (!s->lcd_cached_line_cycles) s->lcd_cached_line_cycles = 1u;
    }
    s->lcd_timing_valid = 1;
}

static uint64_t lcd_panel_frame_cycles(s3c2400_t *s) {
    lcd_refresh_timing_cache(s);
    return s->lcd_cached_frame_cycles;
}

static void lcd_observation_deadline(s3c2400_t *s, int horizontal) {
    unsigned bit = horizontal ? 2u : 1u;
    if (!s->cpu_run_active || (s->cpu_lcd_deadline_set & bit)) return;
    s->cpu_lcd_deadline_set |= bit;
    lcd_refresh_timing_cache(s);
    uint64_t frame = s->lcd_cached_frame_cycles, line = s->lcd_cached_line_cycles;
    uint64_t phase = s->lcd_line_accum % frame;
    uint64_t remaining = line - phase % line;
    if (horizontal && lcd_is_tft(s->lcd_regs)) {
        uint64_t pixel = 2u * (GP32_BITS(s->lcd_regs[0], 17, 8) + 1u);
        uint64_t pos = phase % line;
        uint64_t end = pixel * (GP32_BITS(s->lcd_regs[3], 7, 0) + 1u);
        if (pos >= end) end += pixel * (GP32_BITS(s->lcd_regs[2], 25, 19) + 1u);
        if (pos >= end) end += pixel * (GP32_BITS(s->lcd_regs[2], 18, 8) + 1u);
        if (pos >= end) end = line;
        remaining = end - pos;
    } else if (!lcd_is_tft(s->lcd_regs)) {
        uint64_t zero = (s->lcd_cached_visible - 1u) * line;
        if (phase >= zero) remaining = frame - phase;
        else if (remaining > frame - phase) remaining = frame - phase;
    }
    if (lcd_is_tft(s->lcd_regs)) {
        uint64_t numerator = remaining * s3c2400_run_clock_hz(s) - s->lcd_hclk_remainder;
        uint32_t hclk = s3c2400_hclk_hz(s);
        remaining = (numerator + hclk - 1u) / hclk;
    }
    if (remaining < UINT32_MAX) arm920t_limit_run(s->cpu_irq_sink, (uint32_t)remaining);
}

static uint32_t lcd_current_line_count(s3c2400_t *s) {
    if (!s) return 0u;
    lcd_observation_deadline(s, 0);
    if (s->lcd_line_valid) return s->lcd_cached_line;
    lcd_refresh_timing_cache(s);
    uint64_t line = (s->lcd_line_accum % s->lcd_cached_frame_cycles) / s->lcd_cached_line_cycles;
    uint32_t visible = s->lcd_cached_visible;
    if (lcd_is_tft(s->lcd_regs)) {
        uint32_t start = GP32_BITS(s->lcd_regs[1], 5, 0) + 1u + GP32_BITS(s->lcd_regs[1], 31, 24) + 1u;
        line = line < start ? 0u : line - start;
    }
    s->lcd_cached_line = line >= visible ? 0u : visible - 1u - (uint32_t)line;
    s->lcd_line_valid = 1;
    return s->lcd_cached_line;
}

static uint32_t lcd_current_status(s3c2400_t *s) {
    if (!(s->lcd_regs[0] & 1u) || !lcd_is_tft(s->lcd_regs)) return 0u;
    lcd_observation_deadline(s, 1);
    lcd_refresh_timing_cache(s);
    uint64_t phase = s->lcd_line_accum % s->lcd_cached_frame_cycles;
    uint32_t v = (uint32_t)(phase / s->lcd_cached_line_cycles);
    uint32_t pixel = 2u * (GP32_BITS(s->lcd_regs[0], 17, 8) + 1u);
    uint32_t h = (uint32_t)((phase % s->lcd_cached_line_cycles) / pixel);
    uint32_t vs = GP32_BITS(s->lcd_regs[1], 5, 0) + 1u;
    uint32_t vb = vs + GP32_BITS(s->lcd_regs[1], 31, 24) + 1u;
    uint32_t hs = GP32_BITS(s->lcd_regs[3], 7, 0) + 1u;
    uint32_t hb = hs + GP32_BITS(s->lcd_regs[2], 25, 19) + 1u;
    uint32_t vstatus = v < vs ? 0u : v < vb ? 1u : v < vb + s->lcd_cached_visible ? 2u : 3u;
    uint32_t hstatus = h < hs ? 0u : h < hb ? 1u : h < hb + GP32_BITS(s->lcd_regs[2],18,8) + 1u ? 2u : 3u;
    return (vstatus << 19) | (hstatus << 17);
}

static void iis_fifo_write16(s3c2400_t *s, uint16_t sample) {
    if (!s) return;
    s->iis_fifo[s->iis_fifo_index++] = sample;
    if (s->iis_fifo_index >= 2u) {
        s->iis_fifo_index = 0;
        iis_refresh_clock_cache(s);
        audio_append_stereo(s, (int16_t)s->iis_fifo[0], (int16_t)s->iis_fifo[1], s->iis_cached_rate_hz);
    }
}

static uint32_t iis_frame_rate_hz(const s3c2400_t *s) {
    static const uint32_t codeclk_table[2] = { 256u, 384u };
    uint32_t pclk = clk_pclk(s, MPLLCON);
    if (!pclk) pclk = 12000000u;
    uint32_t prescaler_a = GP32_BITS(s->iis[2], 9, 5);
    uint32_t codeclk = codeclk_table[GP32_BIT(s->iis[1], 2)];
    /*
     * The GP32 BIOS "nobody" sample is a useful sanity check here. v13
     * matched its approximate duration by tagging the emitted stream as an
     * 8 kHz MCLK-derived stream, but that lowered the pitch by roughly a
     * third versus a hardware recording. The S3C2400 IIS block is driven by
     * the current peripheral clock; for the BIOS registers this gives about
     * 11.025 kHz, which aligns the dominant spectral peaks with the hardware
     * capture.
     *
     * The timer below advances once per complete stereo output frame. 16-bit
     * DMA still performs two FIFO halfword writes during that frame.
     */
    uint64_t rate = (uint64_t)pclk / ((uint64_t)(prescaler_a + 1u) * (uint64_t)codeclk);
    if (rate < 4000u) rate = 4000u;
    if (rate > 96000u) rate = 96000u;
    return (uint32_t)rate;
}

static uint64_t iis_period_cpu_cycles(const s3c2400_t *s) {
    /* Legacy wire-cache value only; the running scheduler carries fractions. */
    uint32_t runclk = clk_run(s, MPLLCON);
    uint32_t rate = iis_frame_rate_hz(s);
    if (!runclk) runclk = 40000000u;
    if (!rate) return 1u;
    uint64_t period = ((uint64_t)runclk + (uint64_t)rate - 1u) / (uint64_t)rate;
    if (period < 256u) period = 256u;
    return period;
}

static uint64_t rescale_period_progress(uint64_t progress, uint64_t old_period, uint64_t new_period) {
    if (old_period == new_period) return progress;
    /* Preserve any whole pending periods as well as the fractional phase.
     * Peripheral periods are bounded by their register widths; split the
     * product so a long accumulated history is not multiplied directly. */
    return (progress / old_period) * new_period +
           ((progress % old_period) * new_period) / old_period;
}

static void clock_invalidate(s3c2400_t *s) {
    clock_refresh_values(s);
    s->iis_clock_dirty = 1;
    s->pwm_clock_dirty = 1;
    s->lcd_line_valid = 0;
    s->lcd_timing_valid = 0;
}

static void clock_apply(s3c2400_t *s, const uint32_t *registers) {
    uint64_t old_lcd_period = lcd_panel_frame_cycles(s);
    uint64_t old_iis_clock = s3c2400_run_clock_hz(s);
    uint64_t old_pwm_period[5];
    pwm_refresh_clock_cache(s);
    memcpy(old_pwm_period, s->pwm_period_cycles, sizeof(old_pwm_period));
    memcpy(s->clkpow, registers, sizeof(s->clkpow));
    clock_invalidate(s);
    pwm_refresh_clock_cache(s);
    for (unsigned t = 0; t < 5u; ++t)
        s->pwm_accum[t] = rescale_period_progress(s->pwm_accum[t], old_pwm_period[t], s->pwm_period_cycles[t]);
    s->iis_accum = rescale_period_progress(s->iis_accum, old_iis_clock, s3c2400_run_clock_hz(s));
    s->audio_idle_phase = rescale_period_progress(s->audio_idle_phase, old_iis_clock, s3c2400_run_clock_hz(s));
    uint64_t new_lcd_period = lcd_panel_frame_cycles(s);
    if (lcd_is_tft(s->lcd_regs)) {
        s->lcd_hclk_remainder = (uint32_t)((uint64_t)s->lcd_hclk_remainder * s3c2400_run_clock_hz(s) / old_iis_clock);
    } else if (old_lcd_period != new_lcd_period) {
        /* Completed scanouts are already counted separately. Only translate
         * the current frame's phase, avoiding a product of the full history. */
        s->lcd_line_accum = (s->lcd_line_accum % old_lcd_period) * new_lcd_period / old_lcd_period;
    }
}

static void clock_write(s3c2400_t *s, uint32_t addr, uint32_t value, uint32_t mask) {
    uint32_t old_run_clock = clk_run(s, MPLLCON);
    uint32_t old_pclk = clk_pclk(s, MPLLCON);
    uint32_t registers[GP32_ARRAY_COUNT(s->clkpow)];
    memcpy(registers, s->clkpow, sizeof(registers));
    reg_array_write(registers, sizeof(registers), addr - 0x14800000u, value, mask);
    if (s->cpu_run_active) {
        if (!s->cpu_run_clock_written) {
            memcpy(s->clkpow_before_run, s->clkpow, sizeof(s->clkpow));
            s->cpu_run_clock_written = 1;
        }
        /* Reads in this instruction see the write. Phase conversion waits
         * until its elapsed cycles have been processed at the previous rate. */
        memcpy(s->clkpow, registers, sizeof(s->clkpow));
        clock_invalidate(s);
    } else {
        clock_apply(s, registers);
    }
    /* A PCLK-only change also ends the slice, even if the CPU rate is stable. */
    if ((old_run_clock != clk_run(s, MPLLCON) || old_pclk != clk_pclk(s, MPLLCON)) &&
        arm920t_is_running(s->cpu_irq_sink))
        arm920t_stop_run(s->cpu_irq_sink);
}

int s3c2400_set_cpu_speed_percent(s3c2400_t *s, uint32_t percent) {
    if (!s || s->cpu_run_active || percent < 50u || percent > 400u) return 0;
    if (s3c2400_cpu_speed_percent(s) == percent) return 1;
    /* Settle every cached period at the old instruction rate first, then
     * convert phases exactly as a guest clock write between slices would. */
    pwm_refresh_clock_cache(s);
    (void)lcd_panel_frame_cycles(s);
    uint32_t registers[GP32_ARRAY_COUNT(s->clkpow)];
    memcpy(registers, s->clkpow, sizeof(registers));
    s->cpu_speed_percent = percent;
    clock_apply(s, registers);
    return 1;
}

static uint32_t iis_dma_transfers_per_frame(const s3c2400_t *s) {
    if (!s) return 1u;
    const uint32_t *r = &s->dma[2u << 3];
    unsigned dsz = GP32_BITS(r[2], 21, 20);
    /*
     * One tick of the simplified IIS scheduler represents one complete stereo
     * audio frame. Every unit transfer performs one IISFIF write, and each
     * 8- or 16-bit write pushes one FIFO halfword, so both need two units
     * (left/right) in that frame period, while a 32-bit DMA write carries
     * both halfwords.
     */
    if (dsz == 0u) return 2u;
    if (dsz == 1u) return 2u;
    return 1u;
}

static void iis_refresh_clock_cache(s3c2400_t *s) {
    if (!s || !s->iis_clock_dirty) return;
    s->iis_cached_rate_hz = iis_frame_rate_hz(s);
    s->iis_cached_run_hz = s3c2400_run_clock_hz(s);
    s->iis_cached_period_cycles = iis_period_cpu_cycles(s);
    if (!s->iis_cached_period_cycles) s->iis_cached_period_cycles = 1u;
    s->iis_clock_dirty = 0;
}

static uint64_t pwm_period_cpu_cycles(s3c2400_t *s, unsigned t) {
    if (!s || t >= 5u) return 1u;
    pwm_refresh_clock_cache(s);
    return s->pwm_period_cycles[t] ? s->pwm_period_cycles[t] : 1u;
}

static void dma_request_iis(s3c2400_t *s) {
    uint32_t *r = &s->dma[2 << 3];
    if ((r[6] & 2u) && GP32_BIT(r[2], 23) && GP32_BITS(r[2], 25, 24) == 0u) dma_trigger(s, 2);
}

static void dma_request_pwm(s3c2400_t *s) {
    for (int ch = 0; ch < 4; ++ch) {
        if (ch == 1) continue;
        uint32_t *r = &s->dma[ch << 3];
        if ((r[6] & 2u) && GP32_BIT(r[2], 23) && GP32_BITS(r[2], 25, 24) == 3u) dma_trigger(s, ch);
    }
}

void s3c2400_tick(s3c2400_t *s, uint32_t cpu_cycles) {
    if (!s || !cpu_cycles) return;
    uint64_t lcd_frame_cycles = lcd_panel_frame_cycles(s);
    int lcd_frame_ready = 0;
    if (lcd_is_tft(s->lcd_regs)) {
        if (s->lcd_regs[0] & 1u) {
            uint32_t runclk = s3c2400_run_clock_hz(s);
            uint32_t hclk = s3c2400_hclk_hz(s);
            if (hclk == runclk) {
                /* The retained fraction is already below RUN. Equal clocks
                 * advance by whole cycles without changing that fraction. */
                s->lcd_line_accum += cpu_cycles;
            } else {
                uint64_t scaled = (uint64_t)cpu_cycles * hclk + s->lcd_hclk_remainder;
                s->lcd_line_accum += scaled / runclk;
                s->lcd_hclk_remainder = (uint32_t)(scaled % runclk);
            }
        }
        /* TFT phase is normalized by reset, timing writes and state load.
         * Most CPU slices stay within this frame: no division is needed to
         * discover that. Large slices can still cross several frames. */
        lcd_frame_ready = s->lcd_line_accum >= lcd_frame_cycles;
    } else {
        uint64_t old_lcd_accum = s->lcd_line_accum;
        s->lcd_line_accum += cpu_cycles;
        lcd_frame_ready = old_lcd_accum / lcd_frame_cycles != s->lcd_line_accum / lcd_frame_cycles;
    }
    s->lcd_line_valid = 0;
    if ((s->lcd_regs[0] & 1u) && lcd_frame_ready) {
        /*
         * The LCD output should be a completed scanout, not a fresh full-frame
         * decode of VRAM/palette at whatever cycle the host frontend asks for
         * pixels.  BIOS boot effects rewrite the framebuffer and palette
         * between vblank waits; rendering only when the emulated panel reaches
         * the frame boundary prevents random one-line snapshots of those
         * in-progress updates while preserving ordinary game rendering.
         */
        s3c2400_render_lcd(s);
    }
    if (lcd_is_tft(s->lcd_regs) && lcd_frame_ready) s->lcd_line_accum %= lcd_frame_cycles;
    if (s->iis[0] & 1u) {
        iis_refresh_clock_cache(s);
        /* Carry fractional CPU periods instead of rounding each sample's
         * deadline up. Over one emulated second this emits exactly the rate
         * advertised to every frontend, independent of tick partitioning. */
        uint64_t period = s->iis_cached_run_hz;
        s->iis_accum += (uint64_t)cpu_cycles * s->iis_cached_rate_hz;
        if (s->iis_accum >= period) {
            uint64_t periods64 = s->iis_accum / period;
            uint32_t transfers_per_frame = iis_dma_transfers_per_frame(s);
            s->iis_accum -= periods64 * period;
            while (periods64) {
                uint32_t frame_batch = periods64 > 2048u ? 2048u : (uint32_t)periods64;
                uint32_t transfer_count = frame_batch * transfers_per_frame;
                while (transfer_count) {
                    /* Auto-reload can finish one RAM span and expose another
                     * in this tick. Revalidate it through the same fast gate
                     * instead of sending the entire remainder one unit at a
                     * time. Unsupported sources retain their bus side effects. */
                    uint32_t done = dma_request_iis_fast_count(s, transfer_count);
                    if (!done) {
                        for (uint32_t i = 0; i < transfer_count; ++i) dma_request_iis(s);
                        break;
                    }
                    transfer_count -= done;
                }
                periods64 -= frame_batch;
            }
        }
    } else {
        s->iis_accum = 0;
        audio_tick_idle(s, cpu_cycles);
    }
    static const unsigned start_mask[5] = { 0x000001u, 0x000100u, 0x001000u, 0x010000u, 0x100000u };
    static const unsigned auto_mask[5]  = { 0x000008u, 0x000800u, 0x008000u, 0x080000u, 0x400000u };
    static const unsigned irq_no[5] = { INT_TIMER0, INT_TIMER1, INT_TIMER2, INT_TIMER3, INT_TIMER4 };
    unsigned pwm_dma_timer = GP32_BITS(s->pwm[1], 23, 20);
    for (unsigned t = 0; t < 5; ++t) {
        if (!(s->pwm[2] & start_mask[t])) { s->pwm_accum[t] = 0; continue; }
        uint64_t period = pwm_period_cpu_cycles(s, t);
        s->pwm_accum[t] += cpu_cycles;
        while (s->pwm_accum[t] >= period) {
            s->pwm_accum[t] -= period;
            if (pwm_dma_timer == t + 1u) dma_request_pwm(s);
            else request_irq(s, irq_no[t]);
            if (!(s->pwm[2] & auto_mask[t])) { s->pwm[2] &= ~start_mask[t]; s->pwm_accum[t] = 0; break; }
        }
    }
}

static uint32_t pwm_event_budget(s3c2400_t *s, uint32_t budget) {
    static const uint32_t start_mask[5] = {1u, 0x100u, 0x1000u, 0x10000u, 0x100000u};
    if (!(s->pwm[2] & 0x111101u)) return budget;
    pwm_refresh_clock_cache(s);
    for (unsigned t = 0; t < 5u; ++t) {
        if (!(s->pwm[2] & start_mask[t])) continue;
        uint64_t period = s->pwm_period_cycles[t];
        uint64_t remaining = period > s->pwm_accum[t] ? period - s->pwm_accum[t] : 1u;
        if (remaining < budget) budget = (uint32_t)remaining;
    }
    return budget;
}

static uint32_t iis_dma_irq_budget(s3c2400_t *s, uint32_t budget) {
    const uint32_t *r = &s->dma[2u << 3];
    if (!(s->iis[0] & 1u) || !(r[6] & 2u) || !GP32_BIT(r[2], 28) ||
        !GP32_BIT(r[2], 23) || GP32_BITS(r[2], 25, 24) != 0u) return budget;
    iis_refresh_clock_cache(s);
    uint32_t count = r[3] & 0x000fffffu;
    uint32_t frames = iis_dma_transfers_per_frame(s) == 2u ? (count + 1u) / 2u : count;
    /* Whole-service mode drains its count on the next request. A zero count
     * also reloads/disables and requests the IRQ on that first request. */
    if (!frames || GP32_BIT(r[2], 26)) frames = 1u;
    uint64_t terminal = (uint64_t)frames * s->iis_cached_run_hz;
    uint64_t progress = s->iis_accum + (uint64_t)budget * s->iis_cached_rate_hz;
    if (progress < terminal) return budget;
    if (terminal <= s->iis_accum) return 1u;
    uint64_t remaining = terminal - s->iis_accum;
    return (uint32_t)((remaining + s->iis_cached_rate_hz - 1u) / s->iis_cached_rate_hz);
}

uint32_t s3c2400_run_cpu(s3c2400_t *s, uint32_t cpu_cycles) {
    if (!s || !s->cpu_irq_sink || !cpu_cycles) return 0;
    /* Let the guest service terminal-count IRQs before consuming the rest of
     * a large host slice. DMA/IIS/clock writes already yield and settle the
     * old state, so the next call derives a fresh deadline after such writes. */
    cpu_cycles = iis_dma_irq_budget(s, cpu_cycles);
    /* Timer IRQs and timer-driven DMA must become visible before the next
     * period. Guest timer writes also yield, settling the old interval first. */
    cpu_cycles = pwm_event_budget(s, cpu_cycles);
    s->cpu_run_active = 1;
    s->cpu_run_clock_written = 0;
    s->cpu_lcd_deadline_set = 0;
    s->cpu_io_write_count = 0;
    uint32_t done = arm920t_run(s->cpu_irq_sink, cpu_cycles);
    s->cpu_run_active = 0;
    if (s->cpu_run_clock_written) {
        uint32_t registers[GP32_ARRAY_COUNT(s->clkpow)];
        memcpy(registers, s->clkpow, sizeof(registers));
        memcpy(s->clkpow, s->clkpow_before_run, sizeof(s->clkpow));
        s->cpu_run_clock_written = 0;
        clock_invalidate(s);
        s3c2400_tick(s, done);
        clock_apply(s, registers);
    } else {
        s3c2400_tick(s, done);
    }
    for (unsigned i = 0; i < s->cpu_io_write_count; ++i)
        io_write32(s, s->cpu_io_writes[i].addr, s->cpu_io_writes[i].value, s->cpu_io_writes[i].mask);
    s->cpu_io_write_count = 0;
    return done;
}

uint32_t s3c2400_debug_read32(s3c2400_t *s, uint32_t addr) {
    return s ? s3c2400_read32(s, addr) : 0xffffffffu;
}

const uint32_t *s3c2400_framebuffer(s3c2400_t *s, uint32_t *w, uint32_t *h, uint32_t *stride, uint64_t *frames) {
    if (!s) return NULL;
    if (s->frame_counter == 0u && (s->lcd_regs[0] & 1u)) s3c2400_render_lcd(s);
    if (w) *w = s->fb_w;
    if (h) *h = s->fb_h;
    if (stride) *stride = 240;
    if (frames) *frames = s->frame_counter;
    return s->fb;
}

const int16_t *s3c2400_audio_samples(s3c2400_t *s, uint64_t *frames, uint32_t *sample_rate_hz) {
    if (!s) return NULL;
    const audio_boundary_t *span = s->audio_boundary_head < s->audio_boundary_count ?
        &s->audio_boundaries[s->audio_boundary_head] : NULL;
    if (frames) *frames = (span ? span->end_frame : s->audio_frames) - s->audio_read_frames;
    if (sample_rate_hz) *sample_rate_hz = span ? span->rate_hz : s->audio_sample_rate_hz;
    return s->audio ? s->audio + (size_t)s->audio_read_frames * 2u : NULL;
}

int s3c2400_audio_consume(s3c2400_t *s, uint64_t frames) {
    if (!s) return 0;
    uint64_t available = 0;
    s3c2400_audio_samples(s, &available, NULL);
    if (frames > available) return 0;
    s->audio_read_frames += frames;
    if (s->audio_read_frames == s->audio_frames) {
        s3c2400_audio_clear(s);
    } else if (frames == available && s->audio_boundary_head < s->audio_boundary_count) {
        if (++s->audio_boundary_head == s->audio_boundary_count)
            s->audio_boundary_head = s->audio_boundary_count = 0;
    }
    return 1;
}

void s3c2400_audio_clear(s3c2400_t *s) {
    if (!s) return;
    /*
     * This clears the host-facing captured PCM buffer only. Do not reset
     * iis_fifo_index here: it is emulated IIS FIFO state, and SDL calls this
     * once per video frame after consuming audio. Resetting it at arbitrary
     * frontend chunk boundaries drops a pending left/right halfword and causes
     * audible clicks/chopping.
     */
    s->audio_frames = s->audio_read_frames = 0;
    s->audio_boundary_head = s->audio_boundary_count = 0;
}

typedef struct s3c2400_state_image {
    uint8_t bios[BIOS_SIZE];
    size_t ram_size;
    uint32_t buttons;
    uint32_t fb[320 * 240];
    uint32_t fb_w, fb_h;
    uint64_t frame_counter;
    uint32_t lcd_vpos;
    uint64_t lcd_line_accum;
    uint8_t eeprom[0x2000];
    uint8_t iic_data[4];
    int iic_data_index;
    uint16_t iic_address;
    uint32_t lcd_regs[0x400/4];
    uint16_t lcd_palette[0x400/2];
    uint32_t memcon[0x34/4];
    uint32_t usb_host[0x5c/4];
    uint32_t irq[0x18/4];
    uint32_t dma[0x7c/4];
    uint32_t clkpow[0x18/4];
    uint32_t uart0[0x2c/4];
    uint32_t uart1[0x2c/4];
    uint32_t pwm[0x44/4];
    uint64_t pwm_accum[5];
    uint32_t usb_dev[0xbc/4];
    uint32_t watchdog[0x0c/4];
    uint32_t iic[0x10/4];
    uint32_t iis[0x14/4];
    uint16_t iis_fifo[2];
    unsigned iis_fifo_index;
    uint64_t iis_accum;
    uint64_t audio_frames;
    uint32_t audio_sample_rate_hz;
    uint32_t iis_cached_rate_hz;
    uint64_t iis_cached_period_cycles;
    uint8_t iis_clock_dirty;
    uint32_t gpio[0x60/4];
    uint32_t rtc[0x4c/4];
    uint32_t adc[0x08/4];
    uint32_t spi[0x18/4];
    uint32_t mmc[0x40/4];
    lcd_state_t lcd;
    gp32_smc_lines_t smc_lines;
} s3c2400_state_image_t;

int s3c2400_state_save_io(const s3c2400_t *s, state_io_t *io) {
    if (!s || !io || s->audio_frames > SIZE_MAX / (2u * sizeof(int16_t))) return 0;
#ifdef GP32EMU_WASM
    static s3c2400_state_image_t st_storage;
    s3c2400_state_image_t *st = &st_storage;
#else
    s3c2400_state_image_t st_storage;
    s3c2400_state_image_t *st = &st_storage;
#endif
    memset(st, 0, sizeof(*st));
    memcpy(st->bios, s->bios, sizeof(st->bios));
    st->ram_size = s->ram_size;
    st->buttons = s->buttons;
    memcpy(st->fb, s->fb, sizeof(st->fb));
    st->fb_w = s->fb_w; st->fb_h = s->fb_h;
    st->frame_counter = s->frame_counter;
    st->lcd_vpos = s->lcd_vpos;
    /* Retain legacy RUN/60 phase in the old body. v7 appends exact phase. */
    st->lcd_line_accum = s->lcd_line_accum;
    if (lcd_is_tft(s->lcd_regs)) {
        uint64_t period = lcd_tft_line_period(s->lcd_regs) * lcd_tft_total_lines(s->lcd_regs);
        uint64_t legacy_period = ((uint64_t)s3c2400_run_clock_hz(s) + 30u) / 60u;
        st->lcd_line_accum = (s->lcd_line_accum % period) * legacy_period / period;
    }
    memcpy(st->eeprom, s->eeprom, sizeof(st->eeprom));
    memcpy(st->iic_data, s->iic_data, sizeof(st->iic_data));
    st->iic_data_index = s->iic_data_index;
    st->iic_address = s->iic_address;
    memcpy(st->lcd_regs, s->lcd_regs, sizeof(st->lcd_regs));
    memcpy(st->lcd_palette, s->lcd_palette, sizeof(st->lcd_palette));
    memcpy(st->memcon, s->memcon, sizeof(st->memcon));
    memcpy(st->usb_host, s->usb_host, sizeof(st->usb_host));
    memcpy(st->irq, s->irq, sizeof(st->irq));
    memcpy(st->dma, s->dma, sizeof(st->dma));
    memcpy(st->clkpow, s->clkpow, sizeof(st->clkpow));
    memcpy(st->uart0, s->uart0, sizeof(st->uart0));
    memcpy(st->uart1, s->uart1, sizeof(st->uart1));
    memcpy(st->pwm, s->pwm, sizeof(st->pwm));
    memcpy(st->pwm_accum, s->pwm_accum, sizeof(st->pwm_accum));
    memcpy(st->usb_dev, s->usb_dev, sizeof(st->usb_dev));
    memcpy(st->watchdog, s->watchdog, sizeof(st->watchdog));
    memcpy(st->iic, s->iic, sizeof(st->iic));
    memcpy(st->iis, s->iis, sizeof(st->iis));
    memcpy(st->iis_fifo, s->iis_fifo, sizeof(st->iis_fifo));
    st->iis_fifo_index = s->iis_fifo_index;
    /* Keep the legacy body in CPU-cycle units. v6 appends the exact phase. */
    st->iis_accum = s->iis_accum / iis_frame_rate_hz(s);
    st->audio_frames = s->audio_frames - s->audio_read_frames;
    st->audio_sample_rate_hz = s->audio_sample_rate_hz;
    st->iis_cached_rate_hz = s->iis_cached_rate_hz;
    st->iis_cached_period_cycles = s->iis_cached_period_cycles;
    st->iis_clock_dirty = s->iis_clock_dirty;
    memcpy(st->gpio, s->gpio, sizeof(st->gpio));
    memcpy(st->rtc, s->rtc, sizeof(st->rtc));
    memcpy(st->adc, s->adc, sizeof(st->adc));
    memcpy(st->spi, s->spi, sizeof(st->spi));
    memcpy(st->mmc, s->mmc, sizeof(st->mmc));
    st->lcd = s->lcd;
    st->smc_lines = s->smc_lines;
    if (!state_io_write(io, st, sizeof(*st))) return 0;
    if (!state_io_write(io, s->ram, st->ram_size)) return 0;
    if (!smc_state_save_io(s->smc, io)) return 0;
    const int16_t *unread = s->audio ? s->audio + (size_t)s->audio_read_frames * 2u : NULL;
    if (!state_io_write(io, unread, (size_t)st->audio_frames * 2u * sizeof(int16_t))) return 0;
    uint32_t count = s->audio_boundary_count - s->audio_boundary_head;
    if (!state_io_write(io, &count, sizeof(count))) return 0;
    for (uint32_t i = s->audio_boundary_head; i < s->audio_boundary_count; ++i) {
        audio_boundary_t span = s->audio_boundaries[i];
        span.end_frame -= s->audio_read_frames;
        if (!state_io_write(io, &span, sizeof(span))) return 0;
    }
    uint64_t lcd_phase[2] = {s->lcd_line_accum, s->lcd_hclk_remainder};
    uint8_t codec[6] = {s->codec.address, s->codec.shift, s->codec.bits,
                       s->codec.volume, s->codec.control, s->codec.status};
    return state_io_write(io, &s->iis_accum, sizeof(s->iis_accum)) &&
           state_io_write(io, lcd_phase, sizeof(lcd_phase)) &&
           state_io_write(io, &s->audio_idle_phase, sizeof(s->audio_idle_phase)) &&
           state_io_write(io, codec, sizeof(codec));
}

int s3c2400_state_load_io_checked(s3c2400_t *s, state_io_t *io, int has_audio_spans, int has_iis_phase, int has_lcd_phase, int has_idle_phase, int has_codec,
                                uint32_t expected_ram_size, uint32_t expected_run_clock, uint32_t required_ram_size,
                                smc_state_format_t card_format) {
    if (!s || !io) return 0;
#ifdef GP32EMU_WASM
    static s3c2400_state_image_t st_storage;
    s3c2400_state_image_t *st = &st_storage;
#else
    s3c2400_state_image_t st_storage;
    s3c2400_state_image_t *st = &st_storage;
#endif
    if (!state_io_read(io, st, sizeof(*st))) return 0;
    if (st->ram_size == 0u || st->ram_size > (size_t)64u * 1024u * 1024u || st->audio_frames > (uint64_t)10u * 60u * 44100u) return 0;
    if ((expected_ram_size && st->ram_size != expected_ram_size) || st->ram_size < required_ram_size) return 0;
    /* Restored indices address fixed arrays on later IIC/IIS register writes.
     * An out-of-range value turns the next guest write into an out-of-bounds
     * store inside this heap object, so the image must be rejected here. */
    if (st->iis_fifo_index >= GP32_ARRAY_COUNT(st->iis_fifo) || st->iic_data_index < 0) return 0;
    /* Every IIC index above the 4-byte address stage shares one meaning; a
     * larger stored value can only overflow iic_data_index++ at INT_MAX. */
    if (st->iic_data_index > 4) st->iic_data_index = 4;
    uint8_t *new_ram = (uint8_t *)malloc(st->ram_size);
    if (!new_ram) return 0;
    if (!state_io_read(io, new_ram, st->ram_size)) { free(new_ram); return 0; }
    /* SmartMedia precedes audio in v0002. The section is parsed and validated
     * against the mounted card and the state base before the live device is
     * touched, so a truncated stream or a stream built on a different frontend
     * image cannot leave a half-replaced card. */
    smc_state_stage_t *card = smc_state_stage_begin(s->smc, io, card_format);
    if (!card) { free(new_ram); return 0; }
    int16_t *new_audio = NULL;
    uint64_t new_audio_cap = 0;
    if (st->audio_frames) {
        new_audio = (int16_t *)malloc((size_t)st->audio_frames * 2u * sizeof(int16_t));
        if (!new_audio) { smc_state_stage_destroy(card); free(new_ram); return 0; }
        if (!state_io_read(io, new_audio, (size_t)st->audio_frames * 2u * sizeof(int16_t))) { free(new_audio); smc_state_stage_destroy(card); free(new_ram); return 0; }
        new_audio_cap = st->audio_frames;
    }
    uint32_t span_count = 0;
    audio_boundary_t *new_spans = NULL;
    if (has_audio_spans) {
        if (!state_io_read(io, &span_count, sizeof(span_count)) ||
            span_count > st->audio_frames || span_count > SIZE_MAX / sizeof(*new_spans)) goto bad_audio;
        if (span_count) {
            new_spans = malloc((size_t)span_count * sizeof(*new_spans));
            if (!new_spans || !state_io_read(io, new_spans, (size_t)span_count * sizeof(*new_spans))) goto bad_audio;
            uint64_t previous = 0;
            for (uint32_t i = 0; i < span_count; ++i) {
                if (new_spans[i].end_frame <= previous || new_spans[i].end_frame >= st->audio_frames ||
                    !new_spans[i].rate_hz || new_spans[i].reserved) goto bad_audio;
                previous = new_spans[i].end_frame;
            }
        }
    }
    uint64_t phase = 0;
    if (has_iis_phase && !state_io_read(io, &phase, sizeof(phase))) goto bad_audio;
    /* Leave room for a maximum-width tick before committing any state. */
    if (phase > UINT64_MAX - (uint64_t)UINT32_MAX * 96000u ||
        st->iis_accum > UINT64_MAX / 96000u - UINT32_MAX) goto bad_audio;
    uint64_t lcd_phase[2] = {st->lcd_line_accum, 0};
    uint32_t fclk = pll_frequency(st->clkpow[MPLLCON]);
    uint32_t hclk = (st->clkpow[5] & 2u) ? fclk / 2u : fclk;
    uint32_t runclk = run_clock_values(fclk, hclk);
    if (expected_run_clock && runclk != expected_run_clock) goto bad_audio;
    uint64_t legacy_period = ((uint64_t)runclk + 30u) / 60u;
    if (!legacy_period) legacy_period = 1u;
    uint64_t lcd_period = lcd_is_tft(st->lcd_regs) ?
        lcd_tft_line_period(st->lcd_regs) * lcd_tft_total_lines(st->lcd_regs) : legacy_period;
    if (has_lcd_phase) {
        if (!state_io_read(io, lcd_phase, sizeof(lcd_phase)) ||
            lcd_phase[1] >= runclk ||
            (lcd_is_tft(st->lcd_regs) ? lcd_phase[0] >= lcd_period :
             lcd_phase[0] > UINT64_MAX - UINT32_MAX)) goto bad_audio;
    } else if (lcd_is_tft(st->lcd_regs)) {
        /* Legacy files cannot reconstruct the missing hardware phase. Keep
         * their normalized scan position as an explicit approximate migration. */
        lcd_phase[0] = (st->lcd_line_accum % legacy_period) * lcd_period / legacy_period;
    }
    uint64_t idle_phase = 0;
    if (has_idle_phase && (!state_io_read(io, &idle_phase, sizeof(idle_phase)) || idle_phase >= runclk)) goto bad_audio;
    uint8_t codec[6] = {0};
    if (has_codec && (!state_io_read(io, codec, sizeof(codec)) || codec[2] > 8u || codec[3] > 63u)) goto bad_audio;
    smc_state_stage_commit(s->smc, card);
    free(s->ram);
    free(s->audio);
    s->ram = new_ram;
    s->ram_size = st->ram_size;
    s->audio = new_audio;
    s->audio_cap_frames = new_audio_cap;
    free(s->audio_boundaries);
    s->audio_boundaries = new_spans;
    s->audio_boundary_count = s->audio_boundary_cap = span_count;
    s->audio_read_frames = s->audio_boundary_head = 0;
    memcpy(s->bios, st->bios, sizeof(s->bios));
    s->buttons = st->buttons;
    buttons_refresh(s);
    memcpy(s->fb, st->fb, sizeof(s->fb));
    s->fb_w = st->fb_w; s->fb_h = st->fb_h;
    s->frame_counter = st->frame_counter;
    s->lcd_vpos = st->lcd_vpos;
    s->lcd_line_accum = lcd_phase[0];
    s->lcd_hclk_remainder = (uint32_t)lcd_phase[1];
    s->audio_idle_phase = idle_phase;
    s->codec = (gp32_codec_t){codec[0], codec[1], codec[2], codec[3], codec[4], codec[5]};
    s->codec_gain_q16 = gp32_codec_gain_q16(&s->codec);
    s->lcd_line_valid = 0;
    s->lcd_timing_valid = 0;
    memcpy(s->eeprom, st->eeprom, sizeof(s->eeprom));
    memcpy(s->iic_data, st->iic_data, sizeof(s->iic_data));
    s->iic_data_index = st->iic_data_index;
    s->iic_address = st->iic_address;
    memcpy(s->lcd_regs, st->lcd_regs, sizeof(s->lcd_regs));
    memcpy(s->lcd_palette, st->lcd_palette, sizeof(s->lcd_palette));
    memcpy(s->memcon, st->memcon, sizeof(s->memcon));
    memcpy(s->usb_host, st->usb_host, sizeof(s->usb_host));
    memcpy(s->irq, st->irq, sizeof(s->irq));
    memcpy(s->dma, st->dma, sizeof(s->dma));
    memcpy(s->clkpow, st->clkpow, sizeof(s->clkpow));
    clock_refresh_values(s);
    memcpy(s->uart0, st->uart0, sizeof(s->uart0));
    memcpy(s->uart1, st->uart1, sizeof(s->uart1));
    memcpy(s->pwm, st->pwm, sizeof(s->pwm));
    memcpy(s->pwm_accum, st->pwm_accum, sizeof(s->pwm_accum));
    s->pwm_clock_dirty = 1;
    memcpy(s->usb_dev, st->usb_dev, sizeof(s->usb_dev));
    memcpy(s->watchdog, st->watchdog, sizeof(s->watchdog));
    memcpy(s->iic, st->iic, sizeof(s->iic));
    memcpy(s->iis, st->iis, sizeof(s->iis));
    memcpy(s->iis_fifo, st->iis_fifo, sizeof(s->iis_fifo));
    s->iis_fifo_index = st->iis_fifo_index;
    s->iis_accum = has_iis_phase ? phase : st->iis_accum * iis_frame_rate_hz(s);
    s->iis_cached_run_hz = s3c2400_run_clock_hz(s);
    s->audio_frames = st->audio_frames;
    s->audio_sample_rate_hz = st->audio_sample_rate_hz ? st->audio_sample_rate_hz : 44100u;
    s->iis_cached_rate_hz = st->iis_cached_rate_hz;
    s->iis_cached_period_cycles = st->iis_cached_period_cycles;
    s->iis_clock_dirty = st->iis_clock_dirty;
    memcpy(s->gpio, st->gpio, sizeof(s->gpio));
    memcpy(s->rtc, st->rtc, sizeof(s->rtc));
    memcpy(s->adc, st->adc, sizeof(s->adc));
    memcpy(s->spi, st->spi, sizeof(s->spi));
    memcpy(s->mmc, st->mmc, sizeof(s->mmc));
    s->lcd = st->lcd;
    s->smc_lines = st->smc_lines;
    /* The new card object, restored GPIO and SmartMedia lines must be visible
     * to the live mirrors before any IRQ can resume the CPU. */
    live_read32_refresh(s);
    check_irq(s);
    return 1;
bad_audio:
    free(new_spans);
    free(new_audio);
    smc_state_stage_destroy(card);
    free(new_ram);
    return 0;
}

int s3c2400_state_load_io(s3c2400_t *s, state_io_t *io, int has_audio_spans, int has_iis_phase, int has_lcd_phase, int has_idle_phase, int has_codec) {
    return s3c2400_state_load_io_checked(s, io, has_audio_spans, has_iis_phase, has_lcd_phase, has_idle_phase, has_codec, 0u, 0u, 0u,
                                         SMC_STATE_FORMAT_V14);
}

int s3c2400_state_save(const s3c2400_t *s, FILE *f) {
    state_io_t io = state_io_file(f);
    return state_io_write(&io, "GP32SOC9", 8u) && s3c2400_state_save_io(s, &io);
}

int s3c2400_state_load(s3c2400_t *s, FILE *f) {
    uint8_t magic[8];
    if (!f || fread(magic, 1, sizeof(magic), f) != sizeof(magic)) return 0;
    int has_codec = !memcmp(magic, "GP32SOC9", sizeof(magic));
    int has_idle = has_codec || !memcmp(magic, "GP32SOC8", sizeof(magic));
    int has_lcd = has_idle || !memcmp(magic, "GP32SOC7", sizeof(magic));
    int has_phase = has_lcd || !memcmp(magic, "GP32SOC6", sizeof(magic));
    if (!has_phase && fseek(f, -(long)sizeof(magic), SEEK_CUR)) return 0;
    state_io_t io = state_io_file(f);
    return s3c2400_state_load_io(s, &io, 1, has_phase, has_lcd, has_idle, has_codec);
}
