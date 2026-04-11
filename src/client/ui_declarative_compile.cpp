// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "ui_declarative_compile.h"

#include "client/ui_font.h"
#include "client/ui_manager.h"
#include "log.h"
#include "settings.h"

#include <cctype>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace {

static constexpr int MAX_TREE_DEPTH = 64;
static constexpr int MAX_NODE_COUNT = 1000;

/** Marker table from core.ui.bind(key); must match l_client.cpp registration. */
static const char LUA_BIND_MARK[] = "__luui_bind";
static bool g_warned_box_shadow_unsupported = false;

/** Lua 5.1 has no lua_absindex; keep indices stable for nested tables. */
static int absindex(lua_State *L, int idx)
{
	if (idx > 0 || idx <= LUA_REGISTRYINDEX)
		return idx;
	return lua_gettop(L) + idx + 1;
}

static std::string escape_for_rml_text(const std::string &s)
{
	std::string out;
	out.reserve(s.size());
	for (unsigned char uc : s) {
		char c = static_cast<char>(uc);
		switch (c) {
		case '&':
			out += "&amp;";
			break;
		case '<':
			out += "&lt;";
			break;
		case '>':
			out += "&gt;";
			break;
		case '"':
			out += "&quot;";
			break;
		default:
			out += c;
			break;
		}
	}
	return out;
}

static bool is_valid_element_id(const std::string &s)
{
	if (s.empty() || s.size() > 128)
		return false;
	for (unsigned char uc : s) {
		if (std::isalnum(uc) || uc == '_' || uc == '-')
			continue;
		return false;
	}
	return true;
}

/** User `id` values may not use the engine prefix (e.g. luaui_btn_0). */
static bool is_reserved_luaui_id_namespace(const std::string &s)
{
	return s.size() >= 6 && s.compare(0, 6, "luaui_") == 0;
}

static bool read_node_id(lua_State *L, int node_abs, std::string *out_id, std::string &error_out)
{
	lua_getfield(L, node_abs, "id");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		out_id->clear();
		return true;
	}
	if (lua_type(L, -1) != LUA_TSTRING) {
		lua_pop(L, 1);
		error_out = "UI node 'id' must be a string when set";
		return false;
	}
	*out_id = lua_tostring(L, -1);
	lua_pop(L, 1);
	if (out_id->empty()) {
		error_out = "UI node 'id' must be non-empty when set";
		return false;
	}
	if (!is_valid_element_id(*out_id)) {
		error_out = "UI node 'id' must use only letters, digits, underscore, hyphen";
		return false;
	}
	if (is_reserved_luaui_id_namespace(*out_id)) {
		error_out = "UI node 'id' must not use reserved prefix luaui_";
		return false;
	}
	return true;
}

static bool try_read_bind_key(lua_State *L, int value_abs, std::string *out_key)
{
	if (!lua_istable(L, value_abs))
		return false;
	lua_getfield(L, value_abs, LUA_BIND_MARK);
	if (lua_type(L, -1) != LUA_TSTRING) {
		lua_pop(L, 1);
		return false;
	}
	*out_key = lua_tostring(L, -1);
	lua_pop(L, 1);
	return !out_key->empty();
}

static int gap_px_from_props(lua_State *L, int props_rel)
{
	if (lua_isnil(L, props_rel) || !lua_istable(L, props_rel))
		return 8;
	lua_getfield(L, props_rel, "gap");
	if (lua_type(L, -1) == LUA_TNUMBER) {
		int px = static_cast<int>(lua_tonumber(L, -1));
		lua_pop(L, 1);
		if (px < 0)
			px = 0;
		if (px > 256)
			px = 256;
		return px;
	}
	if (!lua_isstring(L, -1)) {
		lua_pop(L, 1);
		return 8;
	}
	const char *g = lua_tostring(L, -1);
	int px = 8;
	if (!strcmp(g, "sm"))
		px = 8;
	else if (!strcmp(g, "md"))
		px = 12;
	else if (!strcmp(g, "lg"))
		px = 16;
	lua_pop(L, 1);
	return px;
}

static int gap_px_from_props_or(lua_State *L, int props_rel, int default_px)
{
	if (lua_isnil(L, props_rel) || !lua_istable(L, props_rel))
		return default_px;
	lua_getfield(L, props_rel, "gap");
	if (lua_type(L, -1) == LUA_TNUMBER) {
		int px = static_cast<int>(lua_tonumber(L, -1));
		lua_pop(L, 1);
		if (px < 0)
			px = 0;
		if (px > 256)
			px = 256;
		return px;
	}
	if (!lua_isstring(L, -1)) {
		lua_pop(L, 1);
		return default_px;
	}
	const char *g = lua_tostring(L, -1);
	int px = default_px;
	if (!strcmp(g, "sm"))
		px = 8;
	else if (!strcmp(g, "md"))
		px = 12;
	else if (!strcmp(g, "lg"))
		px = 16;
	lua_pop(L, 1);
	return px;
}

