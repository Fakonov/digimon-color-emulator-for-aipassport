// Headless driver for the FoloToy AI Passport simulator (ESP-EMU wasm), used to
// run the digimon-color C3 firmware on the PC before touching hardware.
//
//   node tools/sim_color.mjs <firmware-full.bin> [outDir] [--seconds=N] [--shot-every=S]
//
// The image must contain the ROM partition at 0x210000 (tools/make_sim_image.ps1),
// otherwise guest_init() reports "no usable ROM".
//
// Why this works: run_batch(n) advances the emulated C3 by n instructions and
// returns the bytes UART0 produced.  emulator.cycles() counts CPU cycles, so
// virtual time = cycles / 160e6 is "how long the firmware would take on a
// 160 MHz C3 that never misses flash".  Flash timing and the 16 KB ICache are not
// modelled, so QIO / IRAM gains do not show up here - only RISC-V work does.
// That makes this the right place to measure the AOT rewriter.

import { readFile, writeFile, mkdir } from 'node:fs/promises';
import { deflateSync } from 'node:zlib';
import { pathToFileURL } from 'node:url';
import path from 'node:path';

const SIM = 'C:/Users/DuWeisi/passport-sim/FoloToy-Passport-Simulator-main';

function parseArgs(argv) {
  const out = { _: [] };
  for (const a of argv) {
    const m = /^--([\w-]+)(?:=(.*))?$/.exec(a);
    if (m) out[m[1]] = m[2] === undefined ? true : m[2];
    else out._.push(a);
  }
  return out;
}
const args = parseArgs(process.argv.slice(2));
const fwPath = args._[0];
const outDir = args._[1] || 'docs/sim-color';
const seconds = Number(args.seconds || 8);
const shotEvery = Number(args['shot-every'] || 1);
if (!fwPath) {
  console.error('usage: node tools/sim_color.mjs <firmware-full.bin> [outDir] [--seconds=N]');
  process.exit(2);
}
await mkdir(outDir, { recursive: true });

const { initSync, WasmEmulator } = await import(pathToFileURL(path.join(SIM, 'public/wasm/pkg/esp_emu.js')).href);
const { AiPassportBoard } = await import(pathToFileURL(path.join(SIM, 'public/wasm/ai-passport-board.js')).href);

const firmware = await readFile(fwPath);
const wasm = initSync({ module: await readFile(path.join(SIM, 'public/wasm/pkg/esp_emu_bg.wasm')) });
const emulator = new WasmEmulator('esp32c3');
// The simulator needs the C3 boot ROM before it can run the 2nd-stage bootloader
// from flash offset 0.  boot_from_rom=false means "boot the merged image", not
// "execute the game ROM" (that is the Digimon-direct path).
emulator.load_default_rom();
emulator.set_boot_from_rom(false);
emulator.load_firmware(firmware);

let frames = 0;
const board = new AiPassportBoard(wasm, { emulator, onFrame() { frames++; }, onAudio() {} });
board.releaseButtons();

/* ---- PNG ----------------------------------------------------------------- */
function crc32b(buf) {
  let c, crc = 0xFFFFFFFF;
  for (let n = 0; n < buf.length; n++) {
    c = (crc ^ buf[n]) & 0xFF;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
    crc = c ^ (crc >>> 8);
  }
  return (crc ^ 0xFFFFFFFF) >>> 0;
}
function chunk(type, data) {
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type, 'ascii'), data]);
  const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32b(td));
  return Buffer.concat([len, td, crc]);
}
function pngFromFramebuffer(fb, w, h) {
  const raw = Buffer.alloc((w * 4 + 1) * h);
  for (let y = 0; y < h; y++) {
    raw[y * (w * 4 + 1)] = 0;
    Buffer.from(fb.buffer, fb.byteOffset + y * w * 4, w * 4).copy(raw, y * (w * 4 + 1) + 1);
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 6;
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]),
    chunk('IHDR', ihdr),
    chunk('IDAT', deflateSync(raw)),
    chunk('IEND', Buffer.alloc(0)),
  ]);
}

/* ---- run ----------------------------------------------------------------- */
const TARGET_CYCLES = seconds * 160e6;
const BATCH = 200_000;
let uart = '';
let lastShot = -1;
const shots = [];
let batch = 0;
let stalls = 0;
let lastCyc = -1;
while (emulator.cycles() < TARGET_CYCLES) {
  uart += emulator.run_batch(BATCH);
  board.drain();
  batch++;
  if (emulator.cycles() === lastCyc) {
    if (++stalls >= 10) throw new Error(`CPU stalled at ${emulator.cycles()} cycles`);
  } else {
    stalls = 0;
    lastCyc = emulator.cycles();
  }
  if (batch % 50 === 0) {
    console.log(`  ... batch=${batch} t=${(emulator.cycles() / 160e6).toFixed(3)}s frames=${frames} uart=${uart.length}B`);
  }
  const t = emulator.cycles() / 160e6;
  if (t - lastShot >= shotEvery) {
    lastShot = t;
    const d = board.display;
    const fb = d.framebuffer;
    let lit = 0;
    for (let i = 0; i < fb.length; i += 4) if (fb[i] | fb[i + 1] | fb[i + 2]) lit++;
    const name = path.join(outDir, `t${t.toFixed(1).padStart(5, '0')}.png`);
    await writeFile(name, pngFromFramebuffer(fb, d.width, d.height));
    shots.push({ t, name, lit, frames });
  }
}

await writeFile(path.join(outDir, 'uart.log'), uart, 'utf8');
console.log(`virtual=${(emulator.cycles() / 160e6).toFixed(2)}s frames=${frames} shots=${shots.length}`);
for (const s of shots) console.log(`  ${s.name}  t=${s.t.toFixed(1)}s  lit=${s.lit}px  frames=${s.frames}`);
const lines = uart.split('\n').filter((l) => l.trim());
console.log(`uart lines=${lines.length}`);
for (const l of lines.slice(-40)) console.log('  ' + l);
