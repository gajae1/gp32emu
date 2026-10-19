# Existing AArch64 Release FIR lowering: GCC 13.3

**Observed result: contiguous source is scalar; planar history already uses
NEON.** This is read-only disassembly of the completed matrix object, not a new
build, candidate or performance experiment.

## Provenance

- Frozen source commit: `06c4d879543f90d1003d174718d738e27a8ed6b8`. The current audio source hash matches that revision.
- Source SHA-256: `913b01cd38cf1ade9ce0be4cd456782a7eb2518f615058887dff551be7199a4e`.
- Header SHA-256: `002a78b438627b9d1446e33911ec4825d61996256b5884db8101cfeb0c5c1a15`.
- Object SHA-256: `ea9b453f77c36a4a41e3b97ef67c863542737165a0c39bcea7b9301984dc90d1`.
- Object: `/tmp/gp32-round4-build-aarch64/CMakeFiles/gp32emu.dir/src/audio/gp32_audio_resampler.c.o`.
- Producer `.comment`: GCC Ubuntu 13.3.0-6ubuntu2~24.04.1.
- Decoder: GNU objdump 2.42.

The [recorded compile command](audio-a64-gcc13-06c4d879-compile-command.json)
uses `-O3 -DNDEBUG -std=c2x -fPIC` and warning/include/feature flags. It contains
no explicit `-mcpu` or `-march` option. These observations concern this exact
GCC object; they do not establish Apple/Android/Clang or device-specific lowering.

## Observed instructions

The standalone `poly_gather` symbol is 0x4fc bytes. See its
[complete disassembly](poly-gather-gcc13-06c4d879.objdump.txt). Addresses below
are object `.text` offsets.

| Path | Observed lowering |
|---|---|
| Source setup, 0x00 | Extract phase, compute the 130-byte coefficient-row stride, select `min(n,64)`, adjust source for the carried sample. |
| Source body, 0x40 | Three `LDRSH` loads and two scalar `SMADDL` operations into 64-bit x11/x10 sums; source pointer decreases by four bytes per tap. Nine instructions including updates and loop control. No vector alternative is present for this loop. |
| Index-zero sample, 0x80 | Select `prev` or source frame zero, multiply in 32 bits and add with sign extension to the 64-bit sums. |
| History main groups, 0xc0 | Eight taps per channel: `LDR q`, duplicate-vector moves, `TBL` halfword reversal, `SMULL`/`SMULL2`, then `SXTL` and `SADDW`/`SADDW2` into 64-bit lanes. Up to eight groups are unrolled with count-dependent exits. |
| History reduction, 0x374 or 0x3a4 | `ADDP` over `.2d` lanes, `FMOV` bit transfers to general registers, then add the prior scalar sum. No floating-point arithmetic is used for this reduction. |
| Four-tap history remainder, 0x404 | `LDR d`, `REV64 .4h`, `SMULL` and widening 64-bit additions. The remaining one to three taps use scalar `SMADDL`. |
| Output packing, 0x15c8 | Caller receives both wide sums, adds 16384, uses `ASR #15`, compares/selects signed clamp bounds, and stores halfwords. See [packing excerpt](audio-a64-gcc13-06c4d879-pack-excerpt.txt). |

History counts below eight skip the main vector groups and enter remainder
handling. The [TBL constant](audio-a64-gcc13-06c4d879-constants.txt) is
`0e 0f 0c 0d 0a 0b 08 09 06 07 04 05 02 03 00 01`: it reverses eight halfwords
while preserving each halfword's bytes. Separate channel vectors stay separate.
The observed widening additions do not first accumulate products at 32 bits.

## Implication

The previous source-based uncertainty is now resolved for this object. A future
paired device experiment can target the scalar source loop and/or the history
shuffle, widening-add and unrolling costs, using this mixed implementation as
its normal optimized baseline. History-heavy tiny blocks already execute
compiler-generated SIMD; source-dominated outputs at `n >= 64` run 64 scalar
source iterations. Instruction count and code shape alone do not establish that
handwritten NEON is faster. No production change is proposed from this inspection.
