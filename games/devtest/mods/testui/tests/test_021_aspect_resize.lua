--[[
	TEST_021: HUD resize + placement-aware adaptation (9-way region, orientation + variant).

	Run: /testui 021
	Requires: instrument mode (entered automatically); ESC exits.

	Surfaces:
	- A: keep_aspect (ratio from initial size), top-left anchor
	- B: keep_aspect + aspect_ratio = 1 (square), center anchor
	- C: keep_aspect off (free resize + anchor-aware edges), top-right anchor
	- D: adaptive “hotbar” — edge horizontal / vertical bar; `core.rmlui_adapt_instrument` on drag/resize end
	- E: corner variant accents (borders) when region kind is corner
	- F: same orientation policy as D + variant accents; corner snake layout not implemented
]]

local ui = core.ui

-- Set true locally to trace adaptation + resize context (verbose; off by default).
local TEST_021_DIAG = false

local function diag_021(ctx, note, opts, adapt_patch)
	if not TEST_021_DIAG then
		return
	end
	local pl = ctx.placement or {}
	local ir = ctx.initial_rect or {}
	local cr = ctx.current_rect or {}
	local inst = ctx.instrument or {}
	local r = tonumber(inst.aspect_ratio) or (ir.w and ir.h and (ir.w / ir.h)) or 0
	local opt_s = ""
	if type(opts) == "table" then
		opt_s = string.format(
			" opts=[min %s,%s max %s,%s]",
			tostring(opts.min_w),
			tostring(opts.min_h),
			tostring(opts.max_w),
			tostring(opts.max_h)
		)
	end
	local adapt_s = ""
	if type(adapt_patch) == "table" and type(adapt_patch.adaptation) == "table" then
		local ad = adapt_patch.adaptation
		adapt_s = string.format(
			" adapt=[o=%s v=%s reg=%s k=%s]",
			tostring(ad.orientation),
			tostring(ad.variant),
			tostring(ad.region),
			tostring(ad.kind)
		)
	end
	core.log(
		"action",
		string.format(
			"[testui021] %s phase=%s evkind=%s handle=%s anchor=%s pl_xy=%s,%s region=%s plkind=%s "
				.. "ir=%s,%s %sx%s cr=%s,%s %sx%s kasp=%s r=%.4f sid=%s%s%s",
			note,
			tostring(ctx.phase),
			tostring(ctx.kind),
			tostring(ctx.resize_handle),
			tostring(pl.anchor),
			tostring(pl.x),
			tostring(pl.y),
			tostring(pl.region),
			tostring(pl.kind),
			tostring(ir.x),
			tostring(ir.y),
			tostring(ir.w),
			tostring(ir.h),
			tostring(cr.x),
			tostring(cr.y),
			tostring(cr.w),
			tostring(cr.h),
			tostring(inst.keep_aspect),
			r,
			tostring(ctx.surface_id),
			opt_s,
			adapt_s
		)
	)
end

local function hud_panel(root_id, title, subtitle, wpx, hpx)
	return ui.hud_box({
		id = root_id,
		shadow = false,
		style = {
			width = wpx,
			height = hpx,
			overflow = "hidden",
			background_color = "#1a2430",
			border = "2px solid #6ac",
			border_radius = "8px",
			padding = "0px",
			color = "#eaf2fb",
			box_sizing = "border-box",
		},
		children = {
			ui.box({
				style = {
					display = "block",
					padding = "6px 10px",
					background_color = "#243444",
					border_radius = "6px 6px 0px 0px",
					box_sizing = "border-box",
				},
				children = {
					ui.row({
						gap = "sm",
						children = { ui.text(title) },
					}),
				},
			}),
			ui.box({
				style = { display = "block", padding = "6px 10px", box_sizing = "border-box" },
				children = { ui.text(subtitle) },
			}),
		},
	})
end

local registered = false
local running = {}

