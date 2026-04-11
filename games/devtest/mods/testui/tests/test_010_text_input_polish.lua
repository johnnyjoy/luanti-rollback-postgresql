--[[
	Text input polish proof (v1 completion):
	- Correct targeting: typing goes only to the focused input
	- Editing: backspace/delete/arrows/home/end behave predictably (as supported by RmlUi)
	- Focus: click-to-focus and Tab/Shift+Tab navigation are reliable
	- Modal integration: modal owns input; no stale/buffered input after close
]]

local ui = core.ui

local function background_panel_tree()
	return ui.box({
		style = {
			position = "fixed",
			top = "40px",
			left = "40px",
			width = "460px",
			background_color = "#203040",
			border_radius = "10px",
			padding = "16px",
			box_sizing = "border-box",
		},
		children = {
			ui.column({
				gap = "sm",
				children = {
					ui.text("TEST_010 Background Panel (typing must target the focused input)"),
					ui.text("Click the input and type. Then click elsewhere; typing must not go to an unfocused input."),
					ui.input({ id = "bg_in", value = "", placeholder = "Background input (panel)" }),
					ui.button({
						text = "BG BUTTON (should only activate on click / focus+Enter)",
						on_press = function()
							core.log("action", "[testui] TEST_010: background button pressed")
						end,
					}),
				},
			}),
		},
	})
end

local function modal_tree(close_modal)
	return ui.box({
		style = {
			position = "fixed",
			top = "120px",
			left = "540px",
			width = "520px",
			background_color = "#1e2a36",
			border_radius = "10px",
			padding = "16px",
			box_sizing = "border-box",
		},
		children = {
			ui.column({
				gap = "sm",
				children = {
					ui.text("TEST_010 Modal Text Input Polish (exclusive)"),
					ui.text("Click Input A, type: abc, then Backspace, then type: d  => expected: abd"),
					ui.text("Arrow keys should move the caret; Delete should remove the next character."),
					ui.text("Home/End: move caret to start/end (if supported)."),
					ui.text("Tab/Shift+Tab: focus must cycle within modal only."),
					ui.input({ id = "in_a", value = "", placeholder = "Input A" }),
					ui.input({ id = "in_b", value = "", placeholder = "Input B" }),
					ui.button({
						text = "Close modal",
						on_press = function()
							core.log("action", "[testui] TEST_010: close requested")
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
			core.chat_send_player(name, "[testui] TEST_010: Opened background panel + modal.")
			core.chat_send_player(name, "[testui] TEST_010: While modal is open, typing must affect only the focused modal input (no gameplay effects).")
			core.chat_send_player(name, "[testui] TEST_010: Close modal via ESC or outside click; then type again: no characters should 'appear late' in remaining UI.")

			local bg_handle, bg_err = ui.panel({
				player = name,
				id = "test_010_background",
				state = {},
				content = background_panel_tree(),
			})
			if not bg_handle then
				core.log("warning", "[testui] TEST_010 FAILED: background panel did not open: " .. tostring(bg_err))
				return
			end

			local modal_handle
			local function close_modal()
				if modal_handle and modal_handle.is_open and modal_handle:is_open() then
					modal_handle:close()
				end
			end

			modal_handle = select(1, ui.modal({
				player = name,
				id = "test_010_modal_text_input",
				dismiss = "outside_or_escape",
				state = {},
				content = modal_tree(close_modal),
				on_dismiss = function(ctx)
					core.log("action", "[testui] TEST_010 MODAL CLOSED reason=" .. tostring(ctx and ctx.reason))
					if modal_handle and modal_handle.is_open and modal_handle:is_open() then
						modal_handle:close()
					end
				end,
			}))
			if not modal_handle then
				core.log("warning", "[testui] TEST_010 FAILED: modal did not open")
				bg_handle:close()
				return
			end
		end)
	end)
end

return { run = run }

