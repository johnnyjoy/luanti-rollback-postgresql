-- Luanti
-- SPDX-License-Identifier: LGPL-2.1-or-later
--
-- Reference Lua policy for HUD instrument resize (free + aspect-locked).
-- Loaded for INIT == "game" and INIT == "client" from builtin/init.lua.
-- Call: core.rmlui_compute_instrument_resize_patch(ctx, root_element_id, opts?)

local floor = math.floor

local function n(v, d)
	if type(v) == "number" then
		return v
	end
	if type(v) == "string" then
		local x = tonumber(v)
		if x then
			return x
		end
	end
	return d or 0
end

local function base_xy(anchor, w, h, vw, vh)
	if vw <= 0 or vh <= 0 then
		return 0, 0
	end
	if anchor == "center" then
		return floor((vw - w) / 2 + 0.5), floor((vh - h) / 2 + 0.5)
	elseif anchor == "top-left" then
		return 0, 0
	elseif anchor == "top-right" then
		return vw - w, 0
	elseif anchor == "bottom-left" then
		return 0, vh - h
	elseif anchor == "bottom-right" then
		return vw - w, vh - h
	elseif anchor == "top" then
		return floor((vw - w) / 2 + 0.5), 0
	elseif anchor == "bottom" then
		return floor((vw - w) / 2 + 0.5), vh - h
	elseif anchor == "left" then
		return 0, floor((vh - h) / 2 + 0.5)
	elseif anchor == "right" then
		return vw - w, floor((vh - h) / 2 + 0.5)
	end
	return floor((vw - w) / 2 + 0.5), floor((vh - h) / 2 + 0.5)
end

local function placement_from_abs(anchor, abs_x, abs_y, w, h, vw, vh, keep_in_view)
	local bx, by = base_xy(anchor, w, h, vw, vh)
	return {
		anchor = anchor,
		x = floor(abs_x - bx + 0.5),
		y = floor(abs_y - by + 0.5),
		keep_in_view = keep_in_view ~= false,
	}
end

local function horiz_mode(anchor)
	if anchor == "center" or anchor == "top" or anchor == "bottom" then
		return "center"
	end
	if type(anchor) == "string" and anchor:find("right", 1, true) then
		return "max"
	end
	return "min"
end

local function vert_mode(anchor)
	if anchor == "center" or anchor == "left" or anchor == "right" then
		return "center"
	end
	if type(anchor) == "string" and anchor:find("bottom", 1, true) then
		return "max"
	end
	return "min"
end

local function apply_x(x0, w0, w1, mode)
	if mode == "max" then
		return x0 + w0 - w1
	end
	if mode == "center" then
		return x0 + floor((w0 - w1) / 2 + 0.5)
	end
	return x0
end

local function apply_y(y0, h0, h1, mode)
	if mode == "max" then
		return y0 + h0 - h1
	end
	if mode == "center" then
		return y0 + floor((h0 - h1) / 2 + 0.5)
	end
	return y0
end

local function enforce_ratio(w, h, r)
	if r <= 0 or w <= 0 or h <= 0 then
		return w, h
	end
	local cur = w / h
	if cur > r then
		return floor(h * r + 0.5), h
	end
	if cur < r then
		return w, floor(w / r + 0.5)
	end
	return w, h
end

local function clamp_rect(x, y, w, h, vw, vh, min_w, min_h, max_w, max_h)
	w = math.max(min_w, math.min(max_w, w))
	h = math.max(min_h, math.min(max_h, h))
	if w > vw then
		w = vw
	end
	if h > vh then
		h = vh
	end
	x = math.max(0, math.min(vw - w, x))
	y = math.max(0, math.min(vh - h, y))
	return x, y, w, h
end

--- Keep the rect center fixed in absolute space (resize_start rect) for symmetric HUD resize.
local function center_fixed_tl(x0, y0, w0, h0, w1, h1)
	local cx = x0 + floor(w0 / 2 + 0.5)
	local cy = y0 + floor(h0 / 2 + 0.5)
	return cx - floor(w1 / 2 + 0.5), cy - floor(h1 / 2 + 0.5)
