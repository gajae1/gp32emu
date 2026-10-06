# EEPROM firmware service and IIC repeated START

The direct-FXE firmware hook implements SDK `_gp_e2prom_read` and
`_gp_e2prom_write` (ARM SWI `0x105`). Previously the extended-service fallback
returned success without transferring any data. BIOS mode still executes the
original firmware through the ordinary SWI exception vector.

## Contract and storage

The official SDK stubs and EU v1.6.6 firmware agree on these arguments:

| Register | Meaning |
| --- | --- |
| r0 | EEPROM byte offset; signed-negative values clamp to zero |
| r1 | Byte count, at least one |
| r2 | Guest destination for read, source for write |
| r3 | Zero reads, any nonzero value writes |

Success returns r0=0. A range outside the first 4096 bytes returns r0=0x21
without transferring data. A transfer ending exactly at 4096 is valid. The
HLE checks the range using subtraction: unlike the firmware's signed addition,
malformed overflowing ranges cannot bypass this limit. It preserves r1 and
callee-saved registers; firmware scratch registers and NZCV after `0x105` are
not an exact instruction-level emulation contract.

Transfers use the guest byte bus and the same 8192-byte EEPROM array as the
SoC IIC peripheral. There is no parallel HLE copy. Reset retains its contents;
existing machine states save and restore them, without a state-format change.
This does not add a separate EEPROM persistence file between emulator sessions.
IIC register side effects and firmware transfer latency are not simulated by
the direct HLE service.

## Device ID service (0x104)

The direct-FXE hook also implements SDK `_gp_dev_id_get` (ARM SWI `0x104`).
The firmware body at 0x51f4 takes a single argument, r0 = pointer to a 16-byte
output buffer (r1 is not read), reads EEPROM offset 0x10 for 16 bytes through
its own read body, and XORs byte i with the repeating firmware key
"SANGHYUK" (constant at 0x7d00). Success returns r0=0; a failed read would be
propagated, but offset 0x10/count 16 always fits the 4096-byte window, so the
HLE path cannot leave r0 nonzero. r1 and CPSR are preserved: unlike 0x105,
the 0x104 dispatcher returns with `movs pc,lr`, restoring SPSR.

On an erased chip the buffer becomes `ac be b1 b8 b7 a6 aa b4` repeated. After
seeding EEPROM[0x10..0x1f] with `i ^ 0x5a` through 0x105 it becomes
`09 1a 16 1e 16 06 09 16 01 12 1e 16 1e 0e 01 1e`. The regression test seeds
through 0x105 and calls the real SWI 0x104 in interpreter and JIT modes,
checking r0, r1, both fixed 16-byte results and guard bytes around the buffer.

Runtime evidence is narrower here than for 0x105. A raw SWI 0x104 from an
IRQ-enabled caller stalled in a wait at 0x4930 in the parent's firmware
harness: the SWI entry masks IRQs, unlike 0x105, which restores the caller's
CPSR before its transfer. So raw-SWI 0x104 runtime parity is not claimed; the
comparison executes the firmware body at 0x51f4 directly with IRQs enabled.
That comparison matches status and 64 destination/guard bytes for both erased
and seeded EEPROM. The body clobbers r1; the SWI dispatcher, modeled by HLE,
restores it. No fix to the raw firmware IRQ wait is inferred from this test.

## Shared hardware correction

The firmware random-read helper switches IICSTAT from master transmit `0xf0`
to master receive `0xb0`, without a STOP. The pending IICCON acknowledgement
then sends the new device address, followed by data on the next acknowledgement.
The old model retained the transmit byte index across this repeated START. It
returned the first EEPROM byte during the address phase, which firmware
discards, so firmware reads returned bytes beginning at offset+1.

The mode switch now resets the byte index and leaves the pending acknowledgement
to advance the new address phase. This corrects BIOS EEPROM reads generally;
it has no game-specific conditions. An erased all-0xff chip concealed the bug.

## Evidence and limits

- EU v1.6.6 dispatcher at 0x6af0; write body 0x4a20, read body 0x4ad0;
  random-read helper 0x48f8. SDK `gpstdlib.a` / `asm_gpstdlib.o` names the
  corresponding wrappers. Firmware and SDK binaries are not distributed here.
- `gp32_eeprom_test` executes synthetic ARM SWIs in interpreter and JIT modes,
  observes HLE writes through IIC and IIC writes through HLE, covers repeated
  START with distinct bytes, boundary/error cases, and state/reset retention.
- The original success-only HLE fails the regression. A private real-BIOS
  comparison boots firmware and enables IRQs before calling it: eight normal,
  negative-offset, boundary and rejected-range cases match HLE status, r1,
  64 buffer bytes and the entire EEPROM array after the hardware correction.
  Before that correction, the two reads of written data were shifted by one.
- Private probes and results: `results/resume127-compat/` outside the checkout.
- Focused EEPROM, existing timing and transactional state checks pass on
  Windows. The EEPROM regression also passes natively on the handheld from `/tmp`;
  installed core/config hashes are unchanged and physical volume is untouched.

This is a compatibility/data-integrity correction, not measured FPS improvement.
Extended card ID service 0x103 still needs implementation; static presence of
its SDK stub does not prove a title calls it at runtime.
