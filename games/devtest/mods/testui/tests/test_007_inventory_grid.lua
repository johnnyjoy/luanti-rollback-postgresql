--[[
	Inventory grid proof (game UI primitive):
	- fixed grid layout
	- slot click dispatch exactly once per click
	- server receives ctx.slot_index
	- state updates (items table) patch immediately via ctx.set
]]

local ui = core.ui

local function content_tree()
	return ui.box({
		style = {
			position = "fixed",
			top = "70px",
			left = "70px",
			width = "520px",
			background_color = "#1e2a36",
			border_radius = "10px",
			padding = "16px",
			box_sizing = "border-box",
		},
		children = {
			ui.column({
				gap = "md",
				children = {
					ui.text("Inventory Grid Test"),
					ui.text("Click a slot. Expect exactly one log line and the slot label becomes X."),
					ui.inventory_grid({
						id = "inv",
						cols = 8,
						rows = 4,
						slot_size = 48,
						gap = 4,
						items = ui.bind("items"),
						on_action = function(ctx)
							local n = tonumber(ctx and ctx.slot_index)
							if not n then
								core.log("warning", "[testui] TEST_007 CLICK slot=(missing)")
								return
							end
							core.log("action", "[testui] TEST_007 CLICK slot=" .. n)
							-- Patch only the one slot; engine expands items[n] into state key items.n.
							ctx.set({ items = { [n] = "X" } })
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
			core.chat_send_player(name, "[testui] TEST_007: Modal opens with an 8x4 grid.")
			core.chat_send_player(name, "[testui] TEST_007: Click any slot once: expect one log line and slot becomes X.")
			core.chat_send_player(name, "[testui] TEST_007: Empty slots are visible; A/B/C are prefilled.")

			local handle
			handle = ui.modal({
				player = name,
				id = "test_007_inventory_grid",
				dismiss = "outside_or_escape",
				state = {
					items = {
						[1] = "A",
						[5] = "B",
						[12] = "C",
					},
				},
				content = content_tree(),
				on_dismiss = function(ctx)
					core.log("action", "[testui] TEST_007 CLOSED reason=" .. tostring(ctx and ctx.reason))
					if handle and handle.is_open and handle:is_open() then
						handle:close()
					end
				end,
			})

			if not handle then
				core.log("warning", "[testui] TEST_007 FAILED: modal did not open")
				return
			end
		end)
	end)
end

return { run = run }

