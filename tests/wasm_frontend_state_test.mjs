/* Execute the real browser helper/handler declarations with bounded host
 * collaborators. The WASM runtime suite separately exercises the real core. */
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import vm from 'node:vm';

const source = await readFile(process.argv[2] || new URL('../web/gp32-wasm.js', import.meta.url), 'utf8');
function declaration(start, next) {
  const begin = source.indexOf(start), end = source.indexOf(next, begin + start.length);
  assert.ok(begin >= 0 && end > begin, `find actual declaration: ${start}`);
  return source.slice(begin, end);
}
const code = declaration('function copyToWasm(', 'async function fileBytes(') +
  declaration('function looksLikeGp32State(', 'function openDb(') +
  declaration('async function loadBiosFile(', 'async function saveStateToStorage(') +
  declaration('async function loadStateFromStorage(', 'async function exportStateToFile(');
function state(version) {
  const bytes = new Uint8Array(32);
  bytes.set(new TextEncoder().encode(`GP32STATEv${String(version).padStart(4, '0')}`));
  return bytes;
}
function environment({version = 2, accepted = false, running = true, oom = false} = {}) {
  const events = [], delivery = [123, -456], statuses = [];
  const memory = {buffer: new ArrayBuffer(128)};
  new Uint8Array(memory.buffer).fill(0x5a);
  const context = vm.createContext({
    Uint8Array, TextEncoder, memory, events, delivery, statuses,
    running, raf: 9, lastDrawnCoreFrame: 4, config: {stateSlot: 1}, loop() {},
    exp: {
      gp32_wasm_get_status: () => 2,
      gp32_wasm_malloc: () => {events.push('allocate'); return oom ? 0 : 64;},
      gp32_wasm_load_state: () => {events.push('load'); return accepted ? 1 : 0;}
    },
    getStateBytes: async () => state(version),
    errorText: () => 'rejected fixture',
    resetAudioQueues: () => {events.push('reset audio'); delivery.length = 0;},
    clearAllInput: () => events.push('clear input'),
    cancelAnimationFrame: () => events.push('cancel frame'),
    requestAnimationFrame: () => {events.push('request frame'); return 10;},
    updatePauseButton: () => {}, drawFrame: () => events.push('draw'), updateStats: () => {},
    setStorageStatus: message => statuses.push(message), setState: () => {},
    document: {body: {classList: {toggle: () => {}}}}
  });
  vm.runInContext(code, context);
  return context;
}
let failures = 0, checks = 0;
function check(condition, label) {
  checks++;
  if (!condition) {console.error(`FAIL: ${label}`); failures++;}
}

const format = environment();
const supported = new Set([2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 15, 16, 17]);
for (let version = 0; version <= 19; version++)
  check(format.looksLikeGp32State(state(version)) === supported.has(version), `state version ${version}`);
for (const cut of [0, 13, 14, 15])
  check(!format.looksLikeGp32State(state(17).slice(0, cut)), `header cut ${cut}`);
for (const offset of [0, 9, 10, 11, 12, 13, 14, 15]) {
  const corrupt = state(17); corrupt[offset] = 0xff;
  check(!format.looksLikeGp32State(corrupt), `malformed header byte ${offset}`);
}

