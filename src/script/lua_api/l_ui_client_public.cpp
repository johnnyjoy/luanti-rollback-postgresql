// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "lua_api/l_ui_client_public.h"

#include "client/client.h"
#include "client/ui_declarative_compile.h"
#include "client/ui_manager.h"
#include "lua_api/l_base.h"
#include "scripting_client.h"

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

static std::atomic<u64> s_surface_seq{0};

/** Must match `LUA_BIND_MARK` in ui_declarative_compile.cpp (core.ui.bind marker table). */
static const char LUA_UI_BIND_MARK[] = "__luui_bind";

static int absindex(lua_State *L, int i)
{
	if (i > 0 || i <= LUA_REGISTRYINDEX)
		return i;
	return lua_gettop(L) + i + 1;
}

/**
 * Compile declarative table, unmount surface id, mount new RML, store on_click refs.
 */
static bool apply_declarative_tree(
		lua_State *L, const char *surface_id, int table_index, UiLayer layer, std::string &err_out)
{
	UiManager *ui = ModApiBase::getClient(L)->getUiManager();
	if (!ui || !ui->isReady()) {
		err_out = "RmlUi UI not available";
		return false;
	}

	std::string rml;
	std::string compile_err;
	std::vector<int> button_refs;
	std::vector<UiDeclarativeBindingEntry> bindings;
	bool modal_mount = false;
	UiDismissPolicy dismiss_policy = UiDismissPolicy::None;
	if (!compile_declarative_ui_from_lua(L, table_index, rml, compile_err, &button_refs,
			    &bindings, &modal_mount, &dismiss_policy)) {
		for (int r : button_refs)
			luaL_unref(L, LUA_REGISTRYINDEX, r);
		err_out = compile_err;
		return false;
	}

	ClientScripting *script = ModApiBase::getClient(L)->getScript();
	ui->unmount(surface_id);

	std::string doc_url = std::string("rmlui://lua/") + surface_id + "/mount";
	std::string err;
	const std::vector<UiDeclarativeBindingEntry> *bind_ptr =
			bindings.empty() ? nullptr : &bindings;

	// Minimal positioning options for Lua-side public mounts (client scripting).
	UiSurfacePositioning pos_spec;
	const UiSurfacePositioning *pos_ptr = nullptr;
	bool explicit_pos = false;

	UiSurfaceLayout layout_spec;
	const UiSurfaceLayout *layout_ptr = nullptr;

	auto parse_anchor = [](std::string tok) -> std::optional<UiAnchor> {
		for (char &c : tok)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		if (tok == "center")
			return UiAnchor::Center;
		if (tok == "top-left" || tok == "topleft")
			return UiAnchor::TopLeft;
		if (tok == "top-right" || tok == "topright")
			return UiAnchor::TopRight;
		if (tok == "bottom-left" || tok == "bottomleft")
			return UiAnchor::BottomLeft;
		if (tok == "bottom-right" || tok == "bottomright")
			return UiAnchor::BottomRight;
		if (tok == "top")
			return UiAnchor::Top;
		if (tok == "bottom")
			return UiAnchor::Bottom;
		if (tok == "left")
			return UiAnchor::Left;
		if (tok == "right")
			return UiAnchor::Right;
		return std::nullopt;
	};

	auto table_has_author_positioning = [&](int root_idx) -> bool {
		root_idx = absindex(L, root_idx);
		lua_getfield(L, root_idx, "style");
		if (!lua_istable(L, -1)) {
			lua_pop(L, 1);
			return false;
		}
		static const char *keys[] = {"position", "top", "left", "right", "bottom", "transform",
				"margin_left", "margin_top", "margin_right", "margin_bottom"};
		for (const char *k : keys) {
			lua_getfield(L, -1, k);
			const bool present = !lua_isnil(L, -1);
			lua_pop(L, 1);
			if (present) {
				lua_pop(L, 1);
				return true;
			}
		}
		lua_pop(L, 1);
		return false;
	};

	// Read opts from the mount spec table.
	{
		table_index = absindex(L, table_index);
		lua_getfield(L, table_index, "anchor");
		if (lua_type(L, -1) == LUA_TSTRING) {
			if (auto a = parse_anchor(lua_tostring(L, -1))) {
				pos_spec.anchor = *a;
				explicit_pos = true;
			}
		}
		lua_pop(L, 1);

		lua_getfield(L, table_index, "x");
		if (lua_isnumber(L, -1)) {
			pos_spec.x = static_cast<s32>(std::floor(lua_tonumber(L, -1) + 0.5));
			explicit_pos = true;
		}
		lua_pop(L, 1);

		lua_getfield(L, table_index, "y");
		if (lua_isnumber(L, -1)) {
			pos_spec.y = static_cast<s32>(std::floor(lua_tonumber(L, -1) + 0.5));
			explicit_pos = true;
		}
		lua_pop(L, 1);

		lua_getfield(L, table_index, "keep_in_view");
		if (lua_isboolean(L, -1)) {
			pos_spec.keep_in_view = lua_toboolean(L, -1);
			explicit_pos = true;
		}
		lua_pop(L, 1);
	}

	// Optional instrument eligibility contract (only meaningful for HUD layer).
	{
		table_index = absindex(L, table_index);
		lua_getfield(L, table_index, "instrument");
		if (lua_istable(L, -1)) {
			const int lo = lua_gettop(L);
			lua_getfield(L, lo, "movable");
			if (lua_isboolean(L, -1))
				layout_spec.movable = lua_toboolean(L, -1);
			lua_pop(L, 1);
			lua_getfield(L, lo, "resizable");
			if (lua_isboolean(L, -1))
				layout_spec.resizable = lua_toboolean(L, -1);
			lua_pop(L, 1);
			lua_getfield(L, lo, "sticky");
			if (lua_isboolean(L, -1))
				layout_spec.sticky = lua_toboolean(L, -1);
			lua_pop(L, 1);

			lua_getfield(L, lo, "drag_handle");
			if (lua_type(L, -1) == LUA_TSTRING)
				layout_spec.drag_handle_id = lua_tostring(L, -1);
			lua_pop(L, 1);

			lua_getfield(L, lo, "anchors");
			if (lua_istable(L, -1)) {
				const int aidx = lua_gettop(L);
				const size_t n = lua_objlen(L, aidx);
				for (size_t i = 1; i <= n; ++i) {
					lua_rawgeti(L, aidx, static_cast<int>(i));
					if (lua_type(L, -1) == LUA_TSTRING) {
						if (auto a = parse_anchor(lua_tostring(L, -1)))
							layout_spec.allowed_anchors.push_back(*a);
					}
					lua_pop(L, 1);
				}
			}
			lua_pop(L, 1); // anchors
			layout_ptr = &layout_spec;
		}
		lua_pop(L, 1); // instrument
	}

	// Determine author positioning on the compiled tree root (body wrapper or direct root).
	int compile_root = table_index;
	lua_getfield(L, table_index, "body");
	if (lua_istable(L, -1)) {
		compile_root = lua_gettop(L);
	} else {
		lua_pop(L, 1);
	}
	const bool author_pos = table_has_author_positioning(compile_root);
	if (compile_root != table_index)
		lua_pop(L, 1); // body

	if (explicit_pos) {
		pos_ptr = &pos_spec;
	} else if (modal_mount && !author_pos) {
		pos_spec.anchor = UiAnchor::Center;
		pos_spec.x = 0;
		pos_spec.y = 0;
		pos_spec.keep_in_view = true;
		pos_ptr = &pos_spec;
	}

	UiMountOptions mount_opts;
	mount_opts.lua_button_count = static_cast<int>(button_refs.size());
	mount_opts.bindings = bind_ptr;
	mount_opts.modal_document = modal_mount;
	mount_opts.dismiss_policy = dismiss_policy;
	mount_opts.positioning = pos_ptr;
	mount_opts.layout = layout_ptr;
	if (!ui->mount(surface_id, layer, 0, rml.c_str(), doc_url.c_str(), err, mount_opts)) {
		for (int r : button_refs)
			luaL_unref(L, LUA_REGISTRYINDEX, r);
		err_out = err;
		return false;
	}

	script->setRmlUiServerButtonRefs(surface_id, std::move(button_refs));
	return true;
}

