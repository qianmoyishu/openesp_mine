local environmental_sensor = require("environmental_sensor")

local sensor = environmental_sensor.new()
local sample = sensor:read()

print(string.format("temperature: %.2f C", sample.temperature))
print(string.format("pressure: %.2f Pa", sample.pressure))
print(string.format("humidity: %.2f %%", sample.humidity))
print(string.format("gas resistance: %.2f ohm", sample.gas_resistance))
print(string.format("status: 0x%02X", sample.status))

sensor:close()