/** `props.wrap` → opt-in `flex-wrap: wrap` for `row` (default is nowrap). */
static bool row_wrap_from_props(lua_State *L, int props_rel)
{
	if (lua_isnil(L, props_rel) || !lua_istable(L, props_rel))
		return false;
	lua_getfield(L, props_rel, "wrap");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return false;
	}
	const bool w = lua_toboolean(L, -1) != 0;
	lua_pop(L, 1);
	return w;
}

static bool ensure_no_children(lua_State *L, int node_abs, const char *kind, std::string &error_out)
{
	lua_getfield(L, node_abs, "children");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return true;
	}
	if (!lua_istable(L, -1)) {
		error_out = std::string(kind) + " node: 'children' must be a table";
		lua_pop(L, 1);
		return false;
	}
	size_t n = lua_objlen(L, -1);
	lua_pop(L, 1);
	if (n != 0) {
		error_out = std::string(kind) + " node cannot have children";
		return false;
	}
	return true;
}

/** Build a CSS fragment from a Lua style table (keys use underscore → hyphen, e.g. background_color). */
static std::string style_table_to_css(lua_State *L, int table_idx)
{
	table_idx = absindex(L, table_idx);
	std::string out;
	lua_pushnil(L);
	while (lua_next(L, table_idx) != 0) {
		if (lua_type(L, -2) != LUA_TSTRING) {
			lua_pop(L, 1);
			continue;
		}
		std::string key = lua_tostring(L, -2);
		for (char &c : key) {
			if (c == '_')
				c = '-';
		}
		std::string val;
		if (lua_type(L, -1) == LUA_TSTRING) {
			val = lua_tostring(L, -1);
		} else if (lua_isnumber(L, -1)) {
			char buf[64];
			std::snprintf(buf, sizeof(buf), "%.17g", lua_tonumber(L, -1));
			val = buf;
		} else {
			lua_pop(L, 1);
			continue;
		}

		// RmlUi RCSS compatibility normalization:
		// - RmlUi does not support 'outline'/'outline-offset' properties. Translate 'outline'
		//   to a border, and ignore outline-offset.
		// - RmlUi 'border' shorthand does not accept CSS border-style keywords (e.g. 'solid').
		//   Normalize common CSS values '1px solid #fff' -> '1px #fff'.
		if (key == "outline-offset") {
			lua_pop(L, 1);
			continue;
		}

		auto tokenize_ws = [](const std::string &s) {
			std::vector<std::string> t;
			std::string cur;
			for (char ch : s) {
				if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
					if (!cur.empty()) {
						t.push_back(cur);
						cur.clear();
					}
				} else {
					cur.push_back(ch);
				}
			}
			if (!cur.empty())
				t.push_back(cur);
			return t;
		};

		auto is_border_style_keyword = [](const std::string &tok) {
			return tok == "none" || tok == "hidden" || tok == "dotted" || tok == "dashed" || tok == "solid" ||
					tok == "double" || tok == "groove" || tok == "ridge" || tok == "inset" || tok == "outset";
		};

		auto normalize_border_shorthand_value = [&](const std::string &in) -> std::string {
			std::vector<std::string> toks = tokenize_ws(in);
			if (toks.size() < 3)
				return in;
			// Most common CSS form is: <width> <style> <color>. RmlUi expects only width+color.
			std::vector<std::string> out_toks;
			out_toks.reserve(toks.size());
			for (const std::string &tok : toks) {
				if (is_border_style_keyword(tok))
					continue;
				out_toks.push_back(tok);
			}
			if (out_toks.empty())
				return in;
			std::string joined;
			for (size_t i = 0; i < out_toks.size(); ++i) {
				if (i)
					joined += " ";
				joined += out_toks[i];
			}
			return joined;
		};

		if (key == "outline") {
			// Expected common CSS forms:
			//   "2px solid #4da3ff"
			//   "1px solid rgba(...)"
			// We translate this into an equivalent border:
			//   border: <spread> <color>;
			std::vector<std::string> toks = tokenize_ws(val);
			if (toks.empty() || toks[0] == "none") {
				lua_pop(L, 1);
				continue;
			}
			std::string spread = toks[0];
			std::string color = toks.back();
			out += "border:" + spread + " " + color + ";";
			out += "box-sizing:border-box;";
			lua_pop(L, 1);
			continue;
		}

		// box-shadow is quarantined in this branch (advanced RenderInterface features not implemented).
		if (key == "box-shadow") {
			if (!g_warned_box_shadow_unsupported) {
				g_warned_box_shadow_unsupported = true;
				warningstream << "[ui_declarative] box-shadow is unsupported; "
						"use the Lua-level ui.shadow_box helper" << std::endl;
			}
			lua_pop(L, 1);
			continue;
		}

		if (key == "border" || key == "border-top" || key == "border-right" || key == "border-bottom" || key == "border-left") {
			val = normalize_border_shorthand_value(val);
		}

		out += key + ":" + val + ";";
		lua_pop(L, 1);
	}
	return out;
}

