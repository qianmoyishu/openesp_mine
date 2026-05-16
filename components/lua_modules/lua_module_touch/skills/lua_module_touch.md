This skill describes how to use `touch` from Lua.

Quick rules:
- Import it with `local touch = require("touch")`
- Open the board device with `local keys = touch.new("touch_keys")`
- Call `keys:read()` to get the current touch-key state table
- Call `keys:is_pressed(index)` when you only need one key
- Call `keys:close()` when finished
