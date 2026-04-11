// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

struct lua_State;

/** Registers client `core.ui` API: `panel`, `bind`, and handle userdata (no bridge verbs). */
void registerUiClientPublic(lua_State *L, int ui_top);
