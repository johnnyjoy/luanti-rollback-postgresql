// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "lua_api/l_ui_server_public.h"

#include "constants.h"
#include "exceptions.h"
#include "lua_api/l_base.h"
#include "lua_api/l_internal.h"
#include "network/networkpacket.h"
#include "network/networkprotocol.h"
#include "remoteplayer.h"
#include "server.h"
#include "serverenvironment.h"
#include "util/serialize.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

/** Server-side Lua registry refs for `core.ui.panel` button handlers (JSON cannot carry functions). */
static std::unordered_map<std::string, std::vector<int>> g_ui_server_button_refs;
/** Optional server-side dismiss handler for a surface (called when client dismisses a modal). */
static std::unordered_map<std::string, int> g_ui_server_dismiss_refs;
/** Optional inventory grid slot action handler for a surface (decoded from encoded UI action). */
static std::unordered_map<std::string, int> g_ui_server_inventory_grid_refs;
/** Optional server-side instrument handler for a surface (drag/resize events). */
static std::unordered_map<std::string, int> g_ui_server_instrument_event_refs;
static std::unordered_map<std::string, std::map<std::string, std::string>> g_ui_surface_state;
static std::unordered_map<std::string, std::string> g_ui_surface_owner;

using UiServerDispatchFn = void (*)(lua_State *L, RemotePlayer *player, const std::string &surface_id,
		u32 button_index);
static UiServerDispatchFn g_ui_server_dispatch_impl = nullptr;

namespace {

/** Minimum client protocol for server-driven RmlUi `core.ui` packets. */
constexpr u16 UI_SERVER_PANEL_PROTO_MIN = 52;

/** Must match `LUA_BIND_MARK` in ui_declarative_compile.cpp (core.ui.bind marker table). */
static const char LUA_UI_BIND_MARK[] = "__luui_bind";

static std::atomic<u64> s_surface_seq{0};

static bool ui_server_table_to_json(lua_State *L, int table_index, std::string &out,
		std::string &err)
{
	lua_getglobal(L, "core");
	lua_getfield(L, -1, "write_json");
	lua_remove(L, -2);
	lua_pushvalue(L, table_index);
	if (lua_pcall(L, 1, 2, 0)) {
		err = luaL_checkstring(L, -1);
		lua_pop(L, 1);
		return false;
	}
	if (lua_isnil(L, -2)) {
		err = lua_isstring(L, -1) ? lua_tostring(L, -1) : "write_json failed";
		lua_pop(L, 2);
		return false;
	}
	if (!lua_isstring(L, -2)) {
		lua_pop(L, 2);
		err = "write_json: expected string";
		return false;
	}
	size_t len = 0;
	const char *p = lua_tolstring(L, -2, &len);
	out.assign(p, len);
	lua_pop(L, 2);
	return true;
}

static bool send_ui_server_packet(Server *server, RemotePlayer *player, u8 op,
		const std::string &surface_id, const std::string &payload, const char *reason_tag,
		std::string &err)
{
	if (player->getPeerId() == PEER_ID_INEXISTENT) {
		err = "invalid peer";
		infostream << "[RmlUi TRACE] stage=2 server_send_packet sid=\"" << surface_id
				<< "\" op=" << static_cast<int>(op)
				<< " reason_tag=" << (reason_tag ? reason_tag : "(null)")
				<< " result=fail reason=invalid_peer"
				<< std::endl;
		return false;
	}
	if (player->protocol_version < UI_SERVER_PANEL_PROTO_MIN) {
		err = "client protocol too old for server UI (RmlUi; need >= 52)";
		infostream << "[RmlUi TRACE] stage=2 server_send_packet sid=\"" << surface_id
				<< "\" op=" << static_cast<int>(op) << " peer_id=" << player->getPeerId()
				<< " reason_tag=" << (reason_tag ? reason_tag : "(null)")
				<< " result=fail reason=protocol_lt_" << UI_SERVER_PANEL_PROTO_MIN << std::endl;
		return false;
	}
	if ((op == 0 || op == 1 || op == 3 || op == 4) && payload.size() > LONG_STRING_MAX_LEN) {
		err = "payload too large";
		infostream << "[RmlUi TRACE] stage=2 server_send_packet sid=\"" << surface_id
				<< "\" op=" << static_cast<int>(op)
				<< " reason_tag=" << (reason_tag ? reason_tag : "(null)")
				<< " result=fail reason=payload_too_large" << std::endl;
		return false;
	}
	try {
		NetworkPacket pkt(TOCLIENT_UI_SERVER, 0, player->getPeerId());
		pkt << op << surface_id;
		if (op == 0 || op == 1 || op == 3 || op == 4)
			pkt.putLongString(payload);
		server->Send(&pkt);
		infostream << "[RmlUi TRACE] stage=2 server_send_packet sid=\"" << surface_id
				<< "\" op=" << static_cast<int>(op)
				<< " reason_tag=" << (reason_tag ? reason_tag : "(null)")
				<< " payload_bytes=" << payload.size()
				<< " peer_id=" << player->getPeerId() << " result=ok" << std::endl;
	} catch (const PacketError &e) {
		err = e.what();
		infostream << "[RmlUi TRACE] stage=2 server_send_packet sid=\"" << surface_id
				<< "\" op=" << static_cast<int>(op)
				<< " reason_tag=" << (reason_tag ? reason_tag : "(null)")
				<< " result=fail reason=packet_error what=\"" << err << "\"" << std::endl;
		return false;
	}
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

/**
 * Expand an array-like `items` table into `"items.<n>" = value` state keys.
 * - Only supports numeric keys 1..4096.
 * - Values are coerced using @ref coerce_state_value.
 *
 * If @p out_map_idx > 0, also sets fields on that Lua table for JSON encoding.
 */
static bool apply_state_items_table(lua_State *L, int value_index,
		std::map<std::string, std::string> *out_state, int out_map_idx,
		std::string &err)
{
	if (!lua_istable(L, value_index)) {
		err = "items must be a table";
		return false;
	}
	lua_pushnil(L);
	while (lua_next(L, value_index) != 0) {
		if (lua_type(L, -2) != LUA_TNUMBER) {
			lua_pop(L, 1);
			continue;
		}
		const lua_Number nk = lua_tonumber(L, -2);
		const int idx = static_cast<int>(nk);
		if (idx <= 0 || idx > 4096) {
			lua_pop(L, 1);
			continue;
		}
		std::string v;
		if (!coerce_state_value(L, -1, v, err)) {
			lua_pop(L, 1);
			return false;
		}
		const std::string key = std::string("items.") + std::to_string(idx);
		if (out_state)
			(*out_state)[key] = v;
		if (out_map_idx > 0) {
			lua_pushstring(L, v.c_str());
			lua_setfield(L, out_map_idx, key.c_str());
		}
		lua_pop(L, 1);
	}
	return true;
}

/**
 * Stack top: optional `state` value from opts (nil, absent as nil, or table).
 * Pops it. On success returns true; on failure pushes nil, err and returns false.
 */
static bool send_panel_initial_state(lua_State *L, Server *server, RemotePlayer *player,
		const std::string &surface_id)
{
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return true;
	}
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		lua_pushnil(L);
		lua_pushstring(L, "core.ui.panel: state must be a table when provided");
		return false;
	}
	lua_newtable(L);
	const int map = lua_gettop(L);
	const int state_idx = map - 1;
	std::string c_err;
	lua_pushnil(L);
	while (lua_next(L, state_idx) != 0) {
		if (lua_type(L, -2) != LUA_TSTRING) {
			lua_pop(L, 2);
			lua_pop(L, 1);
			lua_pop(L, 1);
			lua_pushnil(L);
			lua_pushstring(L, "core.ui.panel: state requires string keys");
			return false;
		}
		const char *k = lua_tostring(L, -2);
		if (k && !strcmp(k, "items") && lua_istable(L, -1)) {
			if (!apply_state_items_table(L, lua_gettop(L), nullptr, map, c_err)) {
				lua_pop(L, 2);
				lua_pop(L, 1);
				lua_pop(L, 1);
				lua_pushnil(L);
				lua_pushstring(L, c_err.c_str());
				return false;
			}
			lua_pop(L, 1);
			continue;
		}
		std::string v;
		if (!coerce_state_value(L, -1, v, c_err)) {
			lua_pop(L, 2);
			lua_pop(L, 1);
			lua_pop(L, 1);
			lua_pushnil(L);
			lua_pushstring(L, c_err.c_str());
			return false;
		}
		lua_pushstring(L, v.c_str());
		lua_setfield(L, map, k);
		lua_pop(L, 1);
	}

