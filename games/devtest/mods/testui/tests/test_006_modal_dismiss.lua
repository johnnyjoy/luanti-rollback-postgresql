--[[
	Modal dismiss policy proof:
	- ESC closes modal (no pause menu)
	- click outside modal content closes modal
	- click inside does not close

	Public API: core.ui.modal, core.ui.box, core.ui.column, core.ui.text, handle:close
]]

local ui = core.ui

local function content_tree()
	return ui.box({
		style = {
			position = "fixed",
			top = "80px",
			left = "80px",
			width = "360px",
			min_height = "180px",
			background_color = "#2a3a4a",
			border_radius = "8px",
			padding = "16px",
			box_sizing = "border-box",
		},
		children = {
			ui.column({
				gap = "md",
				children = {
					ui.text("Dismiss Test"),
					ui.text("Press ESC or click outside this box to close."),
					ui.text("Clicking inside should not close it."),
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
			core.log("action", "[testui] TEST_006 OPENING")
			core.chat_send_player(
				name,
				"[testui] TEST_006: Press ESC or click outside the box. The modal should close. Clicking inside should not close it."
			)

			local handle
			handle = ui.modal({
				player = name,
				id = "test_006_modal_dismiss",
				dismiss = "outside_or_escape",
				content = content_tree(),
				on_dismiss = function(ctx)
					core.log(
						"action",
						"[testui] TEST_006 CLOSED_BY_DISMISS reason=" .. tostring(ctx and ctx.reason)
					)
					if handle and handle.is_open and handle:is_open() then
						handle:close()
					end
				end,
			})

			if not handle then
				core.log("warning", "[testui] TEST_006 FAILED: modal did not open")
				return
			end
		end)
	end)
end

return { run = run }

