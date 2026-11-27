-- v4: same boot timing that worked in v1 (B at 240, C at 380), then open each
-- icon menu immediately after selecting it (selection appears to expire).
local machine = manager.machine
local porta = machine.ioport.ports[":PORTA"]
local btn = {
  A = porta.fields["P1 Button 1"],
  B = porta.fields["P1 Button 2"],
  C = porta.fields["P1 Button 3"],
}
local space = machine.devices[":maincpu"].spaces["program"]
local scr = machine.screens[":screen"]
local OUT = "C:/Users/DuWeisi/digimon-color-passport/tmp/mameshots4/"

local function set(k, v) pcall(function() btn[k]:set_value(v) end) end
local function shot(n)
  local ok, err = pcall(function() scr:snapshot(OUT .. n) end)
  local st = "?"
  pcall(function() st = string.format("%04X", space:read_u16(0x0C88)) end)
  print(string.format("SHOT %-14s %s state=%s", n, ok and "ok" or tostring(err), st))
end

local ev = {}
local function down(f, k) ev[#ev + 1] = { f, "down", k } end
local function up(f, k) ev[#ev + 1] = { f, "up", k } end
local function tap(f, k, n) n = n or 8; down(f, k); up(f + n, k) end
local function taps(f, k, n, gap) gap = gap or 40; for i = 0, n - 1 do tap(f + i * gap, k) end return f + n * gap end
local function at(f, n) ev[#ev + 1] = { f, "shot", n } end

at(60, "00_boot")
tap(240, "B")
tap(380, "C")
at(1000, "01_main")

local f = 1060
local function step(name, presses)
  f = taps(f, "C", 2, 30)
  f = taps(f, "A", presses, 40)
  tap(f + 20, "B")
  at(f + 90, name)
  f = f + 150
end
step("02_status", 1)
step("03_food", 2)
step("04_train", 3)
step("05_battle", 4)
step("06_sleep", 6)
step("07_heal", 7)

f = taps(f, "C", 3, 30)
down(f + 10, "C"); up(f + 180, "C")
at(f + 250, "08_settings")
at(f + 330, "09_settings2")

ev[#ev + 1] = { f + 380, "exit", "" }
table.sort(ev, function(a, b) return a[1] < b[1] end)

local idx, frame = 1, 0
emu.register_frame_done(function()
  frame = frame + 1
  while idx <= #ev and frame >= ev[idx][1] do
    local e = ev[idx]
    if e[2] == "shot" then shot(e[3])
    elseif e[2] == "down" then set(e[3], 1)
    elseif e[2] == "up" then set(e[3], 0)
    elseif e[2] == "exit" then machine:exit() end
    idx = idx + 1
  end
end)
