
local machine = manager.machine
local porta = machine.ioport.ports[":PORTA"]
local b1 = porta.fields["P1 Button 1"]
local frame = 0
emu.register_frame_done(function()
  frame = frame + 1
  if frame == 120 or (frame > 120 and frame % 360 == 0) then b1:set_value(0); print("press @F"..frame) end
  if frame == 125 or (frame > 125 and frame % 360 == 5) then b1:set_value(1) end
  if frame % 600 == 0 then
    print(string.format("F%04d PC=%06X", frame, machine.devices[":maincpu"].state["PC"].value))
  end
  if frame >= 2700 then machine:exit() end
end)
