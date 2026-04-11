--[[
	TEST_014: Post-modal recovery after stacked modal unwind (A under B).

	Scenario 2:
	- Open background surface (interactive)
	- Open Modal A
	- Open Modal B on top
	- Close B (A must remain usable)
	- Close A (background must be usable immediately)

	Notes:
	- Client-side verification harness (UiManager temporary diagnostics) performs synthetic
	  close clicks and background verification and logs [RmlUi VERIFY] evidence.
]]

local ui = core.ui

local function bg_tree()
	return ui.box({
		style = {
			position = "fixed",
			top = "40px",
			left = "40px",
			width = "520px",
			background_color = "#203040",
			border_radius = "10px",
			padding = "16px",
			box_sizing = "border-box",
			border = "2px solid #3a556a",
		},
		children = {
			ui.column({
				gap = "sm",
				children = {
					ui.text("TEST_014 Background (must be usable after A+B unwind)"),
					{
						type = "input",
						id = "bg_in",
						props = {
							value = "",
							placeholder = "BG input (focus + typing after stacked close)",
						},
					},
					ui.button({
						text = "BG button (dispatch must work after stacked close)",
						on_press = function()
							core.log("action", "[testui] TEST_014: BG button pressed")
						end,
					}),
				},
			}),
		},
	})
end

local function modal_a_tree(close_a)
	return ui.box({
		style = {
			position = "fixed",
			top = "140px",
			left = "160px",
			width = "440px",
			background_color = "#2a3a4a",
			border_radius = "10px",
			padding = "16px",
			box_sizing = "border-box",
			border = "2px solid #3a556a",
		},
		children = {
			ui.column({
				gap = "md",
				children = {
					ui.text("TEST_014 Modal A (lower)"),
					{
						type = "input",
						id = "a_in",
						props = { value = "", placeholder = "A input", autofocus = true },
					},
					ui.button({
						text = "Close A",
						on_press = function()
							core.log("action", "[testui] TEST_014: close A pressed")
							close_a()
						end,
					}),
				},
			}),
		},
	})
end

local function modal_b_tree(close_b)
	return ui.box({
		style = {
			position = "fixed",
			top = "190px",
			left = "230px",
			width = "440px",
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
					ui.text("TEST_014 Modal B (upper)"),
					{
						type = "input",
						id = "b_in",
						props = { value = "", placeholder = "B input", autofocus = true },
					},
					ui.button({
						text = "Close B",
						on_press = function()
							core.log("action", "[testui] TEST_014: close B pressed")
							close_b()
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
			core.chat_send_player(name, "[testui] TEST_014: mounts BG + stacked modals (A then B).")
			core.chat_send_player(name, "[testui] TEST_014: proof is via client-side [RmlUi VERIFY] logs.")

			local bg_handle, bg_err = ui.panel({
				player = name,
				id = "test_014_bg",
				state = {},
				content = bg_tree(),
			})
			if not bg_handle then
				core.log("warning", "[testui] TEST_014 FAILED: background panel did not open: " .. tostring(bg_err))
				return
			end

			local a_handle
			local b_handle

			local function close_a()
				if a_handle and a_handle.is_open and a_handle:is_open() then
					local ok, err = a_handle:close()
					if not ok then
						core.log("warning", "[testui] TEST_014: close A failed err=" .. tostring(err))
					end
				end
			end

			local function close_b()
				if b_handle and b_handle.is_open and b_handle:is_open() then
					local ok, err = b_handle:close()
					if not ok then
						core.log("warning", "[testui] TEST_014: close B failed err=" .. tostring(err))
					end
				end
			end

			a_handle = ui.modal({
				player = name,
				id = "test_014_modal_a",
				dismiss = "escape",
				state = {},
				content = modal_a_tree(close_a),
				on_dismiss = function(ctx)
					core.log("action", "[testui] TEST_014: A dismissed reason=" .. tostring(ctx and ctx.reason))
					close_a()
				end,
			})
			if not a_handle then
				core.log("warning", "[testui] TEST_014 FAILED: modal A did not open")
				return
			end

			core.after(0.25, function()
				b_handle = ui.modal({
					player = name,
					id = "test_014_modal_b",
					dismiss = "escape",
					state = {},
					content = modal_b_tree(close_b),
					on_dismiss = function(ctx)
						core.log("action", "[testui] TEST_014: B dismissed reason=" .. tostring(ctx and ctx.reason))
						close_b()
					end,
				})
				if not b_handle then
					core.log("warning", "[testui] TEST_014 FAILED: modal B did not open")
				end
			end)
		end)
	end)
end

return { run = run }

