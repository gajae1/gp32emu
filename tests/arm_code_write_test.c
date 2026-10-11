/* Non-CPU writers must not leave translated code behind. A guest calls X,
 * replaces X through memory-to-memory DMA, an HLE file read or an HLE .gpg
 * autoload, then calls X again without any CP15 c7 operation. With the
 * I-cache disabled (CP15 c1 bit 12 clear) ARM920T fetches from memory, so the
 * second call must run the new code. Both a re-entered block that traces into
 * X and an indirectly called X block are checked, with the JIT on and off.
 * DMA edge cases: a single-service transfer that delivers one unit, a write
 * that starts in a page without code and ends in X's page, and X executed
 * through an MMU alias while DMA writes its physical address. */
#include "../src/gp32.c"

#define M_BASE   (GP32_RAM_BASE + 0x1000u)
#define LIT_POOL (GP32_RAM_BASE + 0x1800u)
#define STUBS    (GP32_RAM_BASE + 0x2000u)
#define SRC_CODE (GP32_RAM_BASE + 0x3000u)
#define PATH_BIN (GP32_RAM_BASE + 0x4000u)
#define PATH_GPG (GP32_RAM_BASE + 0x4100u)
#define LITERAL  (GP32_RAM_BASE + 0x4204u)
#define SLOT     (GP32_RAM_BASE + 0x4300u)
#define HANDLE   (GP32_RAM_BASE + 0x4400u)
#define COUNT    (GP32_RAM_BASE + 0x4404u)
#define SIZEOUT  (GP32_RAM_BASE + 0x4408u)
#define RES      (GP32_RAM_BASE + 0x4500u)
#define X_ADDR   (GP32_RAM_BASE + 0x8000u)
#define TTB      (GP32_RAM_BASE + 0x10000u)
#define X_ALIAS  (X_ADDR + 0x00100000u) /* section 0x0c1 mapped onto 0x0c0 */

#define OLD0 0xe3a00011u /* mov r0,#0x11 */
#define NEW0 0xe3a00022u /* mov r0,#0x22 */
#define BXLR 0xe12fff1eu

enum { PATH_DMA, PATH_FILE, PATH_AUTOLOAD, PATH_DMA_PARTIAL, PATH_DMA_SPAN, PATH_DMA_MMU };
static const char *const path_name[] = {"dma", "file-read", "autoload", "dma-partial", "dma-page-span", "dma-mmu-alias"};

typedef struct { gp32_t *g; uint32_t pc; uint32_t nlit; } as_t;
static void emit(as_t *a, uint32_t w) { s3c2400_write32(a->g->soc, a->pc, w); a->pc += 4u; }
static void ldr_const(as_t *a, unsigned rd, uint32_t v) {
    uint32_t lit = LIT_POOL + 4u * a->nlit++;
    s3c2400_write32(a->g->soc, lit, v);
    emit(a, 0xe59f0000u | (rd << 12) | (lit - (a->pc + 8u)));
}
static uint32_t branch(uint32_t op, uint32_t pc, uint32_t target) {
    return op | ((uint32_t)((int32_t)(target - (pc + 8u)) >> 2) & 0x00ffffffu);
}
static void put_str(gp32_t *g, uint32_t at, const char *s) {
    for (size_t i = 0; i <= strlen(s); ++i) s3c2400_write8(g->soc, at + (uint32_t)i, (uint8_t)s[i]);
}

static void emit_dma(as_t *a, uint32_t src, uint32_t dst, uint32_t dcon) {
    ldr_const(a, 1, 0x14600000u);                                  /* DMA channel 0 */
    ldr_const(a, 2, src); emit(a, 0xe5812000u);                    /* DISRC0 */
    ldr_const(a, 2, dst); emit(a, 0xe5812004u);                    /* DIDST0 */
    ldr_const(a, 2, dcon); emit(a, 0xe5812008u);                   /* DCON0 */
    emit(a, 0xe3a02003u); emit(a, 0xe5812018u);                    /* DMASKTRIG0 = ON | SW_TRIG */
}