static void strip_css_property_inplace(std::string &css, const std::string &prop_name)
{
	size_t pos = 0;
	while (true) {
		pos = css.find(prop_name, pos);
		if (pos == std::string::npos)
			return;
		if (pos > 0 && css[pos - 1] != ';' && css[pos - 1] != ' ' && css[pos - 1] != '\t') {
			pos += prop_name.size();
			continue;
		}
		size_t colon = css.find(':', pos + prop_name.size());
		if (colon == std::string::npos)
			return;
		size_t end = css.find(';', colon);
		if (end == std::string::npos) {
			css.erase(pos);
			return;
		}
		css.erase(pos, end - pos + 1);
	}
}

static bool compile_node(lua_State *L, int node_abs, int depth, int &node_count, std::string &out,
		std::string &error_out, std::vector<int> *out_lua_button_refs,
		std::vector<UiDeclarativeBindingEntry> *out_bindings,
		std::unordered_set<std::string> *used_element_ids,
		std::unordered_set<std::string> *used_state_keys)
{
	node_abs = absindex(L, node_abs);
	if (++node_count > MAX_NODE_COUNT) {
		error_out = "UI tree exceeds maximum node count";
		return false;
	}
	if (depth > MAX_TREE_DEPTH) {
		error_out = "UI tree exceeds maximum depth";
		return false;
	}

	if (!lua_istable(L, node_abs)) {
		error_out = "UI node must be a table";
		return false;
	}

	lua_getfield(L, node_abs, "type");
	if (lua_type(L, -1) != LUA_TSTRING) {
		lua_pop(L, 1);
		error_out = "UI node missing string field 'type'";
		return false;
	}
	std::string type = lua_tostring(L, -1);
	lua_pop(L, 1);
	if (type.empty()) {
		error_out = "UI node 'type' must be a non-empty string";
		return false;
	}

	lua_getfield(L, node_abs, "props");
	if (!lua_isnil(L, -1) && !lua_istable(L, -1)) {
		lua_pop(L, 1);
		error_out = "UI node 'props' must be a table when present";
		return false;
	}
	int props_rel = lua_gettop(L); // props table or nil on stack

	if (type == "text") {
		if (!lua_istable(L, props_rel)) {
			lua_pop(L, 1);
			error_out = "text node requires a 'props' table";
			return false;
		}

		std::string node_id;
		if (!read_node_id(L, node_abs, &node_id, error_out)) {
			lua_pop(L, 1);
			return false;
		}

		lua_getfield(L, props_rel, "value");
		const int vt = lua_gettop(L);
		std::string value;

		if (lua_type(L, vt) == LUA_TSTRING) {
			value = lua_tostring(L, vt);
			lua_pop(L, 1);
			if (!node_id.empty()) {
				if (used_element_ids->count(node_id) != 0) {
					lua_pop(L, 1);
					error_out = "duplicate element id in UI tree: " + node_id;
					return false;
				}
				used_element_ids->insert(node_id);
			}
		} else if (lua_istable(L, vt)) {
			std::string bind_key;
			if (!try_read_bind_key(L, vt, &bind_key)) {
				lua_pop(L, 2);
				error_out = "text node props.value must be a string or core.ui.bind(...)";
				return false;
			}
			lua_pop(L, 1);
			if (node_id.empty()) {
				lua_pop(L, 1);
				error_out = "text node: core.ui.bind requires a non-empty node id";
				return false;
			}
			if (!out_bindings) {
				lua_pop(L, 1);
				error_out = "text node: core.ui.bind is not supported in this compile mode";
				return false;
			}
			if (used_state_keys->count(bind_key) != 0) {
				lua_pop(L, 1);
				error_out = "duplicate state key in UI tree: " + bind_key;
				return false;
			}
			used_state_keys->insert(bind_key);
			if (used_element_ids->count(node_id) != 0) {
				lua_pop(L, 1);
				error_out = "duplicate element id in UI tree: " + node_id;
				return false;
			}
			used_element_ids->insert(node_id);
			UiDeclarativeBindingEntry be;
			be.state_key = std::move(bind_key);
			be.element_id = node_id;
			be.kind = UiDeclarativeBindingEntry::Kind::TextValue;
			out_bindings->push_back(std::move(be));
			value.clear();
		} else {
			lua_pop(L, 2);
			error_out = "text node requires props.value (string or core.ui.bind)";
			return false;
		}

		if (!ensure_no_children(L, node_abs, "text", error_out)) {
			lua_pop(L, 1);
			return false;
		}
		lua_pop(L, 1); // props

		out += "<div ";
		if (!node_id.empty())
			out += "id=\"" + node_id + "\" ";
		out += "style=\"display:block;line-height:1.35;\">";
		out += escape_for_rml_text(value);
		out += "</div>";
		return true;
	}

	if (type == "label") {
		std::string text;
		lua_getfield(L, node_abs, "text");
		if (lua_type(L, -1) == LUA_TSTRING) {
			text = lua_tostring(L, -1);
			lua_pop(L, 1);
		} else {
			lua_pop(L, 1);
			if (!lua_istable(L, props_rel)) {
				lua_pop(L, 1);
				error_out = "label node requires string field 'text' or props.text";
				return false;
			}
			lua_getfield(L, props_rel, "text");
			if (lua_type(L, -1) != LUA_TSTRING) {
				lua_pop(L, 2);
				error_out = "label node requires props.text (string)";
				return false;
			}
			text = lua_tostring(L, -1);
			lua_pop(L, 1);
		}

		std::string style_str;
		lua_getfield(L, node_abs, "style");
		if (lua_istable(L, -1)) {
			style_str = style_table_to_css(L, lua_gettop(L));
			lua_pop(L, 1);
		} else if (lua_isstring(L, -1)) {
			style_str = lua_tostring(L, -1);
			strip_css_property_inplace(style_str, "box-shadow");
			lua_pop(L, 1);
		} else {
			lua_pop(L, 1);
		}

		lua_pop(L, 1); // props

		if (!ensure_no_children(L, node_abs, "label", error_out))
			return false;

		out += "<div style=\"display:block;" + style_str + "\">";
		out += escape_for_rml_text(text);
		out += "</div>";
		return true;
	}

	if (type == "button") {
		if (!lua_istable(L, props_rel)) {
			lua_pop(L, 1);
			error_out = "button node requires a 'props' table";
			return false;
		}
		lua_getfield(L, props_rel, "text");
		if (lua_type(L, -1) != LUA_TSTRING) {
			lua_pop(L, 2);
			error_out = "button node requires props.text (string)";
			return false;
		}
		std::string label = lua_tostring(L, -1);
		lua_pop(L, 1);

		int btn_slot = -1;
		static const char *handler_keys[] = {"on_press", "on_click"};
		for (const char *hk : handler_keys) {
			lua_getfield(L, props_rel, hk);
			if (lua_isnil(L, -1)) {
				lua_pop(L, 1);
				continue;
			}
			if (!out_lua_button_refs) {
				lua_pop(L, 2);
				error_out = "button node: props.on_press/on_click is not supported in this compile mode";
				return false;
			}
			if (!lua_isfunction(L, -1)) {
				lua_pop(L, 2);
				error_out = "button node: props.on_press/on_click must be a function when set";
				return false;
			}
			const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
			out_lua_button_refs->push_back(ref);
			btn_slot = static_cast<int>(out_lua_button_refs->size() - 1);
			break;
		}

		if (!ensure_no_children(L, node_abs, "button", error_out)) {
			lua_pop(L, 1);
			return false;
		}
		lua_pop(L, 1); // props

		if (btn_slot >= 0) {
			const std::string btn_id =
					std::string("luaui_btn_") + std::to_string(btn_slot);
			if (used_element_ids->count(btn_id) != 0) {
				error_out = "internal UI error: duplicate generated button id";
				return false;
			}
			used_element_ids->insert(btn_id);
		}

		// RmlUi Click targets FindFocusElement(hover); default div focus:none would send Click to body.
		out += "<div class=\"luaui_focusable luaui_clickable\" ";
		if (btn_slot >= 0) {
			out += "id=\"luaui_btn_";
			out += std::to_string(btn_slot);
			out += "\" ";
		}
		out += "style=\"display:inline-block;padding:8px 14px;margin-top:4px;"
				"background:rgba(90,100,120,255);color:#f0f0f5;font-weight:bold;"
				"border:1px rgba(0,0,0,0);box-sizing:border-box;"
				"border-radius:4px;text-align:center;cursor:pointer;";
		if (btn_slot >= 0)
			out += "focus:auto;tab-index:auto;";
		out += "\">";
		out += escape_for_rml_text(label);
		out += "</div>";
		return true;
	}

	if (type == "inventory_grid") {
		if (!lua_istable(L, props_rel)) {
			lua_pop(L, 1);
			error_out = "inventory_grid node requires a 'props' table";
			return false;
		}

		std::string node_id;
		if (!read_node_id(L, node_abs, &node_id, error_out)) {
			lua_pop(L, 1); // props
			return false;
		}
		if (!node_id.empty()) {
			if (used_element_ids->count(node_id) != 0) {
				lua_pop(L, 1);
				error_out = "duplicate element id in UI tree: " + node_id;
				return false;
			}
			used_element_ids->insert(node_id);
		}

		lua_getfield(L, props_rel, "cols");
		if (!lua_isnumber(L, -1)) {
			lua_pop(L, 2);
			error_out = "inventory_grid node requires props.cols (number)";
			return false;
		}
		int cols = static_cast<int>(lua_tonumber(L, -1));
		lua_pop(L, 1);
		lua_getfield(L, props_rel, "rows");
		if (!lua_isnumber(L, -1)) {
			lua_pop(L, 2);
			error_out = "inventory_grid node requires props.rows (number)";
			return false;
		}
		int rows = static_cast<int>(lua_tonumber(L, -1));
		lua_pop(L, 1);
		lua_getfield(L, props_rel, "slot_size");
		if (!lua_isnumber(L, -1)) {
			lua_pop(L, 2);
			error_out = "inventory_grid node requires props.slot_size (number)";
			return false;
		}
		int slot_px = static_cast<int>(lua_tonumber(L, -1));
		lua_pop(L, 1);

		if (cols < 1)
			cols = 1;
		if (cols > 32)
			cols = 32;
		if (rows < 1)
			rows = 1;
		if (rows > 32)
			rows = 32;
		if (slot_px < 8)
			slot_px = 8;
		if (slot_px > 128)
			slot_px = 128;

		const int gap_px = gap_px_from_props_or(L, props_rel, 4);

		std::string items_key;
		lua_getfield(L, props_rel, "items");
		if (!lua_istable(L, -1) || !try_read_bind_key(L, lua_gettop(L), &items_key)) {
			lua_pop(L, 2);
			error_out = "inventory_grid node requires props.items = core.ui.bind(\"...\")";
			return false;
		}
		lua_pop(L, 1);
		if (!out_bindings) {
			lua_pop(L, 1);
			error_out = "inventory_grid node: core.ui.bind is not supported in this compile mode";
			return false;
		}

		std::string style_str;
		lua_getfield(L, node_abs, "style");
		if (lua_istable(L, -1)) {
			style_str = style_table_to_css(L, lua_gettop(L));
			lua_pop(L, 1);
		} else if (lua_isstring(L, -1)) {
			style_str = lua_tostring(L, -1);
			lua_pop(L, 1);
		} else {
			lua_pop(L, 1);
		}

		if (!ensure_no_children(L, node_abs, "inventory_grid", error_out)) {
			lua_pop(L, 1); // props
			return false;
		}
		lua_pop(L, 1); // props

		const int total = cols * rows;
		const int width_px = cols * slot_px + (cols - 1) * gap_px;

		out += "<div ";
		if (!node_id.empty()) {
			out += "id=\"";
			out += node_id;
			out += "\" ";
		}
		out += "style=\"display:flex;flex-direction:row;flex-wrap:wrap;align-items:flex-start;";
		out += "gap:" + std::to_string(gap_px) + "px;";
		out += "width:" + std::to_string(width_px) + "px;";
		out += "user-select:none;";
		out += style_str;
		out += "\">";

		for (int i = 1; i <= total; ++i) {
			const std::string slot_id = std::string("luaui_slot_") + std::to_string(i);
			const std::string text_id = std::string("luaui_slot_text_") + std::to_string(i);
			const std::string state_key = items_key + "." + std::to_string(i);

			if (used_element_ids->count(slot_id) != 0 || used_element_ids->count(text_id) != 0) {
				error_out = "internal UI error: duplicate generated inventory_grid id";
				return false;
			}
			used_element_ids->insert(slot_id);
			used_element_ids->insert(text_id);

			if (used_state_keys->count(state_key) != 0) {
				error_out = "duplicate state key in UI tree: " + state_key;
				return false;
			}
			used_state_keys->insert(state_key);

			UiDeclarativeBindingEntry be;
			be.state_key = state_key;
			be.element_id = text_id;
			be.kind = UiDeclarativeBindingEntry::Kind::TextValue;
			out_bindings->push_back(std::move(be));

			out += "<div class=\"luaui_focusable luaui_clickable\" id=\"" + slot_id + "\" ";
			out += "style=\"display:flex;align-items:center;justify-content:center;";
			out += "width:" + std::to_string(slot_px) + "px;height:" + std::to_string(slot_px) + "px;";
			out += "box-sizing:border-box;border:1px rgba(120,140,165,180);";
			out += "background:rgba(30,35,45,220);border-radius:4px;";
			out += "cursor:pointer;focus:auto;tab-index:auto;\">";
			out += "<div id=\"" + text_id
					+ "\" style=\"display:block;line-height:1.0;font-weight:bold;color:#f0f4f8;\">"
					+ "</div>";
			out += "</div>";
		}
		out += "</div>";
		return true;
	}

	if (type == "input") {
		if (!lua_istable(L, props_rel)) {
			lua_pop(L, 1);
			error_out = "input node requires a 'props' table";
			return false;
		}

		std::string node_id;
		if (!read_node_id(L, node_abs, &node_id, error_out)) {
			lua_pop(L, 1); // props
			return false;
		}
		if (!node_id.empty()) {
			if (used_element_ids->count(node_id) != 0) {
				lua_pop(L, 1);
				error_out = "duplicate element id in UI tree: " + node_id;
				return false;
			}
			used_element_ids->insert(node_id);
		}

		std::string value;
		lua_getfield(L, props_rel, "value");
		if (lua_isstring(L, -1))
			value = lua_tostring(L, -1);
		lua_pop(L, 1);

		std::string placeholder;
		lua_getfield(L, props_rel, "placeholder");
		if (lua_isstring(L, -1))
			placeholder = lua_tostring(L, -1);
		lua_pop(L, 1);

		bool disabled = false;
		lua_getfield(L, props_rel, "disabled");
		if (lua_isboolean(L, -1))
			disabled = lua_toboolean(L, -1);
		lua_pop(L, 1);

		bool autofocus = false;
		lua_getfield(L, props_rel, "autofocus");
		if (lua_isboolean(L, -1))
			autofocus = lua_toboolean(L, -1);
		lua_pop(L, 1);

		std::string style_str;
		lua_getfield(L, node_abs, "style");
		if (lua_istable(L, -1)) {
			style_str = style_table_to_css(L, lua_gettop(L));
			lua_pop(L, 1);
		} else if (lua_isstring(L, -1)) {
			style_str = lua_tostring(L, -1);
			lua_pop(L, 1);
		} else {
			lua_pop(L, 1);
		}

		if (!ensure_no_children(L, node_abs, "input", error_out)) {
			lua_pop(L, 1); // props
			return false;
		}
		lua_pop(L, 1); // props

		out += "<input class=\"luaui_focusable\" type=\"text\" ";
		if (!node_id.empty()) {
			out += "id=\"";
			out += node_id;
			out += "\" ";
		}
		if (!placeholder.empty()) {
			out += "placeholder=\"";
			out += escape_for_rml_text(placeholder);
			out += "\" ";
		}
		out += "value=\"";
		out += escape_for_rml_text(value);
		out += "\" ";
		if (disabled)
			out += "disabled ";
		if (autofocus)
			out += "autofocus ";
		out += "style=\"display:block;padding:8px 10px;margin-top:4px;"
				"background:rgba(20,24,32,220);color:#f0f0f5;"
				"border:1px rgba(120,140,165,180);border-radius:4px;"
				"box-sizing:border-box;focus:auto;tab-index:auto;";
		out += style_str;
		out += "\"/>";
		return true;
	}

	if (type == "div") {
		std::string node_id;
		if (!read_node_id(L, node_abs, &node_id, error_out)) {
			lua_pop(L, 1); // props
			return false;
		}
		if (!node_id.empty()) {
			if (used_element_ids->count(node_id) != 0) {
				lua_pop(L, 1);
				error_out = "duplicate element id in UI tree: " + node_id;
				return false;
			}
			used_element_ids->insert(node_id);
		}

		std::string style_str;
		lua_getfield(L, node_abs, "style");
		if (lua_istable(L, -1)) {
			style_str = style_table_to_css(L, lua_gettop(L));
			lua_pop(L, 1);
		} else if (lua_isstring(L, -1)) {
			style_str = lua_tostring(L, -1);
			strip_css_property_inplace(style_str, "box-shadow");
			lua_pop(L, 1);
		} else {
			lua_pop(L, 1);
		}

		// Declarative instrument handles (v2):
		// - drag_handle = true
		// - resize_handle = "n|s|e|w|ne|nw|se|sw"
		bool drag_handle = false;
		std::string resize_handle;
		lua_getfield(L, node_abs, "drag_handle");
		if (lua_isboolean(L, -1))
			drag_handle = lua_toboolean(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, node_abs, "resize_handle");
		if (lua_type(L, -1) == LUA_TSTRING)
			resize_handle = lua_tostring(L, -1);
		lua_pop(L, 1);

		auto append_css = [&](const std::string &frag) {
			if (frag.empty())
				return;
			if (!style_str.empty() && style_str.back() != ';')
				style_str += ";";
			style_str += frag;
		};
		if (drag_handle)
			append_css("cursor:move;");
		if (!resize_handle.empty()) {
			if (resize_handle == "e" || resize_handle == "w")
				append_css("cursor:ew-resize;");
			else if (resize_handle == "n" || resize_handle == "s")
				append_css("cursor:ns-resize;");
			else if (resize_handle == "ne" || resize_handle == "sw")
				append_css("cursor:nesw-resize;");
			else if (resize_handle == "nw" || resize_handle == "se")
				append_css("cursor:nwse-resize;");
		}

		// Ensure handles have an id so hit-testing and events can report element_id deterministically.
		if ((drag_handle || !resize_handle.empty()) && node_id.empty()) {
			if (drag_handle)
				node_id = std::string("luaui_dh_") + std::to_string(node_count);
			else
				node_id = std::string("luaui_rh_") + resize_handle + "_" + std::to_string(node_count);
			if (used_element_ids->count(node_id) != 0) {
				lua_pop(L, 1);
				error_out = "internal UI error: duplicate generated handle id";
				return false;
			}
			used_element_ids->insert(node_id);
		}

		lua_pop(L, 1); // props

		out += "<div ";
		if (!node_id.empty()) {
			out += "id=\"";
			out += node_id;
			out += "\" ";
		}
		if (drag_handle)
			out += "data-luui-drag-handle=\"1\" ";
		if (!resize_handle.empty())
			out += "data-luui-resize-handle=\"" + resize_handle + "\" ";
		out += "style=\"display:block;" + style_str + "\">";

		lua_getfield(L, node_abs, "children");
		if (lua_isnil(L, -1)) {
			lua_pop(L, 1);
			out += "</div>";
			return true;
		}
		if (!lua_istable(L, -1)) {
			lua_pop(L, 1);
			error_out = "div node: 'children' must be a table";
			return false;
		}
		int ch_rel = lua_gettop(L);
		size_t n = lua_objlen(L, ch_rel);
		for (size_t i = 1; i <= n; i++) {
			lua_rawgeti(L, ch_rel, i);
			if (!lua_istable(L, -1)) {
				lua_pop(L, 2);
				error_out = "div children must be tables";
				return false;
			}
			int child_abs = lua_gettop(L);
			if (!compile_node(L, child_abs, depth + 1, node_count, out, error_out,
					    out_lua_button_refs, out_bindings, used_element_ids,
					    used_state_keys)) {
				lua_pop(L, 2);
				return false;
			}
			lua_pop(L, 1);
		}
		lua_pop(L, 1); // children table
		out += "</div>";
		return true;
	}

	if (type == "column" || type == "panel") {
		int gap_px = gap_px_from_props(L, props_rel);
		lua_pop(L, 1); // props

		out += "<div style=\"display:flex;flex-direction:column;align-items:stretch;";
		out += "gap:" + std::to_string(gap_px) + "px;\">";

		lua_getfield(L, node_abs, "children");
		if (lua_isnil(L, -1)) {
			lua_pop(L, 1);
			out += "</div>";
			return true;
		}
		if (!lua_istable(L, -1)) {
			lua_pop(L, 1);
			error_out = "column/panel node: 'children' must be a table";
			return false;
		}
		int ch_rel = lua_gettop(L);
		size_t n = lua_objlen(L, ch_rel);
		for (size_t i = 1; i <= n; i++) {
			lua_rawgeti(L, ch_rel, i);
			if (!lua_istable(L, -1)) {
				lua_pop(L, 2);
				error_out = "column children must be tables";
				return false;
			}
			int child_abs = lua_gettop(L);
			if (!compile_node(L, child_abs, depth + 1, node_count, out, error_out,
					    out_lua_button_refs, out_bindings, used_element_ids,
					    used_state_keys)) {
				lua_pop(L, 2);
				return false;
			}
			lua_pop(L, 1);
		}
		lua_pop(L, 1); // children table
		out += "</div>";
		return true;
	}

	if (type == "row") {
		int gap_px = gap_px_from_props(L, props_rel);
		const bool wrap = row_wrap_from_props(L, props_rel);
		lua_pop(L, 1); // props

		out += "<div style=\"display:flex;flex-direction:row;align-items:center;";
		out += wrap ? "flex-wrap:wrap;" : "flex-wrap:nowrap;";
		out += "gap:" + std::to_string(gap_px) + "px;\">";

		lua_getfield(L, node_abs, "children");
		if (lua_isnil(L, -1)) {
			lua_pop(L, 1);
			out += "</div>";
			return true;
		}
		if (!lua_istable(L, -1)) {
			lua_pop(L, 1);
			error_out = "row node: 'children' must be a table";
			return false;
		}
		int ch_rel = lua_gettop(L);
		size_t n = lua_objlen(L, ch_rel);
		for (size_t i = 1; i <= n; i++) {
			lua_rawgeti(L, ch_rel, i);
			if (!lua_istable(L, -1)) {
				lua_pop(L, 2);
				error_out = "row children must be tables";
				return false;
			}
			int child_abs = lua_gettop(L);
			if (!compile_node(L, child_abs, depth + 1, node_count, out, error_out,
					    out_lua_button_refs, out_bindings, used_element_ids,
					    used_state_keys)) {
				lua_pop(L, 2);
				return false;
			}
			lua_pop(L, 1);
		}
		lua_pop(L, 1); // children table
		out += "</div>";
		return true;
	}

	lua_pop(L, 1); // props
	error_out = "unsupported UI node type '" + type + "'";
	return false;
}

} // namespace

