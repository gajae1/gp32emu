# Round5 focused AddressSanitizer verification

The five requested memory and saved-state regressions passed in a fresh GCC
AddressSanitizer build. Each test ran once, separately and sequentially. There
were no AddressSanitizer diagnostics, test failures, or retries. This evidence
commit changes documentation only.

## Source and configuration

The build used the clean integrated source at `/tmp/gp32-round4`:

- Commit: `865f88e2d095f71ac80935bd775a38f3b8985246`
- Tree: `e300f1f9b419f24da5f96b1e831640e47a9a32ce`
- Build directory: `/tmp/gp32-round5-asan`, absent before this run
- Host: x86-64 Linux, kernel `6.18.55`
- Compiler: GCC 13.3.0, Ubuntu package `13.3.0-6ubuntu2~24.04.1`
- CMake and CTest: 4.4.4; generator: Unix Makefiles; Make: 4.3
- Build type: `RelWithDebInfo` (`-O2 -g -DNDEBUG`)
- Compiler flags: `-fsanitize=address -fno-omit-frame-pointer`
- Executable linker flags: `-fsanitize=address`
- Required C23, focused tests, and CPU profiling: enabled
- IPO: disabled
- Test environment: `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1`

CMake selected its C23 dialect with GCC's `-std=c2x`; this GCC version reports
`__STDC_VERSION__=202000L`. The evidence does not equate that draft-version macro
with complete final C23 conformance. All five executables linked `libasan.so.8`.

The run hashed every tracked file under `src`, `include`, and `tests`, plus
`CMakeLists.txt`, before configuration. After all tests, the commit and tree were
unchanged, the source worktree remained clean, and every source hash matched.
The [source manifest](SHA256SUMS) and [final source check](source-after.log)
retain those checks.

## Results

| Test | Test-reported result | CTest exit status |
|---|---|---:|
| `s3c2400_fastmem` | 199 cases, 0 failures | 0 |
| `s3c2400_bios_load` | 12 checks, 0 failures | 0 |
| `gp32_bios_reload` | 12 cases, 0 failures | 0 |
| `s3c2400_pwm_state` | 105 scenarios, 0 assertion failures | 0 |
| `gp32_state_speed` | 62 cases, 0 failures | 0 |

This run covers the integrated span-bound checks, staged and overlapping BIOS
loads, cached BIOS instruction invalidation, PWM state decoding and rejection,
and saved-state phase preservation exercised by those tests. It is a focused
host AddressSanitizer check. Leak detection was disabled as requested because
of the already identified LeakSanitizer limitation in this environment; these
results make no leak-detection claim. No performance measurements were taken.

The compiler's existing type-limit warnings in `s3c2400.c` remain visible in
the build log. They did not prevent compilation or the five requested runs.

## Reproduction and retained evidence

[run.sh](run.sh) contains the exact run procedure. It requires the source commit
above, a clean source worktree, and an absent build directory before starting.
Invoke it with `bash docs/benchmarks/1.1.0-round5/asan/run.sh` from the evidence
worktree. The script configures the fresh build, builds only the shared core
and five requested executables, and then invokes CTest five times with `-j 1`
and an anchored expression selecting one test each time.

- [commands.log](commands.log): exact configure, build, and test commands, with
  their output destinations
- [toolchain.log](toolchain.log): start time, source identity, host, compiler,
  CMake, CTest, Make, sanitizer-library path, and C version macro
- [configure.log](configure.log) and [build.log](build.log): raw configure and
  compiler output
- [build-flags.log](build-flags.log): selected cache values, generated compile
  flags, and each executable's link command
- [linked-runtime.log](linked-runtime.log): linked runtime libraries for all
  five executables
- [results.log](results.log): exit statuses for configuration, compilation,
  and the five sequential tests
- [s3c2400_fastmem.log](s3c2400_fastmem.log),
  [s3c2400_bios_load.log](s3c2400_bios_load.log),
  [gp32_bios_reload.log](gp32_bios_reload.log),
  [s3c2400_pwm_state.log](s3c2400_pwm_state.log), and
  [gp32_state_speed.log](gp32_state_speed.log): raw verbose CTest output
- [source-after.log](source-after.log): finish time, unchanged source identity,
  clean worktree, and verification of every recorded source hash

CTest's wall-clock durations are operational log output, not benchmark data.
