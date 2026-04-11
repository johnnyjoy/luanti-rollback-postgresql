--[[
	TEST_019: Minimal positioning layer validation (anchors + pixel offsets + keep_in_view).

	This test is meant to be manual/visual:
	- Verify anchor placement is deterministic
	- Verify viewport clamping keeps panels reachable
	- Resize window: elements should remain visible and re-clamp
]]

local ui = core.ui

local function mk_panel(title, subtitle, close_self, with_close)
	return ui.shadow_box({
		shadow = { dx = 0, dy = 7, softness = 1, color = "#000000", opacities = { 0.18, 0.10 } },
		style = {
			width = "360px",
			height = "170px",
			background_color = "#22304a",
			border = "2px solid #4f6aa0",
			border_radius = "12px",
			padding = "16px",
			box_sizing = "border-box",
			color = "#eaf2fb",
		},
		children = {
			ui.column({
				gap = "sm",
				children = {
					ui.text(title),
					ui.text(subtitle),
					with_close and ui.button({
						text = "Close",
						on_press = function()
							if close_self then
								close_self("button")
							end
						end,
					}) or ui.text("(panel is non-modal; close via ESC on the centered modal)"),
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

		core.chat_send_player(name, "[testui] TEST_019: validating anchor/x/y + keep_in_view clamping.")
		core.chat_send_player(name, "[testui] TEST_019: resize the window smaller/larger; panels should stay visible.")

		local alive = true
		local h_tl, h_br, h_c

		local function stop_all(reason)
			if not alive then
				return
			end
			alive = false
			running[name] = nil
			for _, h in ipairs({ h_tl, h_br, h_c }) do
				if h and h.is_open and h:is_open() then
					h:close()
				end
			end
			core.log("action", "[testui] TEST_019: stop_all reason=" .. tostring(reason))
		end

		core.register_on_leaveplayer(function(p)
			if p and p:get_player_name() == name then
				stop_all("leaveplayer")
			end
		end)
		core.register_on_shutdown(function()
			stop_all("shutdown")
		end)

		local function close_tl(why)
			if h_tl and h_tl.is_open and h_tl:is_open() then
				h_tl:close()
			end
			core.log("action", "[testui] TEST_019: close TL why=" .. tostring(why))
		end

		local function close_br(why)
			if h_br and h_br.is_open and h_br:is_open() then
				h_br:close()
			end
			core.log("action", "[testui] TEST_019: close BR why=" .. tostring(why))
		end

		local function close_c(why)
			if h_c and h_c.is_open and h_c:is_open() then
				h_c:close()
			end
			core.log("action", "[testui] TEST_019: close center why=" .. tostring(why))
			stop_all("closed_center")
		end

		-- Top-left, with negative offsets (should clamp to 0,0 when keep_in_view=true).
		h_tl = ui.panel({
			player = name,
			id = "test_019_topleft",
			anchor = "top-left",
			x = -40,
			y = -40,
			keep_in_view = true,
			content = mk_panel(
				"TEST_019 top-left",
				"anchor=top-left x=-40 y=-40 keep_in_view=true (should clamp)",
				close_tl,
				false
			),
		})

		-- Bottom-right, with positive offsets (should clamp to stay fully visible).
		h_br = ui.panel({
			player = name,
			id = "test_019_bottomright",
			anchor = "bottom-right",
			x = 40,
			y = 40,
			keep_in_view = true,
			content = mk_panel(
				"TEST_019 bottom-right",
				"anchor=bottom-right x=40 y=40 keep_in_view=true (should clamp)",
				close_br,
				false
			),
		})

		-- Centered modal (default interactive surface target).
		h_c = ui.modal({
			player = name,
			id = "test_019_center_modal",
			dismiss = "escape",
			anchor = "center",
			x = 0,
			y = 0,
			keep_in_view = true,
			state = {},
			content = mk_panel(
				"TEST_019 centered modal",
				"anchor=center keep_in_view=true (should remain centered/clamped on resize)",
				close_c,
				true
			),
			on_dismiss = function(ctx)
				close_c("dismiss:" .. tostring(ctx and ctx.reason))
			end,
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

