#!/usr/bin/env bash
set -euo pipefail

# Run from any directory. This deliberately requires a fresh build directory.
asan_source=/tmp/gp32-round4
asan_build=/tmp/gp32-round5-asan
asan_evidence=$(cd -- "$(dirname -- "$0")" && pwd)
asan_cmake=/tmp/gp32-round4-tools/cmake/data/bin/cmake
asan_ctest=/tmp/gp32-round4-tools/cmake/data/bin/ctest
asan_expected=865f88e2d095f71ac80935bd775a38f3b8985246
asan_tests=(s3c2400_fastmem s3c2400_bios_load gp32_bios_reload s3c2400_pwm_state gp32_state_speed)

test "$(git -C "$asan_source" rev-parse HEAD)" = "$asan_expected"
test -z "$(git -C "$asan_source" status --porcelain)"
test ! -e "$asan_build"
mkdir -p "$asan_build"
: > "$asan_evidence/commands.log"
: > "$asan_evidence/results.log"

run_logged() {
    local asan_log=$1
    shift
    printf '%q ' "$@" >> "$asan_evidence/commands.log"
    printf '> %q 2>&1\n' "$asan_evidence/$asan_log" >> "$asan_evidence/commands.log"
    local asan_status=0
    "$@" > "$asan_evidence/$asan_log" 2>&1 || asan_status=$?
    printf '%s exit_status=%d\n' "$asan_log" "$asan_status" | tee -a "$asan_evidence/results.log"
    return "$asan_status"
}

{
    date -u '+%Y-%m-%dT%H:%M:%SZ'
    git -C "$asan_source" rev-parse HEAD 'HEAD^{tree}'
    git -C "$asan_source" status --short
    uname -srm
    gcc-13 --version
    "$asan_cmake" --version
    "$asan_ctest" --version
    make --version
    gcc-13 -print-file-name=libasan.so
    gcc-13 -std=c2x -dM -E -x c /dev/null | rg '__STDC_VERSION__'
} > "$asan_evidence/toolchain.log" 2>&1

git -C "$asan_source" ls-files -z CMakeLists.txt src include tests |
    (cd "$asan_source" && xargs -0 sha256sum) > "$asan_evidence/SHA256SUMS"

run_logged configure.log "$asan_cmake" -S "$asan_source" -B "$asan_build" \
    -G 'Unix Makefiles' \
    -DCMAKE_C_COMPILER=gcc-13 \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    '-DCMAKE_C_FLAGS=-fsanitize=address -fno-omit-frame-pointer' \
    -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DGP32EMU_REQUIRE_C23=ON \
    -DGP32EMU_BUILD_TESTS=ON \
    -DGP32EMU_CPU_PROFILE=ON \
    -DGP32EMU_ENABLE_IPO=OFF

run_logged build.log "$asan_cmake" --build "$asan_build" --parallel 2 --target \
    s3c2400_fastmem_test s3c2400_bios_load_test gp32_bios_reload_test \
    s3c2400_pwm_state_test gp32_state_speed_test

{
    rg '^(CMAKE_(BUILD_TYPE|C_COMPILER|C_FLAGS|EXE_LINKER_FLAGS)|GP32EMU_(REQUIRE_C23|BUILD_TESTS|CPU_PROFILE|ENABLE_IPO))' "$asan_build/CMakeCache.txt"
    cat "$asan_build/CMakeFiles/gp32emu.dir/flags.make"
    for asan_test in "${asan_tests[@]}"; do
        cat "$asan_build/CMakeFiles/${asan_test}_test.dir/flags.make"
        cat "$asan_build/CMakeFiles/${asan_test}_test.dir/link.txt"
    done
} > "$asan_evidence/build-flags.log" 2>&1

{
    for asan_test in "${asan_tests[@]}"; do
        printf '%s\n' "$asan_test"
        ldd "$asan_build/${asan_test}_test"
    done
} > "$asan_evidence/linked-runtime.log" 2>&1

asan_failed=0
for asan_test in "${asan_tests[@]}"; do
    if ! run_logged "$asan_test.log" env ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
        "$asan_ctest" --test-dir "$asan_build" -j 1 -V --output-on-failure \
        -R "^${asan_test}$"; then
        asan_failed=1
    fi
done

{
    date -u '+%Y-%m-%dT%H:%M:%SZ'
    git -C "$asan_source" rev-parse HEAD 'HEAD^{tree}'
    git -C "$asan_source" status --short
    cd "$asan_source"
    sha256sum --check "$asan_evidence/SHA256SUMS"
} > "$asan_evidence/source-after.log" 2>&1

exit "$asan_failed"
