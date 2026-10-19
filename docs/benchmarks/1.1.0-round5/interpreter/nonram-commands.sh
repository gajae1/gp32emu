#!/bin/sh
# Functional reproduction commands, run from the repository root.
# The native companion reconstructs the candidate snapshots recorded in
# nonram-isolated-sources.json and ../jit/nonram-final-combined-sources.json.
set -eu
rare_repo=$(pwd)
rare_build=/tmp/gp32-round5-build-interpreter
rare_final=/tmp/gp32-round5-jit-nonram-final-combined/src
rare_prior=/tmp/gp32-round5-jit-nonram-combined/src
rare_before=$rare_build/nonram-isolated-before
rare_docs=$rare_repo/docs/benchmarks/1.1.0-round5/interpreter

# This frozen source retains the external watched-word refinement and final
# mapping certificate, then removes only the rare-region common/native fix.
mkdir -p "$rare_before/src"
cp "$rare_final/arm920t.c" "$rare_final/arm920t.h" \
  "$rare_final/arm920t_jit_a64.inc" "$rare_final/arm920t_jit_ios.inc" "$rare_before/src/"
(cd "$rare_before" && git apply --unidiff-zero "$rare_docs/nonram-isolated-before.patch")
gcc -std=c2x -O3 -DNDEBUG -D_XOPEN_SOURCE=700 \
  "-DCORE_UNDER_TEST=\"$rare_before/src/arm920t.c\"" -Isrc -Iinclude \
  tests/arm_nonram_code_write_test.c -o "$rare_build/arm_nonram_code_write_isolated_before"
set +e
"$rare_build/arm_nonram_code_write_isolated_before" > "$rare_docs/nonram-code-isolated-before.log" 2>&1
rare_status=$?
set -e
test "$rare_status" -eq 1

# Final native GCC/Clang/AArch64 commands and output are also retained by the
# native companion. These paths contain the exact common patch and two hooks.
gcc -std=c2x -O3 -DNDEBUG -D_XOPEN_SOURCE=700 \
  "-DCORE_UNDER_TEST=\"$rare_final/arm920t.c\"" -Isrc -Iinclude \
  tests/arm_nonram_code_write_test.c -o "$rare_build/arm_nonram_code_write_final_gcc"
"$rare_build/arm_nonram_code_write_final_gcc"
aarch64-linux-gnu-gcc -std=c2x -O3 -DNDEBUG -D_XOPEN_SOURCE=700 \
  "-DCORE_UNDER_TEST=\"$rare_final/arm920t.c\"" -Isrc -Iinclude \
  tests/arm_nonram_code_write_test.c -o "$rare_build/arm_nonram_code_write_final_a64"
qemu-aarch64 -L /usr/aarch64-linux-gnu "$rare_build/arm_nonram_code_write_final_a64"

# Exact local Clang/UBSan source differs from final only in a comment.
clang-18 -std=c23 -O3 -DNDEBUG -D_XOPEN_SOURCE=700 \
  "-DCORE_UNDER_TEST=\"$rare_prior/arm920t.c\"" -Isrc -Iinclude \
  tests/arm_nonram_code_write_test.c -o "$rare_build/arm_nonram_code_write_combined_clang"
"$rare_build/arm_nonram_code_write_combined_clang" > "$rare_docs/nonram-code-clang.log" 2>&1
gcc -std=c2x -O1 -g -fsanitize=undefined -fno-sanitize-recover=all -D_XOPEN_SOURCE=700 \
  "-DCORE_UNDER_TEST=\"$rare_prior/arm920t.c\"" -Isrc -Iinclude \
  tests/arm_nonram_code_write_test.c -o "$rare_build/arm_nonram_code_write_combined_ubsan"
"$rare_build/arm_nonram_code_write_combined_ubsan" > "$rare_docs/nonram-code-ubsan.log" 2>&1

# Portable executor and common change in isolation use this worktree's source.
gcc -std=c2x -O3 -DNDEBUG -D_XOPEN_SOURCE=700 -DGP32EMU_DISABLE_JIT=1 \
  -Isrc -Iinclude tests/arm_nonram_code_write_test.c -o "$rare_build/arm_nonram_code_write_portable"
"$rare_build/arm_nonram_code_write_portable" > "$rare_docs/nonram-code-portable.log" 2>&1
gcc -std=c2x -O3 -DNDEBUG -D_XOPEN_SOURCE=700 -Isrc -Iinclude \
  tests/arm_nonram_code_write_test.c -o "$rare_build/arm_nonram_code_write_common_only"
set +e
"$rare_build/arm_nonram_code_write_common_only" > "$rare_docs/nonram-code-common-only.log" 2>&1
rare_status=$?
set -e
# Expected only on the isolated common branch before native companion integration.
test "$rare_status" -eq 1

gcc -std=c2x -O3 -DNDEBUG -D_XOPEN_SOURCE=700 -DGP32EMU_CPU_PROFILE=1 \
  "-DARM_SOURCE=\"$rare_prior/arm920t.c\"" -Isrc -Iinclude \
  tests/arm_external_write_test.c -o "$rare_build/arm_external_write_nonram_combined"
"$rare_build/arm_external_write_nonram_combined" > "$rare_docs/nonram-external-existing.log" 2>&1
