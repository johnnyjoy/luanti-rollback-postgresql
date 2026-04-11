--[[
	TEST_017: Random modal churn stress test (1000 modals).

	Goal:
	- Generate a long stream of modal mounts with random lifetimes so they close out-of-order.
	- Lifetime per modal: random in [0.5, 1.75] seconds (sped up).
	- Total generated: 1000 (default).

	Notes:
	- Uses an upper bound on concurrently active modals to avoid pathological stalls on slow machines.
	- Explicit handle:close() does NOT invoke on_dismiss; we cleanup in both paths.
]]

local ui = core.ui

-- Time scale: keep the same test shape but run faster.
-- Requested: "halve the time" while still generating 1000 modals.
local MIN_LIFE_S = 0.2
local MAX_LIFE_S = 1
local DEFAULT_TOTAL = 1000
local DEFAULT_MAX_ACTIVE = 100
local CLOSE_FINALIZE_GRACE_S = 0.20

-- Modal sizing is intentionally fixed: size is not part of the stress test.
local MODAL_W_PX = 460
local MODAL_H_PX = 180
local MODAL_MARGIN_PX = 8

local function clamp(x, lo, hi)
	if x < lo then
		return lo
	end
	if x > hi then
		return hi
	end
	return x
end

local function rndf()
	return math.random()
end

local function rand_life_s()
	return MIN_LIFE_S + (MAX_LIFE_S - MIN_LIFE_S) * rndf()
end

local function rand_spawn_delay_s()
	-- Must be shorter than MIN_LIFE_S sometimes to guarantee overlap (hence out-of-order closes).
	-- Keep wide enough to avoid slamming the client too hard.
	return 0.03 + 0.10 * rndf()
end

local function tohex2(n)
	return string.format("%02x", clamp(math.floor(n + 0.5), 0, 255))
end

local function rgb_to_hex(r, g, b)
	return "#" .. tohex2(r) .. tohex2(g) .. tohex2(b)
end

local function hsl_to_rgb(h, s, l)
	-- h in [0, 360), s,l in [0, 1]
	h = (h % 360) / 360
	local function hue2rgb(p, q, t)
		if t < 0 then t = t + 1 end
		if t > 1 then t = t - 1 end
		if t < 1/6 then return p + (q - p) * 6 * t end
		if t < 1/2 then return q end
		if t < 2/3 then return p + (q - p) * (2/3 - t) * 6 end
		return p
	end
	local r, g, b
	if s == 0 then
		r, g, b = l, l, l
	else
		local q = (l < 0.5) and (l * (1 + s)) or (l + s - l * s)
		local p = 2 * l - q
		r = hue2rgb(p, q, h + 1/3)
		g = hue2rgb(p, q, h)
		b = hue2rgb(p, q, h - 1/3)
	end
	return r * 255, g * 255, b * 255
end

local function nice_colors()
	-- Dark but colorful background + brighter border accent.
	local hue = 360 * rndf()
	local sat = 0.45 + 0.30 * rndf()
	local bg_l = 0.16 + 0.08 * rndf()
	local bd_l = clamp(bg_l + 0.22 + 0.06 * rndf(), 0, 1)

	local br, bg, bb = hsl_to_rgb(hue, sat, bg_l)
	local rr, rg, rb = hsl_to_rgb(hue, clamp(sat + 0.10, 0, 1), bd_l)

	return {
		bg = rgb_to_hex(br, bg, bb),
		border = rgb_to_hex(rr, rg, rb),
		text = "#eaf2fb",
		subtext = "#b8c7d6",
	}
end