static std::string make_surface_id(lua_State *L, int opts_index)
{
	u64 n = ++s_surface_seq;
	lua_getfield(L, opts_index, "id");
	if (lua_isstring(L, -1)) {
		const char *hint = lua_tostring(L, -1);
		std::string h;
		for (const char *p = hint; *p && h.size() < 64; ++p) {
			char c = *p;
			if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
					(c >= '0' && c <= '9') || c == '_')
				h.push_back(c);
		}
		lua_pop(L, 1);
		if (!h.empty())
			return "luui_" + std::to_string(n) + "_" + h;
	} else {
		lua_pop(L, 1);
	}
	return "luui_" + std::to_string(n);
}

static bool coerce_state_value(lua_State *L, int index, std::string &out, std::string &err)
{
	int t = lua_type(L, index);
	if (t == LUA_TSTRING) {
		out = lua_tostring(L, index);
		return true;
	}
	if (t == LUA_TNUMBER) {
		char buf[64];
		snprintf(buf, sizeof(buf), "%.14g", lua_tonumber(L, index));
		out = buf;
		return true;
	}
	if (t == LUA_TBOOLEAN) {
		out = lua_toboolean(L, index) ? "true" : "false";
		return true;
	}
	if (t == LUA_TNIL) {
		err = "nil is not a valid ui state value (omit the key)";
		return false;
	}
	err = "ui state values must be string, number, or boolean in v1";
	return false;
}