	// Avoid sending an empty state patch. Some JSON encoders serialize an empty Lua table as `[]`,
	// which the client-side apply path rejects (expects an object). An empty patch is a no-op anyway.
	lua_pushnil(L);
	if (lua_next(L, map) == 0) {
		lua_pop(L, 1); // pop map
		lua_pop(L, 1); // pop original state table
		return true;
	}
	lua_pop(L, 2); // pop value + key from the lua_next() probe

	std::string json, jerr;
	if (!ui_server_table_to_json(L, map, json, jerr)) {
		lua_pop(L, 1);
		lua_pop(L, 1);
		lua_pushnil(L);
		lua_pushstring(L, jerr.c_str());
		return false;
	}
	lua_pop(L, 1);
	lua_pop(L, 1);
	std::string serr;
	if (!send_ui_server_packet(server, player, 1, surface_id, json, "set_state", serr)) {
		lua_pushnil(L);
		lua_pushstring(L, serr.c_str());
		return false;
	}
	return true;
}

static void ui_server_release_surface_resources(lua_State *L, const std::string &surface_id)
{
	auto it = g_ui_server_button_refs.find(surface_id);
	if (it != g_ui_server_button_refs.end()) {
		for (int r : it->second) {
			if (r != LUA_NOREF && r != LUA_REFNIL)
				luaL_unref(L, LUA_REGISTRYINDEX, r);
		}
		g_ui_server_button_refs.erase(it);
	}
	auto it_dis = g_ui_server_dismiss_refs.find(surface_id);
	if (it_dis != g_ui_server_dismiss_refs.end()) {
		const int r = it_dis->second;
		if (r != LUA_NOREF && r != LUA_REFNIL)
			luaL_unref(L, LUA_REGISTRYINDEX, r);
		g_ui_server_dismiss_refs.erase(it_dis);
	}
	auto it_grid = g_ui_server_inventory_grid_refs.find(surface_id);
	if (it_grid != g_ui_server_inventory_grid_refs.end()) {
		const int r = it_grid->second;
		if (r != LUA_NOREF && r != LUA_REFNIL)
			luaL_unref(L, LUA_REGISTRYINDEX, r);
		g_ui_server_inventory_grid_refs.erase(it_grid);
	}
	auto it_inst = g_ui_server_instrument_event_refs.find(surface_id);
	if (it_inst != g_ui_server_instrument_event_refs.end()) {
		const int r = it_inst->second;
		if (r != LUA_NOREF && r != LUA_REFNIL)
			luaL_unref(L, LUA_REGISTRYINDEX, r);
		g_ui_server_instrument_event_refs.erase(it_inst);
	}
	g_ui_surface_state.erase(surface_id);
	g_ui_surface_owner.erase(surface_id);
}

static void copy_table_without_functions(lua_State *L, int src_idx, int dst_idx)
{
	if (src_idx < 0)
		src_idx = lua_gettop(L) + 1 + src_idx;
	if (dst_idx < 0)
		dst_idx = lua_gettop(L) + 1 + dst_idx;

	lua_pushnil(L);
	while (lua_next(L, src_idx) != 0) {
		// stack: ... key value
		if (!lua_isfunction(L, -1)) {
			lua_pushvalue(L, -2); // key
			lua_pushvalue(L, -2); // value
			lua_settable(L, dst_idx);
		}
		lua_pop(L, 1); // pop value, keep key
	}
}

