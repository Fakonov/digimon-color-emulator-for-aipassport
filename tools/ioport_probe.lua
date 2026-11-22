
local machine = manager.machine
local cpu = machine.devices[":maincpu"]
local frame = 0
emu.register_frame_done(function()
  frame = frame + 1
  if frame == 1 then
    for tag, port in pairs(machine.ioport.ports) do
      print("PORT", tag)
      for name, field in pairs(port.fields) do
        local v = "?"
        local ok, r = pcall(function() return field:value() end)
        if ok then v = r end
        print("  FIELD", name, string.format("%04X", field.mask), "value", v, "val2", tostring(field.value))
      end
    end
  end
  if frame >= 60 then machine:exit() end
end)
