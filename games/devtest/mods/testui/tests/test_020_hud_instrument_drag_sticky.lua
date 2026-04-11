--[[
	TEST_020: HUD instrument mode — draggable HUD surfaces + sticky anchors.

	Instructions:
	- Run: /testui 020
	- Instrument mode is enabled automatically; ESC exits.
	- Drag HUD boxes near edges/corners/center to snap.
	- Drag from the title bar; resize by grabbing an edge/corner.
	- Resize window; elements should remain reachable (keep_in_view).
]]

local ui = core.ui

local function hud_box(root_id, title, subtitle)
	return ui.hud_box({
		id = root_id,
		-- HUD is flat-by-default; set shadow=true if you want it.
		shadow = false,
		style = {
			width = "260px",
			height = "120px",
			background_color = "#1e2a36",
			border = "2px solid #4da3ff",
			border_radius = "10px",
			padding = "0px",
			color = "#eaf2fb",
		},
		children = {
			-- Drag handle region (instrument mode): clicking here starts drag, not actions.
			ui.box({
				id = "instrument_drag",
				drag_handle = true,
				style = {
					display = "block",
					padding = "8px 12px",
					background_color = "#223447",
					border_radius = "8px 8px 0px 0px",
					box_sizing = "border-box",
				},
				children = {
					ui.row({
						gap = "sm",
						children = {
							ui.text("≡"),
							ui.text(title),
						},
					}),
				},
			}),
			ui.box({
				style = { display = "block", padding = "8px 12px", box_sizing = "border-box" },
				children = {
					ui.text(subtitle),
				},
			}),
		},
	})
end

local registered = false
local running = {}

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

		core.chat_send_player(name, "[testui] TEST_020: Mounted 2 HUD surfaces. Instrument mode is enabled; ESC exits.")
		core.chat_send_player(name, "[testui] TEST_020: Drag near edges/corners/center to test sticky anchors + clamping.")
		core.chat_send_player(name, "[testui] TEST_020: Resize by grabbing an edge/corner (no visible handle).")

		local alive = true
		local handles = {}

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
			local best = nil
			local best_score = nil
			local candidates = { "center", "top-left", "top-right", "bottom-left", "bottom-right", "top", "bottom", "left", "right" }
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

		local function make_instrument_handler(allowed_anchors, root_id)
			return function(ctx)
				if not alive then
					return nil
				end
				if ctx.kind == "drag" and (ctx.phase == "drag_move" or ctx.phase == "drag_end") then
					local abs_x = ctx.initial_rect.x + ctx.delta.x
					local abs_y = ctx.initial_rect.y + ctx.delta.y
					local w = ctx.initial_rect.w
					local h = ctx.initial_rect.h
					local vw = ctx.viewport.w
					local vh = ctx.viewport.h

					if ctx.phase == "drag_end" then
						local a, ox, oy = snap_anchor(abs_x, abs_y, w, h, vw, vh, allowed_anchors)
						if a then
							return { placement = { anchor = a, x = ox, y = oy, keep_in_view = true } }
						end
					end
					return { placement = { anchor = "top-left", x = abs_x, y = abs_y, keep_in_view = true } }
				end

				if ctx.kind == "resize" and (ctx.phase == "resize_move" or ctx.phase == "resize_end") then
					local w0 = ctx.initial_rect.w
					local h0 = ctx.initial_rect.h
					local x0 = ctx.initial_rect.x
					local y0 = ctx.initial_rect.y
					local dx = ctx.delta.x
					local dy = ctx.delta.y

					local w = w0
					local h = h0
					local abs_x = x0
					local abs_y = y0

					local rh = ctx.resize_handle or ""
					if rh:find("e", 1, true) then
						w = w0 + dx
					end
					if rh:find("w", 1, true) then
						w = w0 - dx
						abs_x = x0 + dx
					end
					if rh:find("s", 1, true) then
						h = h0 + dy
					end
					if rh:find("n", 1, true) then
						h = h0 - dy
						abs_y = y0 + dy
					end

					if w < 200 then w = 200 end
					if h < 90 then h = 90 end
					if w > 520 then w = 520 end
					if h > 320 then h = 320 end
					return {
						placement = { anchor = "top-left", x = abs_x, y = abs_y, keep_in_view = true },
						styles = {
							[root_id] = {
								width = tostring(w) .. "px",
								height = tostring(h) .. "px",
							},
						},
					}
				end
				return nil
			end
		end

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
			core.log("action", "[testui] TEST_020: stop_all reason=" .. tostring(reason))
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
			id = "test_020_hud_a",
			anchor = "top-left",
			-- Start below chat log so it doesn't look like a 1px "artifact" in screenshots.
			x = 24,
			y = 110,
			keep_in_view = true,
			instrument = {
				movable = true,
				resizable = true,
				sticky = true,
				anchors = { "top-left", "top-right", "bottom-left", "bottom-right", "center" },
				on_event = make_instrument_handler({ "top-left", "top-right", "bottom-left", "bottom-right", "center" }, "hud_a_root"),
			},
			content = hud_box("hud_a_root", "HUD A (movable)", "Drag me. Snap enabled."),
		})

		handles.b = ui.hud({
			player = name,
			id = "test_020_hud_b",
			anchor = "bottom-right",
			x = -16,
			y = -16,
			keep_in_view = true,
			instrument = {
				movable = true,
				resizable = true,
				sticky = true,
				anchors = { "top-right", "bottom-right", "right", "center" },
				on_event = make_instrument_handler({ "top-right", "bottom-right", "right", "center" }, "hud_b_root"),
			},
			content = hud_box("hud_b_root", "HUD B (movable)", "Allowed anchors: right-side + center."),
		})
	end)
end

local function run()
	if registered then
		return
	end
	registered = true

	core.register_on_joinplayer(start_for_player)
	if core.get_connected_players then
		for _, player in ipairs(core.get_connected_players()) do
			start_for_player(player)
		end
	end
end

return { run = run }