for (const helper of ['copyToWasm', 'copyStringToWasm']) {
  const context = environment({oom: true});
  let threw = false;
  try {context[helper](helper === 'copyToWasm' ? state(17) : 'name.gp32st');}
  catch {threw = true;}
  check(threw, `${helper} reports allocation failure`);
  check(new Uint8Array(context.memory.buffer).every(byte => byte === 0x5a), `${helper} preserves address zero on OOM`);
}
for (const running of [false, true]) {
  const rejected = environment({running});
  await rejected.loadStateFromStorage();
  check(rejected.events.filter(event => event === 'load').length === 1, 'recognized state reaches the core');
  check(rejected.delivery.length === 2 && rejected.delivery[0] === 123, 'rejection preserves queued browser audio');
  check(!rejected.events.includes('clear input') && !rejected.events.includes('draw'), 'rejection preserves input and presentation');
  check(rejected.running === running, 'rejection preserves prior running/paused state');
  check(rejected.events.includes('request frame') === running, 'rejection resumes only a previously running loop');

  const accepted = environment({version: 17, accepted: true, running});
  await accepted.loadStateFromStorage();
  check(accepted.events.filter(event => event === 'load').length === 1, 'v17 load reaches the core');
  check(accepted.events.indexOf('reset audio') > accepted.events.indexOf('load'), 'successful commit precedes browser audio reset');
  check(accepted.events.filter(event => event === 'reset audio').length === 1, 'successful load resets browser audio once');
  check(accepted.running === running && accepted.events.includes('clear input'), 'success restores play state and clears old input');

  const failedAllocation = environment({running, oom: true});
  let escaped = false;
  try {await failedAllocation.loadStateFromStorage();} catch {escaped = true;}
  check(!escaped && failedAllocation.statuses.length === 1, 'load reports OOM through storage status');
  check(failedAllocation.running === running && failedAllocation.delivery.length === 2, 'OOM preserves play state and audio');
  check(!failedAllocation.events.includes('load') && !failedAllocation.events.includes('cancel frame'), 'OOM is handled before pausing or entering the core');
  check(new Uint8Array(failedAllocation.memory.buffer).every(byte => byte === 0x5a), 'load OOM preserves linear memory');
}

// Shared copy helpers also serve BIOS/media loaders. In particular, failure of
// the filename allocation must release the already allocated game bytes.
for (const kind of ['bios', 'game']) {
  for (const failAt of (kind === 'bios' ? [0, 1] : [0, 1, 2])) {
    const owned = new Set(), events = [], messages = [];
    let allocations = 0;
    const memory = {buffer: new ArrayBuffer(512)};
    new Uint8Array(memory.buffer).fill(0x5a);
    const context = vm.createContext({Uint8Array, TextEncoder, memory,
      fileBytes: async () => new Uint8Array(64).fill(0x23),
      exp: {
        gp32_wasm_malloc: () => {
          const index = ++allocations, ptr = index === failAt ? 0 : index * 128;
          if (ptr) owned.add(ptr);
          return ptr;
        },
        gp32_wasm_free: ptr => {events.push('free'); check(owned.delete(ptr), `${kind}: release owned allocation once`);},
        gp32_wasm_load_bios: ptr => {events.push('core'); owned.delete(ptr); return 1;},
        gp32_wasm_load_media: (ptr, size, namePtr) => {
          events.push('core'); owned.delete(ptr); owned.delete(namePtr); return 1;
        }
      },
      hleMode: {checked: false}, biosLoaded: false, currentLaunchUsesHle: false,
      biosMeta: null, gameLoaded: false, gameMeta: null, mediaMeta: () => ({}),
      setState: value => messages.push(value), log: value => messages.push(value),
      start: async () => events.push('start'), errorText: () => 'fixture'
    });
    vm.runInContext(code, context);
    let escaped = false;
    try {await context[kind === 'bios' ? 'loadBiosFile' : 'loadGameFile']({name: `${kind}.fpk`});}
    catch {escaped = true;}
    check(!escaped, `${kind} allocation ${failAt}: no unhandled rejection`);
    check(owned.size === 0, `${kind} allocation ${failAt}: ownership fully transferred or released`);
    check(events.includes('core') === (failAt === 0), `${kind} allocation ${failAt}: only complete input reaches core`);
    check(messages.some(message => String(message).includes('WASM memory')) === (failAt !== 0), `${kind} allocation ${failAt}: report allocation error`);
    check(new Uint8Array(memory.buffer, 0, 64).every(byte => byte === 0x5a), `${kind} allocation ${failAt}: address zero preserved`);
    check(context[kind === 'bios' ? 'biosLoaded' : 'gameLoaded'] === (failAt === 0), `${kind} allocation ${failAt}: loaded flag reflects success`);
  }
  const context = environment();
  context.fileBytes = async () => new Uint8Array(0);
  context.log = message => context.statuses.push(message);
  let escaped = false;
  try {await context[kind === 'bios' ? 'loadBiosFile' : 'loadGameFile']({name: 'empty.bin'});}
  catch {escaped = true;}
  check(!escaped && context.statuses.some(message => message.includes('empty')), `${kind}: empty file is reported`);
  check(context.events.length === 0, `${kind}: empty file does not allocate or enter core`);
}
console.log(`Browser allocation and savestate helpers: ${checks} checks, ${failures} failures`);
if (failures) process.exitCode = 1;
