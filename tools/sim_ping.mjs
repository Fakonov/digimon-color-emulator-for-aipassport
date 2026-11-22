// Boot-mode probe: which combination of load_default_rom / set_boot_from_rom
// actually starts an ESP-IDF image from flash offset 0?
//
//   node tools/sim_ping.mjs <firmware-full.bin>

import { readFile } from 'node:fs/promises';
import { pathToFileURL } from 'node:url';
import path from 'node:path';

const SIM = 'C:/Users/DuWeisi/passport-sim/FoloToy-Passport-Simulator-main';
const fwPath = process.argv[2];
if (!fwPath) { console.error('usage: node tools/sim_ping.mjs <firmware.bin>'); process.exit(2); }

const { initSync, WasmEmulator } = await import(pathToFileURL(path.join(SIM, 'public/wasm/pkg/esp_emu.js')).href);
const firmware = await readFile(fwPath);
const wasmBytes = await readFile(path.join(SIM, 'public/wasm/pkg/esp_emu_bg.wasm'));

const modes = [
  { name: 'default_rom + boot=false', rom: true, boot: false },
  { name: 'default_rom + boot=true ', rom: true, boot: true },
  { name: 'no rom, boot=false       ', rom: false, boot: false },
  { name: 'no rom, boot=true        ', rom: false, boot: true },
];

for (const m of modes) {
  const wasm = initSync({ module: wasmBytes });
  const emu = new WasmEmulator('esp32c3');
  if (m.rom) emu.load_default_rom();
  emu.set_boot_from_rom(m.boot);
  emu.load_firmware(firmware);
  let uart = '';
  const t0 = performance.now();
  for (let i = 0; i < 5; i++) uart += emu.run_batch(200_000);
  const ms = performance.now() - t0;
  const cyc = emu.cycles();
  const txt = uart.replace(/[^\x20-\x7e\n]/g, '');
  console.log(`${m.name}  ${ms.toFixed(0)} ms  cycles=${cyc}  uart=${uart.length}B`);
  if (txt.trim()) console.log('   ' + txt.trim().split('\n').slice(0, 6).join('\n   '));
  emu.free?.();
}
