--[[
	TEST_011: Stacked modal focus restore (manual stacked modals; no background panel).

	Objective:
	- Modal A opens first (true modal)
	- Modal B opens second on top of A (true modal)
	- Click B button:
	  - B closes
	  - A remains mounted and regains focus / interaction ownership
	- Click A button:
	  - A closes
	- No modals remain

	This test intentionally does NOT mount any background non-modal surface.
]]

local ui = core.ui

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
					ui.text("Modal A"),
					ui.input({ id = "a_in", value = "", placeholder = "Modal A input (should receive focus after B closes)", autofocus = true }),
					ui.button({
						text = "A button",
						on_press = function()
							core.log("action", "[testui] TEST_011: A button pressed")
							if close_a then
								close_a()
							end
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
					ui.text("Modal B"),
					ui.input({ id = "b_in", value = "", placeholder = "Modal B input (must be active while B is open)", autofocus = true }),
					ui.button({
						text = "B button",
						on_press = function()
							core.log("action", "[testui] TEST_011: B button pressed")
							if close_b then
								close_b()
							end
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
			core.chat_send_player(name, "[testui] TEST_011: Opening Modal A then Modal B (stacked). No background panel is mounted.")
			core.chat_send_player(name, "[testui] TEST_011: Click B button: B must close, A must remain. Then click A button: A must close.")
			core.chat_send_player(name, "[testui] TEST_011: Dismiss is ESC-only to avoid outside-click interference with the proof.")

			local a_handle
			local function close_a()
				if a_handle and a_handle.is_open and a_handle:is_open() then
					local ok, err = a_handle:close()
					if not ok then
						core.log("warning", "[testui] TEST_011: close A failed err=" .. tostring(err))
					end
				end
			end
			a_handle = ui.modal({
				player = name,
				id = "test_011_modal_a",
				-- Keep A mounted through the whole proof. Outside click dismissal would interfere with
				-- the required "first click after B closes" evidence, so A is escape-only.
				dismiss = "escape",
				state = {},
				content = modal_a_tree(close_a),
				on_dismiss = function(ctx)
					core.log("action", "[testui] TEST_011: A dismissed reason=" .. tostring(ctx and ctx.reason))
					close_a()
				end,
			})
			if not a_handle then
				core.log("warning", "[testui] TEST_011 FAILED: modal A did not open")
				return
			end

			core.after(0.25, function()
				local b_handle
				local function close_b()
					if b_handle and b_handle.is_open and b_handle:is_open() then
						local ok, err = b_handle:close()
						if not ok then
							core.log("warning", "[testui] TEST_011: close B failed err=" .. tostring(err))
						end
					end
				end
				b_handle = ui.modal({
					player = name,
					id = "test_011_modal_b",
					dismiss = "escape",
					state = {},
					content = modal_b_tree(close_b),
					on_dismiss = function(ctx)
						core.log("action", "[testui] TEST_011: B dismissed reason=" .. tostring(ctx and ctx.reason))
						close_b()
					end,
				})
				if not b_handle then
					core.log("warning", "[testui] TEST_011 FAILED: modal B did not open")
					return
				end
			end)
		end)
	end)
end

return { run = run }

