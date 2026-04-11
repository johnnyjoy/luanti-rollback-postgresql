--[[
	ui_foundation — table-only helpers for core.ui.panel / public UI content trees.
	Output is pure data (no functions in mounted trees); safe for server JSON.
]]

local mp = core.get_modpath("ui_foundation")
assert(mp, "[ui_foundation] mod path missing")
ui_foundation = dofile(mp .. "/primitives.lua")
