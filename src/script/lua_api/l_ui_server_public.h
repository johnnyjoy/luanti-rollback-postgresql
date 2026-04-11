// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "config.h"

#include "irrlichttypes.h"

#include <string>

struct lua_State;
class Server;
class RemotePlayer;

/** Client → server RmlUi button action (surface id + button index); runs server Lua handler. */
void ui_server_dispatch_ui_action(lua_State *L, Server *server, RemotePlayer *player,
		const std::string &surface_id, u32 button_index);

/** Client → server instrument event (surface id + JSON payload); runs server Lua handler. */
void ui_server_dispatch_ui_instrument_event(lua_State *L, Server *server, RemotePlayer *player,
		const std::string &surface_id, const std::string &payload_json);

/** Registers public server `core.ui` API: `panel`, `bind`, and handle userdata. */
void registerUiServerPublic(lua_State *L, int ui_top);
