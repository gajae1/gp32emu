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
    stateLoadRequest: 0, stateKey: () => 'fixture-slot',
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
      stateLoadRequest: 0,
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
// Delay only external IndexedDB completion. Run the actual key, database,
// storage and UI functions against isolated in-memory storage, never user data.
const storageCode = declaration('function hashBytesForId(', 'function looksLikeGp32State(') +
  declaration('function dbPut(', 'async function initStorage(') +
  declaration('async function saveStateToStorage(', 'function keyLabel(') +
  declaration('async function start()', 'function pause()');
const slot1 = 'gp32.wasm.savestate.v1.gp32.hle.game.32.12345678.alpha.fpk.1';
const slot2 = 'gp32.wasm.savestate.v1.gp32.hle.game.32.12345678.alpha.fpk.2';
function storageEnvironment() {
  const context = environment({version: 17, accepted: true});
  const local = new Map(), persisted = new Map(), pending = [], downloads = [];
  const db = {
    transaction() {
      const tx = {objectStore: () => ({
        put(value, key) {
          const bytes = value.slice(); // IndexedDB clones at put(), not completion.
          pending.push({key, succeed() {persisted.set(key, bytes); tx.oncomplete();},
            fail() {tx.error = new Error('write rejected'); tx.onerror();}});
        },
        get(key) {
          const req = {};
          pending.push({key, succeed(value = persisted.get(key) || null) {req.result = value; req.onsuccess();},
            fail() {req.error = new Error('read rejected'); req.onerror();}});
          return req;
        }
      })};
      return tx;
    }
  };
  Object.assign(context, {
    db, dbReady: true, STORE_FILES: 'files', DB_STATE_PREFIX: 'state:',
    STORAGE_STATE_PREFIX: 'gp32.wasm.savestate.v1.',
    gameLoaded: true, biosLoaded: false, currentLaunchUsesHle: true,
    gameMeta: {kind: 'game', name: 'alpha.fpk', size: 32, hash: '12345678'}, biosMeta: null,
    localStorage: {getItem: key => local.get(key) ?? null,
      setItem: (key, value) => local.set(key, value), removeItem: key => local.delete(key)},
    btoa: text => Buffer.from(text, 'binary').toString('base64'),
    atob: text => Buffer.from(text, 'base64').toString('binary'),
    wasmU8: () => new Uint8Array(context.memory.buffer),
    Blob, URL: {createObjectURL: () => 'blob:fixture', revokeObjectURL() {}}, setTimeout: fn => fn(),
    ensureAudio: async () => {}, resetFrameLimiter() {}, performance: {now: () => 100},
    hiddenPauseWasRunning: false, framesThisSecond: 0, displayFramesThisSecond: 0,
    lastFpsTime: 0, lastStatsDomTime: 0, lastAudioDomTime: 0,
    fileBytes: async () => new Uint8Array(16), hleMode: {checked: true}, log() {}
  });
  context.document.body.classList.add = () => {};
  context.document.body.appendChild = () => {};
  context.document.createElement = () => ({click() {downloads.push(this.download);}, remove() {}});
  Object.assign(context.exp, {
    gp32_wasm_save_state: () => {new Uint8Array(context.memory.buffer, 64, 32).set(state(17)); return 1;},
    gp32_wasm_get_save_ptr: () => 64, gp32_wasm_get_save_size: () => 32,
    gp32_wasm_start: () => {context.events.push('start core'); return 1;},
    gp32_wasm_load_bios: () => {context.events.push('bios core'); return 1;},
    gp32_wasm_load_media: () => {context.events.push('media core'); return 1;}
  });
  vm.runInContext(storageCode, context);
  return {context, local, persisted, pending, downloads};
}
const encoded = bytes => Buffer.from(bytes).toString('base64');
for (const change of ['slot', 'game']) {
  const {context: c, local, persisted, pending} = storageEnvironment();
  local.set(slot1, 'original fallback'); local.set(slot2, 'unrelated fallback');
  const bytes = state(17), save = c.putStateBytes(bytes);
  if (change === 'slot') c.config.stateSlot = 2;
  else {
    c.gameMeta = {...c.gameMeta, name: 'beta.fpk'};
    local.set('gp32.wasm.savestate.v1.gp32.hle.game.32.12345678.beta.fpk.1', 'other game');
  }
  pending[0].succeed(); await save;
  check(Buffer.from(persisted.get('state:' + slot1)).equals(Buffer.from(bytes)), `${change}: save commits to original database key`);
  check(!local.has(slot1), `${change}: successful put removes only its own old fallback`);
  check(local.get(slot2) === 'unrelated fallback', `${change}: another slot survives delayed put`);
  if (change === 'game') check(local.get('gp32.wasm.savestate.v1.gp32.hle.game.32.12345678.beta.fpk.1') === 'other game', 'another game survives delayed put');
}
{
  const {context: c, local, pending} = storageEnvironment();
  local.set(slot1, encoded(state(16))); local.set(slot2, encoded(state(17)));
  const read = c.getStateBytes(); c.config.stateSlot = 2;
  pending[0].succeed(null);
  check(Buffer.from(await read).equals(Buffer.from(state(16))), 'database miss reads the originally requested fallback slot');
}
{
  const {context: c, local, pending} = storageEnvironment();
  local.set(slot1, 'slot one'); local.set(slot2, 'slot two');
  const write = c.putStateBytes(state(17)); c.config.stateSlot = 2;
  pending[0].fail();
  let rejected = false; try {await write;} catch {rejected = true;}
  check(rejected && local.get(slot1) === 'slot one' && local.get(slot2) === 'slot two', 'failed database commit preserves both fallback saves');
}
for (const action of ['slot', 'game', 'core', 'restart', 'bios', 'media', 'read-error', 'not-running']) {
  const {context: c, pending} = storageEnvironment();
  const loading = c.loadStateFromStorage();
  if (action === 'slot' || action === 'read-error') c.config.stateSlot = 2;
  if (action === 'game') c.gameMeta = {...c.gameMeta, name: 'beta.fpk'};
  if (action === 'core') c.exp = {...c.exp};
  if (action === 'not-running') c.exp.gp32_wasm_get_status = () => 1;
  if (action === 'restart') await c.start();
  if (action === 'bios') await c.loadBiosFile({name: 'fixture.bin'});
  if (action === 'media') await c.loadGameFile({name: 'fixture.fpk'});
  const beforeEvents = c.events.length, beforeStatuses = c.statuses.length;
  if (action === 'read-error') pending[0].fail(); else pending[0].succeed(state(17));
  await loading;
  check(c.events.length === beforeEvents, `${action}: stale load does not allocate, restore, reset delivery or reschedule`);
  check(c.statuses.length === beforeStatuses, `${action}: stale completion does not overwrite current storage status`);
}
{
  const {context: c, pending} = storageEnvironment();
  const older = c.loadStateFromStorage(), newer = c.loadStateFromStorage();
  pending[1].succeed(state(17)); await newer;
  const eventCount = c.events.length, statusCount = c.statuses.length;
  pending[0].succeed(state(16)); await older;
  check(c.events.filter(e => e === 'load').length === 1 && c.events.length === eventCount, 'older same-slot request cannot overwrite newer restored state');
  check(c.statuses.length === statusCount, 'older request cannot overwrite newer load status');
}
{
  const {context: c, pending} = storageEnvironment();
  const save = c.saveStateToStorage(); c.config.stateSlot = 2;
  pending[0].succeed(); await save;
  check(c.statuses.some(s => s.includes('slot 1 saved')), 'save completion identifies its original slot');
}
{
  const {context: c, pending, downloads} = storageEnvironment();
  const exporting = c.exportStateToFile(); c.config.stateSlot = 2; c.gameMeta = {...c.gameMeta, name: 'beta.fpk'};
  pending[0].succeed(state(17)); await exporting;
  check(downloads.length === 1 && downloads[0] === 'alpha_slot01.gp32st', 'export retains the requested game and slot filename');
  check(c.statuses.some(s => s.includes('slot 1')), 'export completion identifies its original slot');
}
{
  // Reloading identical media keeps its storage key. The old read must be
  // canceled at media mutation, even while start() is awaiting audio setup.
  const {context: c, pending} = storageEnvironment();
  const file = {name: 'alpha.fpk'}, bytes = new Uint8Array(16);
  c.gameMeta = c.mediaMeta(file, bytes, 'game');
  const originalKey = c.stateKey();
  let releaseAudio, reachedAudio;
  const waiting = new Promise(resolve => {reachedAudio = resolve;});
  const audioReady = new Promise(resolve => {releaseAudio = resolve;});
  c.ensureAudio = () => {reachedAudio(); return audioReady;};
  const loading = c.loadStateFromStorage(), replacing = c.loadGameFile(file);
  await waiting;
  assert.equal(c.stateKey(), originalKey, 'fixture replaces the same content identity');
  const beforeEvents = c.events.length;
  pending[0].succeed(state(17)); await loading;
  check(c.events.length === beforeEvents, 'same-media replacement cancels old restore before delayed start');
  releaseAudio(); await replacing;
}
{
  const {context: c, pending} = storageEnvironment();
  const loading = c.loadStateFromStorage(); pending[0].fail(); await loading;
  check(c.events.length === 0, 'current read failure preserves delivery and does not allocate');
  check(c.statuses.length === 1 && c.statuses[0].includes('read rejected'), 'current read failure remains visible');
  const retry = c.loadStateFromStorage(); pending[1].succeed(state(17)); await retry;
  check(c.events.filter(e => e === 'load').length === 1, 'valid storage read retries after a database error');
}
{
  const {context: c, local} = storageEnvironment(); c.dbReady = false;
  await c.putStateBytes(state(17));
  check(local.get(slot1) === encoded(state(17)), 'direct fallback preserves the existing storage key and bytes');
  check(Buffer.from(await c.getStateBytes()).equals(Buffer.from(state(17))), 'direct fallback remains discoverable');
}
console.log(`Browser allocation and savestate helpers: ${checks} checks, ${failures} failures`);
if (failures) process.exitCode = 1;
