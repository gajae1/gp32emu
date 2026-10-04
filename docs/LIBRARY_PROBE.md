# Repeatable library exploration

`GP32EMU_BUILD_BENCHMARK=ON` also builds `gp32_library_probe`. For instruction
workload counters enable `GP32EMU_CPU_PROFILE=ON`. No BIOS or game data is bundled.

Run in a separate empty output directory for each game and continuation:

```text
gp32_library_probe BIOS_PATH SMC_PATH FRAME_COUNT STATE_PATH_OR_DASH INPUT_PATH_OR_DASH
```

Pass `-` for a cold start and `-` for the default BIOS confirmation pulses.
Frames are bounded to 1..36000. For a continuation, pass the previous
`end.state` and an input script. Script frames start at zero for each invocation:

```text
0f:=START
12f:=NONE
180f:=A
192f:=NONE
```

The probe uses the same frame pacing as the frontends, consumes all PCM spans,
and emits a JSON line every 300 frames (and at the final frame). Each window
records CPU execution-path counters, current PC/clock, PCM presence/hash and
screen-change count. PPM screenshots are rotated to the landscape orientation.
The final machine state is saved as `end.state`. Existing files with these
output names are overwritten, so do not use a save directory as the working
directory. The input SMC itself is not saved back.

Use screenshots to choose subsequent input, preserving the exact script and
state lineage. Separate playable scenes, story/cutscenes, configuration menus,
BIOS loading and runtime errors. Static screens and silent windows may be
legitimate; neither is automatically a failure. Visual novels do not need a
moving character or HUD to count as an in-game story scene.

Use `gp32_bench --state end.state --warmup 0 --frames 180 --jit --cpu-profile`
with the same BIOS/SMC for detailed workload classification. Parallel or
profile-enabled timings are not performance acceptance. Measure promising
changes separately on a fixed input, backend and device clock, checking CPU,
video and PCM equivalence. A PC run does not validate the AArch64 JIT backend,
RetroArch presentation, audible output or full-game compatibility.

The local 2026-10-04 sweep covers one preferred regional release for each of
the 31 parent game entries in the saved MAME catalog. All 31 families are
present. The user's TOSEC collection contains 40 files: 31 byte-identical to
those selected, seven regional/version alternatives and two additional
homebrews (Dynamate and SmashGP). A file present in the catalog and a game
successfully reaching gameplay are separate checks.