static void emit_replace(as_t *a, int path) {
    if (path == PATH_DMA || path == PATH_DMA_MMU) {
        emit_dma(a, SRC_CODE, X_ADDR, 0x04600002u);                /* whole service, software, word, 2 */
    } else if (path == PATH_DMA_PARTIAL) {
        emit_dma(a, SRC_CODE, X_ADDR, 0x00600002u);                /* single service: one of two words */
    } else if (path == PATH_DMA_SPAN) {
        emit_dma(a, SRC_CODE - 4u, X_ADDR - 4u, 0x04600002u);      /* last word of page 7, first of X */
    } else if (path == PATH_FILE) {
        ldr_const(a, 0, PATH_BIN); emit(a, 0xe3a01001u); ldr_const(a, 2, HANDLE);
        emit(a, branch(0xeb000000u, a->pc, STUBS));                /* open */
        ldr_const(a, 1, HANDLE); emit(a, 0xe5910000u);
        ldr_const(a, 1, X_ADDR); emit(a, 0xe3a02008u); ldr_const(a, 3, COUNT);
        emit(a, branch(0xeb000000u, a->pc, STUBS + 4u));           /* read 8 bytes over X */
    } else {
        ldr_const(a, 0, PATH_GPG); ldr_const(a, 1, SIZEOUT);
        emit(a, branch(0xeb000000u, a->pc, STUBS + 8u));           /* size query runs the autoload */
    }
}

static int one(int path, int indirect, int jit) {
    static uint8_t bin[8], gpg[8u + 256u];
    gp32_st32le(bin, NEW0); gp32_st32le(bin + 4u, BXLR);
    memcpy(gpg, "gpg ", 4u); gp32_st32le(gpg + 4u, 256u);
    gp32_st32le(gpg + 8u, NEW0); gp32_st32le(gpg + 12u, BXLR);
    gp32_t *g = gp32_create(NULL);
    if (!g) return 0;
    fpk_asset_t assets[2];
    memset(assets, 0, sizeof(assets));
    strcpy(assets[0].path, "GAME/FIXTURE/IMAGE.BIN"); assets[0].data = bin; assets[0].size = sizeof(bin);
    strcpy(assets[1].path, "GAME/FIXTURE/CODE.GPG"); assets[1].data = gpg; assets[1].size = sizeof(gpg);
    if (path == PATH_FILE || path == PATH_AUTOLOAD) {
        g->direct_fpk_assets = assets;
        g->direct_fpk_asset_count = 2u;
        direct_set_fxe_mode(g, 1u);
        g->direct_fxe_image_end = X_ADDR;
    }
    s3c2400_write32(g->soc, X_ADDR, OLD0); s3c2400_write32(g->soc, X_ADDR + 4u, BXLR);
    s3c2400_write32(g->soc, SRC_CODE, NEW0); s3c2400_write32(g->soc, SRC_CODE + 4u, BXLR);
    s3c2400_write32(g->soc, SRC_CODE - 4u, 0xe1a00000u);
    /* Sections: RAM and DMA I/O identity, 0x0c1 an alias of 0x0c0. */
    s3c2400_write32(g->soc, TTB + 0xc0u * 4u, 0x0c000002u);
    s3c2400_write32(g->soc, TTB + 0xc1u * 4u, 0x0c000002u);
    s3c2400_write32(g->soc, TTB + 0x146u * 4u, 0x14600002u);
    s3c2400_write32(g->soc, STUBS, 0xef070001u);
    s3c2400_write32(g->soc, STUBS + 4u, 0xef070002u);
    s3c2400_write32(g->soc, STUBS + 8u, 0xef070004u);
    put_str(g, PATH_BIN, "gp:\\game\\fixture\\image.bin");
    put_str(g, PATH_GPG, "gp:\\game\\fixture\\code.gpg");
    put_str(g, LITERAL, "code.gpg");                 /* preceded by the slot word's 0x0c byte */
    s3c2400_write32(g->soc, LITERAL - 4u, SLOT);
    s3c2400_write32(g->soc, SLOT, X_ADDR);

    as_t a = { g, M_BASE, 0u };
    if (path == PATH_DMA_MMU) {
        ldr_const(&a, 1, TTB); emit(&a, 0xee021f10u);             /* mcr p15,0,r1,c2,c0,0 */
        emit(&a, 0xe3e01000u); emit(&a, 0xee031f10u);             /* domains: manager */
        emit(&a, 0xee111f10u); emit(&a, 0xe3811001u); emit(&a, 0xee011f10u); /* c1 |= M */
    }
    if (!indirect) {
        uint32_t loop = a.pc;                         /* both passes enter this block */
        emit(&a, branch(0xeb000000u, a.pc, X_ADDR));  /* bl X */
        emit(&a, 0xe3560000u);                        /* cmp r6,#0 */
        uint32_t bne_at = a.pc; emit(&a, 0u);
        emit(&a, 0xe1a04000u);                        /* mov r4,r0 */
        emit_replace(&a, path);
        emit(&a, 0xe3a06001u);                        /* mov r6,#1 */
        emit(&a, branch(0xea000000u, a.pc, loop));
        s3c2400_write32(g->soc, bne_at, branch(0x1a000000u, bne_at, a.pc));
        emit(&a, 0xe1a05000u);                        /* mov r5,r0 */
    } else {
        emit(&a, 0xe1a0e00fu); emit(&a, 0xe1a0f007u); /* mov lr,pc ; mov pc,r7 */
        emit(&a, 0xe1a04000u);
        emit_replace(&a, path);
        emit(&a, 0xe1a0e00fu); emit(&a, 0xe1a0f007u); /* a second call site */
        emit(&a, 0xe1a05000u);
    }
    ldr_const(&a, 1, RES); emit(&a, 0xe5814000u); emit(&a, 0xe5815004u);
    emit(&a, 0xeafffffeu);

    arm920t_set_jit(g->cpu, jit);
    arm920t_set_cpsr(g->cpu, 0xd3u);
    arm920t_set_reg(g->cpu, 6, 0u);
    arm920t_set_reg(g->cpu, 7, path == PATH_DMA_MMU ? X_ALIAS : X_ADDR);
    arm920t_set_reg(g->cpu, 13, GP32_RAM_BASE + 0x7000u);
    arm920t_set_reg(g->cpu, 15, M_BASE);
    arm920t_run(g->cpu, 200000u);

    uint32_t first = s3c2400_debug_read32(g->soc, RES), second = s3c2400_debug_read32(g->soc, RES + 4u);
    uint32_t c1 = arm920t_get_cp15(g->cpu, 1);
    int ok = (c1 & (1u << 12)) == 0u && ((c1 & 1u) != 0u) == (path == PATH_DMA_MMU) &&
             s3c2400_debug_read32(g->soc, X_ADDR) == NEW0 && first == 0x11u && second == 0x22u;
    if (!ok) fprintf(stderr, "FAIL %s %s jit=%d first=%08x second=%08x\n", path_name[path],
                     indirect ? "indirect" : "re-entered", jit, first, second);
    g->direct_fpk_assets = NULL;
    g->direct_fpk_asset_count = 0u;
    gp32_destroy(g);
    return ok;
}