local function modal_tree(i, life_s, created_at, placement, colors, close_self)
	local top = placement.top
	local left = placement.left
	local w = placement.w
	local h = placement.h
	local center_anchor = placement._center_anchor

	return ui.shadow_box({
		shadow = {
			dx = 0,
			dy = 10,
			softness = 2,
			color = "#000000",
			opacities = { 0.26, 0.14, 0.08 },
		},
		style = {
			position = "fixed",
			top = top,
			left = left,
			transform = center_anchor and "translate(-50%, -50%)" or nil,
			width = tostring(w) .. "px",
			height = tostring(h) .. "px",
			background_color = colors.bg,
			border_radius = "10px",
			padding = "14px",
			box_sizing = "border-box",
			border = "2px solid " .. colors.border,
			color = colors.text,
		},
		children = {
			ui.column({
				gap = "sm",
				children = {
					ui.text("TEST_017 modal #" .. i),
					ui.text(string.format("life=%.3fs  created=%.3f", life_s, created_at)),
					ui.input({
						id = "in_" .. i,
						value = "",
						placeholder = "type here (optional)",
						autofocus = (i % 17 == 0),
					}),
					ui.button({
						text = "Close this modal now",
						on_press = function()
							core.log("action", string.format("[testui] TEST_017: modal #%d close button pressed", i))
							if close_self then
								close_self("button")
							end
						end,
					}),
				},
			}),
		},
	})
end

local registered = false
local running = {}

