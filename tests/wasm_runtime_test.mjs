import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

const binary = await readFile(process.argv[2]);
let hostTimeMs = 1234.75;
const imports = { pcfx: { read_host_file: () => 0, sin: Math.sin, now_ms: () => hostTimeMs } };
const module = await WebAssembly.compile(binary);
const freshCore = async () => (await WebAssembly.instantiate(module, imports)).exports;
const core = await freshCore();

for (const [srcRate, dstRate] of [[11025, 44100], [23144, 44100], [48000, 44100], [23144, 48000]]) {
  for (const level of [10000, -10000]) {
    const frames = core.test_resample_constant(srcRate, dstRate, level);
    assert.ok(frames > 0 && frames <= 8192, 'resampler produces a bounded block');
    const samples = new Int16Array(core.memory.buffer, core.test_audio_ptr(), frames * 2);
    const bad = samples.findIndex(sample => sample !== level);
    assert.equal(bad, -1,
      `${srcRate} -> ${dstRate}: constant ${level} became ${samples[bad]} at sample ${bad}`);
    for (let phase = 0; phase < 256; ++phase)
      assert.equal(core.test_coefficient_sum(phase), 32768, `unity filter gain at phase ${phase}`);
  }
}
console.log('PASS: WASM resampler preserves constant PCM and unity filter gain');

assert.equal(core.test_smc_host_time_ms(), 1234n, 'autosave uses host elapsed milliseconds');
hostTimeMs = 5000000000.25;
assert.equal(core.test_smc_host_time_ms(), 5000000000n, 'elapsed time is not truncated to 32 bits');
console.log('PASS: WASM autosave observes the monotonic host clock');

let failures = 0;
const card = await freshCore();
const geometryFailures = card.test_smc_state_geometry();
assert.equal(geometryFailures, 0, 'wasm32 rejects corrupt geometry and bounds NAND erase arithmetic');
console.log('PASS: WASM SmartMedia saved geometry and erase address bounds');
for (const name of ['test_vfs_growing_writes', 'test_vfs_close_full',
                    'test_vfs_clear_ownership', 'test_vfs_growth_failure']) {
  const vfs = await freshCore();
  const line = vfs[name]();
  if (line) {
    console.error(`FAIL: ${name} at wasm_runtime_test.c:${line}; heap=${vfs.gp32_wasm_heap_used()} bytes`);
    failures++;
  } else console.log(`PASS: ${name}`);
}

/* Use the WASM-side save path used by the browser: temporary VFS writes,
 * publication, staging and removal. The tiny synthetic BIOS never executes. */
const saved = await freshCore();
assert.equal(saved.gp32_wasm_init(44100), 1);
const bios = saved.gp32_wasm_malloc(32);
assert.notEqual(bios, 0);
new Uint32Array(saved.memory.buffer, bios, 8).fill(0xeafffffe);
assert.equal(saved.gp32_wasm_load_bios(bios, 32), 1);
assert.equal(saved.gp32_wasm_start(), 1);
assert.equal(saved.gp32_wasm_save_state(), 1);
const heap = saved.gp32_wasm_heap_used();
const size = saved.gp32_wasm_get_save_size();
assert.ok(size > 8 * 1024 * 1024, 'save includes emulated RAM');
/* The retained staging allocation can hide leaks in free space below it for
 * several saves. Repeat long enough to exceed that space in the regression. */
for (let i = 0; i < 31; ++i) {
  assert.equal(saved.gp32_wasm_save_state(), 1, `save ${i + 2} succeeds`);
  assert.equal(saved.gp32_wasm_get_save_size(), size);
  if (saved.gp32_wasm_heap_used() !== heap) {
    console.error(`FAIL: repeated save grew heap from ${heap} to ${saved.gp32_wasm_heap_used()} bytes`);
    failures++;
    break;
  }
}
if (!failures) console.log(`PASS: repeated ${size}-byte WASM saves reuse heap (${heap} bytes)`);

/* The browser transfers ownership even when an import is attempted before
 * start. Rejection must neither leak the buffer nor poison READY/RUNNING. */
const ready = await freshCore();
assert.equal(ready.gp32_wasm_init(44100), 1);
const readyHeap = ready.gp32_wasm_heap_used();
const untransferred = ready.gp32_wasm_malloc(64);
assert.notEqual(untransferred, 0);
assert.ok(ready.gp32_wasm_heap_used() > readyHeap);
ready.gp32_wasm_free(untransferred);
ready.gp32_wasm_free(0);
assert.equal(ready.gp32_wasm_heap_used(), readyHeap, 'caller can release input before ownership transfer');
const premature = ready.gp32_wasm_malloc(16);
assert.ok(premature);
assert.equal(ready.gp32_wasm_load_state(premature, 16), 0);
assert.equal(ready.gp32_wasm_get_status(), 1, 'rejected premature import preserves READY');
assert.equal(ready.gp32_wasm_heap_used(), readyHeap, 'early rejection frees the transferred buffer');

function captureState(core) {
  assert.equal(core.gp32_wasm_save_state(), 1);
  const bytes = core.gp32_wasm_get_save_size();
  return Buffer.from(new Uint8Array(core.memory.buffer, core.gp32_wasm_get_save_ptr(), bytes));
}
function importState(core, bytes) {
  const ptr = core.gp32_wasm_malloc(bytes.length);
  assert.ok(ptr);
  new Uint8Array(core.memory.buffer, ptr, bytes.length).set(bytes);
  return core.gp32_wasm_load_state(ptr, bytes.length);
}
saved.gp32_wasm_set_input(1);
const live = captureState(saved);
assert.equal(live.subarray(0, 14).toString(), 'GP32STATEv0017');
const pcm = live.indexOf(Buffer.from('PCM1'));
assert.ok(pcm > 0);
for (const cut of [16, pcm + 3, pcm + 80, pcm + 163, live.length - 1]) {
  assert.equal(importState(saved, live.subarray(0, cut)), 0, `reject truncated import at ${cut}`);
  assert.equal(saved.gp32_wasm_get_status(), 2, 'rejected import leaves the running guest available');
  assert.notEqual(saved.gp32_wasm_get_error(), 0, 'rejection remains observable to the browser');
  assert.equal(Buffer.compare(captureState(saved), live), 0, 'rejection preserves complete live guest bytes');
}
assert.equal(importState(saved, live), 1, 'valid import still succeeds after rejection');
assert.equal(Buffer.compare(captureState(saved), live), 0, 'v17 public file/VFS round trip');
assert.equal(saved.test_vfs_fill(), 0, 'occupy the VFS publication slots');
assert.equal(saved.pcfx_wasm_vfs_count(), 128);
assert.equal(saved.gp32_wasm_save_state(), 0, 'full VFS rejects save publication');
assert.equal(saved.gp32_wasm_get_status(), 2, 'failed save leaves the guest running');
assert.equal(importState(saved, live), 0, 'full VFS rejects import staging');
assert.equal(saved.gp32_wasm_get_status(), 2, 'failed import staging leaves the guest running');
saved.pcfx_wasm_vfs_clear();
assert.equal(Buffer.compare(captureState(saved), live), 0, 'publication/staging failure preserves guest bytes');
assert.equal(saved.gp32_wasm_frame(), 1, 'guest execution continues after state-operation failures');
console.log('PASS: WASM state rejection preserves execution/ownership; valid replay and retry succeed');
if (failures) process.exitCode = 1;