local function snap_anchor(abs_x, abs_y, w, h, vw, vh, allowed)
	local function allowed_anchor(a)
		if not allowed or #allowed == 0 then
			return true
		end
		for _, aa in ipairs(allowed) do
			if aa == a then
				return true
			end
		end
		return false
	end

	local function base(a)
		if a == "center" then
			return math.floor((vw - w) / 2), math.floor((vh - h) / 2)
		end
		if a == "top-left" then
			return 0, 0
		end
		if a == "top-right" then
			return vw - w, 0
		end
		if a == "bottom-left" then
			return 0, vh - h
		end
		if a == "bottom-right" then
			return vw - w, vh - h
		end
		if a == "top" then
			return math.floor((vw - w) / 2), 0
		end
		if a == "bottom" then
			return math.floor((vw - w) / 2), vh - h
		end
		if a == "left" then
			return 0, math.floor((vh - h) / 2)
		end
		if a == "right" then
			return vw - w, math.floor((vh - h) / 2)
		end
		return math.floor((vw - w) / 2), math.floor((vh - h) / 2)
	end

	local thresh = 24
	local best, best_score = nil, nil
	local candidates = {
		"center",
		"top-left",
		"top-right",
		"bottom-left",
		"bottom-right",
		"top",
		"bottom",
		"left",
		"right",
	}
	for _, a in ipairs(candidates) do
		if allowed_anchor(a) then
			local bx, by = base(a)
			local dx = abs_x - bx
			local dy = abs_y - by
			if math.abs(dx) <= thresh and math.abs(dy) <= thresh then
				local score = dx * dx + dy * dy
				if not best_score or score < best_score then
					best_score = score
					best = a
				end
			end
		end
	end
	if not best then
		return nil
	end
	local bx, by = base(best)
	return best, abs_x - bx, abs_y - by
end

local function placement_for_drag(ctx, abs_x, abs_y)
	local w = ctx.initial_rect.w
	local h = ctx.initial_rect.h
	local vw = ctx.viewport.w
	local vh = ctx.viewport.h
	local anch = "top-left"
	if type(ctx.placement) == "table" and type(ctx.placement.anchor) == "string" then
		anch = ctx.placement.anchor
	end
	local function base(a)
		if a == "center" then
			return math.floor((vw - w) / 2), math.floor((vh - h) / 2)
		end
		if a == "top-left" then
			return 0, 0
		end
		if a == "top-right" then
			return vw - w, 0
		end
		if a == "bottom-left" then
			return 0, vh - h
		end
		if a == "bottom-right" then
			return vw - w, vh - h
		end
		if a == "top" then
			return math.floor((vw - w) / 2), 0
		end
		if a == "bottom" then
			return math.floor((vw - w) / 2), vh - h
		end
		if a == "left" then
			return 0, math.floor((vh - h) / 2)
		end
		if a == "right" then
			return vw - w, math.floor((vh - h) / 2)
		end
		return math.floor((vw - w) / 2), math.floor((vh - h) / 2)
	end
	local bx, by = base(anch)
	return {
		anchor = anch,
		x = math.floor(abs_x - bx + 0.5),
		y = math.floor(abs_y - by + 0.5),
		keep_in_view = true,
	}
end

local function merge_style_patches(a, b)
	if not a then
		return b
	end
	if not b then
		return a
	end
	local out = {}
	for k, v in pairs(a) do
		out[k] = v
	end
	if type(b.styles) == "table" then
		out.styles = out.styles or {}
		for eid, props in pairs(b.styles) do
			out.styles[eid] = out.styles[eid] or {}
			for pk, pv in pairs(props) do
				out.styles[eid][pk] = pv
			end
		end
	end
	return out
end

