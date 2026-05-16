This skill describes how to use `ir` from Lua.

Quick rules:
- Import it with `local ir = require("ir")`
- Open the board device with `local dev = ir.new("ir_blaster"[, opts])`
  - `opts.tx_gpio`, `opts.rx_gpio`, `opts.ctrl_gpio` override board defaults.
  - `opts.ctrl_active_level` (default 0) sets the IR power-enable pin level.
  - `opts.carrier_hz` defaults to 38000.
- Call `dev:send_nec(address, command)` to transmit a 32-bit NEC frame.
- Call `dev:send_raw(symbols)` with an array of `{level0, duration0, level1, duration1}` tables for arbitrary protocols.
- Call `dev:receive(timeout_ms)` to learn one IR frame; returns the captured symbol table or `nil, "timeout"`.
- Call `dev:info()` for a table of the resolved configuration, `dev:name()` for the device name.
- Call `dev:close()` when finished.
