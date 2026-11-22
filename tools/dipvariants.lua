
local machine = manager.machine
local port = machine.ioport.ports[":PORTB"]
local f = port.fields["Charger Connected"]
local sp = machine.devices[":maincpu"].spaces["program"]
local frame = 0
local function dump(tag)
  print(string.format("%s port=%04X iob=%04X", tag, port:read(), sp:read_u16(0x3004)))
end
emu.register_frame_done(function()
  frame = frame + 1
  if frame == 5 then dump("before") end
  if frame == 6 then print("set 1 ->", f:set_value(1)); dump("after1") end
  if frame == 7 then print("set 0x10 ->", f:set_value(0x10)); dump("after10") end
  if frame == 8 then print("set 0 ->", f:set_value(0)); dump("after0") end
  if frame >= 12 then machine:exit() end
end)
