--[[
	RmlUi UI lab (protocol >= 52).
	Server-driven: core.ui.panel / handle:set / handle:close (public API; bridge internal).

	Server-driven buttons: handlers are stored server-side; the client sends
	TOSERVER_UI_ACTION (surface id + button index). Launcher chat
	flows remain for demos that do not use declarative on_press.

	Multiple surfaces = multiple independent mounted documents (not one formspec).
]]

if not core.ui or not core.ui.panel then
	return
end

-- Set devtest_disable_testui_mod = true (main menu → Settings → devtest) to load only
-- server_ui or other mods without testui surfaces (layout isolation).
if core.settings and core.settings:get_bool("devtest_disable_testui_mod", false) then
	return
end

local U = ui_foundation

local S = {
	launcher = "testui_launcher",
	buttons = "testui_buttons",
	counter = "testui_counter",
	multi_a = "testui_multi_a",
	multi_b = "testui_multi_b",
	modal = "testui_modal",
	toast = "testui_toast",
	lifecycle = "testui_lifecycle",
	foundation_demo = "testui_foundation_demo",
	placement_lab = "testui_placement_lab",
	--- Engine-only document: one red rect, no declarative panel wrapper (render path check).
	minimal = "testui_builtin_minimal",
}

local MOUNT_DELAY = 0.5

local function bind(key)
	return U.bind(key)
end

--- [logical surface key] = handle when mounted (see S.*)
local session = {}

local function ensure_session(name)
	if not session[name] then
		session[name] = {
			mounted = {},
			counter = 0,
			acc = 0,
			btn_line = "Use: /testui btn1   or   /testui btn2",
			multi_a = "Panel A (independent surface)",
			multi_b = "Panel B (independent surface)",
			modal_note = "Mounted as its own surface; draw order follows mount order.",
			toast_msg = "",
			lc_phase = "off",
			lifecycle_i = 0,
			launcher_hint = "Type /testui help",
		}
	end
	return session[name]
end

local function set_state(name, surface_id, fields)
	local s = ensure_session(name)
	local handle = s.mounted[surface_id]
	if not handle then
		return false
	end
	local ok, err = handle:set(fields)
	if not ok then
		core.log("warning", "[testui] handle:set: " .. tostring(err))
	end
	return ok
end

local function unmount_s(name, surface_id)
	local s = session[name]
	local handle = s and s.mounted[surface_id]
	if handle then
		handle:close()
	end
	if s then
		s.mounted[surface_id] = nil
	end
end

local function mount_tree(name, surface_id, tree)
	-- tree may be a declarative table or a zero-arg builder function (see toggle()).
	local tre = tree
	if type(tree) == "function" then
		tre = tree()
	end
	if type(tre) ~= "table" then
		return false, "internal: UI tree must be a table or function returning a table"
	end
	local handle, err = core.ui.panel({
		player = name,
		id = surface_id,
		content = tre,
	})
	if not handle then
		return false, err
	end
	local s = ensure_session(name)
	s.mounted[surface_id] = handle
	return true
end

-- ——— panel trees (each becomes its own RmlUi document / surface) ———

--- Single built-in RML document (see UiManager / ui_declarative_compile): no column wrapper.
local function tree_builtin_minimal()
	return { template = "builtin:test_layout_minimal" }
end

local function tree_launcher()
	-- Top-right — distinct from server_ui top-left.
	return U.panel({
		id = "launcher_root",
		style = {
			position = "fixed",
			top = "24px",
			right = "24px",
			left = "auto",
			width = "300px",
			height = "200px",
			background_color = U.theme.warning_surface,
		},
	}, {
		U.column({ gap = "sm" }, {
			U.label("TEST UI WORKS", { large = true, style = { color = "#ffffff" } }),
			U.line("Launcher built with ui_foundation.", { style = { color = "#eeeeee" } }),
		}),
	})
end

local function tree_buttons()
	return U.panel({
		id = "buttons_root",
		style = {
			top = "16px",
			left = "50%",
			margin_left = "-170px",
			width = "340px",
			height = "220px",
		},
	}, {
		U.column({ gap = "sm" }, {
			U.label("Button panel", { bold = true }),
			U.line("Buttons test (state via chat)"),
			U.text_bound("btn_state", "btn.state"),
			U.row({ gap_px = 10 }, {
				U.button("Button A"),
				U.button("Button B"),
			}),
			U.spacer({ size = "4px" }),
			U.text_bound("btn_help", "btn.help"),
		}),
	})
