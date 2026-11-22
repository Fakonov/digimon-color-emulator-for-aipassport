
local machine = manager.machine
local porta = machine.ioport.ports[":PORTA"]
local b1 = porta.fields["P1 Button 1"]
local b2 = porta.fields["P1 Button 2"]
local b3 = porta.fields["P1 Button 3"]
local sp = machine.devices[":maincpu"].spaces["program"]
local frame = 0
local function press(f, v) local ok,err = pcall(function() f:set_value(v) end); if not ok then print("ERR", err) end end
emu.register_frame_done(function()
  frame = frame + 1
  if frame == 60 then print("PORTA before", string.format("%04X", porta:read())) end
  if frame == 61 then press(b1,1); print("pressed B1", string.format("%04X", porta:read())) end
  if frame == 75 then press(b1,0); print("released B1", string.format("%04X", porta:read())) end
  if frame == 300 then press(b2,1); print("pressed B2", string.format("%04X", porta:read())) end
  if frame == 315 then press(b2,0) end
  if frame == 1080 then press(b3,1); print("pressed B3 @18s", string.format("%04X", porta:read())) end
  if frame == 1095 then press(b3,0) end
  if frame == 1200 then press(b1,1); print("pressed B1 @20s", string.format("%04X", porta:read())) end
  if frame == 1215 then press(b1,0) end
  if frame % 300 == 0 then
    print(string.format("F%04d PC=%06X PORTA=%04X", frame, machine.devices[":maincpu"].state["PC"].value, porta:read()))
  end
  if frame >= 1500 then machine:exit() end
end)
