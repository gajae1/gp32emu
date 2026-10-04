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
