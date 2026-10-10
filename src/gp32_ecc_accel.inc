/* Equivalent software NAND parity loop. The guest prologue and the remaining
 * ECC calculation execute normally. Complete iterations keep their original
 * instruction count, flags and scratch writes; partial budgets use ARM code. */
#define GP32_ECC_LOOP_PC 0x0c048f48u
#define GP32_ECC_LOOP_BYTES 0x160u

static uint64_t gp32_ecc_code_hash(const uint8_t *code, unsigned bytes) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (unsigned i = 0; i < bytes; ++i)
        hash = (hash ^ code[i]) * UINT64_C(1099511628211);
    return hash;
}
static unsigned gp32_ecc_nibble_parity(unsigned n) {
    n ^= n >> 2; n ^= n >> 1;
    return n & 1u;
}
static int gp32_ecc_overlap(uint32_t a, unsigned n, uint32_t b, unsigned m) {
    return (uint64_t)a < (uint64_t)b + m && (uint64_t)b < (uint64_t)a + n;
}
static uint32_t gp32_ecc_accelerate(void *user, arm920t_t *cpu, uint32_t budget) {
    (void)user;
    uint32_t fp = arm920t_get_reg(cpu, 11u);
    if ((fp & 3u) || budget < 57u) return 0;
    uint32_t frame_addr = fp - 0x16cu;
    if (gp32_ecc_overlap(frame_addr, 0x170u, 0x0c048d70u, 0x1274u)) return 0;
    uint8_t *frame = arm920t_peek_identity_ram(cpu, frame_addr, 0x170u, 1);
    const uint8_t *code = arm920t_peek_identity_ram(cpu, GP32_ECC_LOOP_PC, GP32_ECC_LOOP_BYTES, 0);
    if (!frame || !code || gp32_ecc_code_hash(code, GP32_ECC_LOOP_BYTES) != UINT64_C(0xeddcac638c51a003)) return 0;
    unsigned index = gp32_ld32le(frame + 340u);
    uint32_t input = gp32_ld32le(frame + 348u), cursor = gp32_ld32le(frame + 8u);
    if (index >= 256u || cursor != frame_addr + 80u + index ||
        gp32_ecc_overlap(input, 256u - index, frame_addr, 0x170u)) return 0;
    const uint8_t *data = arm920t_peek_identity_ram(cpu, input, 256u - index, 0);
    if (!data) return 0;
    /* These live copies were produced by the original guest memcpy calls.
     * Recheck them after IRQs or state loads rather than trusting boot data. */
    for (unsigned n = 0; n < 16u; ++n) {
        unsigned p = gp32_ecc_nibble_parity(n);
        if (frame[44u+n] != (p ^ 1u) || frame[60u+n] != p) return 0;
    }
    unsigned done = 0, consumed = 0, last_parity = 0, last_high = 0;
    while (index < 256u) {
        unsigned value = data[consumed], high = value >> 4;
        unsigned high_parity = gp32_ecc_nibble_parity(high), cost = 58u - high_parity;
        if (cost > budget - done) break;
        last_parity = high_parity ^ gp32_ecc_nibble_parity(value & 15u);
        last_high = high;
        frame[42] ^= (uint8_t)value;
        frame[78] = value & 15u; frame[79] = (uint8_t)high;
        frame[80u+index] = (uint8_t)last_parity;
        ++index; ++consumed; ++cursor; ++input;
        gp32_st32le(frame + 340u, index);
        gp32_st32le(frame + 8u, cursor);
        gp32_st32le(frame + 348u, input);
        done += cost;
    }
    if (done) {
        arm920t_set_reg(cpu, 0u, fp - 12u);
        arm920t_set_reg(cpu, 1u, fp - 12u);
        arm920t_set_reg(cpu, 12u, fp - 12u);
        arm920t_set_reg(cpu, 2u, last_parity);
        arm920t_set_reg(cpu, 3u, input);
        arm920t_set_cpsr(cpu, (arm920t_get_cpsr(cpu) & 0x0fffffffu) |
                            (last_high == 15u ? 0x60000000u : 0x80000000u));
    }
    return done;
}

/* The final row fold XORs the first 128 parity bytes into one frame byte.
 * Its exit path still runs as ARM code, including the outer counter update. */