class LuaUiHandle
{
public:
	static const char className[];

	LuaUiHandle(std::string surface_id) :
			m_surface_id(std::move(surface_id)),
			m_closed(false)
	{
	}

	static void Register(lua_State *L)
	{
		const luaL_Reg methods[] = {
			{"close", l_close},
			{"show", l_show},
			{"hide", l_hide},
			{"set", l_set},
			{"is_open", l_is_open},
			{nullptr, nullptr},
		};
		const luaL_Reg metamethods[] = {
			{"__gc", gc_object},
			{nullptr, nullptr},
		};
		ModApiBase::registerClass<LuaUiHandle>(L, methods, metamethods);
	}

	static void create(lua_State *L, const std::string &surface_id)
	{
		auto *o = new LuaUiHandle(surface_id);
		auto **ud = reinterpret_cast<LuaUiHandle **>(lua_newuserdata(L, sizeof(LuaUiHandle *)));
		*ud = o;
		luaL_getmetatable(L, className);
		lua_setmetatable(L, -2);
	}

	static int gc_object(lua_State *L)
	{
		auto **ud = reinterpret_cast<LuaUiHandle **>(lua_touserdata(L, 1));
		if (!ud || !*ud)
			return 0;
		(*ud)->unmount_if_needed(L);
		delete *ud;
		*ud = nullptr;
		return 0;
	}

	void unmount_if_needed(lua_State *L)
	{
		if (m_closed)
			return;
		Client *cl = ModApiBase::getClient(L);
		UiManager *ui = cl->getUiManager();
		if (ui)
			ui->unmount(m_surface_id);
		if (ClientScripting *script = cl->getScript())
			script->releaseRmlUiServerButtonRefs(m_surface_id);
		m_closed = true;
	}

