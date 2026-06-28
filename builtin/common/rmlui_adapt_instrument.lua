-- Luanti
-- SPDX-License-Identifier: LGPL-2.1-or-later
--
-- Placement-aware adaptive HUD instruments (Lua-owned policy).
-- Loaded for INIT == "game" and INIT == "client" from builtin/init.lua.
-- Call: core.rmlui_adapt_instrument(ctx, root_element_id, config)

local default_orientation_map = {
	top = "horizontal",
	bottom = "horizontal",
	left = "vertical",
	right = "vertical",
	["top-left"] = "vertical",
	["top-right"] = "vertical",
	["bottom-left"] = "vertical",
	["bottom-right"] = "vertical",
	center = "horizontal",
}

local default_variant_map = {
	["top-left"] = "corner_nw",
	["top-right"] = "corner_ne",
	["bottom-left"] = "corner_sw",
	["bottom-right"] = "corner_se",
	top = "edge_top",
	bottom = "edge_bottom",
	left = "edge_left",
	right = "edge_right",
	center = "center",
}

local default_variant_styles = {
	corner_nw = {
		["border-left"] = "4px #f90",
		["border-top"] = "4px #f90",
	},
	corner_ne = {
		["border-right"] = "4px #f90",
		["border-top"] = "4px #f90",
	},
	corner_sw = {
		["border-left"] = "4px #f90",
		["border-bottom"] = "4px #f90",
	},
	corner_se = {
		["border-right"] = "4px #f90",
		["border-bottom"] = "4px #f90",
	},
	edge_top = { ["border-top"] = "3px #6cf" },
	edge_bottom = { ["border-bottom"] = "3px #6cf" },
	edge_left = { ["border-left"] = "3px #6cf" },
	edge_right = { ["border-right"] = "3px #6cf" },
	center = {
		["border-left"] = "2px #8a9",
		["border-right"] = "2px #8a9",
		["border-top"] = "2px #8a9",
		["border-bottom"] = "2px #8a9",
	},
}

-- RmlUi property names follow CSS (min-height). Lua tables often use min_height; normalize on merge.
local function normalize_rml_property_key(k)
	if type(k) ~= "string" then
		return k
	end
	if k:find("_", 1, true) then
		return (k:gsub("_", "-"))
	end
	return k
end

--- @param ctx table instrument event context (server; JSON → Lua)
--- @param root_element_id string primary element to style (e.g. hud root)
--- @param config table optional { adaptive = { ... }, elements = { bar = "id", ... } }
--- @return table|nil patch { styles = { [id] = { prop = val } } } or nil
function core.rmlui_adapt_instrument(ctx, root_element_id, config)
	if type(ctx) ~= "table" or type(root_element_id) ~= "string" or root_element_id == "" then
		return nil
	end
	config = config or {}
	local adaptive = config.adaptive
	if adaptive == nil and config.enabled ~= nil then
		adaptive = config
	end
	if type(adaptive) ~= "table" or adaptive.enabled ~= true then
		return nil
	end

	local pl = ctx.placement or {}
	local region = pl.region
	if type(region) ~= "string" or region == "" then
		return nil
	end

	local ori_block = adaptive.orientation or {}
	local mode = ori_block.mode == "fixed" and "fixed" or "auto"
	local ori_map = ori_block.map or default_orientation_map
	local require_edge_for_side = ori_block.require_edge_for_side == true

	local orientation = "horizontal"
	if mode == "fixed" then
		orientation = ori_block.fixed or "horizontal"
	else
		orientation = ori_map[region] or default_orientation_map[region] or "horizontal"
		if require_edge_for_side and (region == "left" or region == "right") and pl.kind ~= "edge" then
			orientation = ori_map.center or default_orientation_map.center or "horizontal"
		end
	end
	if orientation ~= "horizontal" and orientation ~= "vertical" then
		orientation = "horizontal"
	end

	local var_block = adaptive.variant or {}
	local var_map = var_block.map or default_variant_map
	local variant_region = pl.variant_region or region
	local variant = var_map[variant_region] or default_variant_map[variant_region] or "center"
	local prevent_overflow = adaptive.prevent_overflow ~= false

	local flex_dir = orientation == "horizontal" and "row" or "column"

	local geom = adaptive.geometry
	local g_for = orientation == "horizontal" and geom and geom.horizontal or geom and geom.vertical

	-- Style patches merge per-property; reset bar flex props when switching horizontal/vertical.
	local bar_layout_reset = {
		width = "100%",
		flex = "0",
		["min-width"] = "0",
		["min-height"] = "0",
		["flex-wrap"] = "nowrap",
		["max-width"] = "none",
		["align-content"] = "stretch",
	}

	local styles = {}
	local function merge_props(id, props)
		if type(id) ~= "string" or id == "" or type(props) ~= "table" then
			return
		end
		local cur = styles[id]
		if not cur then
			cur = {}
			styles[id] = cur
		end
		for k, v in pairs(props) do
			cur[normalize_rml_property_key(k)] = v
		end
	end

	local elems = config.elements
	local has_sub = type(elems) == "table" and next(elems) ~= nil
	if has_sub then
		local root_props = {
			display = "flex",
			["flex-direction"] = "column",
		}
		if prevent_overflow then
			root_props.overflow = "hidden"
		end
		merge_props(root_element_id, root_props)
		for _, eid in pairs(elems) do
			if type(eid) == "string" and eid ~= "" then
				merge_props(eid, {
					display = "flex",
					["flex-direction"] = flex_dir,
				})
			end
		end
	else
		local root_props = {
			display = "flex",
			["flex-direction"] = flex_dir,
		}
		if prevent_overflow then
			root_props.overflow = "hidden"
		end
		merge_props(root_element_id, root_props)
	end

	if type(g_for) == "table" then
		if type(g_for.root) == "table" then
			merge_props(root_element_id, g_for.root)
		end
		if type(elems) == "table" and type(g_for.elements) == "table" then
			for _, eid in pairs(elems) do
				if type(eid) == "string" and eid ~= "" then
					merge_props(eid, bar_layout_reset)
				end
			end
			for key, eid in pairs(elems) do
				local gp = g_for.elements[key]
				if type(eid) == "string" and eid ~= "" and type(gp) == "table" then
					merge_props(eid, gp)
				end
			end
		end
	end

	if var_block.enabled ~= false then
		local base = var_block.base_style
		if type(base) ~= "table" then
			base = {
				["border-left"] = "2px #6ac",
				["border-right"] = "2px #6ac",
				["border-top"] = "2px #6ac",
				["border-bottom"] = "2px #6ac",
			}
		end
		merge_props(root_element_id, base)

		local vstyles = var_block.styles or default_variant_styles
		local vs = vstyles[variant]
		if type(vs) == "table" then
			merge_props(root_element_id, vs)
		end
	end

	return {
		styles = styles,
		adaptation = {
			orientation = orientation,
			variant = variant,
			region = region,
			kind = pl.kind,
			variant_region = variant_region,
		},
	}
end
