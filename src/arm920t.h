#ifndef GP32EMU_ARM920T_H
#define GP32EMU_ARM920T_H

#include "common.h"
#include "state_io.h"
#include "gp32emu/gp32.h" /* gp32_cpu_profile_t */

typedef struct arm920t arm920t_t;

typedef uint8_t  (*arm_read8_fn)(void *user, uint32_t addr);
typedef uint16_t (*arm_read16_fn)(void *user, uint32_t addr);
typedef uint32_t (*arm_read32_fn)(void *user, uint32_t addr);
typedef void (*arm_write8_fn)(void *user, uint32_t addr, uint8_t value);
typedef void (*arm_write16_fn)(void *user, uint32_t addr, uint16_t value);
typedef void (*arm_write32_fn)(void *user, uint32_t addr, uint32_t value);
typedef uint8_t *(*arm_fastmem_fn)(void *user, uint32_t addr, size_t bytes, int write);
typedef void (*arm_log_fn)(void *user, const char *line);
typedef int (*arm_swi_fn)(void *user, arm920t_t *cpu, uint32_t imm, uint32_t pc, int thumb);

typedef struct arm_bus {
    arm_read8_fn read8;
    arm_read16_fn read16;
    arm_read32_fn read32;
    /* Optional direct physical-I/O word read used by the x64 JIT for identity-mapped S3C2400 MMIO. */
    arm_read32_fn read32_io;
    arm_write8_fn write8;
    arm_write16_fn write16;
    arm_write32_fn write32;
    arm_fastmem_fn fastmem;
    void *user;
    /* Optional: aligned physical word reads which are side-effect-free and
     * constant until arm920t_run returns (no peripheral ticks within a run). */
    int (*is_stable_read32)(void *user, uint32_t addr);
    /* Optional direct physical-I/O word write used for the identity-mapped
     * S3C2400 MMIO window (0x14000000..0x16000000) once the address is proven
     * aligned and outside every RAM/BIOS window. For such an address it must do
     * exactly what write32 does; buses may leave it NULL. Kept last so every
     * earlier bus entry keeps its offset. */
    arm_write32_fn write32_io;
} arm_bus_t;

typedef struct arm_live_read32 {
    uint32_t pa;
    const volatile uint32_t *word;
} arm_live_read32_t;

/* Optional certificate for side-effect-free physical word reads. Each aligned,
 * host-order word must always equal the ordinary bus read, without observers,
 * logging, CPU mutation or deadline changes. Values are read anew per access;
 * volatile is not synchronization: CPU/bus mutation remains synchronous.
 * The immutable descriptors and word storage must outlive this registration.
 * Register/replace/disable only outside arm920t_run; success flushes native
 * code, reset/state load preserve registration. NULL + zero disables it.
 * Returns zero without changes for invalid arguments or a running CPU.
 * Decorated buses must explicitly opt in only if they preserve this contract. */
int arm920t_set_live_read32(arm920t_t *cpu, const arm_live_read32_t *reads, size_t count);

arm920t_t *arm920t_create(const arm_bus_t *bus);
void arm920t_destroy(arm920t_t *cpu);
void arm920t_reset(arm920t_t *cpu, uint32_t vector);
uint32_t arm920t_run(arm920t_t *cpu, uint32_t cycles);
/* From a synchronous bus/SWI callback: shrink this run's total cycle budget,
 * measured from its start, never from the callback. Outside a run: no-op.
 * The in-flight instruction completes; an already-passed deadline cannot
 * undo elapsed cycles. No following instruction runs beyond the new limit.
 * The limit is transient, reset by every arm920t_run, and never serialized. */
void arm920t_limit_run(arm920t_t *cpu, uint32_t max_cycles_from_run_start);
/* From a bus/SWI handler: end the current run after this instruction. The
 * following run resumes normally; this does not request a guest CPU halt. */
void arm920t_stop_run(arm920t_t *cpu);
int arm920t_is_running(const arm920t_t *cpu);
void arm920t_add_idle_cycles(arm920t_t *cpu, uint32_t cycles);
void arm920t_set_jit(arm920t_t *cpu, int enabled);
void arm920t_flush_jit(arm920t_t *cpu);
void arm920t_set_irq(arm920t_t *cpu, int state);
void arm920t_set_fiq(arm920t_t *cpu, int state);
void arm920t_set_trace(arm920t_t *cpu, int enabled, arm_log_fn log, void *user);
void arm920t_set_swi_handler(arm920t_t *cpu, arm_swi_fn fn, void *user);
void arm920t_set_reg(arm920t_t *cpu, unsigned reg, uint32_t value);
void arm920t_set_cpsr(arm920t_t *cpu, uint32_t value);
uint32_t arm920t_get_pc(const arm920t_t *cpu);
uint64_t arm920t_get_cycles(const arm920t_t *cpu);
uint32_t arm920t_get_reg(const arm920t_t *cpu, unsigned reg);
uint32_t arm920t_get_cpsr(const arm920t_t *cpu);
uint32_t arm920t_get_cp15(const arm920t_t *cpu, unsigned reg);
uint64_t arm920t_get_jit_hits(const arm920t_t *cpu);
uint64_t arm920t_get_jit_misses(const arm920t_t *cpu);
uint64_t arm920t_get_jit_fallbacks(const arm920t_t *cpu);
void arm920t_get_cpu_profile(const arm920t_t *cpu, gp32_cpu_profile_t *out);
void arm920t_reset_cpu_profile(arm920t_t *cpu);
/* HLE guest calls preserve CPU registers across all modes while retaining
 * their memory, peripheral, CP15 and cycle-count effects. Not a wire image. */
typedef struct arm920t_register_context {
    uint32_t r[16], cpsr;
    uint32_t bank_usr[7], bank_fiq[7];
    uint32_t bank_svc[2], bank_abt[2], bank_irq[2], bank_und[2];
    uint32_t spsr_fiq, spsr_svc, spsr_abt, spsr_irq, spsr_und;
} arm920t_register_context_t;
void arm920t_get_register_context(const arm920t_t *cpu, arm920t_register_context_t *out);
void arm920t_set_register_context(arm920t_t *cpu, const arm920t_register_context_t *saved);

/* Fixed v0002 wire image; stage it before committing a whole-machine load. */
typedef struct arm920t_state_image {
    uint32_t r[16];
    uint32_t cpsr;
    uint32_t bank_usr[7];
    uint32_t bank_fiq[7];
    uint32_t bank_svc[2];
    uint32_t bank_abt[2];
    uint32_t bank_irq[2];
    uint32_t bank_und[2];
    uint32_t spsr_fiq, spsr_svc, spsr_abt, spsr_irq, spsr_und;
    uint32_t cp15[16];
    uint32_t tlb_va_base[4096];
    uint32_t tlb_pa_base[4096];
    uint32_t tlb_mask[4096];
    uint8_t tlb_valid[4096];
    uint64_t cycles_total;
    int irq_line, fiq_line;
    int halted;
} arm920t_state_image_t;

void arm920t_state_apply(arm920t_t *cpu, const arm920t_state_image_t *state);
int arm920t_state_save(const arm920t_t *cpu, FILE *f);
int arm920t_state_load(arm920t_t *cpu, FILE *f);
int arm920t_state_save_io(const arm920t_t *cpu, state_io_t *io);
int arm920t_state_load_io(arm920t_t *cpu, state_io_t *io);

#endif
