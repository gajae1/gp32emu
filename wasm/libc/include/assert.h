#ifndef PCFX_WASM_ASSERT_H
#define PCFX_WASM_ASSERT_H
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#define static_assert _Static_assert
#endif
#ifdef NDEBUG
#define assert(x) ((void)0)
#else
void abort(void);
#define assert(x) ((x) ? (void)0 : abort())
#endif
#endif
