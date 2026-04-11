// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// JSON declarative UI compile path (server-driven UI; no Lua table / no client scripting).

#include "ui_declarative_compile.h"

#include "client/ui_font.h"
#include "client/ui_manager.h"
#include "log.h"
#include "settings.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <json/json.h>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

static constexpr int MAX_TREE_DEPTH = 64;
static constexpr int MAX_NODE_COUNT = 1000;
static const char JSON_BIND_MARK[] = "__luui_bind";
static bool g_warned_box_shadow_unsupported = false;

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

static bool is_reserved_luaui_id_namespace(const std::string &s)
{
	return s.size() >= 6 && s.compare(0, 6, "luaui_") == 0;
}

static bool read_node_id_json(const Json::Value &node, std::string *out_id, std::string &error_out)
{
	if (!node.isMember("id") || node["id"].isNull()) {
		out_id->clear();
		return true;
	}
	if (!node["id"].isString()) {
		error_out = "UI node 'id' must be a string when set";
		return false;
	}
	*out_id = node["id"].asString();
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

static bool try_read_bind_key_json(const Json::Value &v, std::string *out_key)
{
	if (!v.isObject())
		return false;
	Json::Value b = v[JSON_BIND_MARK];
	if (!b.isString())
		return false;
	*out_key = b.asString();
	return !out_key->empty();
}

static int gap_px_from_props_json(const Json::Value &props)
{
	if (props.isNull() || !props.isObject())
		return 8;
	if (!props.isMember("gap"))
		return 8;
	const Json::Value &g = props["gap"];
	if (g.isNumeric()) {
		int px = static_cast<int>(g.asDouble());
		if (px < 0)
			px = 0;
		if (px > 256)
			px = 256;
		return px;
	}
	if (!g.isString())
		return 8;
	const std::string s = g.asString();
	if (s == "sm")
		return 8;
	if (s == "md")
		return 12;
	if (s == "lg")
		return 16;
	return 8;
}

static int gap_px_from_props_json_or(const Json::Value *props_ptr, int default_px)
{
	if (!props_ptr || props_ptr->isNull() || !props_ptr->isObject())
		return default_px;
	const Json::Value &props = *props_ptr;
	if (!props.isMember("gap"))
		return default_px;
	const Json::Value &g = props["gap"];
	if (g.isNumeric()) {
		int px = static_cast<int>(g.asDouble());
		if (px < 0)
			px = 0;
		if (px > 256)
			px = 256;
		return px;
	}
	if (!g.isString())
		return default_px;
	const std::string s = g.asString();
	if (s == "sm")
		return 8;
	if (s == "md")
		return 12;
	if (s == "lg")
		return 16;
	return default_px;
}

static bool row_wrap_from_props_json(const Json::Value &props)
{
	if (props.isNull() || !props.isObject())
		return false;
	if (!props.isMember("wrap") || !props["wrap"].isBool())
		return false;
	return props["wrap"].asBool();
}

static bool ensure_no_children_json(const Json::Value &node, const char *kind, std::string &error_out)
{
	if (!node.isMember("children") || node["children"].isNull())
		return true;
	if (!node["children"].isArray()) {
		error_out = std::string(kind) + " node: 'children' must be an array";
		return false;
	}
	if (node["children"].size() != 0) {
		error_out = std::string(kind) + " node cannot have children";
		return false;
	}
	return true;
}

static std::string style_table_to_css_json(const Json::Value &obj)
{
	if (!obj.isObject())
		return "";
	std::string out;

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

	for (const auto &name : obj.getMemberNames()) {
		const Json::Value &v = obj[name];
		std::string key = name;
		for (char &c : key) {
			if (c == '_')
				c = '-';
		}
		std::string val;
		if (v.isString()) {
			val = v.asString();
		} else if (v.isNumeric()) {
			char buf[64];
			std::snprintf(buf, sizeof(buf), "%.17g", v.asDouble());
			val = buf;
		} else {
			continue;
		}

		// RmlUi RCSS compatibility normalization:
		// - No support for 'outline'/'outline-offset'. Translate outline to a border, ignore outline-offset.
		// - 'border' shorthand does not accept CSS border-style keywords (e.g. 'solid').
		// - box-shadow is quarantined in this branch (advanced RenderInterface features not implemented).
		if (key == "outline-offset")
			continue;
		if (key == "outline") {
			std::vector<std::string> toks = tokenize_ws(val);
			if (toks.empty() || toks.front() == "none")
				continue;
			const std::string spread = toks.front();
			const std::string color = toks.back();
			out += "border:" + spread + " " + color + ";";
			out += "box-sizing:border-box;";
			continue;
		}
		if (key == "box-shadow") {
			if (!g_warned_box_shadow_unsupported) {
				g_warned_box_shadow_unsupported = true;
				warningstream << "[ui_declarative] box-shadow is unsupported; "
						"use the Lua-level ui.shadow_box helper" << std::endl;
			}
			continue;
		}
		if (key == "border" || key == "border-top" || key == "border-right" || key == "border-bottom" || key == "border-left")
			val = normalize_border_shorthand_value(val);

		out += key + ":" + val + ";";
	}
	return out;
}

static void strip_css_property_inplace(std::string &css, const std::string &prop_name)
{
	// Very small helper for raw CSS strings: remove occurrences of `prop_name:...;`
	// Keeps other declarations intact. Case-sensitive (our compiler emits lowercase keys).
	size_t pos = 0;
	while (true) {
		pos = css.find(prop_name, pos);
		if (pos == std::string::npos)
			return;
		// Require it to look like a property name (start or after ';')
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

static bool compile_json_node(const Json::Value &node, int depth, int &node_count, std::string &out,
		std::string &error_out, std::vector<UiDeclarativeBindingEntry> *out_bindings,
		std::unordered_set<std::string> *used_element_ids,
		std::unordered_set<std::string> *used_state_keys, int *max_btn_slot_out)
{
	if (++node_count > MAX_NODE_COUNT) {
		error_out = "UI tree exceeds maximum node count";
		return false;
	}
	if (depth > MAX_TREE_DEPTH) {
		error_out = "UI tree exceeds maximum depth";
		return false;
	}
	if (!node.isObject()) {
		error_out = "UI node must be an object";
		return false;
	}
	if (!node.isMember("type") || !node["type"].isString()) {
		error_out = "UI node missing string field 'type'";
		return false;
	}
	const std::string type = node["type"].asString();
	if (type.empty()) {
		error_out = "UI node 'type' must be a non-empty string";
		return false;
	}

	const Json::Value *props_ptr = nullptr;
	if (node.isMember("props") && !node["props"].isNull()) {
		if (!node["props"].isObject()) {
			error_out = "UI node 'props' must be an object when present";
			return false;
		}
		props_ptr = &node["props"];
	}

	if (type == "text") {
		if (!props_ptr) {
			error_out = "text node requires a 'props' object";
			return false;
		}
		const Json::Value &props = *props_ptr;
		std::string node_id;
		if (!read_node_id_json(node, &node_id, error_out))
			return false;

		if (!props.isMember("value")) {
			error_out = "text node requires props.value";
			return false;
		}
		const Json::Value &pv = props["value"];
		std::string value;

		if (pv.isString()) {
			value = pv.asString();
			if (!node_id.empty()) {
				if (used_element_ids->count(node_id) != 0) {
					error_out = "duplicate element id in UI tree: " + node_id;
					return false;
				}
				used_element_ids->insert(node_id);
			}
		} else if (pv.isObject()) {
			std::string bind_key;
			if (!try_read_bind_key_json(pv, &bind_key)) {
				error_out = "text node props.value must be a string or {\"__luui_bind\":\"key\"}";
				return false;
			}
			if (node_id.empty()) {
				error_out = "text node: bind requires a non-empty node id";
				return false;
			}
			if (!out_bindings) {
				error_out = "text node: bind is not supported in this compile mode";
				return false;
			}
			if (used_state_keys->count(bind_key) != 0) {
				error_out = "duplicate state key in UI tree: " + bind_key;
				return false;
			}
			used_state_keys->insert(bind_key);
			if (used_element_ids->count(node_id) != 0) {
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
			error_out = "text node requires props.value (string or bind object)";
			return false;
		}

		if (!ensure_no_children_json(node, "text", error_out))
			return false;

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
		if (node.isMember("text") && node["text"].isString()) {
			text = node["text"].asString();
		} else if (props_ptr && props_ptr->isMember("text") && (*props_ptr)["text"].isString()) {
			text = (*props_ptr)["text"].asString();
		} else {
			error_out = "label node requires string field 'text' or props.text";
			return false;
		}

		std::string style_str;
		if (node.isMember("style")) {
			const Json::Value &st = node["style"];
			if (st.isObject())
				style_str = style_table_to_css_json(st);
			else if (st.isString())
				style_str = st.asString();
		}

		if (!ensure_no_children_json(node, "label", error_out))
			return false;

		out += "<div style=\"display:block;" + style_str + "\">";
		out += escape_for_rml_text(text);
		out += "</div>";
		return true;
	}

	if (type == "inventory_grid") {
		if (!props_ptr) {
			error_out = "inventory_grid node requires a 'props' object";
			return false;
		}
		const Json::Value &props = *props_ptr;

		std::string node_id;
		if (!read_node_id_json(node, &node_id, error_out))
			return false;
		if (!node_id.empty()) {
			if (used_element_ids->count(node_id) != 0) {
				error_out = "duplicate element id in UI tree: " + node_id;
				return false;
			}
			used_element_ids->insert(node_id);
		}

		if (!props.isMember("cols") || !props["cols"].isNumeric()) {
			error_out = "inventory_grid node requires props.cols (number)";
			return false;
		}
		if (!props.isMember("rows") || !props["rows"].isNumeric()) {
			error_out = "inventory_grid node requires props.rows (number)";
			return false;
		}
		if (!props.isMember("slot_size") || !props["slot_size"].isNumeric()) {
			error_out = "inventory_grid node requires props.slot_size (number)";
			return false;
		}
		int cols = static_cast<int>(props["cols"].asDouble());
		int rows = static_cast<int>(props["rows"].asDouble());
		int slot_px = static_cast<int>(props["slot_size"].asDouble());

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

		const int gap_px = gap_px_from_props_json_or(props_ptr, 4);

		if (!props.isMember("items")) {
			error_out = "inventory_grid node requires props.items";
			return false;
		}
		std::string items_key;
		if (!try_read_bind_key_json(props["items"], &items_key)) {
			error_out = "inventory_grid node requires props.items = { __luui_bind = \"...\" }";
			return false;
		}
		if (items_key.empty()) {
			error_out = "inventory_grid node requires non-empty bind key";
			return false;
		}
		if (!out_bindings) {
			error_out = "inventory_grid node: bind is not supported in this compile mode";
			return false;
		}

		std::string style_str;
		if (node.isMember("style") && !node["style"].isNull()) {
			if (node["style"].isObject())
				style_str = style_table_to_css_json(node["style"]);
			else if (node["style"].isString())
				style_str = node["style"].asString();
		}

		if (!ensure_no_children_json(node, "inventory_grid", error_out))
			return false;

		const int total = cols * rows;
		const int width_px = cols * slot_px + (cols - 1) * gap_px;

		out += "<div ";
		if (!node_id.empty())
			out += "id=\"" + node_id + "\" ";
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
		if (!props_ptr) {
			error_out = "input node requires a 'props' object";
			return false;
		}
		const Json::Value &props = *props_ptr;

		std::string node_id;
		if (!read_node_id_json(node, &node_id, error_out))
			return false;
		if (!node_id.empty()) {
			if (used_element_ids->count(node_id) != 0) {
				error_out = "duplicate element id in UI tree: " + node_id;
				return false;
			}
			used_element_ids->insert(node_id);
		}

		std::string value;
		if (props.isMember("value") && props["value"].isString())
			value = props["value"].asString();

		std::string placeholder;
		if (props.isMember("placeholder") && props["placeholder"].isString())
			placeholder = props["placeholder"].asString();

		bool disabled = false;
		if (props.isMember("disabled") && props["disabled"].isBool())
			disabled = props["disabled"].asBool();

		bool autofocus = false;
		if (props.isMember("autofocus") && props["autofocus"].isBool())
			autofocus = props["autofocus"].asBool();

		std::string style_str;
		if (node.isMember("style")) {
			const Json::Value &st = node["style"];
			if (st.isObject())
				style_str = style_table_to_css_json(st);
			else if (st.isString())
				style_str = st.asString();
		}

		if (!ensure_no_children_json(node, "input", error_out))
			return false;

		out += "<input class=\"luaui_focusable\" type=\"text\" ";
		if (!node_id.empty())
			out += "id=\"" + node_id + "\" ";
		if (!placeholder.empty())
			out += "placeholder=\"" + escape_for_rml_text(placeholder) + "\" ";
		out += "value=\"" + escape_for_rml_text(value) + "\" ";
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

	if (type == "button") {
		if (!props_ptr) {
			error_out = "button node requires a 'props' object";
			return false;
		}
		const Json::Value &props = *props_ptr;
		if (!props.isMember("text") || !props["text"].isString()) {
			error_out = "button node requires props.text (string)";
			return false;
		}
		const std::string label = props["text"].asString();
		int btn_slot = -1;
		if (props.isMember("__luui_btn")) {
			const Json::Value &bs = props["__luui_btn"];
			if (!bs.isInt() && !bs.isUInt()) {
				error_out = "button node: props.__luui_btn must be an integer when set";
				return false;
			}
			btn_slot = bs.asInt();
			if (btn_slot < 0 || btn_slot > 1024) {
				error_out = "button node: props.__luui_btn out of range";
				return false;
			}
			if (max_btn_slot_out)
				*max_btn_slot_out = std::max(*max_btn_slot_out, btn_slot + 1);
		}

		if (!ensure_no_children_json(node, "button", error_out))
			return false;

		// RmlUi dispatches EventId::Click only to the element returned by FindFocusElement(hover)
		// (see Context::ProcessMouseButtonUp): plain divs use focus:none, so focus becomes body and
		// Click never targets this node. focus:auto makes the button the click target.
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

	if (type == "div") {
		std::string node_id;
		if (!read_node_id_json(node, &node_id, error_out))
			return false;
		if (!node_id.empty()) {
			if (used_element_ids->count(node_id) != 0) {
				error_out = "duplicate element id in UI tree: " + node_id;
				return false;
			}
			used_element_ids->insert(node_id);
		}

		std::string style_str;
		if (node.isMember("style")) {
			const Json::Value &st = node["style"];
			if (st.isObject()) {
				style_str = style_table_to_css_json(st);
			} else if (st.isString()) {
				style_str = st.asString();
				// Never emit native box-shadow from raw CSS strings.
				// We cannot safely rewrite structure here, so strip box-shadow declarations if present.
				strip_css_property_inplace(style_str, "box-shadow");
			}
		}

		// Declarative instrument handles (v2):
		// - drag_handle = true
		// - resize_handle = "n|s|e|w|ne|nw|se|sw"
		const bool drag_handle = node.isMember("drag_handle") && node["drag_handle"].isBool()
				? node["drag_handle"].asBool()
				: false;
		std::string resize_handle;
		if (node.isMember("resize_handle") && node["resize_handle"].isString())
			resize_handle = node["resize_handle"].asString();

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

		if ((drag_handle || !resize_handle.empty()) && node_id.empty()) {
			if (drag_handle)
				node_id = std::string("luaui_dh_") + std::to_string(node_count);
			else
				node_id = std::string("luaui_rh_") + resize_handle + "_" + std::to_string(node_count);
			if (used_element_ids->count(node_id) != 0) {
				error_out = "internal UI error: duplicate generated handle id";
				return false;
			}
			used_element_ids->insert(node_id);
		}

		out += "<div ";
		if (!node_id.empty())
			out += "id=\"" + node_id + "\" ";
		if (drag_handle)
			out += "data-luui-drag-handle=\"1\" ";
		if (!resize_handle.empty())
			out += "data-luui-resize-handle=\"" + resize_handle + "\" ";
		out += "style=\"display:block;" + style_str + "\">";

		if (!node.isMember("children") || node["children"].isNull()) {
			out += "</div>";
			return true;
		}
		if (!node["children"].isArray()) {
			error_out = "div node: 'children' must be an array";
			return false;
		}
		const Json::Value &ch = node["children"];
		for (Json::ArrayIndex i = 0; i < ch.size(); ++i) {
			if (!ch[i].isObject()) {
				error_out = "div children must be objects";
				return false;
			}
			if (!compile_json_node(ch[i], depth + 1, node_count, out, error_out, out_bindings,
					    used_element_ids, used_state_keys, max_btn_slot_out))
				return false;
		}
		out += "</div>";
		return true;
	}

	if (type == "column" || type == "panel") {
		const Json::Value props_for_gap =
				props_ptr ? *props_ptr : Json::Value(Json::objectValue);
		int gap_px = gap_px_from_props_json(props_for_gap);

		out += "<div style=\"display:flex;flex-direction:column;align-items:stretch;";
		out += "gap:" + std::to_string(gap_px) + "px;\">";

		if (!node.isMember("children") || node["children"].isNull()) {
			out += "</div>";
			return true;
		}
		if (!node["children"].isArray()) {
			error_out = "column/panel node: 'children' must be an array";
			return false;
		}
		const Json::Value &ch = node["children"];
		for (Json::ArrayIndex i = 0; i < ch.size(); ++i) {
			if (!ch[i].isObject()) {
				error_out = "column children must be objects";
				return false;
			}
			if (!compile_json_node(ch[i], depth + 1, node_count, out, error_out, out_bindings,
					    used_element_ids, used_state_keys, max_btn_slot_out))
				return false;
		}
		out += "</div>";
		return true;
	}

	if (type == "row") {
		const Json::Value props_for_gap =
				props_ptr ? *props_ptr : Json::Value(Json::objectValue);
		int gap_px = gap_px_from_props_json(props_for_gap);
		const bool wrap = row_wrap_from_props_json(props_for_gap);

		out += "<div style=\"display:flex;flex-direction:row;align-items:center;";
		out += wrap ? "flex-wrap:wrap;" : "flex-wrap:nowrap;";
		out += "gap:" + std::to_string(gap_px) + "px;\">";

		if (!node.isMember("children") || node["children"].isNull()) {
			out += "</div>";
			return true;
		}
		if (!node["children"].isArray()) {
			error_out = "row node: 'children' must be an array";
			return false;
		}
		const Json::Value &ch = node["children"];
		for (Json::ArrayIndex i = 0; i < ch.size(); ++i) {
			if (!ch[i].isObject()) {
				error_out = "row children must be objects";
				return false;
			}
			if (!compile_json_node(ch[i], depth + 1, node_count, out, error_out, out_bindings,
					    used_element_ids, used_state_keys, max_btn_slot_out))
				return false;
		}
		out += "</div>";
		return true;
	}

	error_out = "unsupported UI node type '" + type + "'";
	return false;
}

} // namespace

bool compile_declarative_ui_from_json(const Json::Value &root, std::string &rml_out,
		std::string &error_out, std::vector<UiDeclarativeBindingEntry> *out_bindings,
		int *out_lua_button_count, bool *out_modal, UiDismissPolicy *out_dismiss)
{
	if (!root.isObject()) {
		error_out = "mount spec must be a JSON object";
		return false;
	}
	if (out_bindings)
		out_bindings->clear();
	if (out_lua_button_count)
		*out_lua_button_count = 0;
	if (out_modal)
		*out_modal = false;
	if (out_dismiss)
		*out_dismiss = UiDismissPolicy::None;

	// Server may wrap the tree as `{ "modal": bool, "body": { ... } }` for modal documents.
	Json::Value tree = root;
	if (root.isMember("body") && root["body"].isObject()) {
		tree = root["body"];
		if (out_modal && root.isMember("modal")) {
			const Json::Value &mf = root["modal"];
			if (mf.isBool())
				*out_modal = mf.asBool();
			else if (mf.isInt())
				*out_modal = mf.asInt() != 0;
		}
		if (out_dismiss && root.isMember("dismiss")) {
			const Json::Value &df = root["dismiss"];
			if (df.isNull()) {
				// default
			} else if (df.isString()) {
				const std::string tok = df.asString();
				if (tok == "escape") {
					*out_dismiss = UiDismissPolicy::Escape;
				} else if (tok == "outside_or_escape") {
					*out_dismiss = UiDismissPolicy::OutsideOrEscape;
				} else {
					error_out = "invalid modal dismiss policy (expected \"escape\" or \"outside_or_escape\")";
					return false;
				}
			} else {
				error_out = "modal dismiss policy must be a string when set";
				return false;
			}
		}
	}

	const bool has_type = tree.isMember("type") && tree["type"].isString();
	const bool has_template = tree.isMember("template") && tree["template"].isString();

	if (has_type && has_template) {
		error_out = "cannot use both 'type' and 'template' on the same mount";
		return false;
	}

	if (!has_type && has_template) {
		const std::string templ = tree["template"].asString();
		if (templ == "builtin:test_overlay") {
			if (out_bindings)
				out_bindings->clear();
			rml_out = UiManager::getBuiltinOverlayTestRmlDocument();
			return true;
		}
		if (templ == "builtin:test_layout_minimal") {
			if (out_bindings)
				out_bindings->clear();
			rml_out = "<rml>\n<head><style>\n"
					".luaui_focusable:focus{border-color:#4da3ff;}\n"
					".luaui_clickable:active{opacity:0.85;}\n"
					".luaui_clickable:hover{border-color:rgba(77,163,255,153);}\n"
					"</style></head>\n<body style=\"";
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
	int max_btn = 0;
	int *btn_ptr = out_lua_button_count ? &max_btn : nullptr;
	if (!compile_json_node(tree, 0, node_count, inner, error_out, out_bindings, &used_element_ids,
			    &used_state_keys, btn_ptr)) {
		if (out_bindings)
			out_bindings->clear();
		return false;
	}
	if (out_lua_button_count)
		*out_lua_button_count = max_btn;

	const std::string fam_css = ui_font_declarative_default_family_css();
	// Full-viewport shell only: transparent. Each surface should define its own root panel
	// (position/size/background) as the first node — avoids overlapping grey boxes and matches
	// the server/Lua declarative layout contract.
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
