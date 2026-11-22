
local machine = manager.machine
local porta = machine.ioport.ports[":PORTA"]
local b = porta.fields["P1 Button 2"]
local frame = 0
emu.register_frame_done(function()
  frame = frame + 1
  if frame == 120 then b:set_value(0); print("press B2 @2s") end
  if frame == 123 then b:set_value(1); print("release B2") end
  if frame >= 900 then machine:exit() end
end)
