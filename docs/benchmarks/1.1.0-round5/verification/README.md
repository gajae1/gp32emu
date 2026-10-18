# Integrated Round 5 verification

These are functional checks of the complete integrated core, not performance
measurements. All four builds started in fresh directories. The source/test
freeze is local commit `865f88e2d095f71ac80935bd775a38f3b8985246`, tree
`e300f1f9b419f24da5f96b1e831640e47a9a32ce`. Later review/evidence commits preserve
the recorded production and test bytes. Each matrix records 194 input hashes
and checks that they still match after execution.

The prior public review is `2bace9afd9c3d1e5e7cac9d23379e2f60d167fec`, whose
tree is identical to local `85d36d55a47fc6da191e2ad6fd2e5079a4e91e4f`.
Local audit commits need not be reachable in the public repository: use the
published final source for the after run and the public prior review for the
before run. The source manifests, not a similarly named build directory,
identify what was actually checked.

## Final matrices

| Configuration | Registered / passed | C23 compile commands | CTest reported seconds |
| --- | ---: | ---: | ---: |
| GCC 13.3 x86-64 Release | 64 / 64 | 82 / 82 | 9.82 |
| Clang 18.1.3 x86-64 Release | 64 / 64 | 82 / 82 | 9.55 |
| GCC 13.3 AArch64 Release, QEMU 8.2.2 | 64 / 64 | 82 / 82 | 86.45 |
| GCC 13.3 x86-64 RelWithDebInfo, UBSan | 64 / 64 | 82 / 82 | 11.33 |

All configurations require C23, enable tests, libretro, headless and CPU
profiling, and disable IPO. GCC emits `-std=c2x`; Clang emits `-std=c23`.
Selecting the supported C23 dialect does not certify every final C23 language
or library feature. In particular GCC 13's version macro remains `202000L`.
The UBSan build places `-fsanitize=undefined -fno-sanitize-recover=undefined`
on every recorded compile command and uses
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. It reports no runtime
diagnostics. QEMU is generated-code functional evidence, not AArch64 device
timing. The four builds/tests overlap in wall time and were never used as
performance comparisons.

Each compiler directory retains `metadata.json`, the exact `runner.py`,
configure/build/test logs, the 64-entry JSON inventory, all 82 compile commands
and `LastTest.log`. All stage return codes are zero. No failed test was omitted
from these final integrated inventories.

CTest's raw JSON inventory output contains spaces before some newlines. These
four `inventory.log` files retain their exact stdout and recorded hashes.
[review-format.json](review-format.json) records those specific whitespace
exceptions; source, Markdown and other evidence pass the separate whitespace
check. No runtime or source warning was suppressed by this formatting check.

The separate [focused ASan run](../asan/README.md) builds the same frozen
source and passes the five specifically requested memory/state checks once,
sequentially, without diagnostics or retries. Leak detection is disabled with
`detect_leaks=0`, so it is not a leak-detection claim. The
[WASM record](../wasm/README.md) supplies actual 32-bit freestanding runtime and
public save/load checks. Android/iOS SDK builds, physical handhelds, real game
images, and browser presentation/audio output are outside these executions.

## Reproduction

From a clean checkout of the delivered revision, use a new output directory:

```sh
audit_revision=$(git rev-parse HEAD)
python3 scripts/verify_core_round5.py . /tmp/gp32-round5-recheck-gcc \
  --frozen-head "$audit_revision" --kind gcc --cmake cmake
```

Use `--kind clang`, `aarch64`, or `ubsan` with separate fresh directories.
The recorded environment uses CMake/CTest 4.4.4, `clang-18`,
`aarch64-linux-gnu-gcc`, and `qemu-aarch64 -L /usr/aarch64-linux-gnu`.
The wrapper refuses an existing build directory, checks the expected 64-test
inventory, checks actual compiler flags, retains errors, and exits nonzero on
any failed stage or source mutation. It copies the script that was run so a
later change to the repository helper cannot silently replace this evidence.

