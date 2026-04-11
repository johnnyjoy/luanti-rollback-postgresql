--[[
	Layer 1 — RmlUi rendering smoke: one box (div) + one text node.

	Public API: core.ui.panel({ player, id?, content }) → handle, nil / nil, err
]]

local function build_tree()
	return {
		type = "div",
		style = {
			position = "fixed",
			top = "80px",
			left = "80px",
			width = "240px",
			min_height = "120px",
			background_color = "#2a3a4a",
			border_radius = "8px",
			padding = "16px",
			box_sizing = "border-box",
		},
		children = {
			{
				type = "text",
				props = { value = "Hello UI" },
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
			core.log("action", "[testui] UI MOUNTING")
			local handle, err = core.ui.panel({
				player = name,
				id = "test_001_render",
				content = build_tree(),
			})
			if not handle then
				core.log("warning", "[testui] test_001 panel failed: " .. tostring(err))
				return
			end
			core.after(1.0, function()
				core.log("action", "[testui] UI UNMOUNTING")
				handle:close()
			end)
		end)
	end)
end

return { run = run }