end

local function tree_counter()
	return U.panel({
		id = "counter_root",
		style = {
			top = "280px",
			left = "16px",
			width = "300px",
			height = "140px",
		},
	}, {
		U.column({ gap = "sm" }, {
			U.label("Live-updating panel", { bold = true }),
			U.line("set_state every 1s"),
			U.text_bound("ctr", "counter.val"),
			U.label("Close: /testui close counter", { style = { color = U.theme.muted, font_size = "12px" } }),
		}),
	})
end

local function tree_multi_a()
	return U.panel({
		id = "multi_a_root",
		style = {
			top = "88px",
			left = "16px",
			width = "280px",
			height = "160px",
			background_color = U.theme.surface_alt,
		},
	}, {
		U.column({ gap = "sm" }, {
			U.label("Multi-window A", { bold = true }),
			U.text_bound("ma", "multi.a"),
		}),
	})
end

local function tree_multi_b()
	return U.panel({
		id = "multi_b_root",
		style = {
			top = "88px",
			right = "16px",
			left = "auto",
			width = "280px",
			height = "160px",
			background_color = U.theme.surface_alt,
		},
	}, {
		U.column({ gap = "sm" }, {
			U.label("Multi-window B", { bold = true }),
			U.text_bound("mb", "multi.b"),
		}),
	})
end

local function tree_modal()
	return U.shadow_panel({
		id = "modal_root",
		shadow = { dx = 0, dy = 10, softness = 2, color = "#000000", opacities = { 0.26, 0.14, 0.08 } },
		style = {
			position = "fixed",
			top = "96px",
			left = "50%",
			margin_left = "-150px",
			width = "300px",
			height = "220px",
			background_color = U.theme.modal_bg,
			border = "1px solid " .. U.theme.modal_border,
			z_index = "1000",
		},
	}, {
		U.column({ gap = "md" }, {
			U.label("Modal-style surface", { bold = true, large = true }),
			U.text_bound("modnote", "modal.note"),
			U.label("Separate document; mount after other surfaces to draw on top.", {
				style = { color = U.theme.muted, font_size = "12px", font_weight = "normal" },
			}),
			U.label("Close: /testui close modal", { style = { color = U.theme.muted, font_size = "12px" } }),
		}),
	})
end

local function tree_toast()
	return U.panel({
		id = "toast_root",
		style = {
			position = "fixed",
			bottom = "24px",
			left = "50%",
			margin_left = "-140px",
			width = "280px",
			min_height = "72px",
		},
	}, {
		U.column({ gap = "sm" }, {
			U.label("Toast", { bold = true }),
			U.text_bound("toastline", "toast.msg"),
		}),
	})
end

local function tree_lifecycle()
	return U.panel({
		id = "lc_root",
		style = {
			top = "440px",
			left = "16px",
			width = "280px",
			height = "120px",
		},
	}, {
		U.column({ gap = "sm" }, {
			U.label("Lifecycle test", { bold = true }),
			U.text_bound("lc", "lc.phase"),
		}),
	})
end

--- One normal panel: primitives demo (centered).
local function tree_foundation_demo()
	return U.panel({
		id = "fdemo_root",
		style = {
			top = "200px",
			left = "50%",
			margin_left = "-165px",
			width = "330px",
			height = "160px",
		},
	}, {
		U.column({ gap = "sm" }, {
			U.label("ui_foundation demo", { bold = true }),
			U.line("Single panel: panel, column, label, line, row, spacer, button."),
			U.row({ gap_px = 8 }, {
				U.button("Action"),
				U.spacer({ horizontal = true, size = "6px" }),
				U.label("row + spacer", { style = { color = U.theme.muted, font_size = "12px" } }),
			}),
		}),
	})
end

local function placement_lab_label(text)
	return {
		type = "label",
		text = text,
		style = { color = "#ffffff", font_size = "11px", font_weight = "bold" },
	}
end

