
local machine = manager.machine
local port = machine.ioport.ports[":PORTB"]
local f = port.fields["Charger Connected"]
local sp = machine.devices[":maincpu"].spaces["program"]
local frame = 0
emu.register_frame_done(function()
  frame = frame + 1
  pcall(function() f:set_value(0x10) end)
  if frame % 300 == 0 then
    local pv = "?"
    local ok,v = pcall(function() return port:read() end)
    if ok then pv = string.format("%04X", v) end
    print(string.format("F%04d PORTB=%s IOB=%04X", frame, pv, sp:read_u16(0x3004)))
  end
  if frame >= 1500 then machine:exit() end
end)
