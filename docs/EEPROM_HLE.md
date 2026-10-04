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
  Windows. The EEPROM regression also passes natively on H700 from `/tmp`;
  installed core/config hashes are unchanged and physical volume is untouched.

This is a compatibility/data-integrity correction, not measured FPS improvement.
Extended device/card ID services 0x104/0x103 still need implementation; static
presence of their SDK stubs does not prove a title calls them at runtime.
