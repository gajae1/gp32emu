import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

const binary = await readFile(process.argv[2]);
let hostTimeMs = 1234.75;
const imports = { pcfx: { read_host_file: () => 0, sin: Math.sin, now_ms: () => hostTimeMs } };
const { instance } = await WebAssembly.instantiate(binary, imports);
const core = instance.exports;

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
