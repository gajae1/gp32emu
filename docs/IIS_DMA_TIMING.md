# IIS DMA interrupt scheduling

The CPU slice is now bounded by the next IRQ-enabled IIS DMA2 completion.
Previously, a large CPU slice could defer a completion IRQ until after several
later serial sample periods had already been processed. A guest interrupt
handler could not refill its one-shot buffer in time, even when it would have
done so with finer scheduling. This is a common SoC timing correction, without
game identification, extra silence, or repeated samples.

`iis_dma_irq_budget()` uses the current DMA transfer count, transfer width,
IIS rate and accumulated fractional phase. Whole-service and zero-count
requests complete on the next request. It retains the existing deferred MMIO
write boundaries. CPU and interrupt-controller masks do not bypass the bound:
the guest may unmask interrupts during the requested execution slice.

## Regression evidence

`s3c2400_iis_irq_test` executes an ARM IRQ handler that acknowledges the
interrupt and restarts DMA2. It compares single-cycle execution with requests
of up to 32768 cycles for the same elapsed cycles, checking all CPU registers,
CPSR and PCM. Before the fix, the simple one-shot case generated 10 versus 4
PCM frames and serviced 2 versus 0 interrupts. After the fix, both schedules
agree. Five cases cover halfword and word transfers, odd counts, fractional
phase, whole-service and auto-reload in both interpreter and JIT modes.

Instrumented PC replays measured active-IIS periods with no generated PCM:

| Sequence | Virtual duration | Before | After |
| --- | ---: | ---: | ---: |
| Astonishia Story R saved title | 30 s | 0 | 0 |
| Princess Maker 2 slot 0 | 30 s | 0 | 0 |
| Blue Angelo slot 0 and left-input script | 30 s | 0 | 0 |
| Astonishia Story R cold start | 100 s | 8 | 0 |
| Dooly Soccer cold start | 100 s | 0 | 0 |

The eight missing frames represented about 0.346 ms at 23144 Hz. These bounded
sequences do not establish the cause of every audible click or cover entire
games. Advancing IRQ service intentionally changes some CPU/video/PCM hashes;
old/new timings are therefore not an exact-output speed comparison.

Focused PC timing, PCM, state and libretro-audio checks pass. The new IRQ test,
PCM test and libretro-audio test also pass on the handheld, staged only in RAM. Release
cores build for Windows x64, handheld, Android arm64 and armv7; Android runtime was
not tested. One handheld Princess replay (600 warmup, 1200 measured frames) reached
91.875 headless fps, with measured-window clock observations varying from
1104 to 1512 MHz. This is bounded emulation throughput, not game animation fps
or a before/after performance gain. Device settings and installed core remain
unchanged; the SD filesystem is read-only after a boot-time FAT error.

## Remaining boundaries

The model still uses a two-halfword staging pair rather than the hardware's
eight-entry FIFO. Available manuals do not settle the serial output value on
FIFO starvation; MAME's held DAC value is not hardware evidence. This change
does not invent that behavior. A genuinely late guest handler can still miss
sample periods. The separate idle-wait path that directly advances the SoC
also bypasses this CPU-run bound and requires separate analysis.

Private reproducibility evidence: `results/resume129-audio/` contains
`scene-results.json`, `cold-results.json`, `candidate-results.json`,
`checks.json`, `protected.json`, `h700-current.json`, `validation.json`,
`dma-deadline-review.md` and `empty-fifo-research.md`.