static bool parse_json_to_table(lua_State *L, const std::string &json, std::string &err)
{
	lua_getglobal(L, "core");
	lua_getfield(L, -1, "parse_json");
	lua_remove(L, -2);
	lua_pushlstring(L, json.data(), json.size());
	if (lua_pcall(L, 1, 2, 0)) {
		err = luaL_checkstring(L, -1);
		lua_pop(L, 1);
		return false;
	}
	if (lua_isnil(L, -2)) {
		err = lua_isstring(L, -1) ? lua_tostring(L, -1) : "parse_json failed";
		lua_pop(L, 2);
		return false;
	}
	if (!lua_istable(L, -2)) {
		lua_pop(L, 2);
		err = "parse_json: expected table";
		return false;
	}
	lua_pop(L, 1); // pop err; keep table
	return true;
}

static void init_surface_state_from_opts(lua_State *L, int opts_index, const std::string &surface_id)
{
	g_ui_surface_state[surface_id].clear();
	lua_getfield(L, opts_index, "state");
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return;
	}
	const int st = lua_gettop(L);
	lua_pushnil(L);
	while (lua_next(L, st) != 0) {
		if (lua_type(L, -2) != LUA_TSTRING) {
			lua_pop(L, 2);
			continue;
		}
		const char *k = lua_tostring(L, -2);
		if (k && !strcmp(k, "items") && lua_istable(L, -1)) {
			std::string err;
			(void)apply_state_items_table(L, lua_gettop(L), &g_ui_surface_state[surface_id], 0, err);
			lua_pop(L, 1);
			continue;
		}
		std::string v;
		std::string err;
		if (!coerce_state_value(L, -1, v, err)) {
			lua_pop(L, 2);
			continue;
		}
		g_ui_surface_state[surface_id][k] = v;
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
}

