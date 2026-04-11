--[[
	Modal focus trap proof (pre-drag foundation):
	- Tab/Shift+Tab must cycle only inside the active modal
	- Background focusable controls must not receive focus while modal is open
	- Hidden/disabled controls must never receive focus
	- Typing must go only to the focused modal input
]]

local ui = core.ui

local function background_panel_tree()
	return ui.box({
		style = {
			position = "fixed",
			top = "40px",
			left = "40px",
			width = "420px",
			background_color = "#203040",
			border_radius = "10px",
			padding = "16px",
			box_sizing = "border-box",
		},
		children = {
			ui.column({
				gap = "sm",
				children = {
					ui.text("TEST_008 Background Panel (must NOT receive focus while modal is open)"),
					ui.input({ id = "bg_input", value = "bg", placeholder = "BG INPUT (must not focus during modal)" }),
					ui.button({
						text = "BG BUTTON (must not focus during modal)",
						on_press = function()
							core.log("warning", "[testui] TEST_008 ERROR: background button activated while modal open")
						end,
					}),
					ui.input({
						id = "bg_hidden_input",
						value = "hidden",
						placeholder = "HIDDEN",
						disabled = true,
						style = { display = "none" },
					}),
				},
			}),
		},
	})
end

local function modal_tree()
	return ui.box({
		style = {
			position = "fixed",
			top = "100px",
			left = "520px",
			width = "420px",
			background_color = "#1e2a36",
			border_radius = "10px",
			padding = "16px",
			box_sizing = "border-box",
		},
		children = {
			ui.column({
				gap = "md",
				children = {
					ui.text("TEST_008 Modal Focus Trap"),
					ui.text("Press Tab repeatedly; focus must stay inside this modal."),
					ui.text("Press Shift+Tab; focus must stay inside this modal (reverse)."),
					ui.text("Type into focused inputs; text must go only to the focused modal input."),
					ui.input({ id = "in_a", value = "", placeholder = "Input A" }),
					ui.input({ id = "in_b", value = "", placeholder = "Input B" }),
					ui.button({
						text = "Submit (focusable 3rd target)",
						on_press = function()
							core.log("action", "[testui] TEST_008 SUBMIT (should be reachable via Tab)")
						end,
					}),
					ui.input({
						id = "modal_hidden_input",
						value = "hidden",
						placeholder = "HIDDEN",
						disabled = true,
						style = { display = "none" },
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
			core.chat_send_player(name, "[testui] TEST_008: Background panel + modal opened.")
			core.chat_send_player(name, "[testui] TEST_008: Tab / Shift+Tab must cycle only inside modal (Input A -> Input B -> Submit -> wrap).")
			core.chat_send_player(name, "[testui] TEST_008: BG INPUT/BG BUTTON must not receive focus while modal is open.")

			local bg_handle, bg_err = ui.panel({
				player = name,
				id = "test_008_background",
				state = {},
				content = background_panel_tree(),
			})
			if not bg_handle then
				core.log("warning", "[testui] TEST_008 FAILED: background panel did not open: " .. tostring(bg_err))
				return
			end

			local modal_handle, modal_err = ui.modal({
				player = name,
				id = "test_008_modal_focus_trap",
				dismiss = "outside_or_escape",
				state = {},
				content = modal_tree(),
				on_dismiss = function(ctx)
					core.log("action", "[testui] TEST_008 CLOSED reason=" .. tostring(ctx and ctx.reason))
					if bg_handle and bg_handle.is_open and bg_handle:is_open() then
						bg_handle:close()
					end
					if modal_handle and modal_handle.is_open and modal_handle:is_open() then
						modal_handle:close()
					end
				end,
			})
			if not modal_handle then
				core.log("warning", "[testui] TEST_008 FAILED: modal did not open: " .. tostring(modal_err))
				return
			end
		end)
	end)
end

return { run = run }

