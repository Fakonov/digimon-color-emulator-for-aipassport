
local machine = manager.machine
local porta = machine.ioport.ports[":PORTA"]
local b1 = porta.fields["P1 Button 1"]
local frame = 0
emu.register_frame_done(function()
  frame = frame + 1
  if frame == 1200 or frame == 1800 or frame == 2100 then b1:set_value(0); print("press @F"..frame) end
  if frame == 1210 or frame == 1810 or frame == 2110 then b1:set_value(1); print("release @F"..frame) end
  if frame % 600 == 0 then
    print(string.format("F%04d PC=%06X", frame, machine.devices[":maincpu"].state["PC"].value))
  end
  if frame >= 2400 then machine:exit() end
end)
