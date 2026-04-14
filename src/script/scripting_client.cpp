// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2013 celeron55, Perttu Ahola <celeron55@gmail.com>
// Copyright (C) 2017 nerzhul, Loic Blot <loic.blot@unix-experience.fr>

#include "scripting_client.h"
#include "client/client.h"
#include "common/c_internal.h"
#include "cpp_api/s_internal.h"
#include "lua_api/l_client.h"
#include "lua_api/l_client_common.h"
#include "lua_api/l_env.h"
#include "lua_api/l_item.h"
#include "lua_api/l_itemstackmeta.h"
#include "lua_api/l_minimap.h"
#include "lua_api/l_modchannels.h"
#include "lua_api/l_particles_local.h"
#include "lua_api/l_storage.h"
#include "lua_api/l_util.h"
#include "lua_api/l_item.h"
#include "lua_api/l_nodemeta.h"
#include "lua_api/l_localplayer.h"
#include "lua_api/l_camera.h"
#include "lua_api/l_settings.h"
#include "lua_api/l_client_sound.h"
#include "client/ui_manager.h"

ClientScripting::ClientScripting(Client *client):
	ScriptApiBase(ScriptingType::Client)
{
	setGameDef(client);

	SCRIPTAPI_PRECHECKHEADER

	// Security is mandatory client side
	initializeSecurityClient();

	lua_getglobal(L, "core");
	int top = lua_gettop(L);

	lua_newtable(L);
	lua_setfield(L, -2, "ui");

	InitializeModApi(L, top);
	lua_pop(L, 1);

	// Push builtin initialization type
	lua_pushstring(L, "client");
	lua_setglobal(L, "INIT");

	infostream << "SCRIPTAPI: Initialized client game modules" << std::endl;
}

void ClientScripting::InitializeModApi(lua_State *L, int top)
{
	LuaItemStack::Register(L);
	ItemStackMetaRef::Register(L);
	LuaRaycast::Register(L);
	StorageRef::Register(L);
	LuaMinimap::Register(L);
	NodeMetaRef::RegisterClient(L);
	LuaLocalPlayer::Register(L);
	LuaCamera::Register(L);
	ModChannelRef::Register(L);
	LuaSettings::Register(L);
	ClientSoundHandle::Register(L);

	ModApiUtil::InitializeClient(L, top);
	ModApiClientCommon::Initialize(L, top);
	ModApiClient::Initialize(L, top);
	ModApiItem::InitializeClient(L, top);
	ModApiStorage::Initialize(L, top);
	ModApiEnv::InitializeClient(L, top);
	ModApiChannels::Initialize(L, top);
	ModApiParticlesLocal::Initialize(L, top);
	ModApiClientSound::Initialize(L, top);
}

void ClientScripting::on_client_ready(LocalPlayer *localplayer)
{
	LuaLocalPlayer::create(getStack(), localplayer);
}

void ClientScripting::on_camera_ready(Camera *camera)
{
	LuaCamera::create(getStack(), camera);
}

void ClientScripting::on_minimap_ready(Minimap *minimap)
{
	LuaMinimap::create(getStack(), minimap);
}

void ClientScripting::releaseRmlUiServerButtonRefs(const std::string &surface_id)
{
	SCRIPTAPI_PRECHECKHEADER

	auto it = m_rmlui_server_button_refs.find(surface_id);
	if (it == m_rmlui_server_button_refs.end())
		return;
	for (int r : it->second) {
		if (r != LUA_NOREF && r != LUA_REFNIL)
			luaL_unref(L, LUA_REGISTRYINDEX, r);
	}
	m_rmlui_server_button_refs.erase(it);
}

void ClientScripting::setRmlUiServerButtonRefs(const std::string &surface_id, std::vector<int> &&refs)
{
	SCRIPTAPI_PRECHECKHEADER
	releaseRmlUiServerButtonRefs(surface_id);
	m_rmlui_server_button_refs[surface_id] = std::move(refs);
}

void ClientScripting::releaseAllRmlUiServerButtonRefs()
{
	SCRIPTAPI_PRECHECKHEADER
	for (auto &p : m_rmlui_server_button_refs) {
		for (int r : p.second) {
			if (r != LUA_NOREF && r != LUA_REFNIL)
				luaL_unref(L, LUA_REGISTRYINDEX, r);
		}
	}
	m_rmlui_server_button_refs.clear();
}

void ClientScripting::setRmlUiInstrumentHandler(int lua_registry_ref)
{
	SCRIPTAPI_PRECHECKHEADER
	if (m_rmlui_instrument_handler_ref != LUA_NOREF &&
			m_rmlui_instrument_handler_ref != LUA_REFNIL) {
		luaL_unref(L, LUA_REGISTRYINDEX, m_rmlui_instrument_handler_ref);
	}
	m_rmlui_instrument_handler_ref = lua_registry_ref;
}

void ClientScripting::clearRmlUiInstrumentHandler()
{
	SCRIPTAPI_PRECHECKHEADER
	if (m_rmlui_instrument_handler_ref != LUA_NOREF &&
			m_rmlui_instrument_handler_ref != LUA_REFNIL) {
		luaL_unref(L, LUA_REGISTRYINDEX, m_rmlui_instrument_handler_ref);
	}
	m_rmlui_instrument_handler_ref = LUA_NOREF;
}

