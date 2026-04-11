-- Minimal deterministic box-shadow repro for RmlUi advanced effects.
-- Goal: one modal, one box, one box-shadow.

local ui = core.ui

local function modal_tree(close_self)
	return ui.shadow_box({
		shadow = {
			dx = 0,
			dy = 10,
			softness = 2,
			color = "#000000",
			opacities = { 0.30, 0.18, 0.10 },
		},
		style = {
			position = "fixed",
			top = "140px",
			left = "160px",
			width = "440px",
			background_color = "#22304a",
			border = "2px solid #4f6aa0",
			border_radius = "12px",
			padding = "16px",
			box_sizing = "border-box",
			color = "#eaf2fb",
		},
		children = {
			ui.column({
				gap = "sm",
				children = {
					ui.text("TEST_018 box-shadow minimal repro"),
						ui.text("Expected: sane, semi-transparent shadow. Not: opaque white slab."),
					ui.button({
						text = "Close",
						on_press = function()
							core.log("action", "[testui] TEST_018: close button pressed")
							if close_self then
								close_self("button")
							end
						end,
					}),
				},
			}),
		},
	})
end

local registered = false
local running = {}

local function start_for_player(player)
	local name = player and player.get_player_name and player:get_player_name() or nil
	if not name then
		return
	end
	if running[name] then
		return
	end
	running[name] = true

	core.after(0, function()
		if not running[name] then
			return
		end
		local alive = true
		local handle = nil

		local function request_close(why)
			if not alive then
				return
			end
			alive = false
			running[name] = nil
			if handle then
				local ok, err = handle:close()
				if not ok then
					core.log("warning", "[testui] TEST_018: handle:close failed err=" .. tostring(err))
					core.chat_send_player(name, "[testui] TEST_018: close failed: " .. tostring(err))
				end
			end
			core.log("action", "[testui] TEST_018: close why=" .. tostring(why))
		end

		core.chat_send_player(name, "[testui] TEST_018: mounting minimal box-shadow modal")
		local h = ui.modal({
			player = name,
			id = "test_018_box_shadow_minimal",
			dismiss = "escape",
			state = {},
			content = modal_tree(request_close),
			on_dismiss = function(ctx)
				request_close("dismiss:" .. tostring(ctx and ctx.reason))
			end,
		})
		if not h then
			core.log("warning", "[testui] TEST_018: failed to mount modal (ui.modal returned nil)")
			running[name] = nil
			return
		end
		handle = h
	end)
end

local function run()
	if registered then
		return
	end
	registered = true

	core.register_on_joinplayer(start_for_player)
	if core.get_connected_players then
		for _, player in ipairs(core.get_connected_players()) do
			start_for_player(player)
		end
	end
end

return { run = run }