static bool strip_button_handlers(lua_State *L, int idx, std::vector<int> &refs, int *invgrid_ref_out,
		std::string &err)
{
	if (!lua_istable(L, idx))
		return true;
	lua_getfield(L, idx, "type");
	const char *t = lua_isstring(L, -1) ? lua_tostring(L, -1) : nullptr;
	lua_pop(L, 1);
	if (t && !strcmp(t, "button")) {
		lua_getfield(L, idx, "props");
		if (!lua_istable(L, -1)) {
			lua_pop(L, 1);
			err = "button node requires props table";
			return false;
		}
		const int props = lua_gettop(L);
		int fn_ref = LUA_NOREF;
		lua_getfield(L, props, "on_press");
		if (lua_isfunction(L, -1)) {
			fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
			lua_pushnil(L);
			lua_setfield(L, props, "on_press");
		} else {
			lua_pop(L, 1);
		}
		if (fn_ref == LUA_NOREF) {
			lua_getfield(L, props, "on_click");
			if (lua_isfunction(L, -1)) {
				fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
				lua_pushnil(L);
				lua_setfield(L, props, "on_click");
			} else {
				lua_pop(L, 1);
			}
		}
		if (fn_ref != LUA_NOREF) {
			const int slot = static_cast<int>(refs.size());
			refs.push_back(fn_ref);
			lua_pushinteger(L, slot);
			lua_setfield(L, props, "__luui_btn");
		}
		lua_pop(L, 1);
		return true;
	}
	if (t && !strcmp(t, "inventory_grid")) {
		lua_getfield(L, idx, "props");
		if (!lua_istable(L, -1)) {
			lua_pop(L, 1);
			err = "inventory_grid node requires props table";
			return false;
		}
		const int props = lua_gettop(L);
		lua_getfield(L, props, "on_action");
		if (!lua_isnil(L, -1)) {
			if (!lua_isfunction(L, -1)) {
				lua_pop(L, 2);
				err = "inventory_grid node: props.on_action must be a function when set";
				return false;
			}
			if (invgrid_ref_out && *invgrid_ref_out != LUA_NOREF && *invgrid_ref_out != LUA_REFNIL) {
				lua_pop(L, 2);
				err = "inventory_grid node: only one props.on_action is supported per surface";
				return false;
			}
			const int fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
			lua_pushnil(L);
			lua_setfield(L, props, "on_action");
			if (invgrid_ref_out)
				*invgrid_ref_out = fn_ref;
		} else {
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
		// Keep walking children if present (defensive).
	}
	lua_getfield(L, idx, "children");
	if (lua_istable(L, -1)) {
		const int ch = lua_gettop(L);
		lua_pushnil(L);
		while (lua_next(L, ch) != 0) {
			if (lua_istable(L, -1)) {
				if (!strip_button_handlers(L, lua_gettop(L), refs, invgrid_ref_out, err))
					return false;
			}
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
	} else {
		lua_pop(L, 1);
	}
	return true;
}

static int l_server_ui_ctx_set(lua_State *L)
{
	const char *surface_id = lua_tostring(L, lua_upvalueindex(1));
	const char *pname = lua_tostring(L, lua_upvalueindex(2));
	if (!surface_id || !pname)
		return 0;
	luaL_checktype(L, 1, LUA_TTABLE);
	GET_ENV_PTR_NO_MAP_LOCK;
	RemotePlayer *player = env->getPlayer(pname);
	if (!player) {
		lua_pushnil(L);
		lua_pushstring(L, "player not found");
		return 2;
	}
	lua_newtable(L);
	const int map = lua_gettop(L);
	std::string c_err;
	lua_pushnil(L);
	while (lua_next(L, 1) != 0) {
		if (lua_type(L, -2) != LUA_TSTRING) {
			lua_pop(L, 2);
			lua_pushnil(L);
			lua_pushstring(L, "core.ui ctx.set: string keys required");
			return 2;
		}
		const char *k = lua_tostring(L, -2);
		if (k && !strcmp(k, "items") && lua_istable(L, -1)) {
			if (!apply_state_items_table(L, lua_gettop(L), &g_ui_surface_state[surface_id], map, c_err)) {
				lua_pop(L, 2);
				lua_pushnil(L);
				lua_pushstring(L, c_err.c_str());
				return 2;
			}
			lua_pop(L, 1);
			continue;
		}
		std::string v;
		if (!coerce_state_value(L, -1, v, c_err)) {
			lua_pop(L, 2);
			lua_pushnil(L);
			lua_pushstring(L, c_err.c_str());
			return 2;
		}
		g_ui_surface_state[surface_id][k] = v;
		lua_pushstring(L, v.c_str());
		lua_setfield(L, map, k);
		lua_pop(L, 1);
	}
	std::string json, jerr;
	if (!ui_server_table_to_json(L, map, json, jerr)) {
		lua_pop(L, 1);
		lua_pushnil(L);
		lua_pushstring(L, jerr.c_str());
		return 2;
	}
	lua_pop(L, 1);
	std::string serr;
	if (!send_ui_server_packet(ModApiBase::getServer(L), player, 1, std::string(surface_id), json,
			    "set_state", serr)) {
		lua_pushnil(L);
		lua_pushstring(L, serr.c_str());
		return 2;
	}
	lua_pushboolean(L, true);
	return 1;
}

class LuaUiHandle
{
public:
	static const char className[];

	LuaUiHandle(std::string player, std::string surface_id) :
			m_player(std::move(player)),
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

	static void create(lua_State *L, const std::string &player, const std::string &surface_id)
	{
		auto *o = new LuaUiHandle(player, surface_id);
		auto **ud = reinterpret_cast<LuaUiHandle **>(lua_newuserdata(L, sizeof(LuaUiHandle *)));
		*ud = o;
		luaL_getmetatable(L, className);
		lua_setmetatable(L, -2);
		infostream << "[RmlUi TRACE] stage=1 server_handle_create player=\"" << player
				<< "\" sid=\"" << surface_id << "\"" << std::endl;
	}

	static int gc_object(lua_State *L)
	{
		auto **ud = reinterpret_cast<LuaUiHandle **>(lua_touserdata(L, 1));
		if (!ud || !*ud)
			return 0;
		infostream << "[RmlUi TRACE] stage=1 server_handle_gc sid=\"" << (*ud)->m_surface_id
				<< "\" player=\"" << (*ud)->m_player << "\" closed=" << ((*ud)->m_closed ? "yes" : "no")
				<< std::endl;
		(*ud)->unmount_if_needed(L);
		delete *ud;
		*ud = nullptr;
		return 0;
	}

	void unmount_if_needed(lua_State *L)
	{
		if (m_closed)
			return;
		GET_ENV_PTR_NO_MAP_LOCK_VOID;
		RemotePlayer *player = env->getPlayer(m_player.c_str());
		if (!player) {
			infostream << "[RmlUi TRACE] stage=2 server_handle_unmount_if_needed sid=\"" << m_surface_id
					<< "\" reason=no_player (gc) player=\"" << m_player << "\"" << std::endl;
			ui_server_release_surface_resources(L, m_surface_id);
			m_closed = true;
			return;
		}
		infostream << "[RmlUi TRACE] stage=2 server_handle_unmount_if_needed sid=\"" << m_surface_id
				<< "\" reason=gc player=\"" << m_player << "\"" << std::endl;
		std::string serr;
		send_ui_server_packet(ModApiBase::getServer(L), player, 2, m_surface_id, "", "gc", serr);
		ui_server_release_surface_resources(L, m_surface_id);
		m_closed = true;
	}

	static int l_close(lua_State *L)
	{
		auto *h = ModApiBase::checkObject<LuaUiHandle>(L, 1);
		if (h->m_closed) {
			lua_pushboolean(L, true);
			return 1;
		}
		infostream << "[RmlUi TRACE] stage=2 server_handle_close sid=\"" << h->m_surface_id
				<< "\" player=\"" << h->m_player << "\" reason=close_call" << std::endl;
		GET_ENV_PTR_NO_MAP_LOCK;
		RemotePlayer *player = env->getPlayer(h->m_player.c_str());
		if (!player) {
			ui_server_release_surface_resources(L, h->m_surface_id);
			h->m_closed = true;
			lua_pushboolean(L, true);
			return 1;
		}
		std::string serr;
		if (!send_ui_server_packet(ModApiBase::getServer(L), player, 2, h->m_surface_id, "",
				    "close_call", serr)) {
			lua_pushnil(L);
			lua_pushstring(L, serr.c_str());
			return 2;
		}
		ui_server_release_surface_resources(L, h->m_surface_id);
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
		GET_ENV_PTR_NO_MAP_LOCK;
		RemotePlayer *player = env->getPlayer(h->m_player.c_str());
		if (!player) {
			lua_pushnil(L);
			lua_pushstring(L, "player not found");
			return 2;
		}
		lua_newtable(L);
		int map = lua_gettop(L);
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
			if (k && !strcmp(k, "items") && lua_istable(L, -1)) {
				if (!apply_state_items_table(L, lua_gettop(L), &g_ui_surface_state[h->m_surface_id], map,
						    c_err)) {
					lua_pop(L, 2);
					lua_pushnil(L);
					lua_pushstring(L, c_err.c_str());
					return 2;
				}
				lua_pop(L, 1);
				continue;
			}
			std::string v;
			if (!coerce_state_value(L, -1, v, c_err)) {
				lua_pop(L, 2);
				lua_pushnil(L);
				lua_pushstring(L, c_err.c_str());
				return 2;
			}
			g_ui_surface_state[h->m_surface_id][k] = v;
			lua_pushstring(L, v.c_str());
			lua_setfield(L, map, k);
			lua_pop(L, 1); // value from lua_next; key remains for next
		}
		std::string json, jerr;
		if (!ui_server_table_to_json(L, map, json, jerr)) {
			lua_pop(L, 1);
			lua_pushnil(L);
			lua_pushstring(L, jerr.c_str());
			return 2;
		}
		lua_pop(L, 1);
		std::string serr;
		if (!send_ui_server_packet(ModApiBase::getServer(L), player, 1, h->m_surface_id, json,
				    "set_state", serr)) {
			lua_pushnil(L);
			lua_pushstring(L, serr.c_str());
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
	std::string m_player;
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

/**
 * @param use_modal_wrapper When true, mount JSON is `{ "modal": true, "body": <content> }` so the
 *        client shows the document with RmlUi modal input ownership.
 */
static int l_ui_mount_server(lua_State *L, bool use_modal_wrapper, const char *layer_tok)
{
	const char *const fn = use_modal_wrapper ? "core.ui.modal" : "core.ui.panel";

	luaL_checktype(L, 1, LUA_TTABLE);
	lua_getfield(L, 1, "player");
	if (!lua_isstring(L, -1)) {
		lua_pop(L, 1);
		lua_pushnil(L);
		lua_pushfstring(L, "%s: server requires opts.player (string)", fn);
		return 2;
	}
	std::string pname(lua_tostring(L, -1));
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

	init_surface_state_from_opts(L, 1, surface_id);
	g_ui_surface_owner[surface_id] = pname;

	int dismiss_ref = LUA_NOREF;
	lua_getfield(L, 1, "on_dismiss");
	if (lua_isfunction(L, -1)) {
		dismiss_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	} else {
		lua_pop(L, 1);
	}

	std::vector<int> btn_refs;
	int invgrid_ref = LUA_NOREF;
	std::string strip_err;
	if (!strip_button_handlers(L, content_idx, btn_refs, &invgrid_ref, strip_err)) {
		if (dismiss_ref != LUA_NOREF && dismiss_ref != LUA_REFNIL)
			luaL_unref(L, LUA_REGISTRYINDEX, dismiss_ref);
		ui_server_release_surface_resources(L, surface_id);
		lua_pop(L, 1);
		lua_pushnil(L);
		lua_pushstring(L, strip_err.c_str());
		return 2;
	}

	std::string json, jerr;
	bool has_pos_meta = false;
	{
		lua_getfield(L, 1, "anchor");
		has_pos_meta = has_pos_meta || lua_type(L, -1) == LUA_TSTRING;
		lua_pop(L, 1);
		lua_getfield(L, 1, "x");
		has_pos_meta = has_pos_meta || lua_isnumber(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, 1, "y");
		has_pos_meta = has_pos_meta || lua_isnumber(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, 1, "keep_in_view");
		has_pos_meta = has_pos_meta || lua_isboolean(L, -1);
		lua_pop(L, 1);
	}

	bool has_instrument_meta = false;
	{
		lua_getfield(L, 1, "instrument");
		has_instrument_meta = lua_istable(L, -1);
		lua_pop(L, 1);
	}

	const bool wrap_mount = use_modal_wrapper || has_pos_meta || has_instrument_meta || (layer_tok != nullptr);
	if (wrap_mount) {
		lua_newtable(L);
		const int wrap_idx = lua_gettop(L);
		if (use_modal_wrapper) {
			lua_pushboolean(L, true);
			lua_setfield(L, wrap_idx, "modal");
			lua_getfield(L, 1, "dismiss");
			if (!lua_isnil(L, -1))
				lua_setfield(L, wrap_idx, "dismiss");
			else
				lua_pop(L, 1);
		}

		// Optional minimal positioning metadata (pixels only).
		lua_getfield(L, 1, "anchor");
		if (!lua_isnil(L, -1))
			lua_setfield(L, wrap_idx, "anchor");
		else
			lua_pop(L, 1);
		lua_getfield(L, 1, "x");
		if (!lua_isnil(L, -1))
			lua_setfield(L, wrap_idx, "x");
		else
			lua_pop(L, 1);
		lua_getfield(L, 1, "y");
		if (!lua_isnil(L, -1))
			lua_setfield(L, wrap_idx, "y");
		else
			lua_pop(L, 1);
		lua_getfield(L, 1, "keep_in_view");
		if (!lua_isnil(L, -1))
			lua_setfield(L, wrap_idx, "keep_in_view");
		else
			lua_pop(L, 1);

		// Optional surface layer.
		if (layer_tok) {
			lua_pushstring(L, layer_tok);
			lua_setfield(L, wrap_idx, "layer");
		}

		// Optional instrument eligibility.
		lua_getfield(L, 1, "instrument");
		if (lua_istable(L, -1)) {
			const int inst_idx = lua_gettop(L);

			// Optional per-surface instrument event handler (not serializable; store registry ref).
			lua_getfield(L, inst_idx, "on_event");
			if (lua_isfunction(L, -1)) {
				const int fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
				g_ui_server_instrument_event_refs[surface_id] = fn_ref;
			} else {
				lua_pop(L, 1);
			}

			// Copy instrument metadata without functions into the JSON wrapper.
			lua_newtable(L);
			const int inst_copy_idx = lua_gettop(L);
			copy_table_without_functions(L, inst_idx, inst_copy_idx);
			lua_setfield(L, wrap_idx, "instrument"); // pops inst_copy_idx

			lua_pop(L, 1); // pop inst_idx
		} else {
			lua_pop(L, 1);
		}

		lua_pushvalue(L, content_idx);
		lua_setfield(L, wrap_idx, "body");
		if (!ui_server_table_to_json(L, wrap_idx, json, jerr)) {
			for (int r : btn_refs) {
				if (r != LUA_NOREF && r != LUA_REFNIL)
					luaL_unref(L, LUA_REGISTRYINDEX, r);
			}
			if (invgrid_ref != LUA_NOREF && invgrid_ref != LUA_REFNIL)
				luaL_unref(L, LUA_REGISTRYINDEX, invgrid_ref);
			if (dismiss_ref != LUA_NOREF && dismiss_ref != LUA_REFNIL)
				luaL_unref(L, LUA_REGISTRYINDEX, dismiss_ref);
			ui_server_release_surface_resources(L, surface_id);
			lua_pop(L, 1);
			lua_pop(L, 1);
			lua_pushnil(L);
			lua_pushstring(L, jerr.c_str());
			return 2;
		}
		lua_pop(L, 1);
	} else if (!ui_server_table_to_json(L, content_idx, json, jerr)) {
		for (int r : btn_refs) {
			if (r != LUA_NOREF && r != LUA_REFNIL)
				luaL_unref(L, LUA_REGISTRYINDEX, r);
		}
		if (invgrid_ref != LUA_NOREF && invgrid_ref != LUA_REFNIL)
			luaL_unref(L, LUA_REGISTRYINDEX, invgrid_ref);
		if (dismiss_ref != LUA_NOREF && dismiss_ref != LUA_REFNIL)
			luaL_unref(L, LUA_REGISTRYINDEX, dismiss_ref);
		ui_server_release_surface_resources(L, surface_id);
		lua_pop(L, 1);
		lua_pushnil(L);
		lua_pushstring(L, jerr.c_str());
		return 2;
	}
	lua_pop(L, 1);

	GET_ENV_PTR_NO_MAP_LOCK;
	RemotePlayer *player = env->getPlayer(pname.c_str());
	if (!player) {
		for (int r : btn_refs) {
			if (r != LUA_NOREF && r != LUA_REFNIL)
				luaL_unref(L, LUA_REGISTRYINDEX, r);
		}
		if (invgrid_ref != LUA_NOREF && invgrid_ref != LUA_REFNIL)
			luaL_unref(L, LUA_REGISTRYINDEX, invgrid_ref);
		if (dismiss_ref != LUA_NOREF && dismiss_ref != LUA_REFNIL)
			luaL_unref(L, LUA_REGISTRYINDEX, dismiss_ref);
		ui_server_release_surface_resources(L, surface_id);
		lua_pushnil(L);
		lua_pushstring(L, "player not found");
		return 2;
	}

	infostream << "[RmlUi TRACE] stage=1 " << fn << " player=\"" << pname << "\" sid=\""
			<< surface_id << "\"" << std::endl;

	std::string serr;
	if (!send_ui_server_packet(ModApiBase::getServer(L), player, 0, surface_id, json, "mount", serr)) {
		for (int r : btn_refs) {
			if (r != LUA_NOREF && r != LUA_REFNIL)
				luaL_unref(L, LUA_REGISTRYINDEX, r);
		}
		if (invgrid_ref != LUA_NOREF && invgrid_ref != LUA_REFNIL)
			luaL_unref(L, LUA_REGISTRYINDEX, invgrid_ref);
		if (dismiss_ref != LUA_NOREF && dismiss_ref != LUA_REFNIL)
			luaL_unref(L, LUA_REGISTRYINDEX, dismiss_ref);
		ui_server_release_surface_resources(L, surface_id);
		lua_pushnil(L);
		lua_pushstring(L, serr.c_str());
		return 2;
	}

	lua_getfield(L, 1, "state");
	if (!send_panel_initial_state(L, ModApiBase::getServer(L), player, surface_id)) {
		std::string uerr;
		(void)send_ui_server_packet(ModApiBase::getServer(L), player, 2, surface_id, "",
				"mount_replace", uerr);
		for (int r : btn_refs) {
			if (r != LUA_NOREF && r != LUA_REFNIL)
				luaL_unref(L, LUA_REGISTRYINDEX, r);
		}
		if (invgrid_ref != LUA_NOREF && invgrid_ref != LUA_REFNIL)
			luaL_unref(L, LUA_REGISTRYINDEX, invgrid_ref);
		if (dismiss_ref != LUA_NOREF && dismiss_ref != LUA_REFNIL)
			luaL_unref(L, LUA_REGISTRYINDEX, dismiss_ref);
		ui_server_release_surface_resources(L, surface_id);
		return 2;
	}

	g_ui_server_button_refs[surface_id] = std::move(btn_refs);
	if (invgrid_ref != LUA_NOREF && invgrid_ref != LUA_REFNIL)
		g_ui_server_inventory_grid_refs[surface_id] = invgrid_ref;
	if (dismiss_ref != LUA_NOREF && dismiss_ref != LUA_REFNIL)
		g_ui_server_dismiss_refs[surface_id] = dismiss_ref;

	LuaUiHandle::create(L, pname, surface_id);
	lua_pushnil(L);
	return 2;
}

static int l_ui_panel(lua_State *L)
{
	return l_ui_mount_server(L, false, nullptr);
}

static int l_ui_modal(lua_State *L)
{
	return l_ui_mount_server(L, true, nullptr);
}

static int l_ui_hud(lua_State *L)
{
	return l_ui_mount_server(L, false, "hud");
}

static int l_enter_instrument_mode(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	lua_getfield(L, 1, "player");
	if (!lua_isstring(L, -1)) {
		lua_pop(L, 1);
		lua_pushnil(L);
		lua_pushstring(L, "core.ui.enter_instrument_mode: requires opts.player (string)");
		return 2;
	}
	std::string pname(lua_tostring(L, -1));
	lua_pop(L, 1);

	GET_ENV_PTR_NO_MAP_LOCK;
	RemotePlayer *player = env->getPlayer(pname.c_str());
	if (!player) {
		lua_pushnil(L);
		lua_pushstring(L, "player not found");
		return 2;
	}
	std::string payload = "{\"active\":true}";
	std::string serr;
	(void)send_ui_server_packet(ModApiBase::getServer(L), player, 3, "instrument_mode", payload,
			"instrument_mode_enter", serr);
	lua_pushboolean(L, serr.empty());
	if (!serr.empty()) {
		lua_pushstring(L, serr.c_str());
		return 2;
	}
	return 1;
}

static int l_exit_instrument_mode(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	lua_getfield(L, 1, "player");
	if (!lua_isstring(L, -1)) {
		lua_pop(L, 1);
		lua_pushnil(L);
		lua_pushstring(L, "core.ui.exit_instrument_mode: requires opts.player (string)");
		return 2;
	}
	std::string pname(lua_tostring(L, -1));
	lua_pop(L, 1);

	GET_ENV_PTR_NO_MAP_LOCK;
	RemotePlayer *player = env->getPlayer(pname.c_str());
	if (!player) {
		lua_pushnil(L);
		lua_pushstring(L, "player not found");
		return 2;
	}
	std::string payload = "{\"active\":false}";
	std::string serr;
	(void)send_ui_server_packet(ModApiBase::getServer(L), player, 3, "instrument_mode", payload,
			"instrument_mode_exit", serr);
	lua_pushboolean(L, serr.empty());
	if (!serr.empty()) {
		lua_pushstring(L, serr.c_str());
		return 2;
	}
	return 1;
}

static void ui_server_dispatch_impl(lua_State *L, RemotePlayer *player, const std::string &surface_id,
		u32 button_index)
{
	if (!L || !player)
		return;
	const std::string &pname = player->getName();
	auto it_owner = g_ui_surface_owner.find(surface_id);
	if (it_owner == g_ui_surface_owner.end() || it_owner->second != pname) {
		warningstream << "[RmlUi] server UI action: owner mismatch or unknown surface sid=\""
				<< surface_id << "\" peer_player=\"" << pname << "\" owner="
				<< (it_owner == g_ui_surface_owner.end() ? std::string("(none)")
								    : it_owner->second)
				<< " btn=" << button_index << std::endl;
		return;
	}

	// Sentinel indices for modal dismiss actions (sent by client UI policy).
	// - escape: int -1 serialized as u32 => 0xFFFFFFFF
	// - outside click: int -2 serialized as u32 => 0xFFFFFFFE
	const bool is_dismiss = (button_index == 0xFFFFFFFFu) || (button_index == 0xFFFFFFFEu);
	const char *dismiss_reason = (button_index == 0xFFFFFFFFu) ? "escape" : "outside";
	if (is_dismiss) {
		auto it_dis = g_ui_server_dismiss_refs.find(surface_id);
		if (it_dis == g_ui_server_dismiss_refs.end()) {
			warningstream << "[RmlUi] server UI dismiss: no on_dismiss handler sid=\"" << surface_id
					<< "\" reason=\"" << dismiss_reason << "\"" << std::endl;
			return;
		}
		const int fn_ref = it_dis->second;
		if (fn_ref == LUA_NOREF || fn_ref == LUA_REFNIL) {
			warningstream << "[RmlUi] server UI dismiss: missing handler ref sid=\"" << surface_id
					<< "\" reason=\"" << dismiss_reason << "\"" << std::endl;
			return;
		}
		const int error_handler = PUSH_ERROR_HANDLER(L);
		lua_rawgeti(L, LUA_REGISTRYINDEX, fn_ref);
		if (!lua_isfunction(L, -1)) {
			warningstream << "[RmlUi] server UI dismiss: handler is not a function sid=\"" << surface_id
					<< "\" reason=\"" << dismiss_reason << "\"" << std::endl;
			lua_pop(L, 2);
			return;
		}
		lua_newtable(L);
		const int ctx = lua_gettop(L);
		lua_pushstring(L, dismiss_reason);
		lua_setfield(L, ctx, "reason");
		lua_pushstring(L, surface_id.c_str());
		lua_setfield(L, ctx, "surface_id");
		lua_pushstring(L, pname.c_str());
		lua_setfield(L, ctx, "player");
		PCALL_RESL(L, lua_pcall(L, 1, 0, error_handler));
		lua_pop(L, 1);
		return;
	}

	// Inventory slot actions: high bit set, low 31 bits are 1-based slot index.
	// This reuses the same UI action pipeline (TOSERVER_UI_ACTION + single-fire latch).
	if ((button_index & 0x80000000u) != 0u) {
		const u32 slot = button_index & 0x7FFFFFFFu;
		if (slot == 0u || slot > 4096u) {
			warningstream << "[RmlUi] server UI slot action: out of range sid=\"" << surface_id
					<< "\" slot=" << slot << std::endl;
			return;
		}
		auto it_grid = g_ui_server_inventory_grid_refs.find(surface_id);
		if (it_grid == g_ui_server_inventory_grid_refs.end()) {
			warningstream << "[RmlUi] server UI slot action: no inventory_grid on_action for sid=\""
					<< surface_id << "\" slot=" << slot << std::endl;
			return;
		}
		const int fn_ref = it_grid->second;
		if (fn_ref == LUA_NOREF || fn_ref == LUA_REFNIL) {
			warningstream << "[RmlUi] server UI slot action: missing handler ref sid=\"" << surface_id
					<< "\" slot=" << slot << std::endl;
			return;
		}
		const int error_handler = PUSH_ERROR_HANDLER(L);
		lua_rawgeti(L, LUA_REGISTRYINDEX, fn_ref);
		if (!lua_isfunction(L, -1)) {
			warningstream << "[RmlUi] server UI slot action: handler is not a function sid=\""
					<< surface_id << "\" slot=" << slot << std::endl;
			lua_pop(L, 2);
			return;
		}
		lua_newtable(L);
		const int ctx = lua_gettop(L);
		lua_pushinteger(L, static_cast<lua_Integer>(slot));
		lua_setfield(L, ctx, "slot_index");
		lua_pushstring(L, surface_id.c_str());
		lua_setfield(L, ctx, "surface_id");
		lua_pushstring(L, pname.c_str());
		lua_setfield(L, ctx, "player");
		lua_pushstring(L, surface_id.c_str());
		lua_pushstring(L, pname.c_str());
		lua_pushcclosure(L, l_server_ui_ctx_set, 2);
		lua_setfield(L, ctx, "set");
		PCALL_RESL(L, lua_pcall(L, 1, 0, error_handler));
		lua_pop(L, 1);
		return;
	}

	auto it_refs = g_ui_server_button_refs.find(surface_id);
	if (it_refs == g_ui_server_button_refs.end() || button_index >= it_refs->second.size()) {
		warningstream << "[RmlUi] server UI action: no button handler for sid=\"" << surface_id
				<< "\" btn=" << button_index << std::endl;
		return;
	}
	const int fn_ref = it_refs->second[button_index];
	if (fn_ref == LUA_NOREF || fn_ref == LUA_REFNIL) {
		warningstream << "[RmlUi] server UI action: missing handler ref sid=\"" << surface_id
				<< "\" btn=" << button_index << std::endl;
		return;
	}

	const int error_handler = PUSH_ERROR_HANDLER(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, fn_ref);
	if (!lua_isfunction(L, -1)) {
		warningstream << "[RmlUi] server UI action: handler is not a function sid=\"" << surface_id
				<< "\" btn=" << button_index << std::endl;
		lua_pop(L, 2);
		return;
	}
	lua_pushvalue(L, -1);
	const int temp_exec_ref = luaL_ref(L, LUA_REGISTRYINDEX);

	lua_newtable(L);
	const int ctx = lua_gettop(L);

	lua_newtable(L);
	auto st_it = g_ui_surface_state.find(surface_id);
	if (st_it != g_ui_surface_state.end()) {
		for (const auto &kv : st_it->second) {
			lua_pushstring(L, kv.second.c_str());
			lua_setfield(L, -2, kv.first.c_str());
		}
	}
	lua_setfield(L, ctx, "state");

	lua_pushstring(L, surface_id.c_str());
	lua_pushstring(L, pname.c_str());
	lua_pushcclosure(L, l_server_ui_ctx_set, 2);
	lua_setfield(L, ctx, "set");

	PCALL_RESL(L, lua_pcall(L, 1, 0, error_handler));
	luaL_unref(L, LUA_REGISTRYINDEX, temp_exec_ref);
	lua_pop(L, 1);
}

struct UiServerDispatchRegistrar
{
	UiServerDispatchRegistrar()
	{
		g_ui_server_dispatch_impl = &ui_server_dispatch_impl;
	}
} g_ui_server_dispatch_registrar;

} // namespace

void ui_server_dispatch_ui_action(lua_State *L, Server *server, RemotePlayer *player,
		const std::string &surface_id, u32 button_index)
{
	(void)server;
	if (g_ui_server_dispatch_impl)
		g_ui_server_dispatch_impl(L, player, surface_id, button_index);
}

static void ui_server_dispatch_ui_instrument_event_impl(lua_State *L, Server *server, RemotePlayer *player,
		const std::string &surface_id, const std::string &payload_json)
{
	if (!L || !server || !player)
		return;

	const std::string &pname = player->getName();
	auto it_owner = g_ui_surface_owner.find(surface_id);
	if (it_owner == g_ui_surface_owner.end() || it_owner->second != pname) {
		warningstream << "[RmlUi] server instrument event: owner mismatch or unknown surface sid=\""
				<< surface_id << "\" peer_player=\"" << pname << "\" owner="
				<< (it_owner == g_ui_surface_owner.end() ? std::string("(none)") : it_owner->second)
				<< std::endl;
		return;
	}

	auto it_fn = g_ui_server_instrument_event_refs.find(surface_id);
	if (it_fn == g_ui_server_instrument_event_refs.end()) {
		// No handler: ignore.
		return;
	}
	const int fn_ref = it_fn->second;
	if (fn_ref == LUA_NOREF || fn_ref == LUA_REFNIL)
		return;

	std::string perr;
	if (!parse_json_to_table(L, payload_json, perr)) {
		warningstream << "[RmlUi] server instrument event: bad payload sid=\"" << surface_id
				<< "\" err=\"" << perr << "\"" << std::endl;
		return;
	}
	const int ctx = lua_gettop(L);
	lua_pushstring(L, surface_id.c_str());
	lua_setfield(L, ctx, "surface_id");
	lua_pushstring(L, pname.c_str());
	lua_setfield(L, ctx, "player");

	const int error_handler = PUSH_ERROR_HANDLER(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, fn_ref);
	if (!lua_isfunction(L, -1)) {
		warningstream << "[RmlUi] server instrument event: handler is not a function sid=\""
				<< surface_id << "\"" << std::endl;
		lua_pop(L, 3); // pop handler value + error handler + ctx table
		return;
	}
	lua_pushvalue(L, ctx);
	// Handler may return an apply patch table or nil.
	if (lua_pcall(L, 1, 1, error_handler) != 0) {
		// Error already reported by PUSH_ERROR_HANDLER.
		lua_pop(L, 3); // pop error result + error handler + ctx table
		return;
	}
	lua_remove(L, error_handler); // remove error handler, keep ctx + result

	// Result: nil => no apply. table => send op=4 payload to client.
	if (lua_istable(L, -1)) {
		std::string json, jerr;
		if (ui_server_table_to_json(L, lua_gettop(L), json, jerr)) {
			std::string serr;
			(void)send_ui_server_packet(server, player, 4, surface_id, json, "instrument_apply", serr);
		} else {
			warningstream << "[RmlUi] server instrument event: apply write_json failed sid=\""
					<< surface_id << "\" err=\"" << jerr << "\"" << std::endl;
		}
	}

	lua_pop(L, 2); // pop result + ctx table
}

void ui_server_dispatch_ui_instrument_event(lua_State *L, Server *server, RemotePlayer *player,
		const std::string &surface_id, const std::string &payload_json)
{
	ui_server_dispatch_ui_instrument_event_impl(L, server, player, surface_id, payload_json);
}

void registerUiServerPublic(lua_State *L, int ui_top)
{
	LuaUiHandle::Register(L);
	ModApiBase::registerFunction(L, "panel", l_ui_panel, ui_top);
	ModApiBase::registerFunction(L, "modal", l_ui_modal, ui_top);
	ModApiBase::registerFunction(L, "hud", l_ui_hud, ui_top);
	ModApiBase::registerFunction(L, "enter_instrument_mode", l_enter_instrument_mode, ui_top);
	ModApiBase::registerFunction(L, "exit_instrument_mode", l_exit_instrument_mode, ui_top);
	ModApiBase::registerFunction(L, "bind", l_ui_bind, ui_top);
}
