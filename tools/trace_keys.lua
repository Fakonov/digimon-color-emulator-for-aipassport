-- Keep the machine awake across a 60 s trace campaign (campaign B).
-- Presses PORTA P1 Button 1 for 5 frames every 6 s starting at frame 120,
-- and taps Button 2 / Button 3 once in a while so menu paths get exercised.
local machine = manager.machine
local porta = machine.ioport.ports[":PORTA"]
local b1 = porta.fields["P1 Button 1"]
local b2 = porta.fields["P1 Button 2"]
local b3 = porta.fields["P1 Button 3"]
local frame = 0

local function tap(field, name)
  field:set_value(0)
  emu.register_periodic(function() end) -- no-op, keeps lua state alive
  print(string.format("press %s @F%d", name, frame))
end

emu.register_frame_done(function()
  frame = frame + 1
  if frame >= 120 and (frame - 120) % 360 == 0 then tap(b1, "B1") end
  if frame >= 125 and (frame - 125) % 360 == 0 then b1:set_value(1) end
  if frame == 300 or frame == 1740 or frame == 3180 then tap(b2, "B2") end
  if frame == 305 or frame == 1745 or frame == 3185 then b2:set_value(1) end
  if frame == 660 or frame == 2100 then tap(b3, "B3") end
  if frame == 665 or frame == 2105 then b3:set_value(1) end
  if frame % 600 == 0 then
    print(string.format("F%04d PC=%06X", frame, machine.devices[":maincpu"].state["PC"].value))
  end
  if frame >= 3660 then machine:exit() end
end)
