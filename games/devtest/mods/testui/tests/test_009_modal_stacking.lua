--[[
	Stacked modal proof:
	- Modal A opens, then Modal B opens on top of A
	- ESC / outside click dismiss only the topmost modal (B first)
	- After B closes, A remains open and usable
	- Only after A closes should gameplay/background resume
]]

local ui = core.ui

local function modal_a_tree()
	return ui.box({
		style = {
			position = "fixed",
			top = "120px",
			left = "120px",
			width = "420px",
			background_color = "#2a3a4a",
			border_radius = "10px",
			padding = "16px",
			box_sizing = "border-box",
		},
		children = {
			ui.column({
				gap = "md",
				children = {
					ui.text("First Modal (A)"),
					ui.input({ id = "a_input", value = "", placeholder = "A input (should become active after B closes)" }),
					ui.button({
						text = "A button",
						on_press = function()
							core.log("action", "[testui] TEST_009 A button pressed")
						end,
					}),
				},
			}),
		},
	})
end

local function modal_b_tree()
	return ui.box({
		style = {
			position = "fixed",
			top = "170px",
			left = "190px",
			width = "420px",
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
					ui.text("Second Modal (B) — topmost"),
					ui.input({ id = "b_input", value = "", placeholder = "B input (must be active while B is open)" }),
					ui.button({
						text = "B button",
						on_press = function()
							core.log("action", "[testui] TEST_009 B button pressed")
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
			core.chat_send_player(name, "[testui] TEST_009: Modal A opens, then Modal B opens on top.")
			core.chat_send_player(name, "[testui] TEST_009: While B is open: Tab/typing must stay in B; ESC/outside must dismiss only B.")
			core.chat_send_player(name, "[testui] TEST_009: After B closes: A must still be open and active; ESC/outside then dismisses A.")

			local a_handle
			a_handle = ui.modal({
				player = name,
				id = "test_009_modal_a",
				dismiss = "outside_or_escape",
				state = {},
				content = modal_a_tree(),
				on_dismiss = function(ctx)
					core.log("action", "[testui] TEST_009 A dismissed reason=" .. tostring(ctx and ctx.reason))
					if a_handle and a_handle.is_open and a_handle:is_open() then
						a_handle:close()
					end
				end,
			})
			if not a_handle then
				core.log("warning", "[testui] TEST_009 FAILED: modal A did not open")
				return
			end

			core.after(0.3, function()
				local b_handle
				b_handle = ui.modal({
					player = name,
					id = "test_009_modal_b",
					dismiss = "outside_or_escape",
					state = {},
					content = modal_b_tree(),
					on_dismiss = function(ctx)
						core.log("action", "[testui] TEST_009 B dismissed reason=" .. tostring(ctx and ctx.reason))
						if b_handle and b_handle.is_open and b_handle:is_open() then
							b_handle:close()
						end
					end,
				})
				if not b_handle then
					core.log("warning", "[testui] TEST_009 FAILED: modal B did not open")
				end
			end)
		end)
	end)
end

return { run = run }