local function start_for_player(player)
	local name = player and player.get_player_name and player:get_player_name() or nil
	if not name then
		return
	end
	if running[name] then
		return
	end
	running[name] = true

	-- Allow tuning without editing the file.
	local total = tonumber(core.settings and core.settings:get("testui_test017_total") or "") or DEFAULT_TOTAL
	local max_active = tonumber(core.settings and core.settings:get("testui_test017_max_active") or "") or DEFAULT_MAX_ACTIVE
	total = math.floor(clamp(total, 1, 5000))
	max_active = math.floor(clamp(max_active, 1, 250))

	core.after(0, function()
		if not running[name] then
			return
		end
		local alive = true

		core.chat_send_player(name, string.format(
			"[testui] TEST_017: starting random modal churn: total=%d, life=[%.1f,%.1f]s, max_active=%d",
			total, MIN_LIFE_S, MAX_LIFE_S, max_active
		))
		core.chat_send_player(name, "[testui] TEST_017: Expect heavy out-of-order closes. ESC can dismiss the topmost modal.")

		-- active[sid] = { h = handle, state = "open"|"closing" }
		local active = {}
		local active_n = 0
		local spawned = 0
		local close_finalized = 0

		local function stop_all(reason)
			if not alive then
				return
			end
			alive = false
			running[name] = nil
			for sid, ent in pairs(active) do
				local h = ent and ent.h
				if h and h.is_open and h:is_open() then
					h:close()
				end
				active[sid] = nil
			end
			active_n = 0
			core.log("action", "[testui] TEST_017: stop_all reason=" .. tostring(reason))
		end

		core.register_on_leaveplayer(function(p)
			if p and p:get_player_name() == name then
				stop_all("leaveplayer")
			end
		end)
		core.register_on_shutdown(function()
			stop_all("shutdown")
		end)

		local function cleanup(sid)
			if active[sid] then
				active[sid] = nil
				active_n = math.max(0, active_n - 1)
			end
		end

		local function mark_close_finalized(sid, why)
			if not alive then
				return
			end
			if not active[sid] then
				return
			end

			cleanup(sid)
			close_finalized = close_finalized + 1

			if close_finalized % 50 == 0 or close_finalized == total then
				core.log("action", string.format(
					"[testui] TEST_017: progress finalized=%d/%d active=%d spawned=%d",
					close_finalized, total, active_n, spawned
				))
			end

			if close_finalized == total then
				core.chat_send_player(name, "[testui] TEST_017: DONE (all modals closed).")
				stop_all("complete")
			end
		end

		local function request_close(sid, why)
			if not alive then
				return
			end
			local ent = active[sid]
			if not ent then
				return
			end
			if ent.state == "closing" then
				return
			end
			ent.state = "closing"

			local h = ent.h
			if h and h.is_open and h:is_open() then
				local ok, err = h:close()
				if not ok then
					core.log("warning", "[testui] TEST_017: close failed sid=" .. sid .. " why=" .. tostring(why) .. " err=" .. tostring(err))
				end
			end

			-- Conservative finalize: do NOT immediately decrement active/backpressure counters.
			-- This keeps generator pressure stable under client lag / delayed unmount.
			core.after(CLOSE_FINALIZE_GRACE_S, function()
				if not alive then
					return
				end
				mark_close_finalized(sid, why)
			end)
		end

		local function spawn_one()
			if not alive then
				return
			end
			if spawned >= total then
				return
			end
			if active_n >= max_active then
				-- Backpressure: wait a little until the active set drains.
				core.after(0.05, spawn_one)
				return
			end

			spawned = spawned + 1
			local i = spawned
			local sid = "test_017_modal_" .. i
			local life_s = rand_life_s()
			local created_at = core.get_us_time() / 1e6
			local placement
			do
				local info = core.get_player_window_information and core.get_player_window_information(name) or nil
				local size = info and info.size or nil
				local vw = size and tonumber(size.x) or nil
				local vh = size and tonumber(size.y) or nil

				if vw and vh and vw > 0 and vh > 0 then
					-- Pixel-accurate clamped placement: guarantees full visibility (unless the viewport is
					-- smaller than the modal itself, in which case we pin to 0,0).
					local max_x = math.max(0, vw - MODAL_W_PX - 2 * MODAL_MARGIN_PX)
					local max_y = math.max(0, vh - MODAL_H_PX - 2 * MODAL_MARGIN_PX)
					local x = MODAL_MARGIN_PX + math.floor(max_x * rndf())
					local y = MODAL_MARGIN_PX + math.floor(max_y * rndf())
					placement = {
						top = tostring(y) .. "px",
						left = tostring(x) .. "px",
						w = MODAL_W_PX,
						h = MODAL_H_PX,
					}
				else
					-- Fallback if window info hasn't arrived yet: keep the old center-anchored behavior.
					-- (We also keep the transform in the style in this case.)
					placement = {
						top = string.format("%.1f%%", 12 + 76 * rndf()),
						left = string.format("%.1f%%", 12 + 76 * rndf()),
						w = MODAL_W_PX,
						h = MODAL_H_PX,
						_center_anchor = true,
					}
				end
			end
			local colors = nice_colors()

			local function close_self(why)
				request_close(sid, why or "button")
			end

			local h = ui.modal({
				player = name,
				id = sid,
				dismiss = "escape",
				state = {},
				content = modal_tree(i, life_s, created_at, placement, colors, close_self),
				on_dismiss = function(ctx)
					core.log("action", "[testui] TEST_017: dismissed sid=" .. sid .. " reason=" .. tostring(ctx and ctx.reason))
					-- Dismiss is a *request* (client->server). Close explicitly and finalize conservatively.
					request_close(sid, "dismiss")
				end,
			})

			if not h then
				core.log("warning", "[testui] TEST_017: failed to open modal sid=" .. sid)
				-- Keep the generator going.
				core.after(rand_spawn_delay_s(), spawn_one)
				return
			end

			active[sid] = { h = h, state = "open" }
			active_n = active_n + 1

			-- Schedule independent close: strongly encourages out-of-order removal.
			core.after(life_s, function()
				request_close(sid, "timer")
			end)

			-- Schedule next spawn.
			core.after(rand_spawn_delay_s(), spawn_one)
		end

		-- Kick off after a tiny delay so join settles.
		core.after(0.20, spawn_one)
	end)
end

local function run()
	if registered then
		return
	end
	registered = true

	-- Deterministic-enough run-to-run, but still varied.
	math.randomseed(os.time())

	core.register_on_joinplayer(start_for_player)
	if core.get_connected_players then
		for _, player in ipairs(core.get_connected_players()) do
			start_for_player(player)
		end
	end
end

return { run = run }