/* A transfer can enter RAM from the preceding, unmapped bus range. Its first
 * word is discarded, but the second word still replaces executable RAM. */
static int dma_enters_ram(int jit) {
    gp32_t *g = gp32_create(NULL);
    if (!g) return 0;
    s3c2400_write32(g->soc, GP32_RAM_BASE, OLD0);
    s3c2400_write32(g->soc, GP32_RAM_BASE + 4u, 0xeafffffeu);
    s3c2400_write32(g->soc, SRC_CODE, 0xe1a00000u);
    s3c2400_write32(g->soc, SRC_CODE + 4u, NEW0);
    arm920t_set_jit(g->cpu, jit);
    arm920t_set_cpsr(g->cpu, 0xd3u);
    arm920t_set_reg(g->cpu, 15, GP32_RAM_BASE);
    arm920t_run(g->cpu, 100u);
    uint32_t first = arm920t_get_reg(g->cpu, 0);
    s3c2400_write32(g->soc, 0x14600000u, SRC_CODE);
    s3c2400_write32(g->soc, 0x14600004u, GP32_RAM_BASE - 4u);
    s3c2400_write32(g->soc, 0x14600008u, 0x04600002u);
    s3c2400_write32(g->soc, 0x14600018u, 3u);
    arm920t_set_reg(g->cpu, 15, GP32_RAM_BASE);
    arm920t_run(g->cpu, 100u);
    uint32_t second = arm920t_get_reg(g->cpu, 0);
    int ok = first == 0x11u && second == 0x22u && s3c2400_debug_read32(g->soc, GP32_RAM_BASE) == NEW0;
    if (!ok) fprintf(stderr, "FAIL dma-enters-ram jit=%d first=%x second=%x\n", jit, first, second);
    gp32_destroy(g);
    return ok;
}

int main(void) {
    unsigned cases = 0u, failures = 0u;
    for (int path = PATH_DMA; path <= PATH_AUTOLOAD; ++path)
        for (int indirect = 0; indirect < 2; ++indirect)
            for (int jit = 0; jit < 2; ++jit) { ++cases; failures += !one(path, indirect, jit); }
    for (int path = PATH_DMA_PARTIAL; path <= PATH_DMA_MMU; ++path)
        for (int jit = 0; jit < 2; ++jit) { ++cases; failures += !one(path, 1, jit); }
    for (int jit = 0; jit < 2; ++jit) { ++cases; failures += !dma_enters_ram(jit); }
    printf("cases=%u failures=%u\n", cases, failures);
    return failures ? 1 : 0;
}

