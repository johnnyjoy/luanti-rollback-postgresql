// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "config.h"

#include "irrlichttypes.h"

#include <memory>
#include <string>

class Client;
struct ClientEvent;

void apply_rmlui_server_network_event(Client *client, u8 op,
		std::unique_ptr<std::string> surface_id, std::unique_ptr<std::string> payload);

/**
 * Apply server-driven RmlUi UI ops without requiring ClientScripting.
 * Owns payload pointers taken from @p event (nulled after call).
 */
void apply_rmlui_server_network_event_from_client_event(Client *client, ClientEvent *event);