	static int l_close(lua_State *L)
	{
		auto *h = ModApiBase::checkObject<LuaUiHandle>(L, 1);
		if (h->m_closed) {
			lua_pushboolean(L, true);
			return 1;
		}
		Client *cl = ModApiBase::getClient(L);
		UiManager *ui = cl->getUiManager();
		if (!ui) {
			h->m_closed = true;
			lua_pushboolean(L, true);
			return 1;
		}
		ui->unmount(h->m_surface_id);
		if (ClientScripting *script = cl->getScript())
			script->releaseRmlUiServerButtonRefs(h->m_surface_id);
		h->m_closed = true;
		lua_pushboolean(L, true);
		return 1;
	}

	static int l_show(lua_State *L)
	{
		ModApiBase::checkObject<LuaUiHandle>(L, 1);
		lua_pushboolean(L, true);
		return 1;
	}

	static int l_hide(lua_State *L)
	{
		ModApiBase::checkObject<LuaUiHandle>(L, 1);
		lua_pushboolean(L, true);
		return 1;
	}

	static int l_set(lua_State *L)
	{
		auto *h = ModApiBase::checkObject<LuaUiHandle>(L, 1);
		luaL_checktype(L, 2, LUA_TTABLE);
		if (h->m_closed) {
			lua_pushnil(L);
			lua_pushstring(L, "core.ui handle: set on closed handle");
			return 2;
		}
		UiManager *ui = ModApiBase::getClient(L)->getUiManager();
		if (!ui || !ui->isReady()) {
			lua_pushnil(L);
			lua_pushstring(L, "RmlUi UI not available");
			return 2;
		}
		std::map<std::string, std::string> state;
		std::string c_err;
		lua_pushnil(L);
		while (lua_next(L, 2) != 0) {
			if (lua_type(L, -2) != LUA_TSTRING) {
				lua_pop(L, 2);
				lua_pushnil(L);
				lua_pushstring(L, "core.ui handle: set requires string keys");
				return 2;
			}
			const char *k = lua_tostring(L, -2);
			std::string v;
			if (!coerce_state_value(L, -1, v, c_err)) {
				lua_pop(L, 2);
				lua_pushnil(L);
				lua_pushstring(L, c_err.c_str());
				return 2;
			}
			state[k] = std::move(v);
			lua_pop(L, 1);
		}
		std::string err;
		if (!ui->setSurfaceState(h->m_surface_id, state, err)) {
			lua_pushnil(L);
			lua_pushstring(L, err.c_str());
			return 2;
		}
		lua_pushboolean(L, true);
		return 1;
	}

	static int l_is_open(lua_State *L)
	{
		auto *h = ModApiBase::checkObject<LuaUiHandle>(L, 1);
		lua_pushboolean(L, !h->m_closed);
		return 1;
	}

private:
	std::string m_surface_id;
	bool m_closed;
};

const char LuaUiHandle::className[] = "LuaUiHandle";

static int l_ui_bind(lua_State *L)
{
	const char *key = luaL_checkstring(L, 1);
	lua_newtable(L);
	lua_pushstring(L, key);
	lua_setfield(L, -2, LUA_UI_BIND_MARK);
	return 1;
}