bool compile_declarative_ui_from_lua(lua_State *L, int table_index, std::string &rml_out,
		std::string &error_out, std::vector<int> *out_lua_button_refs,
		std::vector<UiDeclarativeBindingEntry> *out_bindings, bool *out_modal,
		UiDismissPolicy *out_dismiss)
{
	table_index = absindex(L, table_index);
	if (!lua_istable(L, table_index)) {
		error_out = "mount spec must be a table";
		return false;
	}
	if (out_modal)
		*out_modal = false;
	if (out_dismiss)
		*out_dismiss = UiDismissPolicy::None;
	if (out_lua_button_refs)
		out_lua_button_refs->clear();
	if (out_bindings)
		out_bindings->clear();

	struct PopBody {
		lua_State *L{};
		bool active = false;
		~PopBody()
		{
			if (active && L)
				lua_pop(L, 1);
		}
	} pop_body;
	pop_body.L = L;

	int compile_root = table_index;
	lua_getfield(L, table_index, "body");
	if (lua_istable(L, -1)) {
		pop_body.active = true;
		compile_root = lua_gettop(L);
		lua_getfield(L, table_index, "modal");
		if (lua_isboolean(L, -1)) {
			if (out_modal)
				*out_modal = lua_toboolean(L, -1);
		}
		lua_pop(L, 1);
		if (out_dismiss) {
			lua_getfield(L, table_index, "dismiss");
			if (lua_isnil(L, -1)) {
				// default
			} else if (lua_type(L, -1) == LUA_TSTRING) {
				const char *tok = lua_tostring(L, -1);
				if (!strcmp(tok, "escape")) {
					*out_dismiss = UiDismissPolicy::Escape;
				} else if (!strcmp(tok, "outside_or_escape")) {
					*out_dismiss = UiDismissPolicy::OutsideOrEscape;
				} else {
					lua_pop(L, 1);
					error_out = "invalid modal dismiss policy (expected \"escape\" or \"outside_or_escape\")";
					return false;
				}
			} else {
				lua_pop(L, 1);
				error_out = "modal dismiss policy must be a string when set";
				return false;
			}
			lua_pop(L, 1);
		}
	} else {
		lua_pop(L, 1);
	}

	lua_getfield(L, compile_root, "type");
	const bool has_type = lua_type(L, -1) == LUA_TSTRING;
	lua_pop(L, 1);

	lua_getfield(L, compile_root, "template");
	const bool has_template = lua_type(L, -1) == LUA_TSTRING;
	std::string templ;
	if (has_template)
		templ = lua_tostring(L, -1);
	lua_pop(L, 1);

	if (has_type && has_template) {
		error_out = "cannot use both 'type' and 'template' on the same mount";
		return false;
	}

	// Secondary: builtin test document (no declarative tree).
	if (!has_type && has_template) {
		if (templ == "builtin:test_overlay") {
			if (out_lua_button_refs)
				out_lua_button_refs->clear();
			if (out_bindings)
				out_bindings->clear();
			rml_out = UiManager::getBuiltinOverlayTestRmlDocument();
			return true;
		}
		if (templ == "builtin:test_layout_minimal") {
			if (out_lua_button_refs)
				out_lua_button_refs->clear();
			if (out_bindings)
				out_bindings->clear();
			rml_out = "<rml>\n<head></head>\n<body style=\"";
			rml_out += ui_rml_body_viewport_style();
			rml_out += "\">\n<div style=\"position:absolute;top:0;left:0;width:300px;height:200px;"
					"background-color:#ff0000;\"></div>\n</body>\n</rml>";
			return true;
		}
		error_out = "unknown template (only \"builtin:test_overlay\" or "
			    "\"builtin:test_layout_minimal\" is supported)";
		return false;
	}

	if (!has_type) {
		error_out = "declarative UI mount requires 'type' at the root "
			    "(or template=\"builtin:test_overlay\" for the engine test document)";
		return false;
	}

	std::unordered_set<std::string> used_element_ids;
	std::unordered_set<std::string> used_state_keys;

	int node_count = 0;
	std::string inner;
	if (!compile_node(L, compile_root, 0, node_count, inner, error_out, out_lua_button_refs,
			    out_bindings, &used_element_ids, &used_state_keys)) {
		if (out_lua_button_refs) {
			for (int r : *out_lua_button_refs)
				luaL_unref(L, LUA_REGISTRYINDEX, r);
			out_lua_button_refs->clear();
		}
		if (out_bindings)
			out_bindings->clear();
		return false;
	}

	const std::string fam_css = ui_font_declarative_default_family_css();
	// Full-viewport transparent shell; root content node supplies position/size/background.
	std::string panel = "display:block;position:absolute;left:0;top:0;width:100%;height:100%;"
			"margin:0;padding:0;box-sizing:border-box;overflow:visible;background-color:rgba(0,0,0,0);"
			"color:#e8e8f0;";
	if (!fam_css.empty()) {
		panel += "font-family:";
		panel += fam_css;
		panel += ";";
	}
	panel += "font-size:14px;";

	rml_out = "<rml>\n<head><style>\n"
			".luaui_focusable:focus{border-color:#4da3ff;}\n"
			".luaui_clickable:active{opacity:0.85;}\n"
			".luaui_clickable:hover{border-color:rgba(77,163,255,153);}\n"
			"</style></head>\n<body style=\"";
	rml_out += ui_rml_body_viewport_style();
	rml_out += "\">\n<div id=\"exp_panel\" style=\"";
	rml_out += panel;
	rml_out += "\"><div id=\"exp_pos\" style=\"display:block;\">";
	rml_out += inner;
	rml_out += "</div></div>\n</body>\n</rml>";
	return true;
}
