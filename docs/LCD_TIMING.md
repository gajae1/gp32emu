# TFT scan timing

The TFT controller advances in HCLK units using the programmed divider and
horizontal/vertical sync, porch and visible lengths. Pixel period is
`2 * (CLKVAL + 1)` HCLK cycles. LINECNT and the read-only LCDCON5 HSTATUS /
VSTATUS fields observe the same phase. CPU reads limit the current execution
slice to the next observable boundary; a status read can narrow a deadline
previously set by a LINECNT read. This fixes the former constant LCDCON5 status
that could trap SDK programs in a display-status wait.

Reference: [Samsung S3C2400 manual, chapter 15](https://datasheets.chipdb.org/Samsung/S3C2400.pdf).
Local mirkoSDK graphics and GP32 MAME4ALL display routines supplied independent
examples of these registers being programmed and polled. No SDK implementation
was copied into the emulator.

This changes guest display timing, not host speed limits. Libretro advances one
1/60-second virtual interval per call. Since round 198 it also advertises the
live panel rate: when the derivable TFT period moves by more than 0.05 %, the
following `retro_run` pushes one full `RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO`, so
a frontend that accepts the command paces to the panel clock instead of the
load-time 60 fps default. Guest panel frequency can differ from 60 Hz. In the
supplied Blue Angelo state it is approximately 98.94 Hz; that alone does not
explain the user's report of fast gameplay or prove that changing panel timing
fixes it.

## State compatibility and limits

GP32STATEv0007 and standalone GP32SOC7 append 16 bytes for whole HCLK phase and
its fractional remainder. Loading validates this extension before changing the
live machine. Older GP32 v2-v6 files remain readable. Their missing exact LCD
phase is approximated from the normalized position in the former RUN/60 model;
they cannot reconstruct information never saved. Older emulator builds do not
read v7 saves.

Timing writes settle preceding CPU time first. ENVID rising and panel-mode
changes restart scanout; other timing writes retain the current HCLK position
modulo the new geometry. These live-write/re-enable rules and blanking LINECNT
readback are integration choices awaiting hardware confirmation. They are not
a claim of cycle-exact silicon behavior. STN and invalid TFT CLKVAL=0 retain
the former fallback timing. Direct HLE no longer relies on it: a directly loaded
program starts from the words the retail BIOS leaves (LCDCON1 0x377, LCDCON2-4
0x014fc081/0x0030ef02/0x4, 327 lines of 255 pixel clocks) and the mode setters
rewrite them, so its panel follows the modelled scanout. It also starts from the
BIOS clock tree (FCLK 67.8 MHz, HCLK 33.9 MHz), where the panel runs 50.82 Hz.
CLKVAL follows ROM 0x1fd4: HCLK / 10,006,200 rounded from a quarter, minus one
(3 at 33.9 MHz, 5 at 59.25 MHz). Only a graphics-mode switch recomputes it
(ROM 0x1804: GpGraphicModeSet and the reinit service); surface flips, LCD
enable and palette services keep the divider in LCDCON1, as in BIOS boots, where
a title that changes the clock (SWI 0x0d) after its last mode switch keeps the
old divider (88.8 Hz at 59.25 MHz). HLE vblank waits keep their own 60 Hz
deadline only as a fallback: the direct-mode deadline advances by
`s3c2400_lcd_frame_period()` (period_ns + period_frac/2^20, mapped onto the
existing denominator-60 accumulator, so the save-state wire format is
unchanged) whenever a panel frame period can be derived. STN mode, ENVID off,
CLKVAL 0 and periods outside the 5..500 Hz sanity window keep the exact 1/60 s
slot, which is what BIOS-boot and SWT guests continue to observe.

## Focused evidence

The new LCD fixture fails against the frozen pre-change library and passes on
the candidate. It checks two programmed frame periods, status transitions,
read-only status bits, fractional tick partitioning and save/load, clock
changes, and actual interpreter/JIT mixed LINECNT/HSTATUS polling. Existing
timer and PCM tests also pass. The state fixture covers legacy migration and
rejected malformed/truncated tails without live-machine mutation.
Both LCD and state fixtures also pass natively on the handheld. Handheld release core,
Windows libretro/GUI and Android arm64/armeabi-v7a builds pass. The native test
run left protected frontend files and the installed core unchanged.

The candidate completed 31 bounded PC cold boots, plus existing ASR title,
Blue NPC and Princess cutscene state replays. Completion of the harness is not
full gameplay acceptance. Changed display timing can change the scene reached
by a fixed input script; old/new screenshot hashes need not be identical.
Private reproducible evidence is in `results/resume118-lcd/` outside the repo.

Blue speed/dialogue duration, acoustic fidelity, long sessions and Android
runtime remain open. Device installation is tracked separately from these
source and PC checks.
