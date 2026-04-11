// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "client/ui_declarative_binding.h"
#include "client/ui_manager.h"

#include <json/json.h>
#include <string>
#include <vector>

struct lua_State;

/**
 * Declarative UI: compile a Lua table tree into an RML document string.
 * RML is an internal implementation detail only; the stable API is the Lua tree
 * (type / props / children), not generated markup.
 *
 * Node types: "text" (props.value string or core.ui.bind key), "button" (props.text; props.on_press or props.on_click …),
 * "column" | "panel" (optional props.gap = "sm"|"md"|"lg", optional children array).
 * Optional node `id` (string) sets a stable RmlUi element id for set_state patching;
 * must not start with `luaui_` (reserved for engine-generated ids such as `luaui_btn_*`).
 * Root may instead use template = "builtin:test_overlay" or "builtin:test_layout_minimal" (testing).
 *
 * When @p out_lua_button_refs is non-null, each `button` with props.on_click pushes a
 * registry ref to the function (in order); generated nodes use id `luaui_btn_0` …
 * for the engine to attach RmlUi listeners.
 *
 * When @p out_bindings is non-null, `text` nodes with `core.ui.bind` record patch targets.
 *
 * Returns true and sets @p rml_out on success. On failure sets @p error_out.
 */
bool compile_declarative_ui_from_lua(lua_State *L, int table_index,
		std::string &rml_out, std::string &error_out,
		std::vector<int> *out_lua_button_refs = nullptr,
		std::vector<UiDeclarativeBindingEntry> *out_bindings = nullptr,
		bool *out_modal = nullptr,
		UiDismissPolicy *out_dismiss = nullptr);

/// Same declarative schema as @ref compile_declarative_ui_from_lua, but from JSON (server-driven UI).
/// When @p out_lua_button_count is non-null, sets it to the number of `luaui_btn_*` slots (max index + 1).
bool compile_declarative_ui_from_json(const Json::Value &root, std::string &rml_out, std::string &error_out,
		std::vector<UiDeclarativeBindingEntry> *out_bindings = nullptr,
		int *out_lua_button_count = nullptr,
		bool *out_modal = nullptr,
		UiDismissPolicy *out_dismiss = nullptr);