## Warning triage

The builds passed with retained legacy and test-fixture warnings. Read-only
comparison against the prior review found no new production correctness issue.
This is **not** a warning-free-build claim. The numbers of diagnostic lines are
100 GCC, 9 Clang, 104 AArch64/GCC and 42 UBSan/GCC. White-box inclusion of core
sources and repeated inlining inflate these totals.

| Diagnostic family | Source / disposition |
| --- | --- |
| Possibly uninitialized `c.spsr_*` | Existing `tests/arm_jit_test.c` helper. The context getter does nothing for a null CPU, but the unchanged setup exits on allocation failure and the getter assigns all SPSRs for a valid CPU. |
| Unsigned audio count `>= 0` | Existing redundant assertion expression in `tests/libretro_runahead_test.c`; the frame call still executes. It is not an error-code check. |
| `next->h > 31u` | Existing redundant guard on an unsigned five-bit field in `src/arm920t.c`. |
| Audio allocation type limits | Existing `s3c2400.c` checks against `size_t` allocation bounds; redundant on the tested 64-bit hosts, relevant for smaller `size_t`. |
| A64 conditional-expression signedness | Existing bit-field integer promotion in `arm920t_jit_a64.inc`; the maximum shifted operands fit signed 32-bit `int`. |
| Unused fetch guards / ITLB helper | Existing production helper declarations; some are reached only by white-box tests. |
| Unused `fixture_v14` / `fixture_cpu_prefix` | Existing shared-header test inclusion pattern, also present before the v16 fixture adaptation. |
| `.tmp` suffix truncation | Existing bounded test-path construction in `tests/atomic_save_test.c`, not a production buffer overflow. |
| iOS diagnostic truncation | Existing bounded log message in `ios/bridge/gp32_ios.c`; file access uses the original path. |

Warnings were traced to their types, initialization paths and callers, rather
than suppressed to obtain a clean log. No opportunistic warning cleanup was
mixed into the frozen runtime while measuring it.

## Independent integration review

A separate read-only review checked the final combination after the native,
interpreter, HLE, BIOS and PWM work was integrated. It found no blocker in:

- Store-before-notification ordering, captured callback-sensitive operands,
  and epoch/generation checks before the next decoded/native instruction.
- Physical watch collection for actual, speculative, callback and revalidated
  fetches, including CP15.I=1; separate BIOS and other-region flags; full versus
  lazy invalidation behavior.
- Scalar/bulk HLE writes, palette/stub/callback patching, GXB caller flushes,
  staged BIOS replacement and overlapping buffer input.
- Independent read/write complete-range certificates, live SoC RAM sizing,
  copied bus objects, explicit mapping flushes and state-load invalidation.
- Subtraction-based direct-span bounds, zero-length compatibility, staged file
  cleanup and successful public BIOS reload without CPU architectural reset.

Reviewed hashes include:

| File | SHA-256 |
| --- | --- |
| `src/arm920t.c` | `8e390d734ecd6195a40044341ed841af092492104c3267e53831907b3bd7440c` |
| `src/arm920t_jit_a64.inc` | `1137c007812c8f8acef85b3f88290032e2dd843459a9658f944f5bedfb3d1e01` |
| `src/arm920t.h` | `33990ee4bc2a266adfd76709a1a47a14b7856d88bacd05ecf1d7a94992fa040f` |
| `src/gp32.c` | `d3a49d622522165e01d6bfd28cb6945f5dfbddae5332038e58314b6a2ebc07f8` |
| `src/s3c2400.c` | `925754101aafb2d18398824f32cc3d3281baf7a1d6551524569518cde7f7b05d` |

Retained design limits include conservative rare-region/MMU invalidation,
clipped nonwrapping external notification spans, and the requirement that a
generic bus performs a full mapping flush after changing mappings or
permissions. A code-range notification does not replace that mapping contract.
