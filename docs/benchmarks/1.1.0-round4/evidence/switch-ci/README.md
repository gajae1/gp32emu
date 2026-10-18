# Switch differential workflow compile repair

The manual Switch workflow passed `src/arm920t.c` as a separate compilation input even though `tests/arm_jit_test.c` already includes that file to inspect native internals. The command does not define `ARM920T_NATIVE_BACKEND`, so the separate input compiled the core twice. The baseline AArch64 cross-link failed with **45 multiple-definition diagnostics** and exit status **1**.

The workflow repair removes only that extra source argument and adds an explanatory comment. The fixture, core, mock, compiler flags, and existing workflow gates retain their tested bytes. The corrected cross-build exits **0**. A repository search found no other duplicate-core invocation requiring this repair.

## Full functional results

| Gate | Exit status | JIT events | Fallbacks |
| --- | ---: | ---: | ---: |
| CodeMemory mock | 0 | 115,386 | 0 |
| SetProcessMemoryPermission mock (`SWITCH_JIT_PERMISSION_MODE=1`) | 0 | 115,386 | 0 |

Both modes ran the complete differential fixture without a test selector, under QEMU AArch64 with profiling enabled. The two functional runs were launched concurrently. No libnx SDK build, NRO build, physical Switch test, or remote workflow execution was performed for this repair.

The retained log creation-to-final-output intervals were **48.960 seconds** for CodeMemory and **969.363 seconds** for permission mode. These are filesystem wall-timestamp intervals, not monotonic process timings or performance figures. Independent tool completion statuses establish both zero exits. The permission-mode mock flushes its full allocation when making code executable; its emulated runtime is not evidence of physical Switch performance.

## Reproduction and evidence

[commands.txt](commands.txt) records the exact baseline build, corrected build, and both runtime commands. Run them from a checkout with the source hashes recorded in [metadata.json](metadata.json). The compile uses `aarch64-linux-gnu-gcc` 13.3.0 and `-std=c2x`; the runtime uses QEMU 8.2.2 with `-L /usr/aarch64-linux-gnu`. The retained workflow command itself uses `cc` on its native runner. The cross-compiler substitution and QEMU wrapper are local validation tools; all source inputs and compile flags match the relevant workflow command.

- [metadata.json](metadata.json): exact commands, compiler/QEMU versions, exit statuses, full-source and workflow hashes, binary hash, event counts, timestamp method, and artifact hashes.
- [before-summary.log](before-summary.log) and [before-build.log](before-build.log): concise and full failed-link reproduction.
- [after-build.log](after-build.log): corrected compile output (empty; exit status 0 is retained in metadata).
- [code-memory.log](code-memory.log) and [permission.log](permission.log): complete successful differential output.
- [workflow.patch](workflow.patch): the bounded CI command repair.
