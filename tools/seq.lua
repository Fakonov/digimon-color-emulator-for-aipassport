
local machine = manager.machine
local porta = machine.ioport.ports[":PORTA"]
local b1 = porta.fields["P1 Button 1"]
local b2 = porta.fields["P1 Button 2"]
local b3 = porta.fields["P1 Button 3"]
local frame = 0
local seq = {
  {120,b1},{150,b2},{180,b3},{240,b1},{300,b2},{360,b3},
  {480,b3},{600,b1},{720,b2},{840,b3},{960,b1},{1080,b2}
}
local idx = 1
emu.register_frame_done(function()
  frame = frame + 1
  if idx <= #seq and frame == seq[idx][1] then
    seq[idx][2]:set_value(0)
    print(string.format("press %d @F%d", idx, frame))
  end
  if idx <= #seq and frame == seq[idx][1] + 4 then
    seq[idx][2]:set_value(1)
    idx = idx + 1
  end
  if frame >= 1500 then machine:exit() end
end)
