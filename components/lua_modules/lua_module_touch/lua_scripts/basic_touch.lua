-- Touch key demo: open board device and print two key states.
local touch = require("touch")
local delay = require("delay")

local a = type(args) == "table" and args or {}
local DEVICE_NAME = type(a.device) == "string" and a.device or "touch_keys"
local SAMPLE_COUNT = type(a.samples) == "number" and math.floor(a.samples) or 20
local INTERVAL_MS = type(a.interval_ms) == "number" and math.floor(a.interval_ms) or 200

local keys

local function cleanup()
    if keys then
        pcall(function()
            keys:close()
        end)
        keys = nil
    end
end

local function run()
    keys = touch.new(DEVICE_NAME)
    print("[touch] opened " .. keys:name())
    for i = 1, SAMPLE_COUNT do
        local sample = keys:read()
        for _, key in ipairs(sample.keys) do
            print(string.format(
                "[touch] #%d key%d gpio=%d pressed=%s smooth=%d benchmark=%d delta=%d threshold=%d",
                i, key.index, key.gpio, tostring(key.pressed),
                key.smooth, key.benchmark, key.delta, key.threshold
            ))
        end
        delay.delay_ms(INTERVAL_MS)
    end
end

local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
    error(err)
end