--- One surface: anchors + bars + centered modal (ids must match ui_manager PLACEMENT resize logs).
local function tree_placement_lab()
	return {
		type = "div",
		id = "placement_lab_root",
		style = {
			position = "absolute",
			left = "0",
			top = "0",
			width = "100%",
			height = "100%",
		},
		children = {
			{
				type = "div",
				id = "bar_top",
				style = {
					position = "fixed",
					left = "0",
					right = "0",
					top = "0",
					height = "40px",
					background_color = "#2a2f3a",
					z_index = "1",
					box_sizing = "border-box",
					padding = "8px 12px",
				},
				children = { placement_lab_label("TOP BAR (full width)") },
			},
			{
				type = "div",
				id = "bar_bottom",
				style = {
					position = "fixed",
					left = "0",
					right = "0",
					bottom = "0",
					height = "40px",
					background_color = "#2a2f3a",
					z_index = "1",
					box_sizing = "border-box",
					padding = "8px 12px",
				},
				children = { placement_lab_label("BOTTOM BAR (full width)") },
			},
			{
				type = "div",
				id = "anchor_tl",
				style = {
					position = "fixed",
					z_index = "10",
					top = "52px",
					left = "12px",
					width = "140px",
					height = "86px",
					background_color = "#157a3a",
					border_radius = "6px",
					box_sizing = "border-box",
					padding = "8px",
				},
				children = { placement_lab_label("TL +12 +52") },
			},
			{
				type = "div",
				id = "anchor_tr",
				style = {
					position = "fixed",
					z_index = "10",
					top = "52px",
					right = "12px",
					left = "auto",
					width = "140px",
					height = "86px",
					background_color = "#9a3538",
					border_radius = "6px",
					box_sizing = "border-box",
					padding = "8px",
				},
				children = { placement_lab_label("TR −12 +52") },
			},
			{
				type = "div",
				id = "anchor_bl",
				style = {
					position = "fixed",
					z_index = "10",
					bottom = "52px",
					left = "12px",
					top = "auto",
					width = "140px",
					height = "86px",
					background_color = "#2570b8",
					border_radius = "6px",
					box_sizing = "border-box",
					padding = "8px",
				},
				children = { placement_lab_label("BL +12 −52") },
			},
			{
				type = "div",
				id = "anchor_br",
				style = {
					position = "fixed",
					z_index = "10",
					bottom = "52px",
					right = "12px",
					left = "auto",
					top = "auto",
					width = "140px",
					height = "86px",
					background_color = "#b8732a",
					border_radius = "6px",
					box_sizing = "border-box",
					padding = "8px",
				},
				children = { placement_lab_label("BR −12 −52") },
			},
			{
				U.shadow_panel({
					id = "anchor_center",
					shadow = { dx = 0, dy = 8, softness = 2, color = "#000000", opacities = { 0.22, 0.12, 0.08 } },
					style = {
						position = "fixed",
						z_index = "20",
						left = "50%",
						top = "50%",
						width = "220px",
						height = "100px",
						margin_left = "-110px",
						margin_top = "-50px",
						background_color = "#5a2a8a",
						border_radius = "8px",
						box_sizing = "border-box",
						padding = "10px",
					},
				}, { placement_lab_label("CENTER (modal-like)") }),
			},
		},
	}
end

local function push_buttons_state(name)
	local s = ensure_session(name)
	set_state(name, S.buttons, {
		["btn.state"] = s.btn_line,
		["btn.help"] = "/testui btn1  |  /testui btn2",
	})
end

local function push_launcher_state(_name)
	-- Launcher tree has no core.ui.bind targets; hints are chat-only.
end

local function push_counter_state(name)
	local s = ensure_session(name)
	set_state(name, S.counter, {
		["counter.val"] = string.format("Counter: %d (tick 1s)", s.counter),
	})
end

local function push_multi_state(name)
	local s = ensure_session(name)
	set_state(name, S.multi_a, { ["multi.a"] = s.multi_a })
	set_state(name, S.multi_b, { ["multi.b"] = s.multi_b })
end

local function push_modal_state(name)
	local s = ensure_session(name)
	set_state(name, S.modal, { ["modal.note"] = s.modal_note })
end

local function push_lifecycle_state(name)
	local s = ensure_session(name)
	set_state(name, S.lifecycle, { ["lc.phase"] = s.lc_phase })
end

