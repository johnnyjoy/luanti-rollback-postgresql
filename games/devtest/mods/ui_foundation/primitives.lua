--[[
	Minimal reusable primitives for declarative RmlUi UI trees.
	Each function returns a single table node (or list entries built by callers).

	Engine mapping:
	- panel → root div (position/size usually set per surface)
	- label → type "label"
	- line  → type "text" (static copy)
	- text_bound → type "text" + __luui_bind
	- button → type "button"
	- column → type "column"
	- row → div + flex row
	- spacer → empty div for gap
]]

local M = {}

--- Default colors / spacing (mods may read or override per panel).
M.theme = {
	panel_bg = "#1e3a5f",
	panel_fg = "#f0f4f8",
	muted = "#a8b8c8",
	accent = "#4a9eff",
	surface_alt = "#243a52",
	modal_bg = "#151a22",
	modal_border = "#3d4f66",
	warning_surface = "#6b2a2a",
	radius = "8px",
	pad = "12px",
}

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

--- Server/client bind marker (serializes over JSON).
function M.bind(key)
	return { __luui_bind = key }
end

--[[
	Root panel (div). Merge @p opts.style over soft defaults; set position/size on the surface.
	@param opts { id?, style? }
	@param children table of child nodes
]]
function M.panel(opts, children)
	opts = opts or {}
	local base = {
		position = "absolute",
		box_sizing = "border-box",
		padding = M.theme.pad,
		border_radius = M.theme.radius,
		background_color = M.theme.panel_bg,
		color = M.theme.panel_fg,
		overflow = "auto",
	}
	return {
		type = "div",
		id = opts.id,
		style = merge(base, opts.style or {}),
		children = children,
	}
end

local function is_position_key(k)
	return k == "position"
		or k == "top" or k == "left" or k == "right" or k == "bottom"
		or k == "transform"
		or k == "width" or k == "height"
		or k == "margin" or k == "margin_top" or k == "margin_right" or k == "margin_bottom" or k == "margin_left"
		or k == "z_index"
end

--- Shadowed panel helper (Lua composition; no native box-shadow).
-- opts.shadow supports: dx, dy, spread, softness (0..2), color, opacities.
function M.shadow_panel(opts, children)
	opts = opts or {}
	local sh = opts.shadow or {}

	-- Split style into wrapper (positioning) and inner (panel styling).
	local wrapper_style = {}
	local inner_style = {}
	for k, v in pairs(opts.style or {}) do
		if k == "box_shadow" or k == "box-shadow" then
			-- ignore
		elseif is_position_key(k) then
			wrapper_style[k] = v
		else
			inner_style[k] = v
		end
	end
	if wrapper_style.position == nil then
		wrapper_style.position = "absolute"
	end

	local base_panel = {
		position = "relative",
		z_index = "1",
		box_sizing = "border-box",
		padding = M.theme.pad,
		border_radius = M.theme.radius,
		background_color = M.theme.panel_bg,
		color = M.theme.panel_fg,
		overflow = "auto",
	}

	local dx = tonumber(sh.dx) or 0
	local dy = tonumber(sh.dy) or 8
	local spread0 = tonumber(sh.spread) or 0
	if spread0 < 0 then spread0 = 0 end
	local softness = tonumber(sh.softness) or 2
	if softness < 0 then softness = 0 end
	if softness > 2 then softness = 2 end
	local layers = softness + 1

	local color = sh.color or "#000000"
	local opacities = sh.opacities or { 0.22, 0.12, 0.08 }
	local radius = sh.radius or inner_style.border_radius or base_panel.border_radius

	local kids = {}
	for i = 1, layers do
		local spd = spread0 + (i - 1) * 4
		kids[#kids + 1] = {
			type = "div",
			style = merge({
				display = "block",
				position = "absolute",
				left = (dx - spd) .. "px",
				top = (dy - spd) .. "px",
				right = "-" .. spd .. "px",
				bottom = "-" .. spd .. "px",
				background_color = color,
				opacity = tostring(opacities[i] or opacities[#opacities] or 0.1),
				border_radius = radius,
				z_index = "0",
				focus = "none",
			}, sh.layer_style),
		}
	end

	kids[#kids + 1] = {
		type = "div",
		style = merge(base_panel, inner_style),
		children = children,
	}

	return {
		type = "div",
		id = opts.id,
		style = merge({ display = "block" }, wrapper_style),
		children = kids,
	}
end

function M.column(opts, children)
	opts = opts or {}
	return {
		type = "column",
		props = { gap = opts.gap or "sm" },
		children = children,
	}
end

--- Flex row; gap is CSS gap (string) or number of px via gap_px.
function M.row(opts, children)
	opts = opts or {}
	local g = opts.gap or opts.gap_px
	if g == nil then
		g = "8px"
	elseif type(g) == "number" then
		g = g .. "px"
	end
	return {
		type = "div",
		style = {
			display = "flex",
			flex_direction = "row",
			align_items = "center",
			flex_wrap = "wrap",
			gap = g,
		},
		children = children,
	}
end

--- Vertical or horizontal gap. Default vertical min-height 8px.
function M.spacer(opts)
	opts = opts or {}
	if opts.horizontal then
		return {
			type = "div",
			style = {
				display = "inline-block",
				width = opts.size or "8px",
				flex_shrink = "0",
			},
		}
	end
	return {
		type = "div",
		style = {
			display = "block",
			min_height = opts.size or "8px",
			flex_shrink = "0",
		},
	}
end

--- Label node (static text).
function M.label(text, opts)
	opts = opts or {}
	local st = merge({
		color = M.theme.panel_fg,
		font_size = opts.large and "18px" or "14px",
		font_weight = opts.bold and "bold" or "normal",
	}, opts.style or {})
	return { type = "label", text = text, style = st }
end

--- Static text line (type "text").
function M.line(text, opts)
	opts = opts or {}
	return { type = "text", id = opts.id, props = { value = text } }
end

--- Bound text (set_state targets).
function M.text_bound(id, key, opts)
	opts = opts or {}
	return { type = "text", id = id, props = { value = M.bind(key) } }
end

function M.button(text)
	return { type = "button", props = { text = text } }
end

return M