local function make_handler(root_id, opts, allowed_anchors, sticky)
	sticky = sticky ~= false
	return function(ctx)
		if ctx.kind == "drag" and (ctx.phase == "drag_move" or ctx.phase == "drag_end") then
			local abs_x = ctx.initial_rect.x + ctx.delta.x
			local abs_y = ctx.initial_rect.y + ctx.delta.y
			local w = ctx.initial_rect.w
			local h = ctx.initial_rect.h
			local vw = ctx.viewport.w
			local vh = ctx.viewport.h

			if ctx.phase == "drag_end" and sticky then
				local a, ox, oy = snap_anchor(abs_x, abs_y, w, h, vw, vh, allowed_anchors)
				if a then
					return { placement = { anchor = a, x = ox, y = oy, keep_in_view = true } }
				end
			end
			return { placement = placement_for_drag(ctx, abs_x, abs_y) }
		end

		if ctx.kind == "resize" and (ctx.phase == "resize_move" or ctx.phase == "resize_end") then
			if core.rmlui_compute_instrument_resize_patch then
				local rp = core.rmlui_compute_instrument_resize_patch(ctx, root_id, opts)
				diag_021(ctx, "resize " .. root_id, opts)
				return rp
			end
			return nil
		end
		return nil
	end
end

local function make_handler_with_adapt(root_id, opts, allowed_anchors, sticky, adapt_cfg)
	local base = make_handler(root_id, opts, allowed_anchors, sticky)
	if type(adapt_cfg) ~= "table" then
		return base
	end
	return function(ctx)
		local patch = base(ctx)
		if ctx.phase ~= "drag_end" and ctx.phase ~= "resize_end" then
			return patch
		end
		diag_021(ctx, "adapt_pre " .. root_id, opts)
		if core.rmlui_adapt_instrument then
			local ap = core.rmlui_adapt_instrument(ctx, root_id, adapt_cfg)
			if ap then
				diag_021(ctx, "adapt_post " .. root_id, opts, ap)
				patch = merge_style_patches(patch, ap)
			end
		end
		return patch
	end
end

local function slot_box()
	return ui.box({
		style = {
			width = "22px",
			height = "22px",
			background_color = "#3a5068",
			border = "1px solid #8ac",
			border_radius = "3px",
			box_sizing = "border-box",
		},
	})
end

local function adaptive_hotbar_content(root_id, bar_id, title, subtitle)
	return ui.hud_box({
		id = root_id,
		style = {
			width = "200px",
			min_height = "88px",
			overflow = "hidden",
			background_color = "#1a2430",
			border = "2px solid #6ac",
			border_radius = "8px",
			padding = "8px",
			box_sizing = "border-box",
			color = "#eaf2fb",
		},
		children = {
			ui.box({
				style = { display = "block", font_size = "13px", margin_bottom = "4px" },
				children = { ui.text(title) },
			}),
			ui.box({
				style = { display = "block", font_size = "11px", opacity = "0.85", margin_bottom = "6px" },
				children = { ui.text(subtitle) },
			}),
			ui.box({
				id = bar_id,
				style = {
					display = "flex",
					flex_direction = "row",
					gap = "4px",
					flex = "1",
					align_items = "center",
				},
				children = { slot_box(), slot_box(), slot_box(), slot_box() },
			}),
		},
	})
end

local function simple_adaptive_panel(root_id, title, subtitle)
	return ui.hud_box({
		id = root_id,
		style = {
			width = "200px",
			height = "100px",
			overflow = "hidden",
			background_color = "#1a2430",
			border = "2px solid #6ac",
			border_radius = "8px",
			padding = "10px",
			box_sizing = "border-box",
			color = "#eaf2fb",
		},
		children = {
			ui.text(title),
			ui.box({
				style = { display = "block", margin_top = "6px", font_size = "11px", opacity = "0.9" },
				children = { ui.text(subtitle) },
			}),
		},
	})
end