local function toggle(name, surface_id, tree, push_fn)
	local s = ensure_session(name)
	if s.mounted[surface_id] then
		unmount_s(name, surface_id)
		return true, "Unmounted " .. surface_id .. "."
	end
	local ok, err = mount_tree(name, surface_id, tree)
	if not ok then
		return false, err
	end
	if push_fn then
		push_fn(name)
	end
	return true, "Mounted " .. surface_id .. "."
end

local function help_text()
	return table.concat({
		"— testui (ui_foundation primitives) —",
		"/testui           toggle launcher",
		"/testui help      this list",
		"/testui examples  which commands demo which patterns",
		"/testui 016       run TEST_016 (A then B; after 5s close A, B remains)",
		"/testui 017       run TEST_017 (random modal churn; default 1000 total, max 40 active)",
		"/testui 018       run TEST_018 (minimal shadow modal)",
		"/testui 019       run TEST_019 (minimal positioning: anchors + keep_in_view + resize)",
		"/testui 020       run TEST_020 (HUD instrument mode: drag + sticky anchors)",
		"/testui 021       run TEST_021 (HUD aspect resize + placement adaptation D–F)",
		"/testui instrument  toggle HUD instrument mode (client-only)",
		"/testui foundation  toggle single demo panel (normal panel)",
		"/testui placement  anchor/bar/center lab",
		"/testui minimal   toggle ENGINE red rect (no panel wrapper; isolates render vs layout)",
		"/testui launcher  toggle launcher panel",
		"/testui counter   toggle live counter (set_state / sec)",
		"/testui buttons   toggle buttons demo (chat: btn1, btn2)",
		"/testui multi     two concurrent windows (A left, B right)",
		"/testui modal     modal-like surface (mount after multi for stacked demo)",
		"/testui toast     show toast (auto-unmount ~4s)",
		"/testui lifecycle toggle lifecycle panel",
		"/testui all       mount launcher + counter + multi A/B",
		"/testui close <id>  id: launcher|foundation|placement|buttons|counter|multi|modal|toast|lifecycle|minimal",
		"Primitives: ui_foundation.panel, .column, .row, .label, .line, .text_bound, .button, .spacer",
	}, "\n")
end

local function run_numbered_test(which, playername)
	local path = nil
	if which == "018" then
		path = core.get_modpath("testui") .. "/tests/test_018_box_shadow_minimal.lua"
	elseif which == "017" then
		path = core.get_modpath("testui") .. "/tests/test_017_random_modal_churn_1000.lua"
	elseif which == "016" then
		path = core.get_modpath("testui") .. "/tests/test_016_timer_remove_a_under_b.lua"
	elseif which == "019" then
		path = core.get_modpath("testui") .. "/tests/test_019_minimal_positioning_anchors.lua"
	elseif which == "020" then
		path = core.get_modpath("testui") .. "/tests/test_020_hud_instrument_drag_sticky.lua"
	elseif which == "021" then
		path = core.get_modpath("testui") .. "/tests/test_021_aspect_resize.lua"
	end
	if not path then
		return false, "Unknown test number. Use: 016, 017, 018, 019, 020, or 021."
	end
	local t = dofile(path)
	if not (t and t.run) then
		return false, "Internal error: test script did not return { run = fn }"
	end
	-- `run()` registers on_joinplayer. Our tests also support immediate execution
	-- for already-connected players (see run() implementation in each test).
	t.run(playername)
	return true, "Started TEST_" .. which .. "."
end

