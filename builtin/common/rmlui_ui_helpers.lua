-- Luanti
-- SPDX-License-Identifier: LGPL-2.1-or-later
--
-- Declarative RmlUi helpers for core.ui.* authoring.
-- This is a Lua-level composition layer (no renderer features required).

if not core.ui then
	return
end

local ui = core.ui

local function merge(a, b)
	local o = {}
	for k, v in pairs(a or {}) do
		o[k] = v
	end
	for k, v in pairs(b or {}) do
		o[k] = v
	end
	return o
end

local function is_position_key(k)
	return k == "position"
		or k == "top" or k == "left" or k == "right" or k == "bottom"
		or k == "transform"
		or k == "width" or k == "height"
		or k == "margin" or k == "margin_top" or k == "margin_right" or k == "margin_bottom" or k == "margin_left"
		or k == "z_index"
end

local function px(v)
	if v == nil then
		return nil
	end
	if type(v) == "number" then
		return tostring(math.floor(v + 0.5)) .. "px"
	end
	return tostring(v)
end

local function as_px_num(v, default)
	if v == nil then
		return default
	end
	if type(v) == "number" then
		return v
	end
	local s = tostring(v)
	s = s:gsub("%s+", "")
	s = s:gsub("px$", "")
	local n = tonumber(s)
	if n == nil then
		return default
	end
	return n
end

local function clamp(x, lo, hi)
	if x < lo then
		return lo
	end
	if x > hi then
		return hi
	end
	return x
end

local function concat_arrays(a, b)
	local out = {}
	for i = 1, #(a or {}) do
		out[#out + 1] = a[i]
	end
	for i = 1, #(b or {}) do
		out[#out + 1] = b[i]
	end
	return out
end

--[[
	ui.shadow_box({
	  style = { ... },        -- same style table you would pass to ui.box (may include position/size)
	  children = { ... },     -- same children you would pass to ui.box
	  shadow = {
	    dx = 0, dy = 10,      -- px numbers or "Npx" strings (defaults: 0, 8)
	    spread = 0,           -- px expansion (default: 0)
	    softness = 2,         -- 0..2 (controls 1..3 layers; default: 2)
	    color = "#00000080",  -- any RCSS color string (default: semi-transparent black)
	    radius = "12px",      -- overrides style.border_radius if set
	  }
	})
]]
function ui.shadow_box(spec)
	spec = spec or {}
	local id = spec.id
	local style = spec.style or {}
	local children = spec.children or {}
	local sh = spec.shadow or {}

	-- Split input styles so modders can keep authoring a single style table.
	local wrapper_style = {}
	local content_style = {}
	for k, v in pairs(style) do
		if k == "box_shadow" or k == "box-shadow" then
			-- native box-shadow is quarantined; ignore in helper
		elseif is_position_key(k) then
			wrapper_style[k] = v
		else
			content_style[k] = v
		end
	end

	if wrapper_style.position == nil then
		-- establishes absolute children positioning context
		wrapper_style.position = "relative"
	end

	local dx = as_px_num(sh.dx, 0)
	local dy = as_px_num(sh.dy, 8)
	local spread0 = as_px_num(sh.spread, 0)
	if spread0 < 0 then spread0 = 0 end

	local softness = tonumber(sh.softness) or 2
	softness = clamp(softness, 0, 2)
	local layers = softness + 1 -- 1..3

	local color = sh.color or "#000000"
	local radius = sh.radius or content_style.border_radius

	-- Ensure deterministic paint order even if future layout changes happen.
	content_style.position = content_style.position or "relative"
	content_style.z_index = content_style.z_index or "1"

	local shadow_nodes = {}
	local default_opacities = { "0.22", "0.12", "0.08" }
	for i = 1, layers do
		-- Deterministic softening ladder: each layer expands slightly more.
		local spd = clamp(spread0 + (i - 1) * 2, 0, 42)
		local op = default_opacities[i] or "0.10"
		if type(sh.opacities) == "table" and sh.opacities[i] ~= nil then
			op = tostring(sh.opacities[i])
		elseif sh.opacity ~= nil then
			op = tostring(sh.opacity)
		end
		table.insert(shadow_nodes, ui.box({
			style = merge({
				display = "block",
				position = "absolute",
				left = tostring(dx - spd) .. "px",
				top = tostring(dy - spd) .. "px",
				right = "-" .. tostring(spd) .. "px",
				bottom = "-" .. tostring(spd) .. "px",
				background_color = color,
				opacity = op,
				z_index = "0",
				pointer_events = "none",
				focus = "none",
			}, sh.layer_style),
			children = {},
		}))
		if radius ~= nil then
			shadow_nodes[#shadow_nodes].style.border_radius = radius
		end
	end

	return ui.box({
		id = id,
		style = merge({ display = "block" }, wrapper_style),
		children = concat_arrays(shadow_nodes, {
			ui.box({
				style = merge({ display = "block" }, content_style),
				children = children,
			}),
		}),
	})
end

ui.shadow_panel = ui.shadow_box

--[[
	ui.hud_box({
	  style = { ... },          -- style table for the HUD container
	  children = { ... },       -- children nodes
	  shadow = false|{ ... },   -- false/nil (default) for flat HUD; true or table to enable shadow_box
	})
]]
function ui.hud_box(spec)
	spec = spec or {}
	local id = spec.id
	local style = spec.style or {}
	local children = spec.children or {}

	local sh = spec.shadow
	if sh == true then
		sh = { dx = 0, dy = 6, softness = 1, color = "#000000", opacities = { 0.18, 0.10 } }
	end

	if sh then
		return ui.shadow_box({
			id = id,
			shadow = sh,
			style = style,
			children = children,
		})
	end

	-- Flat-by-default HUD container: no shadow, still provides a consistent box-sizing baseline.
	return ui.box({
		id = id,
		style = merge({
			display = "block",
			box_sizing = "border-box",
		}, style),
		children = children,
	})
end

ui.hud_panel = ui.hud_box

