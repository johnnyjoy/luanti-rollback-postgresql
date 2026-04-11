--[[
	TEST_015: Repeated top-modal churn while underlying modal remains.

	Goal reproduction state:
	1) Modal A exists (underlying, stays open)
	2) Modal B opens/closes repeatedly above A
	3) Churn ends; A is still visible
	4) Press ESC:
	   - Expected: dismiss A (escape)
	   - BUG: game pause menu opens while A remains visible

	Notes:
	- This test does not attempt to "prove" the bug automatically.
	  It is a harness for reproducing the state, while engine logs trace real ESC routing.
]]

local ui = core.ui

local CHURN_N = 400
local CHURN_PERIOD = 0.08
local CHURN_CLOSE_DELAY = 0.01

local function modal_a_tree()
	return ui.box({
		style = {
			position = "fixed",
			top = "140px",
			left = "160px",
			width = "520px",
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
					ui.text("TEST_015 Modal A (underlying) — leave open after churn"),
					{
						type = "input",
						id = "a_in",
						props = { value = "", placeholder = "A input (should stay modal owner after churn)", autofocus = true },
					},
					ui.button({
						text = "A button (log)",
						on_press = function()
							core.log("action", "[testui] TEST_015: A button pressed")
						end,
					}),
				},
			}),
		},
	})
end

local function modal_b_tree(i)
	return ui.box({
		style = {
			position = "fixed",
			top = "210px",
			left = "260px",
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
					ui.text("TEST_015 Modal B (churn) i=" .. tostring(i)),
					{
						type = "input",
						id = "b_in",
						props = { value = "", placeholder = "B input", autofocus = true },
					},
					ui.button({
						text = "B button (log)",
						on_press = function()
							core.log("action", "[testui] TEST_015: B button pressed i=" .. tostring(i))
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
			core.chat_send_player(name, "[testui] TEST_015: Modal A opens and stays. Modal B churns above it then stops.")
			core.chat_send_player(name, "[testui] TEST_015: After churn ends, A should still be visible. Press ESC now.")
			core.chat_send_player(name, "[testui] TEST_015: If pause menu opens while A remains visible, capture logs around '[RmlUi DBG] esc_dump'.")

			local a_handle = ui.modal({
				player = name,
				id = "test_015_modal_a",
				dismiss = "escape",
				state = {},
				content = modal_a_tree(),
				on_dismiss = function(ctx)
					core.log("action", "[testui] TEST_015: A dismissed reason=" .. tostring(ctx and ctx.reason))
				end,
			})
			if not a_handle then
				core.log("warning", "[testui] TEST_015 FAILED: modal A did not open")
				return
			end

			local churn_i = 0
			local function churn_step()
				churn_i = churn_i + 1
				if churn_i > CHURN_N then
					core.log("action", "[testui] TEST_015: churn complete; A should remain visible; press ESC now")
					return
				end
				local b_handle
				b_handle = ui.modal({
					player = name,
					id = "test_015_modal_b",
					dismiss = "escape",
					state = {},
					content = modal_b_tree(churn_i),
					on_dismiss = function(ctx)
						core.log("action", "[testui] TEST_015: B dismissed i=" .. tostring(churn_i) ..
							" reason=" .. tostring(ctx and ctx.reason))
					end,
				})
				if not b_handle then
					core.log("warning", "[testui] TEST_015 FAILED: modal B did not open i=" .. tostring(churn_i))
					core.after(CHURN_PERIOD, churn_step)
					return
				end
				-- Close B quickly via explicit handle close (not dismiss callback).
				core.after(CHURN_CLOSE_DELAY, function()
					if b_handle and b_handle.is_open and b_handle:is_open() then
						b_handle:close()
					end
				end)
				core.after(CHURN_PERIOD, churn_step)
			end

			core.after(0.4, churn_step)
		end)
	end)
end

return { run = run }

