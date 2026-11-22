
local machine = manager.machine
local porta = machine.ioport.ports[":PORTA"]
local b = porta.fields["P1 Button 1"]
local frame = 0
emu.register_frame_done(function()
  frame = frame + 1
  if frame == 120 then b:set_value(0); print("press B1 @2s") end
  if frame == 123 then b:set_value(1); print("release B1") end
  if frame % 300 == 0 then print(string.format("F%04d PC=%06X", frame, machine.devices[":maincpu"].state["PC"].value)) end
  if frame >= 1200 then machine:exit() end
end)