core.register_chatcommand("testui", {
	params = "[016|017|018|019|020|021|help|examples|instrument|foundation|placement|minimal|launcher|counter|buttons|multi|modal|toast|lifecycle|all|close|btn1|btn2]",
	description = "RmlUi UI test launcher (/testui help)",
	func = function(name, param)
		local s = ensure_session(name)
		param = param:gsub("^%s+", ""):gsub("%s+$", "")
		local p0 = param:match("^(%S*)") or ""
		local rest = param:match("^%S+%s+(.+)$") or ""

		if param == "" or p0 == "" then
			local vis = s.mounted[S.launcher]
			local ok, msg = toggle(name, S.launcher, tree_launcher, push_launcher_state)
			if ok and not vis then
				s.launcher_hint = help_text():sub(1, 200) .. "… (see /testui help)"
				push_launcher_state(name)
			end
			return ok, msg
		end

		if p0 == "help" or p0 == "?" then
			return true, help_text()
		end

		if p0:match("^%d%d%d$") then
			return run_numbered_test(p0, name)
		end

		if p0 == "examples" or p0 == "ex" then
			return true,
				table.concat({
					"Patterns:",
					"  /testui foundation — one normal panel (ui_foundation)",
					"  /testui multi — two concurrent windows",
					"  /testui multi then /testui modal — modal-like over other surfaces (stacking)",
					"  /testui counter — live-updating (1s set_state)",
					"  /testui buttons — button panel; /testui btn1 | btn2",
					"  /testui placement — anchors + bars + center",
				}, "\n")
		end

		if p0 == "foundation" or p0 == "fdemo" then
			return toggle(name, S.foundation_demo, tree_foundation_demo, nil)
		end

		if p0 == "placement" or p0 == "place" then
			return toggle(name, S.placement_lab, tree_placement_lab, nil)
		end

		if p0 == "minimal" or p0 == "red" then
			return toggle(name, S.minimal, tree_builtin_minimal, nil)
		end

		if p0 == "instrument" or p0 == "instrumentmode" then
			-- Server-driven entry point (client switches mode locally).
			-- No reliable server-side is_instrument_mode yet; we toggle by sending enter/exit.
			if core.ui and core.ui.enter_instrument_mode and core.ui.exit_instrument_mode then
				local want_exit = (rest == "off" or rest == "0" or rest == "exit")
				if want_exit then
					local ok, err = core.ui.exit_instrument_mode({ player = name })
					if not ok then
						return false, "Failed to exit instrument mode: " .. tostring(err)
					end
					return true, "Exited HUD instrument mode."
				end
				local ok, err = core.ui.enter_instrument_mode({ player = name })
				if not ok then
					return false, "Failed to enter instrument mode: " .. tostring(err)
				end
				return true, "Entered HUD instrument mode (ESC exits). Use: /testui instrument off to exit."
			end
			return false, "Instrument mode API unavailable on this build."
		end

		if p0 == "launcher" or p0 == "l" then
			local ok, msg = toggle(name, S.launcher, tree_launcher, push_launcher_state)
			if ok and s.mounted[S.launcher] then
				s.launcher_hint = "Launcher open. " .. help_text():sub(1, 120) .. "…"
				push_launcher_state(name)
			end
			return ok, msg
		end

		if p0 == "counter" or p0 == "c" then
			local ok, msg = toggle(name, S.counter, tree_counter, push_counter_state)
			return ok, msg
		end

		if p0 == "buttons" or p0 == "b" then
			s.btn_line = "Mounted. Use /testui btn1 or /testui btn2"
			local ok, msg = toggle(name, S.buttons, tree_buttons, function(n)
				push_buttons_state(n)
			end)
			return ok, msg
		end

		if p0 == "btn1" then
			s.btn_line = "Last action: Button A (via chat)"
			if s.mounted[S.buttons] then
				push_buttons_state(name)
			end
			return true, "btn1 → state updated."
		end

		if p0 == "btn2" then
			s.btn_line = "Last action: Button B (via chat)"
			if s.mounted[S.buttons] then
				push_buttons_state(name)
			end
			return true, "btn2 → state updated."
		end

		if p0 == "multi" then
			local a_on = s.mounted[S.multi_a]
			if a_on then
				unmount_s(name, S.multi_a)
				unmount_s(name, S.multi_b)
				return true, "Multi panels unmounted."
			end
			local ok, err = mount_tree(name, S.multi_a, tree_multi_a())
			if not ok then
				return false, err
			end
			ok, err = mount_tree(name, S.multi_b, tree_multi_b())
			if not ok then
				unmount_s(name, S.multi_a)
				return false, err
			end
			push_multi_state(name)
			return true, "Mounted two surfaces (multi_a + multi_b)."
		end

		if p0 == "modal" or p0 == "m" then
			return toggle(name, S.modal, tree_modal, push_modal_state)
		end

		if p0 == "toast" or p0 == "t" then
			if s.mounted[S.toast] then
				unmount_s(name, S.toast)
			end
			s.toast_msg = "Toast at " .. os.date("%H:%M:%S") .. " — auto-dismiss in ~4s"
			local ok, err = mount_tree(name, S.toast, tree_toast())
			if not ok then
				return false, err
			end
			set_state(name, S.toast, { ["toast.msg"] = s.toast_msg })
			core.after(4.0, function()
				if session[name] and session[name].mounted[S.toast] then
					unmount_s(name, S.toast)
				end
			end)
			return true, "Toast shown."
		end

		if p0 == "lifecycle" or p0 == "lc" then
			return toggle(name, S.lifecycle, tree_lifecycle, function(n)
				local ss = ensure_session(n)
				ss.lifecycle_i = ss.lifecycle_i + 1
				ss.lc_phase = string.format("mounted (toggle #%d)", ss.lifecycle_i)
				push_lifecycle_state(n)
			end)
		end

		if p0 == "all" then
			if not s.mounted[S.launcher] then
				local ok, err = mount_tree(name, S.launcher, tree_launcher())
				if not ok then
					return false, err
				end
				s.launcher_hint = "Opened via /testui all"
				push_launcher_state(name)
			end
			if not s.mounted[S.counter] then
				local ok, err = mount_tree(name, S.counter, tree_counter())
				if not ok then
					return false, err
				end
				push_counter_state(name)
			end
			if not s.mounted[S.multi_a] then
				local ok, err = mount_tree(name, S.multi_a, tree_multi_a())
				if not ok then
					return false, err
				end
				ok, err = mount_tree(name, S.multi_b, tree_multi_b())
				if not ok then
					unmount_s(name, S.multi_a)
					return false, err
				end
				push_multi_state(name)
			end
			return true, "Mounted launcher + counter + multi_a + multi_b (if not already open)."
		end

		if p0 == "close" then
			local id = rest:match("^%s*(%S+)") or ""
			id = id:lower()
			local map = {
				launcher = S.launcher,
				foundation = S.foundation_demo,
				placement = S.placement_lab,
				minimal = S.minimal,
				buttons = S.buttons,
				counter = S.counter,
				modal = S.modal,
				toast = S.toast,
				lifecycle = S.lifecycle,
				multi = "multi",
			}
			local sid = map[id]
			if id == "multi" then
				unmount_s(name, S.multi_a)
				unmount_s(name, S.multi_b)
				return true, "Closed multi_a + multi_b."
			end
			if not sid then
				return false,
					"Unknown id. Use: launcher, foundation, placement, minimal, buttons, counter, multi, modal, toast, lifecycle"
			end
			if not s.mounted[sid] then
				return false, "Not mounted: " .. id
			end
			unmount_s(name, sid)
			return true, "Closed " .. id .. "."
		end

		return false, "Unknown subcommand. Try /testui help"
	end,
})

