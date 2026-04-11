--[[
	TEST_012: Out-of-order modal removal (remove lower modal A while upper modal B remains).

	Objective:
	- Open Modal A (true modal)
	- Open Modal B stacked above A (true modal)
	- Confirm both are mounted
	- Remove A intentionally while B remains open
	- Observe whether B remains mounted + interactive + modal owner

	No background non-modal panels. No auto-close timers.
]]

local ui = core.ui

local function modal_a_tree()
	return ui.box({
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
					ui.text("Modal A (lower)"),
					ui.input({
						id = "a_in",
						value = "",
						placeholder = "Modal A input (should be irrelevant once A is removed)",
						autofocus = true,
					}),
					ui.button({
						text = "A button (close A)",
						on_press = function()
							core.log("action", "[testui] TEST_012: A button pressed")
						end,
					}),
				},
			}),
		},
	})
end

local function modal_b_tree(close_a_ooo, close_b)
	return ui.box({
		style = {
			position = "fixed",
			top = "190px",
			left = "230px",
			width = "500px",
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
					ui.text("Modal B (upper)"),
					ui.input({
						id = "b_in",
						value = "",
						placeholder = "Modal B input (must remain active after A is removed)",
						autofocus = true,
					}),
					ui.button({
						text = "Remove A out-of-order (keep B)",
						on_press = function()
							core.log("action", "[testui] TEST_012: B pressed: remove A out-of-order")
							if close_a_ooo then
								close_a_ooo()
							end
						end,
					}),
					ui.button({
						text = "Close B",
						on_press = function()
							core.log("action", "[testui] TEST_012: B pressed: close B")
							if close_b then
								close_b()
							end
						end,
					}),
				},
			}),
		},
	})
end

local registered = false

local function run()
	if registered then
		return
	end
	registered = true

	core.register_on_joinplayer(function(player)
		local name = player:get_player_name()
		core.after(0, function()
			core.chat_send_player(name, "[testui] TEST_012: Out-of-order removal: B will mount, then A will be removed out-of-order while B remains.")
			core.chat_send_player(name, "[testui] TEST_012: (Optional) Use B buttons to remove A out-of-order or close B.")
			core.chat_send_player(name, "[testui] TEST_012: Dismiss is ESC-only; no background panels; no timers.")

			local a_handle
			local b_handle

			local function close_a_out_of_order()
				if a_handle and a_handle.is_open and a_handle:is_open() then
					local ok, err = a_handle:close()
					if not ok then
						core.log("warning", "[testui] TEST_012: close A failed err=" .. tostring(err))
					end
				end
			end

			local function close_b()
				if b_handle and b_handle.is_open and b_handle:is_open() then
					local ok, err = b_handle:close()
					if not ok then
						core.log("warning", "[testui] TEST_012: close B failed err=" .. tostring(err))
					end
				end
			end

			a_handle = ui.modal({
				player = name,
				id = "test_012_modal_a",
				dismiss = "escape",
				state = {},
				content = modal_a_tree(),
				on_dismiss = function(ctx)
					core.log("action", "[testui] TEST_012: A dismissed reason=" .. tostring(ctx and ctx.reason))
					-- Do not auto-close B from A dismiss; keep the test strict.
				end,
			})
			if not a_handle then
				core.log("warning", "[testui] TEST_012 FAILED: modal A did not open")
				return
			end

			core.after(0.25, function()
				b_handle = ui.modal({
					player = name,
					id = "test_012_modal_b",
					dismiss = "escape",
					state = {},
					content = modal_b_tree(close_a_out_of_order, close_b),
					on_dismiss = function(ctx)
						core.log("action", "[testui] TEST_012: B dismissed reason=" .. tostring(ctx and ctx.reason))
						close_b()
					end,
				})
				if not b_handle then
					core.log("warning", "[testui] TEST_012 FAILED: modal B did not open")
					return
				end
				core.log("action", "[testui] TEST_012: auto remove A out-of-order (after requesting B mount)")
				close_a_out_of_order()
			end)
		end)
	end)
end

return { run = run }

