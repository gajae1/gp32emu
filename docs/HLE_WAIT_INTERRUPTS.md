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

The next implementation needs a CPU-owned parked-continuation guard: accept an
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
