local machine = manager.machine
local cpu = machine.devices[":maincpu"]
local frame = 0
local names = {"PC","SP","BP","SR","R0","R1","R2","R3","R4","R5","R6","R7","R8","R9","R10","R11","R12","R13","R14","R15","DS","FR","N","Z","S","C"}
emu.register_frame_done(function()
  frame = frame + 1
  if frame == 120 then
    for _, n in ipairs(names) do
      local ok, v = pcall(function() return cpu.state[n].value end)
      if ok then print("REG", n, string.format("%X", v)) else print("REG", n, "ERR") end
    end
    machine:exit()
  end
end)