static void push_string_array(lua_State *L, const std::vector<std::string> &arr)
{
	lua_newtable(L);
	int i = 1;
	for (const auto &s : arr) {
		lua_pushstring(L, s.c_str());
		lua_rawseti(L, -2, i++);
	}
}

bool ClientScripting::invokeRmlUiInstrumentEvent(const UiInstrumentEvent &ev)
{
	SCRIPTAPI_PRECHECKHEADER

	if (m_rmlui_instrument_handler_ref == LUA_NOREF ||
			m_rmlui_instrument_handler_ref == LUA_REFNIL) {
		return false;
	}

	const int error_handler = PUSH_ERROR_HANDLER(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, m_rmlui_instrument_handler_ref);
	if (!lua_isfunction(L, -1)) {
		lua_pop(L, 2);
		return false;
	}

	lua_newtable(L);
	const int t = lua_gettop(L);

	lua_pushstring(L, ev.phase.c_str());
	lua_setfield(L, t, "phase");
	lua_pushstring(L, ev.surface_id.c_str());
	lua_setfield(L, t, "surface_id");

	lua_pushinteger(L, ev.mouse_x);
	lua_setfield(L, t, "mouse_x");
	lua_pushinteger(L, ev.mouse_y);
	lua_setfield(L, t, "mouse_y");

	lua_newtable(L);
	lua_pushinteger(L, ev.viewport_w);
	lua_setfield(L, -2, "w");
	lua_pushinteger(L, ev.viewport_h);
	lua_setfield(L, -2, "h");
	lua_setfield(L, t, "viewport");

	lua_newtable(L);
	lua_pushinteger(L, ev.abs_x);
	lua_setfield(L, -2, "x");
	lua_pushinteger(L, ev.abs_y);
	lua_setfield(L, -2, "y");
	lua_pushinteger(L, ev.rect_w);
	lua_setfield(L, -2, "w");
	lua_pushinteger(L, ev.rect_h);
	lua_setfield(L, -2, "h");
	lua_setfield(L, t, "rect");

	push_string_array(L, ev.id_path);
	lua_setfield(L, t, "id_path");

	if (ev.has_placement) {
		lua_newtable(L);
		lua_pushstring(L, ev.placement_anchor.c_str());
		lua_setfield(L, -2, "anchor");
		const char *region = ev.placement_region.empty() ? ev.placement_anchor.c_str()
								   : ev.placement_region.c_str();
		lua_pushstring(L, region);
		lua_setfield(L, -2, "region");
		if (!ev.placement_kind.empty()) {
			lua_pushstring(L, ev.placement_kind.c_str());
			lua_setfield(L, -2, "kind");
		}
		lua_pushinteger(L, ev.placement_x);
		lua_setfield(L, -2, "x");
		lua_pushinteger(L, ev.placement_y);
		lua_setfield(L, -2, "y");
		lua_pushboolean(L, ev.placement_keep_in_view);
		lua_setfield(L, -2, "keep_in_view");
		lua_setfield(L, t, "placement");
	} else if (!ev.placement_region.empty() || !ev.placement_kind.empty()) {
		lua_newtable(L);
		if (!ev.placement_region.empty()) {
			lua_pushstring(L, ev.placement_region.c_str());
			lua_setfield(L, -2, "region");
		}
		if (!ev.placement_kind.empty()) {
			lua_pushstring(L, ev.placement_kind.c_str());
			lua_setfield(L, -2, "kind");
		}
		lua_setfield(L, t, "placement");
	}

	if (ev.has_instrument) {
		lua_newtable(L);
		lua_pushboolean(L, ev.instrument_movable);
		lua_setfield(L, -2, "movable");
		lua_pushboolean(L, ev.instrument_resizable);
		lua_setfield(L, -2, "resizable");
		lua_pushboolean(L, ev.instrument_sticky);
		lua_setfield(L, -2, "sticky");
		lua_pushboolean(L, ev.instrument_keep_aspect);
		lua_setfield(L, -2, "keep_aspect");
		if (ev.instrument_aspect_ratio_set) {
			lua_pushnumber(L, static_cast<double>(ev.instrument_aspect_ratio));
			lua_setfield(L, -2, "aspect_ratio");
		}
		push_string_array(L, ev.instrument_anchors);
		lua_setfield(L, -2, "anchors");
		lua_setfield(L, t, "instrument");
	}

	lua_newtable(L);
	lua_pushinteger(L, ev.drag_start_mouse_x);
	lua_setfield(L, -2, "start_x");
	lua_pushinteger(L, ev.drag_start_mouse_y);
	lua_setfield(L, -2, "start_y");
	lua_pushinteger(L, ev.drag_dx);
	lua_setfield(L, -2, "dx");
	lua_pushinteger(L, ev.drag_dy);
	lua_setfield(L, -2, "dy");
	lua_setfield(L, t, "drag");

	bool start_drag = false;
	PCALL_RES(lua_pcall(L, 1, 1, error_handler));
	if (lua_isboolean(L, -1)) {
		start_drag = lua_toboolean(L, -1);
	} else if (lua_istable(L, -1)) {
		lua_getfield(L, -1, "drag");
		if (lua_isboolean(L, -1))
			start_drag = lua_toboolean(L, -1);
		lua_pop(L, 1);
	}
	lua_pop(L, 1); // result
	lua_pop(L, 1); // error handler
	return start_drag;
}
