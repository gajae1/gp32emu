# Direct-HLE display wait: open interrupt boundary

The display callback's synthetic vblank wait advances CPU cycle count, SoC
time and HLE consumers through `direct_consume_idle_wait()`, but does not run
normal ARM exception dispatch. An asserted eligible IRQ/FIQ therefore waits
until the outstanding display wait expires. This is separate from the fixed
IIS/PWM completion deadlines during ordinary CPU execution. It applies to the
direct-HLE display path; no specific commercial game's symptom is attributed
to it by this investigation.

A private diagnostic parks an ARM or Thumb foreground marker for 64 cycles,
asserts IRQ or FIQ, and consumes two 32-cycle GP32 calls. The ISR and foreground
markers remain untouched throughout the wait while elapsed time advances.
Positive-control CPU steps then enter the interrupt vector, execute its marker
and return with `SUBS pc,lr,#4` to the original instruction. A further ordinary
step executes that foreground instruction despite a restored positive wait.
All eight IRQ/FIQ × interpreter/JIT × ARM/Thumb cases reproduce this behavior.
The fixture detaches the SoC IRQ sink so a hardware tick cannot recompute its
manually asserted CPU line; it diagnoses CPU dispatch, not peripheral wiring.

An unconditional CPU run during the wait would execute foreground code before
the deadline. Restoring a saved register context would discard legitimate ISR
effects. Checking CPU mode alone would mishandle FIQ, nested exceptions, mode
switches and scheduler context switches. None of these shortcuts is applied.

One implementation option is a CPU-owned parked-continuation guard: accept an
eligible exception without fetching the foreground, execute the handler under
a bounded budget, and stop before resuming that continuation if time remains.
The guard must cover native/decoded dispatch and chaining. ISR and HLE callback
time must count toward the wait exactly once, with clock changes respected.
Interrupted waits need a compatible save/load representation. Non-returning
handlers must remain bounded without fabricating an exception return.

There is also a park-point issue: the current display callback requests a wait
without immediately ending its CPU slice. A correct guard must capture the
actual post-SWI continuation and define ownership when display calls occur
inside an ISR or HLE callback. Context-switch behavior needs an explicit
contract before a production change is accepted.

Evidence: private `results/resume130-compat/idle-wait-review.md` (source review,
proposed diagnostic and design), `idle-wait-probe.c`, `.exe` and `.log` (parent's
executed reproduction). This remains an open implementation item.

## Guest-loop prototype (2026-10-05, resume131-wait)

A private alternative implements the callback's wait as ordinary guest ARM
instructions, with its deadline and preserved registers in the guest context
and stack. Normal exception entry/return then handles interruption without a
new CPU continuation guard. A stopped display SWI finalizes the deadline after
its elapsed prefix, before subsequent HLE callbacks. This path also permits
an ordinary guest scheduler to save and resume the waiting task's real stack.

The prototype initially exposed a separate controller defect: INTMOD was
ignored and a timer configured as FIQ entered IRQ mode. After the common
controller correction, all 16 combinations of IRQ/FIQ, interpreter/JIT,
ARM/Thumb return and legacy display-entry instruction pass the short probe:
handlers execute before the wait ends, foreground code stays parked, and
the preserved caller registers and stack are restored. This is a private
prototype result, not a production fix for the wait path.

The current low-32-bit cycle mirror is not acceptable for arbitrary task
suspension: after half the 32-bit range beyond expiry, its signed comparison
can mistake an expired deadline for a future one. A wide elapsed-time target
and explicit clock/cadence conversion are needed. The synchronous HLE callback
executor can also exhaust its fixed cycle budget during a display wait and
discard the callback context. Its continuation policy and legacy in-flight
wait migration remain unresolved. New stub entry migration alone does not
repair an old state's already-pending host wait.

Next work should use the simpler guest-loop direction with per-invocation
wide deadlines, preserving ordinary IRQ/FIQ and guest stack semantics, then
address callback continuation and save-state compatibility explicitly. The
prototype is retained outside the repository in
`results/resume131-wait/{gp32-candidate.c,wait-probe.c,wait-after-fiq.log}`;
`design.md` records the review and remaining boundaries. No prototype changes
to `src/gp32.c` were promoted.

## Callback return identity prerequisite

The callback executor previously accepted the private return SWI immediate
from any guest address, even without an active callback. A synthetic ARM
callback that saves LR, executes the same SWI at its own address, restores LR
and writes a marker reproduced early termination in both interpreter and JIT:
the marker was never written. A minimal BIOS SWI handler returns normally, so
the fixture distinguishes a guest exception from an actual callback return.

The handler now requires an active callback and the exact private return-stub
address. The callback reaches its tail before returning through that stub.
PC timer, PCM, file and state checks pass; the timer regression also passes on
the H700 native backend from RAM with protected files unchanged. This is a return-identity fix; the
larger resumable-callback and interruptible-display-wait work remains open.


## Integrated resumable callbacks and guest display wait (2026-10-05)

Direct-HLE refill/timer callbacks now run inside the caller's CPU budget in
slices of at most 4096 cycles and resume across frames instead of being
abandoned after a fixed synchronous limit. The foreground context is restored
only after the callback reaches its private return stub (the 094024d owner and
return-PC check is kept). The SDK display wait runs as a guest loop over a
64-bit elapsed-nanosecond mirror, so guest IRQ/FIQ, LCD, IIS and PWM keep
running while a game waits. The former host idle-cycle wait is removed.
Display cadence is a fixed 60 Hz HLE schedule, not a TFT vblank edge.

State v11 appends the next display slot (85 words). v10 and v2-v9 load; legacy
pending host waits migrate once into the guest loop using the restored clock,
which is an explicit approximation. Each callback has a 1-second emulated
watchdog; timeouts, halts and SDK task switches inside a callback raise a
sticky CPU fault. HLE mixing and GPOS ticks pause during callback CPU time.

Evidence: the e89e85c display reproduction fails on both backends and passes
after integration; a 1.2M-cycle callback completes. All 26 PC tests pass,
including new `gp32_callback` and `gp32_wait` fixtures (nested IRQ/FIQ waits,
wraps, clock changes, save/load). The callback, wait, timer, PCM, state and
file tests pass natively on H700. Princess, Astonishia, Blue Angelo and Her
Knights SMC scenes are identical to `ee3a2de` in JIT and interpreter (600
frames). DynaMate v2.0 runs 60 frames identically on both backends. WinterSports
Eins alpha still leaves RAM at `06cc4dec`, exactly as before. Raw evidence:
`F:/GP32/results/round134-callback/` and `round139-callback/`.
