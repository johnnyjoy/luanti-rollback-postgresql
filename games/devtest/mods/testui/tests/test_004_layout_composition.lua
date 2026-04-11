--[[
	Layout helpers: column + row composition (public authoring).

	Public API: core.ui.panel, core.ui.box, core.ui.column, core.ui.row, core.ui.text, handle:close
]]

local ui = core.ui

local function content_tree()
	return ui.box({
		style = {
			position = "fixed",
			top = "72px",
			left = "72px",
			width = "320px",
			padding = "16px",
			background_color = "#2a3a4a",
			border_radius = "8px",
			box_sizing = "border-box",
		},
		children = {
			ui.column({
				gap = 8,
				children = {
					ui.text("Layout Test"),
					ui.row({
						gap = 12,
						children = {
							ui.text("Left"),
							ui.text("Right"),
						},
					}),
					ui.text("Below"),
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
			core.log("action", "[testui] TEST_004 OPENING")
			core.chat_send_player(name,
				"[testui] TEST_004: expect title \"Layout Test\", \"Left\" and \"Right\" on one line, \"Below\" under that.")

			local handle, err = ui.panel({
				player = name,
				id = "test_004_layout_composition",
				content = content_tree(),
			})
			if not handle then
				core.log("warning", "[testui] TEST_004 FAILED: " .. tostring(err))
				return
			end

			core.after(2.0, function()
				core.log("action", "[testui] TEST_004 CLOSING")
				handle:close()
			end)
		end)
	end)
end

return { run = run }
