# Guest audio codec

The normal BIOS path now decodes the GP32's L3 audio-control bus on
GPE9/CLOCK, GPE10/MODE and GPE11/DATA when all three pins are configured as
outputs. This changes emulated PCM amplitude; it never controls a host mixer.

## Implemented behavior

- Address `0x14` accepts volume and mute/control bytes. Address `0x16`
  retains the status byte, without emulating its other effects yet.
- Volume codes 0/1 are unity, 2..61 follow the 1 dB attenuation ladder,
  and 62/63 are silent. Control bit 2 selects mute independently of volume.
- The data phase samples eight LSB-first bits on rising CLOCK edges.
  MODE rising selects the last eight address bits. This accommodates the
  priming CLOCK edge emitted by the retail firmware before the address.
- A CPU write changing configured codec pins ends the current CPU slice.
  Audio for elapsed cycles uses the previous gain; the write then takes
  effect. Previously queued PCM is never scaled again.
- The unity-gain DMA bulk path remains unchanged apart from a gain check.
  Non-unity gain uses integer Q16 multiplication on newly appended samples.

Version 9 savestates append six explicit bytes: address, partial shift byte,
bit count, volume, control and status. Derived gain is rebuilt on load.
Versions 2..8 remain readable and default to unity, since they contain no
codec history. A legacy state cannot reconstruct earlier volume writes.
Malformed or truncated new trailers are rejected before committing state.

## Evidence and scope

The European v1.6.6 BIOS volume routine at `0x54fc` was executed with argument
21, returning through its normal ARM code. Subsequent PCM `(10000,-10000)`
became `(1000,-1000)`. A first eight-clock-only address decoder incorrectly
selected `0x28`; the firmware trace exposed its priming edge, leading to the
address-window implementation.

Focused tests cover GPIO volume/mute, incomplete-byte savestate restoration,
DMA gain and elapsed-audio ordering, plus existing PCM, timing, state and
libretro audio behavior. Those five targets pass on Windows and H700.
Windows, H700 and Android ARM32/ARM64 release cores build successfully.

A warmed H700 ABBA comparison against the preceding build on the Astonishia
Story R title savestate measured 177.8715 versus 177.9675 benchmark fps at
an observed 1512 MHz. CPU, video and audio hashes matched. This is effectively
unchanged performance on the legacy-state unity path, not an all-game speed
claim or a measurement of every non-unity workload.

This is a steady-state register model. De-emphasis, soft-mute ramps and
status-register clock/format behavior remain incomplete. No claim is made
that menu pops or every game's crackling are fixed, or that this increases
gameplay speed.

## Direct-FXE volume service

Direct HLE handles SDK `GpControlVolume` (`SWI 0x17`) through the same codec
register model. It masks the argument to six bits, then writes control
`0x80`, exactly as the Korean v1.5.6 and European v1.6.6 firmware services do.
It preserves r0-r3 and LR. Normal BIOS execution keeps using its real SWI
vector and GPIO code.

During timed execution the SWI ends the CPU slice. Hardware and software
audio for that elapsed prefix are settled before its volume is applied.
The foreground request is captured locally so an SDK refill callback cannot
overwrite it. Refill commands take effect at the existing sample boundary;
timer-callback commands take effect after their CPU/SoC slice. Already
queued samples retain their previous gain, and resampling phases are kept.

GPOS timer accumulation and lifetime snapshots still happen before audio
generation. Only due-callback dispatch moves after the audio prefix and
foreground volume commit. A timer started by a refill therefore cannot
inherit time from before its start; reconfigured slots retain epoch checks.
Pending commands are dispatch-only and settle before public run returns,
so the savestate wire format stays at version 9.

Regression coverage includes register preservation, wrapped arguments,
unmute after GPIO mute, foreground/timer ordering on interpreter and JIT,
SDK-refill sample boundaries, competing foreground/refill commands and
timers started by refills. This follows the existing HLE scheduling model:
callback CPU time is not recursively credited to software mixers, concurrent
audio producers are not one chronological mix, and manual GPIO codec writes
during direct HLE still need software-audio boundary handling. It is not
cycle-exact BIOS replacement.

The local UDA1330ATS datasheet defines soft mute but supplies no ramp
duration, slope or equation. Volume-step smoothing is also unspecified.
The instantaneous gain remains an explicit steady-state approximation;
accurate transitions require additional chip documentation or measurements.
UDA1330ATS has no RST status bit; the RST layout belongs to UDA1341TS.

The HLE addition passes Windows codec/PCM/timer/state/libretro regressions
and native H700 codec/PCM/timer tests; all four release targets build.
Cold direct runs of Story of Bug Eyed Monster and Funny Soccer 2002 for
1,200 frames match the preceding revision's final PC, video hash, audio
frame count and audio hash. Funny Soccer's observed window has zero PCM,
so that comparison does not establish working audio or full gameplay.

Protocol and gain references: NXP
[UDA1330ATS](https://www.nxp.com/docs/en/data-sheet/UDA1330ATS.pdf),
L3 interface/register tables, and
[UDA1341TS](https://www.nxp.com/docs/en/data-sheet/UDA1341TS.pdf), Table 18.
The UDA1330ATS table merges some tail rows; the model uses its stated 1 dB
ladder and the explicit UDA1341TS row values. Physical analog output has not
been compared against a GP32.
