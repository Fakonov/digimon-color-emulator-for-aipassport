
local machine = manager.machine
local port = machine.ioport.ports[":PORTB"]
local sp = machine.devices[":maincpu"].spaces["program"]
local frame = 0
emu.register_frame_done(function()
  frame = frame + 1
  if frame == 1 then
    print("PORTB read", string.format("%04X", port:read()))
    print("IOB", string.format("%04X", sp:read_u16(0x3004)))
  end
  if frame >= 120 then machine:exit() end
end)
