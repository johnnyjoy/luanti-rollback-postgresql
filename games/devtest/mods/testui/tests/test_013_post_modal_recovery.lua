--[[
	TEST_013: Post-modal recovery + repeated open/close stability.

	Scenarios covered (via one deterministic harness):
	- Scenario 1: single modal recovery (background panel must be usable after close)
	- Scenario 3: repeated cycles (>=10; default 25)

	Notes:
	- No reliance on external input automation. The client-side verification harness
	  (temporary diagnostics in UiManager) performs synthetic click/focus/text injection
	  and logs whether dispatch + focus/value are correct after each close.
]]

local ui = core.ui

local CYCLES = 25

local function bg_tree()
	return ui.box({
		style = {
			position = "fixed",
			top = "40px",
			left = "40px",
			width = "520px",
			background_color = "#203040",
			border_radius = "10px",
			padding = "16px",
			box_sizing = "border-box",
			border = "2px solid #3a556a",
		},
		children = {
			ui.column({
				gap = "sm",
				children = {
					ui.text("TEST_013 Background (must remain usable after every modal close)"),
					{
						type = "input",
						id = "bg_in",
						props = {
							value = "",
							placeholder = "BG input (should accept focus + typing after modal close)",
						},
					},
					ui.button({
						text = "BG button (dispatch must work after close)",
						on_press = function()
							core.log("action", "[testui] TEST_013: BG button pressed")
						end,
					}),
					ui.button({
						text = "Open modal (manual)",
						on_press = function()
							core.log("action", "[testui] TEST_013: manual open requested (ignored in auto harness)")
						end,
					}),
				},
			}),
		},
	})
end

local function modal_tree(cycle_i, close_modal)
	return ui.box({
		style = {
			position = "fixed",
			top = "120px",
			left = "600px",
			width = "460px",
			background_color = "#1e2a36",
			border_radius = "10px",
			padding = "16px",
			box_sizing = "border-box",
			border = "2px solid #4da3ff",
		},
		children = {
			ui.column({
				gap = "sm",
				children = {
					ui.text("TEST_013 Modal (cycle " .. tostring(cycle_i) .. ")"),
					{
						type = "input",
						id = "m_in",
						props = {
							value = "",
							placeholder = "Modal input (client harness will type here before close)",
							autofocus = true,
						},
					},
					ui.button({
						text = "Close modal",
						on_press = function()
							core.log("action", "[testui] TEST_013: close button pressed cycle=" .. tostring(cycle_i))
							close_modal()
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
			core.chat_send_player(name, "[testui] TEST_013: mounts BG + opens/closes a modal repeatedly.")
			core.chat_send_player(name, "[testui] TEST_013: proof is via client-side [RmlUi VERIFY] logs.")

			local bg_handle, bg_err = ui.panel({
				player = name,
				id = "test_013_bg",
				state = {},
				content = bg_tree(),
			})
			if not bg_handle then
				core.log("warning", "[testui] TEST_013 FAILED: background panel did not open: " .. tostring(bg_err))
				return
			end

			local cycle_i = 0

			local function open_modal()
				cycle_i = cycle_i + 1
				if cycle_i > CYCLES then
					core.log("action", "[testui] TEST_013: completed cycles=" .. tostring(CYCLES))
					return
				end

				local modal_handle
				local scheduled_next = false
				local function schedule_next()
					if scheduled_next then
						return
					end
					scheduled_next = true
					core.after(0.15, open_modal)
				end
				local function close_modal()
					if modal_handle and modal_handle.is_open and modal_handle:is_open() then
						local ok, err = modal_handle:close()
						if not ok then
							core.log("warning", "[testui] TEST_013: close modal failed cycle=" .. tostring(cycle_i) ..
								" err=" .. tostring(err))
							return
						end
						modal_handle = nil
						schedule_next()
					end
				end

				modal_handle = ui.modal({
					player = name,
					id = "test_013_modal",
					dismiss = "escape",
					state = {},
					content = modal_tree(cycle_i, close_modal),
					on_dismiss = function(ctx)
						core.log("action", "[testui] TEST_013: modal dismissed cycle=" .. tostring(cycle_i) ..
							" reason=" .. tostring(ctx and ctx.reason))
						close_modal()
						schedule_next()
					end,
				})
				if not modal_handle then
					core.log("warning", "[testui] TEST_013 FAILED: modal did not open cycle=" .. tostring(cycle_i))
					return
				end
			end

			core.after(0.2, open_modal)
		end)
	end)
end

return { run = run }

