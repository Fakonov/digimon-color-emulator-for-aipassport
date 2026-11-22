local machine = manager.machine
local f = machine.ioport.ports[":PORTB"].fields["Charger Connected"]
local mt = getmetatable(f)
print("mt", mt)
if mt and mt.__index then
  for k,v in pairs(mt.__index) do print("IDX", k, v) end
end
local ok, keys = pcall(function() local t={} for k,v in pairs(f) do table.insert(t,k) end return table.concat(t,",") end)
print("pairs", ok, keys)
emu.register_frame_done(function() machine:exit() end)
