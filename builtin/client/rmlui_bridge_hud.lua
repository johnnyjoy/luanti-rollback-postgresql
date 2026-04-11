--[[
	Validation HUD for the RmlUi UI system: core.ui.panel + handle:set.

	- Mounts one surface once localplayer exists and UiManager is ready.
	- Updates bound text via handle:set when HP/breath change.

	Advisory id: rmlui_bridge_hud (engine may prefix surface id; use handle for set/close).
]]

if not core.ui or not core.ui.panel or not core.ui.bind then
	return
end

if not core.settings:get_bool("rmlui_builtin_hud", true) then
	return
end

local hud_handle = nil
local mount_fail_logged = false
local last_hp, last_breath = nil, nil

local function build_tree()
	return {
		type = "column",
		props = { gap = "sm" },
		children = {
			{
				type = "text",
				props = {
					value = "RmlUi — HP / breath (handle:set when values change)",
				},
			},
			{
				type = "text",
				id = "exp_hud_hp",
				props = { value = core.ui.bind("hud.hp_text") },
			},
			{
				type = "text",
				id = "exp_hud_breath",
				props = { value = core.ui.bind("hud.breath_text") },
			},
		},
	}
end

local function try_mount()
	if hud_handle then
		return
	end
	if not core.localplayer then
		return
	end
	local handle, err = core.ui.panel({
		id = "rmlui_bridge_hud",
		content = build_tree(),
	})
	if handle then
		hud_handle = handle
		return
	end
	if not mount_fail_logged then
		core.log("warning", "[rmlui_bridge_hud] panel failed (will retry): " .. tostring(err))
		mount_fail_logged = true
	end
end

local function push_state()
	if not hud_handle or not core.localplayer then
		return
	end
	local lp = core.localplayer
	local hp = lp:get_hp()
	local br = lp:get_breath()
	if hp == last_hp and br == last_breath then
		return
	end
	last_hp, last_breath = hp, br
	hud_handle:set({
		["hud.hp_text"] = string.format("HP: %d", hp),
		["hud.breath_text"] = string.format("Breath: %d", br),
	})
end

core.register_globalstep(function(_dtime)
	try_mount()
	if hud_handle then
		push_state()
	end
end)

core.register_on_shutdown(function()
	if hud_handle then
		hud_handle:close()
		hud_handle = nil
	end
end)
