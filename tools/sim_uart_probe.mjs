#!/usr/bin/env node
// Headless AI Passport wasm simulator runner.
// Loads a full 0x0 flash image and prints UART0 output.  No browser, no board
// display model: sufficient for the recomp self-test firmware.
import { readFile } from 'node:fs/promises';
import { pathToFileURL } from 'node:url';
import path from 'node:path';

const SIM = 'C:/Users/DuWeisi/passport-sim/FoloToy-Passport-Simulator-main';
const fwPath = process.argv[2];
const maxVirtualSeconds = Number(process.argv[3] || 8);
if (!fwPath) {
  console.error('usage: node tools/sim_uart_probe.mjs <full.bin> [virtualSeconds]');
  process.exit(2);
}
const { initSync, WasmEmulator } = await import(
  pathToFileURL(path.join(SIM, 'public/wasm/pkg/esp_emu.js')).href
);
const wasm = initSync({ module: await readFile(path.join(SIM, 'public/wasm/pkg/esp_emu_bg.wasm')) });
const emulator = new WasmEmulator('esp32c3');
emulator.load_default_rom();
emulator.set_boot_from_rom(true);
emulator.load_firmware(await readFile(fwPath));

let out = '';
let last = 0;
const started = Date.now();
while (emulator.cycles() < maxVirtualSeconds * 160e6) {
  const chunk = emulator.run_batch(200_000);
  if (chunk) {
    out += chunk;
    process.stdout.write(chunk);
  }
  if (out.includes('SIM_RESULT')) break;
  if (Date.now() - started > 120_000) {
    console.error('\n[sim] wall-clock timeout');
    break;
  }
  const c = emulator.cycles();
  if (c - last >= 160e6) {
    last = c;
    console.error('[sim] t=' + (c / 160e6).toFixed(1) + 's');
  }
}
const pass = /SIM_RESULT PASS/.test(out);
const fail = /SIM_RESULT FAIL/.test(out);
console.log('\n[sim] final cycles=' + emulator.cycles() + ' virtual=' + (emulator.cycles() / 160e6).toFixed(2) + 's');
console.log('[sim] result: ' + (pass ? 'PASS' : fail ? 'FAIL' : 'NO RESULT'));
process.exitCode = pass ? 0 : 1;
