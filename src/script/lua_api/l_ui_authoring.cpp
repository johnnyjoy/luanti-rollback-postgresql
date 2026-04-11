// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "lua_api/l_ui_authoring.h"
#include "config.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include "lua_api/l_base.h"

#include <cstring>

namespace {

/**
 * core.ui.box(opts) -> table
 * opts: { style?, children?, id? } — same semantics as a declarative `div` root fragment.
 */
static int l_ui_box(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	lua_newtable(L);
	const int res = lua_gettop(L);
	lua_pushstring(L, "div");
	lua_setfield(L, res, "type");

	lua_getfield(L, 1, "style");
	if (!lua_isnil(L, -1))
		lua_setfield(L, res, "style");
	else
		lua_pop(L, 1);

	lua_getfield(L, 1, "children");
	if (!lua_isnil(L, -1))
		lua_setfield(L, res, "children");
	else
		lua_pop(L, 1);

	lua_getfield(L, 1, "id");
	if (!lua_isnil(L, -1))
		lua_setfield(L, res, "id");
	else
		lua_pop(L, 1);

	return 1;
}

/**
 * core.ui.text(value [, opts]) -> table
 * value: string or core.ui.bind("key") marker table.
 * opts: optional { id = "..." (required for bind), style? } — `id` is the element id for patching.
 */
static int l_ui_text(lua_State *L)
{
	const int n = lua_gettop(L);
	if (n < 1)
		return luaL_error(L, "core.ui.text: value expected");

	lua_newtable(L);
	const int res = lua_gettop(L);
	lua_pushstring(L, "text");
	lua_setfield(L, res, "type");

	lua_newtable(L);
	const int props = lua_gettop(L);
	lua_pushvalue(L, 1);
	lua_setfield(L, props, "value");
	lua_setfield(L, res, "props");

	if (n >= 2 && lua_istable(L, 2)) {
		lua_getfield(L, 2, "id");
		if (!lua_isnil(L, -1))
			lua_setfield(L, res, "id");
		else
			lua_pop(L, 1);

		lua_getfield(L, 2, "style");
		if (!lua_isnil(L, -1))
			lua_setfield(L, res, "style");
		else
			lua_pop(L, 1);
	}

	return 1;
}

/**
 * core.ui.input(opts) -> table
 * opts: { id?, value?, placeholder?, disabled?, style? }.
 */
static int l_ui_input(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	lua_newtable(L);
	const int res = lua_gettop(L);
	lua_pushstring(L, "input");
	lua_setfield(L, res, "type");

	lua_pushvalue(L, 1);
	lua_setfield(L, res, "props");

	return 1;
}

/** opts: { gap?, children?, style?, id? } → `column` or `row` declarative node */
static void push_layout_node(lua_State *L, const char *decl_type)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	lua_newtable(L);
	const int res = lua_gettop(L);
	lua_pushstring(L, decl_type);
	lua_setfield(L, res, "type");

	lua_newtable(L);
	const int props = lua_gettop(L);
	lua_getfield(L, 1, "gap");
	if (!lua_isnil(L, -1))
		lua_setfield(L, props, "gap");
	else
		lua_pop(L, 1);
	if (!strcmp(decl_type, "row")) {
		lua_getfield(L, 1, "wrap");
		if (!lua_isnil(L, -1))
			lua_setfield(L, props, "wrap");
		else
			lua_pop(L, 1);
	}
	lua_setfield(L, res, "props");

	lua_getfield(L, 1, "style");
	if (!lua_isnil(L, -1))
		lua_setfield(L, res, "style");
	else
		lua_pop(L, 1);

	lua_getfield(L, 1, "children");
	if (!lua_isnil(L, -1))
		lua_setfield(L, res, "children");
	else
		lua_pop(L, 1);

	lua_getfield(L, 1, "id");
	if (!lua_isnil(L, -1))
		lua_setfield(L, res, "id");
	else
		lua_pop(L, 1);
}

static int l_ui_column(lua_State *L)
{
	push_layout_node(L, "column");
	return 1;
}

static int l_ui_row(lua_State *L)
{
	push_layout_node(L, "row");
	return 1;
}

/**
 * core.ui.button(opts) -> table
 * opts: { text = string, on_press = function(ctx) }
 */
static int l_ui_button(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	lua_newtable(L);
	const int res = lua_gettop(L);
	lua_pushstring(L, "button");
	lua_setfield(L, res, "type");

	lua_newtable(L);
	const int props = lua_gettop(L);
	lua_getfield(L, 1, "text");
	lua_setfield(L, props, "text");
	lua_getfield(L, 1, "on_press");
	if (!lua_isnil(L, -1))
		lua_setfield(L, props, "on_press");
	else
		lua_pop(L, 1);
	lua_setfield(L, res, "props");
	return 1;
}

/**
 * core.ui.inventory_grid(opts) -> table
 * opts: {
 *   id? = string,
 *   cols = number,
 *   rows = number,
 *   slot_size = number, -- px
 *   gap? = number|string, -- px or token ("sm"/"md"/"lg")
 *   items = core.ui.bind("items"),
 *   on_action? = function(ctx) -- server-side only (stripped before JSON mount)
 * }
 */
static int l_ui_inventory_grid(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	lua_newtable(L);
	const int res = lua_gettop(L);
	lua_pushstring(L, "inventory_grid");
	lua_setfield(L, res, "type");

	lua_newtable(L);
	const int props = lua_gettop(L);

	lua_getfield(L, 1, "cols");
	lua_setfield(L, props, "cols");
	lua_getfield(L, 1, "rows");
	lua_setfield(L, props, "rows");
	lua_getfield(L, 1, "slot_size");
	lua_setfield(L, props, "slot_size");
	lua_getfield(L, 1, "gap");
	if (!lua_isnil(L, -1))
		lua_setfield(L, props, "gap");
	else
		lua_pop(L, 1);
	lua_getfield(L, 1, "items");
	lua_setfield(L, props, "items");
	lua_getfield(L, 1, "on_action");
	if (!lua_isnil(L, -1))
		lua_setfield(L, props, "on_action");
	else
		lua_pop(L, 1);

	lua_setfield(L, res, "props");

	lua_getfield(L, 1, "style");
	if (!lua_isnil(L, -1))
		lua_setfield(L, res, "style");
	else
		lua_pop(L, 1);

	lua_getfield(L, 1, "id");
	if (!lua_isnil(L, -1))
		lua_setfield(L, res, "id");
	else
		lua_pop(L, 1);

	return 1;
}

} // namespace

void registerUiAuthoringHelpers(lua_State *L, int ui_top)
{
	ModApiBase::registerFunction(L, "box", l_ui_box, ui_top);
	ModApiBase::registerFunction(L, "text", l_ui_text, ui_top);
	ModApiBase::registerFunction(L, "input", l_ui_input, ui_top);
	ModApiBase::registerFunction(L, "column", l_ui_column, ui_top);
	ModApiBase::registerFunction(L, "row", l_ui_row, ui_top);
	ModApiBase::registerFunction(L, "button", l_ui_button, ui_top);
	ModApiBase::registerFunction(L, "inventory_grid", l_ui_inventory_grid, ui_top);
}