end

--- After clamp changes w/h, restore free-resize handle invariants (fixes 021-C drift when min/max clamp fires).
local function pin_free_resize_tl(rh, x0, y0, w0, h0, w1, h1)
	if rh == "se" then
		return x0, y0
	end
	if rh == "nw" then
		return x0 + w0 - w1, y0 + h0 - h1
	end
	if rh == "ne" then
		return x0, y0 + h0 - h1
	end
	if rh == "sw" then
		return x0 + w0 - w1, y0
	end
	if rh == "e" then
		return x0, y0
	end
	if rh == "w" then
		return x0 + w0 - w1, y0
	end
	if rh == "n" then
		return x0, y0 + h0 - h1
	end
	if rh == "s" then
		return x0, y0
	end
	return x0, y0
end

--- After clamp/ratio changes w,h, re-apply aspect edge/corner stabilization (not center — center uses dedicated path).
local function restabilize_keep_aspect(
	rh,
	is_corner,
	edge_h,
	edge_v,
	anchor,
	x0,
	y0,
	w0,
	h0,
	w1,
	h1
)
	local x1, y1
	if is_corner then
		if rh == "se" then
			x1, y1 = x0, y0
		elseif rh == "nw" then
			x1, y1 = x0 + w0 - w1, y0 + h0 - h1
		elseif rh == "ne" then
			x1, y1 = x0, y0 + h0 - h1
		elseif rh == "sw" then
			x1, y1 = x0 + w0 - w1, y0
		else
			x1, y1 = x0, y0
		end
	elseif edge_h and not edge_v then
		x1 = apply_x(x0, w0, w1, horiz_mode(anchor))
		y1 = apply_y(y0, h0, h1, vert_mode(anchor))
	elseif edge_v and not edge_h then
		x1 = apply_x(x0, w0, w1, horiz_mode(anchor))
		y1 = apply_y(y0, h0, h1, vert_mode(anchor))
	else
		x1 = x0
		y1 = y0
		if rh:find("e", 1, true) or rh:find("w", 1, true) then
			x1 = apply_x(x0, w0, w1, horiz_mode(anchor))
		end
		if rh:find("n", 1, true) or rh:find("s", 1, true) then
			y1 = apply_y(y0, h0, h1, vert_mode(anchor))
		end
	end
	return x1, y1
end