static int l_ui_mount_client(lua_State *L, bool use_modal_wrapper, UiLayer layer)
{
	const char *const fn = use_modal_wrapper ? "core.ui.modal" : "core.ui.panel";

	luaL_checktype(L, 1, LUA_TTABLE);
	lua_getfield(L, 1, "player");
	if (!lua_isnil(L, -1)) {
		lua_pop(L, 1);
		lua_pushnil(L);
		lua_pushfstring(L, "%s: client must not set opts.player", fn);
		return 2;
	}
	lua_pop(L, 1);

	lua_getfield(L, 1, "content");
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		lua_pushnil(L);
		lua_pushfstring(L, "%s: requires opts.content (table)", fn);
		return 2;
	}
	const int content_idx = lua_gettop(L);

	std::string surface_id = make_surface_id(L, 1);

	int mount_idx = content_idx;
	if (use_modal_wrapper) {
		lua_newtable(L);
		const int wrap_idx = lua_gettop(L);
		lua_pushboolean(L, true);
		lua_setfield(L, wrap_idx, "modal");
		lua_getfield(L, 1, "dismiss");
		if (!lua_isnil(L, -1))
			lua_setfield(L, wrap_idx, "dismiss");
		else
			lua_pop(L, 1);
		lua_pushvalue(L, content_idx);
		lua_setfield(L, wrap_idx, "body");
		mount_idx = wrap_idx;
	}

	std::string err;
	if (!apply_declarative_tree(L, surface_id.c_str(), mount_idx, layer, err)) {
		if (use_modal_wrapper)
			lua_pop(L, 1);
		lua_pop(L, 1);
		lua_pushnil(L);
		lua_pushstring(L, err.c_str());
		return 2;
	}
	if (use_modal_wrapper)
		lua_pop(L, 1);
	lua_pop(L, 1);

	LuaUiHandle::create(L, surface_id);
	lua_pushnil(L);
	return 2;
}

static int l_ui_panel(lua_State *L)
{
	return l_ui_mount_client(L, false, UiLayer::OVERLAY);
}

static int l_ui_modal(lua_State *L)
{
	return l_ui_mount_client(L, true, UiLayer::OVERLAY);
}

static int l_ui_hud(lua_State *L)
{
	return l_ui_mount_client(L, false, UiLayer::HUD);
}

static int l_enter_instrument_mode(lua_State *L)
{
	UiManager *ui = ModApiBase::getClient(L)->getUiManager();
	if (!ui || !ui->isReady())
		return 0;
	ui->enterInstrumentMode();
	return 0;
}

static int l_exit_instrument_mode(lua_State *L)
{
	UiManager *ui = ModApiBase::getClient(L)->getUiManager();
	if (!ui || !ui->isReady())
		return 0;
	ui->exitInstrumentMode();
	return 0;
}

static int l_is_instrument_mode(lua_State *L)
{
	UiManager *ui = ModApiBase::getClient(L)->getUiManager();
	lua_pushboolean(L, ui && ui->isReady() && ui->isInstrumentMode());
	return 1;
}

static int l_instrument_set_handler(lua_State *L)
{
	Client *client = ModApiBase::getClient(L);
	if (!client)
		return 0;
	ClientScripting *script = client->getScript();
	if (!script)
		return 0;

	if (lua_isnoneornil(L, 1)) {
		script->clearRmlUiInstrumentHandler();
		return 0;
	}

	luaL_checktype(L, 1, LUA_TFUNCTION);
	lua_pushvalue(L, 1);
	const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
	script->setRmlUiInstrumentHandler(ref);
	return 0;
}

static std::string anchor_to_token(UiAnchor a)
{
	switch (a) {
	case UiAnchor::Center: return "center";
	case UiAnchor::TopLeft: return "top-left";
	case UiAnchor::TopRight: return "top-right";
	case UiAnchor::BottomLeft: return "bottom-left";
	case UiAnchor::BottomRight: return "bottom-right";
	case UiAnchor::Top: return "top";
	case UiAnchor::Bottom: return "bottom";
	case UiAnchor::Left: return "left";
	case UiAnchor::Right: return "right";
	}
	return "center";
}

static std::optional<UiAnchor> token_to_anchor(std::string tok)
{
	for (char &c : tok)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	if (tok == "center")
		return UiAnchor::Center;
	if (tok == "top-left" || tok == "topleft")
		return UiAnchor::TopLeft;
	if (tok == "top-right" || tok == "topright")
		return UiAnchor::TopRight;
	if (tok == "bottom-left" || tok == "bottomleft")
		return UiAnchor::BottomLeft;
	if (tok == "bottom-right" || tok == "bottomright")
		return UiAnchor::BottomRight;
	if (tok == "top")
		return UiAnchor::Top;
	if (tok == "bottom")
		return UiAnchor::Bottom;
	if (tok == "left")
		return UiAnchor::Left;
	if (tok == "right")
		return UiAnchor::Right;
	return std::nullopt;
}

