
local machine = manager.machine
local cpu = machine.devices[":maincpu"]
local frame = 0
local function reg(name)
  local ok, v = pcall(function() return cpu.state[name] end)
  if not ok or v == nil then return "nil" end
  local ok2, val = pcall(function() return v.value end)
  if ok2 and val ~= nil then return tostring(val) end
  return tostring(v)
end
emu.register_frame_done(function()
  frame = frame + 1
  if frame == 1 then
    print("== STATE DUMP ==")
    local ok, err = pcall(function()
      for k,v in pairs(cpu.state) do print("REG", k, tostring(v)) end
    end)
    if not ok then print("state pairs failed", err) end
  end
  if frame % 30 == 0 then
    print(string.format("F%04d PC=%s SR=%s", frame, reg("PC"), reg("SR")))
  end
  if frame >= 480 then machine:exit() end
end)
