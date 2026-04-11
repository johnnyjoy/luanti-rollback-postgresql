--[[
	TEST_016: Timed out-of-order modal removal (remove lower modal A on timer while upper modal B remains).

	Scenario:
	- Mount Modal A (lower)
	- Mount Modal B above A
	- After B appears, wait 5 seconds, then automatically close A (out-of-order)
	- Expected: B remains visible + interactive after A disappears
]]

local ui = core.ui

local function modal_a_tree()
	return ui.shadow_box({
		shadow = { dx = 0, dy = 8, softness = 2, color = "#000000", opacities = { 0.22, 0.12, 0.08 } },
		style = {
			position = "fixed",
			top = "140px",
			left = "160px",
			width = "460px",
			background_color = "#2a3a4a",
			border_radius = "10px",
			padding = "16px",
			box_sizing = "border-box",
			border = "2px solid #3a556a",
		},
		children = {
			ui.column({
				gap = "md",
				children = {
					ui.text("Modal A (lower) — should auto-close 5s after B appears"),
					ui.input({
						id = "a_in",
						value = "",
						placeholder = "Modal A input",
						autofocus = true,
					}),
					ui.button({
						text = "A button (no-op)",
						on_press = function()
							core.log("action", "[testui] TEST_016: A button pressed")
						end,
					}),
				},
			}),
		},
	})
end

local function modal_b_tree(close_a, close_b)
	return ui.shadow_box({
		shadow = { dx = 0, dy = 8, softness = 2, color = "#000000", opacities = { 0.22, 0.12, 0.08 } },
		style = {
			position = "fixed",
			top = "190px",
			left = "230px",
			width = "520px",
			background_color = "#1e2a36",
			border_radius = "10px",
			padding = "16px",
			box_sizing = "border-box",
			border = "2px solid #4da3ff",
		},
		children = {
			ui.column({
				gap = "md",
				children = {
					ui.text("Modal B (upper) — should remain after A is removed"),
					ui.input({
						id = "b_in",
						value = "",
						placeholder = "Modal B input (should still work after A disappears)",
						autofocus = true,
					}),
					ui.button({
						text = "Close A now (out-of-order)",
						on_press = function()
							core.log("action", "[testui] TEST_016: B pressed: close A now")
							if close_a then
								close_a("button")
							end
						end,
					}),
					ui.button({
						text = "Close B",
						on_press = function()
							core.log("action", "[testui] TEST_016: B pressed: close B")
							if close_b then
								close_b("button")
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

	core.after(0, function()
		if not running[name] then
			return
		end
		core.chat_send_player(name, "[testui] TEST_016: Modal A opens, then Modal B opens over it.")
		core.chat_send_player(name, "[testui] TEST_016: 5 seconds after B appears, A auto-closes (out-of-order). B should remain.")
		core.chat_send_player(name, "[testui] TEST_016: Dismiss is ESC-only. You can also use buttons in B.")

		local a_handle
		local b_handle

		local function close_a(reason)
			if a_handle and a_handle.is_open and a_handle:is_open() then
				core.log("action", "[testui] TEST_016: closing A out-of-order reason=" .. tostring(reason))
				local ok, err = a_handle:close()
				if not ok then
					core.log("warning", "[testui] TEST_016: close A failed err=" .. tostring(err))
				end
			end
		end

		local function close_b(reason)
			if b_handle and b_handle.is_open and b_handle:is_open() then
				core.log("action", "[testui] TEST_016: closing B reason=" .. tostring(reason))
				local ok, err = b_handle:close()
				if not ok then
					core.log("warning", "[testui] TEST_016: close B failed err=" .. tostring(err))
				end
			end
		end

		a_handle = ui.modal({
			player = name,
			id = "test_016_modal_a",
			dismiss = "escape",
			state = {},
			content = modal_a_tree(),
			on_dismiss = function(ctx)
				core.log("action", "[testui] TEST_016: A dismissed reason=" .. tostring(ctx and ctx.reason))
			end,
		})
		if not a_handle then
			core.log("warning", "[testui] TEST_016 FAILED: modal A did not open")
			running[name] = nil
			return
		end

		core.after(0.25, function()
			if not running[name] then
				return
			end
			b_handle = ui.modal({
				player = name,
				id = "test_016_modal_b",
				dismiss = "escape",
				state = {},
				content = modal_b_tree(close_a, close_b),
				on_dismiss = function(ctx)
					core.log("action", "[testui] TEST_016: B dismissed reason=" .. tostring(ctx and ctx.reason))
					-- Ensure the handle is closed to avoid leaving a stale surface in session.
					close_b(ctx and ctx.reason or "dismiss")
				end,
			})
			if not b_handle then
				core.log("warning", "[testui] TEST_016 FAILED: modal B did not open")
				running[name] = nil
				return
			end

			core.log("action", "[testui] TEST_016: scheduling auto-close of A in 5s (B should remain)")
			core.after(5.0, function()
				close_a("timer_5s_after_B")
			end)
		end)
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

