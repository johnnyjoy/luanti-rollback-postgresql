--[[
	Devtest: singleplayer validation for server-driven RmlUi UI
	(protocol >= 52).

	API: core.ui.panel, handle:set, handle:close

	Layout contract: engine provides a full-viewport transparent shell; each surface
	should use one root div with explicit position, width, height, and background;
	all content goes inside that div.

	Note: The server sends a JSON snapshot of the declarative tree; Lua callbacks
	are not serialized, so props.on_click cannot run for server mounts. This mod
	includes a non-interactive button shell and uses the /srvui_ping chat command
	to bump a bound state field — same set_player_state path a future wire-up
	would exercise after a click.

	Join behavior: devtest_server_ui_auto_mount (default false) — when false, no
	panel until /srvui_show.
]]

if not core.ui or not core.ui.panel then
	return
end

local U = ui_foundation

local SURFACE_ID = "devtest_srvui_validation"
local MOUNT_DELAY = 0.75

--- When false (default), do not mount on join — use /srvui_show so tests see a clean screen first.
local function server_ui_auto_mount()
	if not core.settings then
		return false
	end
	return core.settings:get_bool("devtest_server_ui_auto_mount", false)
end

local function bind(key)
	return U.bind(key)
end

--- Per-player session: accumulator for 1s tick, counters, mount flag.
local session = {}

local function tree_spec()
	-- Fixed panel (300×240), top-left — ui_foundation.panel + column + primitives.
	return U.panel({
		id = "srvui_root",
		style = {
			top = "16px",
			left = "16px",
			width = "300px",
			height = "240px",
		},
	}, {
		U.column({ gap = "sm" }, {
			U.label("Server UI validation (devtest)", { bold = true }),
			U.text_bound("line_counter", "srv.counter"),
			U.text_bound("line_status", "srv.status"),
			U.text_bound("line_clicks", "srv.clicks"),
			U.button("Ping: chat /srvui_ping"),
		}),
	})
end

local function set_state(name, fields)
	local s = session[name]
	if not s or not s.ui_handle then
		return false
	end
	local ok, err = s.ui_handle:set(fields)
	if not ok then
		core.log("warning", "[server_ui] handle:set: " .. tostring(err))
	end
	return ok
end

local function push_full_state(name)
	local s = session[name]
	if not s or not s.mounted then
		return
	end
	set_state(name, {
		["srv.counter"] = string.format("Counter: %d", s.counter),
		["srv.status"] = s.status or "",
		["srv.clicks"] = string.format("Ping count: %d", s.clicks),
	})
end

local function mount_player_ui(name)
	local s = session[name]
	if not s then
		return false, "no session"
	end
	if s.mounted and s.ui_handle then
		push_full_state(name)
		return true
	end
	local handle, err = core.ui.panel({
		player = name,
		id = SURFACE_ID,
		content = tree_spec(),
	})
	if not handle then
		return false, err
	end
	s.ui_handle = handle
	s.mounted = true
	s.status = "Mounted"
	push_full_state(name)
	return true
end

local function unmount_player_ui(name)
	local s = session[name]
	if not s then
		return false, "no session"
	end
	if s.ui_handle then
		s.ui_handle:close()
		s.ui_handle = nil
	end
	s.mounted = false
	s.status = "Hidden"
	return true
end

core.register_on_joinplayer(function(player)
	local name = player:get_player_name()
	session[name] = {
		acc = 0,
		counter = 0,
		clicks = 0,
		status = "Joining…",
		mounted = false,
	}
	core.after(MOUNT_DELAY, function()
		if not session[name] then
			return
		end
		if not server_ui_auto_mount() then
			session[name].status = "Idle (use /srvui_show to mount)"
			return
		end
		local ok, err = mount_player_ui(name)
		if not ok then
			session[name].status = "Mount failed: " .. tostring(err)
			core.log("warning", "[server_ui] core.ui.panel: " .. tostring(err))
		end
	end)
end)

core.register_globalstep(function(dtime)
	for name, s in pairs(session) do
		if s.mounted then
			s.acc = s.acc + dtime
			if s.acc >= 1.0 then
				s.acc = s.acc - 1.0
				s.counter = s.counter + 1
				push_full_state(name)
			end
		end
	end
end)

core.register_on_leaveplayer(function(player)
	local name = player:get_player_name()
	if session[name] and session[name].mounted then
		unmount_player_ui(name)
	end
	session[name] = nil
end)

local function register_cmd(cmd, def)
	core.register_chatcommand(cmd, def)
end

register_cmd("srvui_help", {
	params = "",
	description = "List server_ui validation chat commands",
	func = function(name, _)
		return true,
			"server_ui: /srvui_show /srvui_hide /srvui_reset /srvui_ping. "
				.. "Default join does not mount (devtest_server_ui_auto_mount=false); use /srvui_show."
	end,
})

register_cmd("srvui_show", {
	params = "",
	description = "Mount the validation server UI for you",
	func = function(name, _)
		if not session[name] then
			return false, "No session."
		end
		local ok, err = mount_player_ui(name)
		if not ok then
			return false, tostring(err)
		end
		return true, "Server UI shown."
	end,
})

register_cmd("srvui_hide", {
	params = "",
	description = "Unmount the validation server UI",
	func = function(name, _)
		if not session[name] then
			return false, "No session."
		end
		unmount_player_ui(name)
		return true, "Server UI hidden."
	end,
})

register_cmd("srvui_reset", {
	params = "",
	description = "Reset the periodic counter (set_state only)",
	func = function(name, _)
		if not session[name] then
			return false, "No session."
		end
		local s = session[name]
		s.counter = 0
		s.acc = 0
		if s.mounted then
			push_full_state(name)
		end
		return true, "Counter reset."
	end,
})

register_cmd("srvui_ping", {
	params = "",
	description = "Simulate a UI action: bump ping count via handle:set",
	func = function(name, _)
		if not session[name] then
			return false, "No session."
		end
		local s = session[name]
		s.clicks = s.clicks + 1
		s.status = "Last ping #" .. (s.clicks) .. " @ " .. os.date("%H:%M:%S")
		if s.mounted then
			push_full_state(name)
		end
		return true, "Ping recorded (state updated)."
	end,
})
