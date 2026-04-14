--[[
	Instrument mode policy (Lua-owned):

	- C++ forwards pointer/drag lifecycle events while instrument mode is active.
	- Lua decides whether a press begins a drag (drag handle region vs controls).
	- Lua applies movement / snapping by calling core.ui.hud_placement_set.

	Resize policy (reference implementation for server handlers):
	- `core.rmlui_compute_instrument_resize_patch(ctx, root_element_id, opts?)` is defined in
	  `builtin/common/rmlui_instrument_resize.lua` (loaded from builtin/init.lua for game + client).
	- Server `instrument.on_event` should call it on resize_move / resize_end and return the patch.

	Default behavior here is intentionally minimal and opt-in:
	- A HUD surface must declare `instrument = { movable = true, ... }` on mount.
	- Server-driven HUD drags: the engine allows implicit drags from non-interactive chrome (see
	  `docs/ui-usage-guide.md`); this client shim still gates `pointer_down` on `instrument_drag` for
	  optional client-local experiments only.
]]

if not core.ui or not core.ui.instrument_set_handler then
	return
end
if not core.ui.hud_placement_set or not core.ui.hud_placement_get then
	return
end

local DRAG_HANDLE_ID = "instrument_drag"
local SNAP_THRESH_PX = 24

local drag = nil -- { sid=string, start_abs={x,y}, start_mouse={x,y} }

local function has_drag_handle(id_path)
	if type(id_path) ~= "table" then
		return false
	end
	for _, id in ipairs(id_path) do
		if id == DRAG_HANDLE_ID then
			return true
		end
	end
	return false
end

local function anchor_base(anchor, vw, vh, w, h)
	if anchor == "center" then
		return math.floor((vw - w) / 2 + 0.5), math.floor((vh - h) / 2 + 0.5)
	elseif anchor == "top-left" then
		return 0, 0
	elseif anchor == "top-right" then
		return vw - w, 0
	elseif anchor == "bottom-left" then
		return 0, vh - h
	elseif anchor == "bottom-right" then
		return vw - w, vh - h
	end
	return 0, 0
end

local function allowed_anchor(instrument, a)
	if type(instrument) ~= "table" then
		return true
	end
	local anchors = instrument.anchors
	if type(anchors) ~= "table" or #anchors == 0 then
		return true
	end
	for _, aa in ipairs(anchors) do
		if aa == a then
			return true
		end
	end
	return false
end

local function snap_anchor(ev)
	local viewport = ev.viewport or {}
	local rect = ev.rect or {}
	local vw = tonumber(viewport.w) or 0
	local vh = tonumber(viewport.h) or 0
	local w = tonumber(rect.w) or 0
	local h = tonumber(rect.h) or 0
	local ax = tonumber(rect.x) or 0
	local ay = tonumber(rect.y) or 0

	if vw <= 0 or vh <= 0 or w <= 0 or h <= 0 then
		return nil
	end

	local best, best_score = nil, nil
	for _, a in ipairs({ "center", "top-left", "top-right", "bottom-left", "bottom-right" }) do
		if allowed_anchor(ev.instrument, a) then
			local bx, by = anchor_base(a, vw, vh, w, h)
			local dx = ax - bx
			local dy = ay - by
			if math.abs(dx) <= SNAP_THRESH_PX and math.abs(dy) <= SNAP_THRESH_PX then
				local score = dx * dx + dy * dy
				if not best_score or score < best_score then
					best_score = score
					best = { anchor = a, x = dx, y = dy }
				end
			end
		end
	end
	return best
end

local function apply_abs_top_left(sid, abs_x, abs_y)
	core.ui.hud_placement_set({
		[sid] = {
			anchor = "top-left",
			x = abs_x,
			y = abs_y,
			keep_in_view = true,
		},
	})
end

core.ui.instrument_set_handler(function(ev)
	local phase = ev and ev.phase or ""

	if phase == "pointer_down" then
		-- Opt-in contract: no instrument meta => no dragging.
		if type(ev.instrument) ~= "table" or not ev.instrument.movable then
			return false
		end
		if not has_drag_handle(ev.id_path) then
			return false
		end
		return true
	end

	if phase == "drag_start" then
		drag = {
			sid = ev.surface_id,
			start_abs = { x = (ev.rect and ev.rect.x) or 0, y = (ev.rect and ev.rect.y) or 0 },
			start_mouse = { x = (ev.drag and ev.drag.start_x) or 0, y = (ev.drag and ev.drag.start_y) or 0 },
		}
		-- Normalize drag representation to absolute top-left.
		apply_abs_top_left(drag.sid, drag.start_abs.x, drag.start_abs.y)
		return
	end

	if phase == "drag_move" then
		if not drag or drag.sid ~= ev.surface_id then
			return
		end
		local dx = (ev.drag and ev.drag.dx) or 0
		local dy = (ev.drag and ev.drag.dy) or 0
		apply_abs_top_left(drag.sid, drag.start_abs.x + dx, drag.start_abs.y + dy)
		return
	end

	if phase == "drag_end" then
		if not drag or drag.sid ~= ev.surface_id then
			drag = nil
			return
		end

		-- Sticky snap is a policy decision in Lua.
		if type(ev.instrument) == "table" and ev.instrument.sticky then
			local s = snap_anchor(ev)
			if s then
				core.ui.hud_placement_set({
					[drag.sid] = {
						anchor = s.anchor,
						x = s.x,
						y = s.y,
						keep_in_view = true,
					},
				})
			end
		end

		drag = nil
		return
	end
end)