local function start_for_player(player)
	local name = player and player.get_player_name and player:get_player_name() or nil
	if not name or running[name] then
		return
	end
	running[name] = true

	core.after(0, function()
		if not running[name] then
			return
		end

		if not core.rmlui_compute_instrument_resize_patch then
			core.chat_send_player(name, "[testui] TEST_021 FAILED: core.rmlui_compute_instrument_resize_patch missing (builtin init?)")
			running[name] = nil
			return
		end

		if not core.rmlui_adapt_instrument then
			core.chat_send_player(name, "[testui] TEST_021 FAILED: core.rmlui_adapt_instrument missing (builtin init?)")
			running[name] = nil
			return
		end

		core.chat_send_player(name, "[testui] TEST_021: Six HUD surfaces (A–C resize, D–F adaptation). Instrument mode on; ESC exits.")
		core.chat_send_player(name, "[testui] TEST_021: A–C = aspect / square / free resize. D–F = drag or resize end → 9-way region → orientation + variant.")
		core.chat_send_player(name, "[testui] TEST_021: Verify placement.region/kind stable; no adaptation during drag_move (only drag_end / resize_end).")

		local all_anchors = {
			"top-left",
			"top-right",
			"bottom-left",
			"bottom-right",
			"center",
			"top",
			"bottom",
			"left",
			"right",
		}

		local geom_hotbar = {
			horizontal = {
				root = { width = "240px", min_height = "104px", height = "auto" },
				elements = { bar = { width = "100%", min_height = "40px" } },
			},
			vertical = {
				root = { width = "120px", min_height = "240px", height = "auto" },
				elements = { bar = { width = "100%", flex = "1", min_width = "40px", min_height = "0px" } },
			},
		}

		-- Vertical bar only on true side columns; corners stay horizontal (no premature vertical).
		local orient_mixed = {
			top = "horizontal",
			bottom = "horizontal",
			left = "vertical",
			right = "vertical",
			["top-left"] = "horizontal",
			["top-right"] = "horizontal",
			["bottom-left"] = "horizontal",
			["bottom-right"] = "horizontal",
			center = "horizontal",
		}

		local adapt_d_cfg = {
			adaptive = {
				enabled = true,
				orientation = { mode = "auto", map = orient_mixed },
				variant = {},
				geometry = geom_hotbar,
			},
			elements = { bar = "test_021_d_bar" },
		}

		local adapt_e_cfg = {
			adaptive = {
				enabled = true,
				orientation = { mode = "fixed", fixed = "horizontal" },
				variant = {},
			},
		}

		local adapt_f_cfg = {
			adaptive = {
				enabled = true,
				orientation = { mode = "auto", map = orient_mixed },
				variant = {},
				geometry = geom_hotbar,
			},
			elements = { bar = "test_021_f_bar" },
		}

		local alive = true
		local handles = {}

		local function stop_all(reason)
			if not alive then
				return
			end
			alive = false
			running[name] = nil
			core.ui.exit_instrument_mode({ player = name })
			for _, h in pairs(handles) do
				if h and h.is_open and h:is_open() then
					h:close()
				end
			end
			core.log("action", "[testui] TEST_021: stop_all reason=" .. tostring(reason))
		end

		core.register_on_leaveplayer(function(p)
			if p and p:get_player_name() == name then
				stop_all("leaveplayer")
			end
		end)
		core.register_on_shutdown(function()
			stop_all("shutdown")
		end)

		core.ui.enter_instrument_mode({ player = name })

		handles.a = ui.hud({
			player = name,
			id = "test_021_hud_a",
			anchor = "top-left",
			x = 20,
			y = 120,
			keep_in_view = true,
			instrument = {
				movable = true,
				resizable = true,
				sticky = true,
				keep_aspect = true,
				anchors = { "top-left", "top-right", "bottom-left", "bottom-right", "center", "top", "left" },
				on_event = make_handler(
					"test_021_a_root",
					{ min_w = 200, min_h = 120, max_w = 560, max_h = 360 },
					{ "top-left", "top-right", "bottom-left", "bottom-right", "center", "top", "left" },
					true
				),
			},
			content = hud_panel(
				"test_021_a_root",
				"021-A aspect (derived)",
				"Implicit ratio from first size; corners + edges.",
				"240px",
				"140px"
			),
		})

		handles.b = ui.hud({
			player = name,
			id = "test_021_hud_b",
			anchor = "center",
			x = 0,
			y = 0,
			keep_in_view = true,
			instrument = {
				movable = true,
				resizable = true,
				sticky = true,
				keep_aspect = true,
				aspect_ratio = 1.0,
				anchors = { "center", "top-left", "top-right", "bottom-left", "bottom-right" },
				on_event = make_handler(
					"test_021_b_root",
					{ min_w = 120, min_h = 120, max_w = 400, max_h = 400 },
					{ "center", "top-left", "top-right", "bottom-left", "bottom-right" },
					true
				),
			},
			content = hud_panel(
				"test_021_b_root",
				"021-B square",
				"aspect_ratio=1; symmetric center resize.",
				"180px",
				"180px"
			),
		})

		handles.c = ui.hud({
			player = name,
			id = "test_021_hud_c",
			anchor = "top-right",
			x = -20,
			y = 200,
			keep_in_view = true,
			instrument = {
				movable = true,
				resizable = true,
				sticky = true,
				keep_aspect = false,
				anchors = { "top-right", "right", "bottom-right", "center" },
				on_event = make_handler(
					"test_021_c_root",
					{ min_w = 200, min_h = 96, max_w = 520, max_h = 300 },
					{ "top-right", "right", "bottom-right", "center" },
					true
				),
			},
			content = hud_panel(
				"test_021_c_root",
				"021-C generic HUD",
				"Same layout contract; free resize (no aspect lock).",
				"260px",
				"110px"
			),
		})

		handles.d = ui.hud({
			player = name,
			id = "test_021_hud_d",
			anchor = "top",
			x = 0,
			y = 16,
			keep_in_view = true,
			instrument = {
				movable = true,
				resizable = true,
				sticky = true,
				keep_aspect = false,
				anchors = all_anchors,
				on_event = make_handler_with_adapt(
					"test_021_d_root",
					{ min_w = 160, min_h = 80, max_w = 480, max_h = 280 },
					all_anchors,
					true,
					adapt_d_cfg
				),
			},
			content = adaptive_hotbar_content(
				"test_021_d_root",
				"test_021_d_bar",
				"021-D edge hotbar",
				"Auto orientation: horizontal on top/bottom, vertical on left/right."
			),
		})

		handles.e = ui.hud({
			player = name,
			id = "test_021_hud_e",
			anchor = "bottom-left",
			x = 24,
			y = -24,
			keep_in_view = true,
			instrument = {
				movable = true,
				resizable = true,
				sticky = true,
				keep_aspect = false,
				anchors = all_anchors,
				on_event = make_handler_with_adapt(
					"test_021_e_root",
					{ min_w = 140, min_h = 72, max_w = 360, max_h = 200 },
					all_anchors,
					true,
					adapt_e_cfg
				),
			},
			content = simple_adaptive_panel(
				"test_021_e_root",
				"021-E corner / edge accents",
				"Fixed horizontal chrome; variant borders follow ctx.placement.region."
			),
		})

		handles.f = ui.hud({
			player = name,
			id = "test_021_hud_f",
			anchor = "center",
			x = -180,
			y = 120,
			keep_in_view = true,
			instrument = {
				movable = true,
				resizable = true,
				sticky = true,
				keep_aspect = false,
				anchors = all_anchors,
				on_event = make_handler_with_adapt(
					"test_021_f_root",
					{ min_w = 160, min_h = 88, max_w = 420, max_h = 260 },
					all_anchors,
					true,
					adapt_f_cfg
				),
			},
			content = adaptive_hotbar_content(
				"test_021_f_root",
				"test_021_f_bar",
				"021-F mixed",
				"Like D: vertical bar only when region is left/right; corners stay horizontal. "
					.. "Corner snake / multi-segment wrap is not implemented."
			),
		})
	end)
end

local function run(playername)
	if registered then
		if playername then
			start_for_player({ get_player_name = function() return playername end })
		end
		return
	end
	registered = true

	core.register_on_joinplayer(start_for_player)
	if core.get_connected_players then
		for _, player in ipairs(core.get_connected_players()) do
			start_for_player(player)
		end
	end
	if playername then
		start_for_player({ get_player_name = function() return playername end })
	end
end

return { run = run }