static int l_hud_placement_get(lua_State *L)
{
	UiManager *ui = ModApiBase::getClient(L)->getUiManager();
	lua_newtable(L);
	if (!ui || !ui->isReady())
		return 1;

	const auto snap = ui->getHudPlacement();
	for (const auto &p : snap) {
		lua_newtable(L);
		lua_pushstring(L, anchor_to_token(p.second.anchor).c_str());
		lua_setfield(L, -2, "anchor");
		lua_pushinteger(L, p.second.x);
		lua_setfield(L, -2, "x");
		lua_pushinteger(L, p.second.y);
		lua_setfield(L, -2, "y");
		lua_pushboolean(L, p.second.keep_in_view);
		lua_setfield(L, -2, "keep_in_view");
		lua_setfield(L, -2, p.first.c_str());
	}
	return 1;
}

static int l_hud_placement_set(lua_State *L)
{
	UiManager *ui = ModApiBase::getClient(L)->getUiManager();
	if (!ui || !ui->isReady())
		return 0;
	luaL_checktype(L, 1, LUA_TTABLE);

	std::map<std::string, UiSurfacePositioning> placement;
	lua_pushnil(L);
	while (lua_next(L, 1) != 0) {
		// key at -2, value at -1
		if (lua_type(L, -2) != LUA_TSTRING || !lua_istable(L, -1)) {
			lua_pop(L, 1);
			continue;
		}
		std::string sid = lua_tostring(L, -2);
		const int t = lua_gettop(L);

		UiSurfacePositioning p;
		lua_getfield(L, t, "anchor");
		if (lua_type(L, -1) == LUA_TSTRING) {
			if (auto a = token_to_anchor(lua_tostring(L, -1)))
				p.anchor = *a;
		}
		lua_pop(L, 1);

		lua_getfield(L, t, "x");
		if (lua_isnumber(L, -1))
			p.x = static_cast<s32>(std::floor(lua_tonumber(L, -1) + 0.5));
		lua_pop(L, 1);

		lua_getfield(L, t, "y");
		if (lua_isnumber(L, -1))
			p.y = static_cast<s32>(std::floor(lua_tonumber(L, -1) + 0.5));
		lua_pop(L, 1);

		lua_getfield(L, t, "keep_in_view");
		if (lua_isboolean(L, -1))
			p.keep_in_view = lua_toboolean(L, -1);
		lua_pop(L, 1);

		placement.emplace(std::move(sid), p);
		lua_pop(L, 1); // value
	}

	ui->setHudPlacement(placement);
	return 0;
}

} // namespace

void registerUiClientPublic(lua_State *L, int ui_top)
{
	LuaUiHandle::Register(L);
	ModApiBase::registerFunction(L, "panel", l_ui_panel, ui_top);
	ModApiBase::registerFunction(L, "modal", l_ui_modal, ui_top);
	ModApiBase::registerFunction(L, "hud", l_ui_hud, ui_top);
	ModApiBase::registerFunction(L, "enter_instrument_mode", l_enter_instrument_mode, ui_top);
	ModApiBase::registerFunction(L, "exit_instrument_mode", l_exit_instrument_mode, ui_top);
	ModApiBase::registerFunction(L, "is_instrument_mode", l_is_instrument_mode, ui_top);
	ModApiBase::registerFunction(L, "instrument_set_handler", l_instrument_set_handler, ui_top);
	ModApiBase::registerFunction(L, "hud_placement_get", l_hud_placement_get, ui_top);
	ModApiBase::registerFunction(L, "hud_placement_set", l_hud_placement_set, ui_top);
	ModApiBase::registerFunction(L, "bind", l_ui_bind, ui_top);
}
