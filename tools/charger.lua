
local machine = manager.machine
local port = machine.ioport.ports[":PORTB"]
local f = port.fields["Charger Connected"]
local sp = machine.devices[":maincpu"].spaces["program"]
local frame = 0
emu.register_frame_done(function()
  frame = frame + 1
  if frame == 1 then
    print("FIELD", f, f.mask)
    local ok, err = pcall(function() f:set_value(0x10) end)
    print("set_value", ok, err)
  end
  if frame % 60 == 0 then
    print(string.format("F%03d IOB=%04X", frame, sp:read_u16(0x3004)))
  end
  if frame >= 180 then machine:exit() end
end)