static uint32_t gp32_ecc_row_accelerate(void *user, arm920t_t *cpu, uint32_t budget) {
    (void)user;
    uint32_t fp = arm920t_get_reg(cpu, 11u);
    if ((fp & 3u) || budget < 20u) return 0;
    uint32_t frame_addr = fp - 0x16cu;
    if (gp32_ecc_overlap(frame_addr, 0x170u, 0x0c048d70u, 0x1274u)) return 0;
    uint8_t *frame = arm920t_peek_identity_ram(cpu, frame_addr, 0x170u, 1);
    const uint8_t *code = arm920t_peek_identity_ram(cpu, 0x0c049868u, 0x6cu, 0);
    if (!frame || !code) return 0;
    if (gp32_ecc_code_hash(code, 0x6cu) != UINT64_C(0x74cc1787c7491938)) return 0;
    unsigned index = gp32_ld32le(frame + 336u);
    if (index >= 128u || gp32_ld32le(frame + 8u) != frame_addr + 80u) return 0;
    unsigned steps = budget / 20u;
    if (steps > 128u - index) steps = 128u - index;
    unsigned old_acc = 0, last_index = index;
    for (unsigned i = 0; i < steps; ++i) {
        old_acc = frame[12]; last_index = index;
        frame[12] ^= frame[80u + index++];
    }
    gp32_st32le(frame + 336u, index);
    arm920t_set_reg(cpu, 0u, 0xfffffeacu);
    arm920t_set_reg(cpu, 1u, fp - 12u);
    arm920t_set_reg(cpu, 2u, old_acc);
    arm920t_set_reg(cpu, 3u, index);
    arm920t_set_reg(cpu, 12u, fp - 12u);
    arm920t_set_cpsr(cpu, (arm920t_get_cpsr(cpu) & 0x0fffffffu) |
                        (last_index == 127u ? 0x60000000u : 0x80000000u));
    return steps * 20u;
}

/* The even parity bytes are folded separately. Each complete iteration costs
 * 21 instructions; the final failing comparison stays in original ARM code. */
[[maybe_unused]] static uint32_t gp32_ecc_stride2_accelerate(void *user, arm920t_t *cpu, uint32_t budget) {
    (void)user;
    uint32_t fp = arm920t_get_reg(cpu, 11u);
    if ((fp & 3u) || budget < 21u) return 0;
    uint32_t frame_addr = fp - 0x16cu;
    if (gp32_ecc_overlap(frame_addr, 0x170u, 0x0c048d70u, 0x1274u)) return 0;
    uint8_t *frame = arm920t_peek_identity_ram(cpu, frame_addr, 0x170u, 1);
    const uint8_t *code = arm920t_peek_identity_ram(cpu, 0x0c04946cu, 0x58u, 0);
    if (!frame || !code || gp32_ecc_code_hash(code, 0x58u) != UINT64_C(0x590e2cefc3a8e5d3)) return 0;
    unsigned index = gp32_ld32le(frame + 340u);
    if (index >= 256u || (index & 1u) ||
        gp32_ld32le(frame + 8u) != frame_addr + 80u + index) return 0;
    unsigned steps = budget / 21u;
    if (steps > (256u - index) / 2u) steps = (256u - index) / 2u;
    unsigned old_acc = 0;
    for (unsigned i = 0; i < steps; ++i) {
        old_acc = frame[26];
        frame[26] ^= frame[80u + index];
        index += 2u;
    }
    gp32_st32le(frame + 340u, index);
    gp32_st32le(frame + 8u, frame_addr + 80u + index);
    arm920t_set_reg(cpu, 0u, fp - 12u);
    arm920t_set_reg(cpu, 1u, old_acc);
    arm920t_set_reg(cpu, 2u, 0xfffffebau);
    arm920t_set_reg(cpu, 3u, frame_addr + 80u + index);
    arm920t_set_reg(cpu, 12u, fp - 12u);
    /* The last completed comparison saw the old even index <=254, so N=1
     * even when the updated counter is 256 at the next loop entry. */
    arm920t_set_cpsr(cpu, (arm920t_get_cpsr(cpu) & 0x0fffffffu) | 0x80000000u);
    return steps * 21u;
}
static const arm_accel_entry_t gp32_ecc_entries[] = {
    { GP32_ECC_LOOP_PC, gp32_ecc_accelerate },
    { 0x0c049868u, gp32_ecc_row_accelerate },
#if defined(__arm__) || defined(_M_ARM) || defined(__aarch64__) || defined(_M_ARM64)
    /* ARM measurements benefit from the fold. Keep the original x64 trace:
     * the added callback did not improve its complete-scene measurements. */
    { 0x0c04946cu, gp32_ecc_stride2_accelerate }
#endif
};
