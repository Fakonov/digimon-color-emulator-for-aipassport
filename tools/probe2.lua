
local machine = manager.machine
local cpu = machine.devices[":maincpu"]
local space = cpu.spaces["program"]
local frame = 0
local function rd(a)
  local ok,v = pcall(function() return space:read_u16(a) end)
  if ok then return string.format("%04X", v) end
  return "ERR"
end
local function pc()
  local ok,v = pcall(function() return cpu.state["PC"].value end)
  if ok then return string.format("%06X", v) end
  return "?"
end
emu.register_frame_done(function()
  frame = frame + 1
  if frame == 1 then
    print("== IOPORTS ==")
    for tag, port in pairs(machine.ioport.ports) do
      print("PORT", tag)
      for name, field in pairs(port.fields) do
        print("  FIELD", name, string.format("%04X", field.mask), field:read())
      end
    end
  end
  if frame % 10 == 0 and frame <= 600 then
    print(string.format("F%03d PC=%s 0c86=%s 0c88=%s 0c89=%s 0c8a=%s 0c8b=%s", frame, pc(), rd(0x0c86), rd(0x0c88), rd(0x0c89), rd(0x0c8a), rd(0x0c8b)))
  end
  if frame >= 600 then machine:exit() end
end)
