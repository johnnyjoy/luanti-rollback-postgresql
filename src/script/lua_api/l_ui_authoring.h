// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

struct lua_State;

/** Registers `core.ui.box` and `core.ui.text` on the table at stack index `ui_top`. */
void registerUiAuthoringHelpers(lua_State *L, int ui_top);