core.register_on_joinplayer(function(player)
	local name = player:get_player_name()
	ensure_session(name)
	core.after(MOUNT_DELAY, function()
		if not session[name] then
			return
		end
		core.chat_send_player(name, "[testui] RmlUi UI lab: type /testui or /testui help")
	end)
end)

core.register_globalstep(function(dtime)
	for name, s in pairs(session) do
		if s.mounted[S.counter] then
			s.acc = s.acc + dtime
			if s.acc >= 1.0 then
				s.acc = s.acc - 1.0
				s.counter = s.counter + 1
				push_counter_state(name)
			end
		end
	end
end)

core.register_on_leaveplayer(function(player)
	local name = player:get_player_name()
	local s = session[name]
	if not s then
		return
	end
	local ids = {}
	for sid in pairs(s.mounted) do
		ids[#ids + 1] = sid
	end
	for _, sid in ipairs(ids) do
		local h = s.mounted[sid]
		if h then
			h:close()
		end
	end
	session[name] = nil
end)

-- Automated join test runner. Prefer selecting via setting instead of editing this file.
-- Example: `testui_autorun_test = 018` in a config file.
core.after(0, function()
	local which = ""
	if core.settings then
		which = tostring(core.settings:get("testui_autorun_test") or "")
	end
	local path = nil
	if which == "018" then
		path = core.get_modpath("testui") .. "/tests/test_018_box_shadow_minimal.lua"
	elseif which == "017" then
		path = core.get_modpath("testui") .. "/tests/test_017_random_modal_churn_1000.lua"
	elseif which == "016" then
		path = core.get_modpath("testui") .. "/tests/test_016_timer_remove_a_under_b.lua"
	end

	if path then
		local t = dofile(path)
		if t and t.run then
			t.run()
		end
	end
end)
