--[[
	Constructor-time `state` on core.ui.panel (no initial handle:set).

	Public API: core.ui.panel, core.ui.box, core.ui.text, core.ui.bind, handle:set, handle:close
]]

local ui = core.ui

local function content_tree()
	return ui.box({
		style = {
			position = "fixed",
			top = "80px",
			left = "80px",
			width = "280px",
			min_height = "120px",
			background_color = "#2a3a4a",
			border_radius = "8px",
			padding = "16px",
			box_sizing = "border-box",
		},
		children = {
			ui.text(ui.bind("count"), { id = "txt_count" }),
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
			core.log("action", "[testui] TEST_003 OPENING")

			local handle, err = ui.panel({
				player = name,
				id = "test_003_constructor_state",
				state = { count = "Count: 1" },
				content = content_tree(),
			})
			if not handle then
				core.log("warning", "[testui] TEST_003 FAILED: " .. tostring(err))
				return
			end

			core.after(1.0, function()
				core.log("action", "[testui] TEST_003 VERIFY (should still show \"Count: 1\")")
			end)

			core.after(1.6, function()
				core.log("action", "[testui] TEST_003 SET count=2")
				local ok, serr = handle:set({ count = 2 })
				if not ok then
					core.log("warning", "[testui] TEST_003 FAILED: " .. tostring(serr))
					return
				end
			end)

			core.after(2.8, function()
				core.log("action", "[testui] TEST_003 CLOSING")
				handle:close()
			end)
		end)
	end)
end

return { run = run }
