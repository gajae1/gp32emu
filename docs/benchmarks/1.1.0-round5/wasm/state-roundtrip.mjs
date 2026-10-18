/* Evidence-only public-API check; the production/test source tree is unchanged.
 * The public load wrapper owns and frees its import allocation. Never pass its
 * retained save staging pointer directly to that wrapper. */
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';

const pwmBytes = 92;
const imports = { pcfx: { read_host_file: () => 0, sin: Math.sin, now_ms: () => 1234.75 } };
const sha = bytes => createHash('sha256').update(bytes).digest('hex');
const savedBytes = core => Buffer.from(new Uint8Array(core.memory.buffer,
  core.gp32_wasm_get_save_ptr(), core.gp32_wasm_get_save_size()));
const loadBytes = (core, bytes) => {
  const pointer = core.gp32_wasm_malloc(bytes.length);
  assert.notEqual(pointer, 0, 'allocate distinct import buffer');
  new Uint8Array(core.memory.buffer, pointer, bytes.length).set(bytes);
  return core.gp32_wasm_load_state(pointer, bytes.length);
};

for (const path of process.argv.slice(2)) {
  const binary = await readFile(path);
  const module = await WebAssembly.compile(binary);
  const core = (await WebAssembly.instantiate(module, imports)).exports;
  assert.equal(core.gp32_wasm_init(44100), 1);
  const bios = core.gp32_wasm_malloc(32);
  assert.notEqual(bios, 0);
  new Uint32Array(core.memory.buffer, bios, 8).fill(0xeafffffe);
  assert.equal(core.gp32_wasm_load_bios(bios, 32), 1);
  assert.equal(core.gp32_wasm_start(), 1);
  assert.equal(core.gp32_wasm_save_state(), 1);
  const template = savedBytes(core);
  assert.ok(template.length > 8 * 1024 * 1024, 'state contains emulated RAM');
  assert.equal(template.subarray(0, 14).toString('ascii'), 'GP32STATEv0016');
  const tailOffset = template.length - pwmBytes;
  assert.equal(template.subarray(tailOffset, tailOffset + 4).toString('ascii'), 'PWM1');
  assert.ok(template.readUInt32LE(tailOffset + 4) > 0, 'valid serialized PWM clock');
  const pc = core.gp32_wasm_get_pc();
  const cycles = core.gp32_wasm_get_cycles_lo();
  let finalState;
  const heapSamples = [];
  for (let iteration = 0; iteration < 32; ++iteration) {
    const state = Buffer.from(template);
    state.writeUInt32LE(0, tailOffset + 8); // No pending auto-reload.
    for (let timer = 0; timer < 5; ++timer) {
      const offset = tailOffset + 12 + timer * 16;
      /* Stopped timers may retain any 16-bit count and a fractional phase.
       * Keep the phase below even the smallest valid clock period, and vary
       * every timer on every import so a stale/ignored trailer cannot pass. */
      state.writeUInt32LE(((iteration + 1) * 257 + timer * 8191) & 0xffff, offset);
      state.writeUInt32LE(0, offset + 4);
      state.writeBigUInt64LE(BigInt((iteration + 1) * 17 + timer), offset + 8);
    }
    assert.equal(loadBytes(core, state), 1, `round trip ${iteration + 1}: load`);
    assert.equal(core.gp32_wasm_get_pc(), pc, 'load preserves CPU PC');
    assert.equal(core.gp32_wasm_get_cycles_lo(), cycles, 'load preserves CPU cycles');
    assert.equal(core.gp32_wasm_save_state(), 1, `round trip ${iteration + 1}: save`);
    finalState = savedBytes(core);
    assert.equal(finalState.length, template.length, 'state size remains fixed');
    assert.deepEqual(finalState.subarray(tailOffset), state.subarray(tailOffset),
      `round trip ${iteration + 1}: every PWM tail byte is preserved`);
    assert.equal(core.pcfx_wasm_vfs_count(), 0, 'save/load staging files are removed');
    heapSamples.push(core.gp32_wasm_heap_used());
    /* Loading commits a replacement 8 MiB RAM allocation. The first-fit
     * allocator alternates its placement around retained save staging.
     * Heap top is not the sum of live allocations. Require the observed
     * two-placement pattern to repeat, with at most one RAM block of slack. */
    if (iteration >= 2) assert.equal(heapSamples[iteration], heapSamples[iteration % 2],
      'heap placement repeats without accumulating growth');
    assert.ok(heapSamples[iteration] <= heapSamples[0] + 8 * 1024 * 1024 + 16,
      'heap remains within one replacement RAM allocation of the first round trip');
  }
  assert.equal(loadBytes(core, finalState.subarray(0, finalState.length - 1)), 0,
    'v16 rejects a truncated required PWM trailer');
  /* set_error intentionally moves the wrapper out of RUNNING. A subsequent
   * save is unavailable until the frontend restarts/reinitializes it. */
  assert.equal(core.gp32_wasm_get_status(), 5, 'rejected import reports ERROR status');
  assert.equal(core.gp32_wasm_get_error(), 5, 'rejected import reports BAD_STATE');
  assert.equal(core.gp32_wasm_get_pc(), pc, 'rejected import preserves CPU PC');
  assert.equal(core.gp32_wasm_get_cycles_lo(), cycles, 'rejected import preserves CPU cycles');
  assert.equal(core.pcfx_wasm_vfs_count(), 0, 'failed import removes its VFS staging');
  console.log(JSON.stringify({
    result: 'PASS', module: path, module_sha256: sha(binary),
    state_version: 'GP32STATEv0016', state_bytes: template.length,
    pwm_magic: 'PWM1', pwm_bytes: pwmBytes, round_trips: 32,
    distinct_timer_count_and_phase_pairs: 160,
    truncated_tail_rejections: 1, cpu_pc: pc, cpu_cycles: cycles,
    heap_min_bytes: Math.min(...heapSamples), heap_max_bytes: Math.max(...heapSamples),
    heap_samples: heapSamples, final_state_sha256: sha(finalState),
    imports: WebAssembly.Module.imports(module),
  }));
}
