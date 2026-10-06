# Optional guest CPU speed

2026-10-05. Core option `gp32emu_cpu_speed` (100% default; 125-300%) and
`gp32_set_cpu_speed_percent()` scale only the guest instruction clock derived
from MPLL/CLKDIVN. Every peripheral converts its own period through that clock,
so IIS sample rate and duration, PWM timer periods and TFT refresh keep real
time. A real GP32 overclock also raises PCLK and therefore changes audio pitch
and timers; this option intentionally does not. Savestates are converted to
100% around save/load and are portable between speeds. Changing speed between
frames rescales peripheral phases the same way a guest clock write does.

## Evidence

A new timing fixture plays IIS at 42,968 Hz with a 1 kHz PWM timer, switching
100% to 200% halfway: exactly 42,968 PCM frames and 1,000 timer IRQs per
emulated second. The regression suite passes on the PC build, and the timing and
state tests also pass on the aarch64 handheld. At 100%, Princess, Astonishia,
Blue Angelo and Her Knights outputs are identical to `f7aaffa` (cycles, PC,
CPSR, clock, video/PCM hashes).

| Scene (PC, JIT) | 100% | 150% | 200% | 300% |
| --- | --- | --- | --- | --- |
| Blue Angelo NPC load, 300 frames: image changes / longest still run | 115 / 66 | 160 / 44 | 193 / 32 | 216 / 21 |
| Her Knights 600 frames: image changes / longest still run | 14 / 43 | 14 / 43 | 14 / 43 | 14 / 43 |
| Princess Maker 2 600 frames: image changes | 5 | - | 5 | - |

Every run produced exactly the nominal audio duration (10.0000 s or
5.0000 s). Blue Angelo's CPU-bound SmartMedia/ECC load shortens; the
round138 trace showed that pause is guest software work, not emulator
waiting. Timer- or vblank-paced titles do not change at all and only spend more
host time in their idle loops.

Handheld, 1.5 GHz Cortex-A53 (stable clock, cold 0-warmup runs, benchmark
frames/s, 60 = real time):

| Scene | 100% | 150% | 200% | 300% |
| --- | --- | --- | --- | --- |
| Blue Angelo NPC load, 300 frames | 60.7 | 54.2 | 44.3 | 35.3 |
| Princess Maker 2 slot0, 600 frames | 97.6 | 68.7 | 53.8 | 37.0 |

On that handheld, settings above 100% can fall below real time, which would make
audio underrun. Keep the default unless a CPU-bound scene benefits and the
device has headroom; faster hosts can use higher values. Raw evidence:
`F:/GP32/results/round139-speed/`.