--- @param ctx table server instrument event (JSON → Lua)
--- @param root_element_id string element id receiving width/height (e.g. hud_a_root)
--- @param opts table optional { min_w, min_h, max_w, max_h }
function core.rmlui_compute_instrument_resize_patch(ctx, root_element_id, opts)
	if type(ctx) ~= "table" or type(root_element_id) ~= "string" or root_element_id == "" then
		return nil
	end
	if ctx.kind ~= "resize" then
		return nil
	end
	local ph = ctx.phase or ""
	if ph ~= "resize_move" and ph ~= "resize_end" then
		return nil
	end

	opts = opts or {}
	local min_w = n(opts.min_w, 120)
	local min_h = n(opts.min_h, 72)
	local max_w = n(opts.max_w, 4096)
	local max_h = n(opts.max_h, 4096)

	local ir = ctx.initial_rect or {}
	local x0 = n(ir.x, 0)
	local y0 = n(ir.y, 0)
	local w0 = n(ir.w, 0)
	local h0 = n(ir.h, 0)
	if w0 < 1 or h0 < 1 then
		return nil
	end

	local d = ctx.delta or {}
	local dx = n(d.x, 0)
	local dy = n(d.y, 0)

	local vp = ctx.viewport or {}
	local vw = n(vp.w, 0)
	local vh = n(vp.h, 0)
	if vw < 1 or vh < 1 then
		return nil
	end

	local placement = ctx.placement or {}
	local anchor = type(placement.anchor) == "string" and placement.anchor or "top-left"
	local kiv = placement.keep_in_view
	if kiv == nil then
		kiv = true
	end

	local inst = ctx.instrument or {}
	local keep_aspect = inst.keep_aspect == true
	local r_decl = inst.aspect_ratio
	local r_num = tonumber(r_decl)
	local r = (r_num and r_num > 0) and r_num or (w0 / h0)

	local rh = tostring(ctx.resize_handle or "")

	local w1, h1, x1, y1 = w0, h0, x0, y0

	-- Free resize: pointer deltas (opposite corner / edge semantics in absolute space).
	if rh:find("e", 1, true) then
		w1 = w0 + dx
	end
	if rh:find("w", 1, true) then
		w1 = w0 - dx
		x1 = x0 + dx
	end
	if rh:find("s", 1, true) then
		h1 = h0 + dy
	end
	if rh:find("n", 1, true) then
		h1 = h0 - dy
		y1 = y0 + dy
	end

	local edge_h = rh == "e" or rh == "w"
	local edge_v = rh == "n" or rh == "s"
	local is_corner = (rh == "nw" or rh == "ne" or rh == "sw" or rh == "se")

	if anchor == "center" then
		-- Symmetric resize around the initial rect center (fixes 021-B / center-anchored wandering).
		if keep_aspect then
			if is_corner then
				if rh == "se" then
					w1 = (x0 + w0 + dx) - x0
					h1 = (y0 + h0 + dy) - y0
				elseif rh == "nw" then
					w1 = (x0 + w0) - (x0 + dx)
					h1 = (y0 + h0) - (y0 + dy)
				elseif rh == "ne" then
					w1 = (x0 + w0 + dx) - x0
					h1 = (y0 + h0) - (y0 + dy)
				elseif rh == "sw" then
					w1 = (x0 + w0) - (x0 + dx)
					h1 = (y0 + h0 + dy) - y0
				end
				w1 = math.max(min_w, w1)
				h1 = math.max(min_h, h1)
				w1, h1 = enforce_ratio(w1, h1, r)
				x1, y1 = center_fixed_tl(x0, y0, w0, h0, w1, h1)
			elseif edge_h and not edge_v then
				w1 = math.max(min_w, w0 + (rh == "e" and dx or -dx))
				h1 = floor(w1 / r + 0.5)
				x1, y1 = center_fixed_tl(x0, y0, w0, h0, w1, h1)
			elseif edge_v and not edge_h then
				h1 = math.max(min_h, h0 + (rh == "s" and dy or -dy))
				w1 = floor(h1 * r + 0.5)
				x1, y1 = center_fixed_tl(x0, y0, w0, h0, w1, h1)
			else
				w1, h1 = enforce_ratio(w1, h1, r)
				x1, y1 = center_fixed_tl(x0, y0, w0, h0, w1, h1)
			end
		else
			-- Free resize, center anchor: w1/h1 from delta block; pin to initial center.
			x1, y1 = center_fixed_tl(x0, y0, w0, h0, w1, h1)
		end
	elseif keep_aspect then
		if is_corner then
			if rh == "se" then
				w1 = (x0 + w0 + dx) - x0
				h1 = (y0 + h0 + dy) - y0
				w1 = math.max(min_w, w1)
				h1 = math.max(min_h, h1)
				w1, h1 = enforce_ratio(w1, h1, r)
				x1, y1 = x0, y0
			elseif rh == "nw" then
				w1 = (x0 + w0) - (x0 + dx)
				h1 = (y0 + h0) - (y0 + dy)
				w1 = math.max(min_w, w1)
				h1 = math.max(min_h, h1)
				w1, h1 = enforce_ratio(w1, h1, r)
				x1 = x0 + w0 - w1
				y1 = y0 + h0 - h1
			elseif rh == "ne" then
				w1 = (x0 + w0 + dx) - x0
				h1 = (y0 + h0) - (y0 + dy)
				w1 = math.max(min_w, w1)
				h1 = math.max(min_h, h1)
				w1, h1 = enforce_ratio(w1, h1, r)
				x1 = x0
				y1 = y0 + h0 - h1
			elseif rh == "sw" then
				w1 = (x0 + w0) - (x0 + dx)
				h1 = (y0 + h0 + dy) - y0
				w1 = math.max(min_w, w1)
				h1 = math.max(min_h, h1)
				w1, h1 = enforce_ratio(w1, h1, r)
				x1 = x0 + w0 - w1
				y1 = y0
			end
		elseif edge_h and not edge_v then
			w1 = math.max(min_w, w0 + (rh == "e" and dx or -dx))
			h1 = floor(w1 / r + 0.5)
			x1 = apply_x(x0, w0, w1, horiz_mode(anchor))
			y1 = apply_y(y0, h0, h1, vert_mode(anchor))
		elseif edge_v and not edge_h then
			h1 = math.max(min_h, h0 + (rh == "s" and dy or -dy))
			w1 = floor(h1 * r + 0.5)
			x1 = apply_x(x0, w0, w1, horiz_mode(anchor))
			y1 = apply_y(y0, h0, h1, vert_mode(anchor))
		else
			w1, h1 = enforce_ratio(w1, h1, r)
			if rh:find("e", 1, true) or rh:find("w", 1, true) then
				x1 = apply_x(x0, w0, w1, horiz_mode(anchor))
			end
			if rh:find("n", 1, true) or rh:find("s", 1, true) then
				y1 = apply_y(y0, h0, h1, vert_mode(anchor))
			end
		end
	else
		-- Free resize (non-center anchors): first block encodes handle motion; do not overwrite
		-- with horiz_mode/vert_mode — that broke compound anchors (021-C).
	end

	-- Free resize: clamp size, re-pin handle geometry, then clamp position (021-C drift fix).
	-- Aspect paths: single clamp_rect preserves ratio invariants until the tail pass below.
	if not keep_aspect then
		w1 = math.max(min_w, math.min(max_w, w1))
		h1 = math.max(min_h, math.min(max_h, h1))
		if w1 > vw then
			w1 = vw
		end
		if h1 > vh then
			h1 = vh
		end
		if anchor ~= "center" then
			x1, y1 = pin_free_resize_tl(rh, x0, y0, w0, h0, w1, h1)
		end
		x1 = math.max(0, math.min(vw - w1, x1))
		y1 = math.max(0, math.min(vh - h1, y1))
	else
		x1, y1, w1, h1 = clamp_rect(x1, y1, w1, h1, vw, vh, min_w, min_h, max_w, max_h)
	end

	if keep_aspect and anchor ~= "center" then
		w1, h1 = enforce_ratio(w1, h1, r)
		x1, y1, w1, h1 = clamp_rect(x1, y1, w1, h1, vw, vh, min_w, min_h, max_w, max_h)
		x1, y1 = restabilize_keep_aspect(rh, is_corner, edge_h, edge_v, anchor, x0, y0, w0, h0, w1, h1)
		x1, y1, w1, h1 = clamp_rect(x1, y1, w1, h1, vw, vh, min_w, min_h, max_w, max_h)
	elseif keep_aspect and anchor == "center" then
		w1, h1 = enforce_ratio(w1, h1, r)
		x1, y1, w1, h1 = clamp_rect(x1, y1, w1, h1, vw, vh, min_w, min_h, max_w, max_h)
		x1, y1 = center_fixed_tl(x0, y0, w0, h0, w1, h1)
		x1, y1, w1, h1 = clamp_rect(x1, y1, w1, h1, vw, vh, min_w, min_h, max_w, max_h)
	end

	local pout = placement_from_abs(anchor, x1, y1, w1, h1, vw, vh, kiv)
	return {
		placement = pout,
		styles = {
			[root_element_id] = {
				width = tostring(w1) .. "px",
				height = tostring(h1) .. "px",
			},
		},
	}
end
