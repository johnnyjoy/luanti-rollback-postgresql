--[[
	Button click → server handler → ctx.set → bound text update (server-driven RmlUi).
	Uses core.ui.modal so the document is shown with modal input ownership (pointer usable for UI).

	Public API: core.ui.modal, core.ui.box, core.ui.column, core.ui.row, core.ui.text,
	core.ui.bind, core.ui.button, handle:close
]]

local ui = core.ui

local function content_tree()
	return ui.box({
		style = {
			position = "fixed",
			top = "80px",
			left = "80px",
			width = "300px",
			min_height = "160px",
			background_color = "#2a3a4a",
			border_radius = "8px",
			padding = "16px",
			box_sizing = "border-box",
		},
		children = {
			ui.column({
				gap = "md",
				children = {
					ui.text("Button Test"),
					ui.row({
						gap = "sm",
						children = {
							ui.text("Clicks:"),
							ui.text(ui.bind("count"), { id = "txt_count" }),
						},
					}),
					ui.button({
						text = "Click Me",
						on_press = function(ctx)
							core.log("action", "[testui] TEST_005 CLICK")
							ctx.set({
								count = ctx.state.count + 1,
							})
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
			core.log("action", "[testui] TEST_005 OPENING")
			core.chat_send_player(
				name,
				"[testui] TEST_005: Modal UI — click the button; count goes up by 1 per click. After close, normal camera/game input returns."
			)

			local handle, err = ui.modal({
				player = name,
				id = "test_005_button_click",
				state = { count = 0 },
				content = content_tree(),
			})
			if not handle then
				core.log("warning", "[testui] TEST_005 FAILED: " .. tostring(err))
				return
			end

			core.after(5.0, function()
				core.log("action", "[testui] TEST_005 CLOSING")
				handle:close()
			end)
		end)
	end)
end

return { run = run }
