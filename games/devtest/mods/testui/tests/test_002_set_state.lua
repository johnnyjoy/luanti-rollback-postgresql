--[[
	Layer 3/8 — Public handle:set after mount (state-backed text via core.ui.bind).

	Public API: core.ui.panel, core.ui.bind, handle:set, handle:close
]]

local ui = core.ui

local function content_tree()
	return {
		type = "div",
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
			{
				type = "text",
				id = "txt_count",
				props = { value = ui.bind("count") },
			},
		},
	}
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
			core.log("action", "[testui] TEST_002 OPENING")

			local handle, err = ui.panel({
				player = name,
				id = "test_002_set_state",
				content = content_tree(),
			})
			if not handle then
				core.log("warning", "[testui] TEST_002 FAILED: " .. tostring(err))
				return
			end

			-- Bound text starts empty at compile; establish visible initial state without remounting.
			local ok, serr = handle:set({ count = "Count: 1" })
			if not ok then
				core.log("warning", "[testui] TEST_002 FAILED: " .. tostring(serr))
				return
			end

			core.after(0.8, function()
				core.log("action", "[testui] TEST_002 SET count=2")
				ok, serr = handle:set({ count = 2 })
				if not ok then
					core.log("warning", "[testui] TEST_002 FAILED: " .. tostring(serr))
					return
				end
			end)

			core.after(2.2, function()
				core.log("action", "[testui] TEST_002 CLOSING")
				handle:close()
			end)
		end)
	end)
end

return { run = run }
