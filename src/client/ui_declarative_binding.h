// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <string>
#include <vector>

/**
 * One declarative binding target on a mounted surface (UI).
 * Populated by compile_declarative_ui_from_lua; consumed by UiManager::setSurfaceState.
 */
struct UiDeclarativeBindingEntry {
	std::string state_key;
	std::string element_id;
	enum class Kind { TextValue } kind;
};
