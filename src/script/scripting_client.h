// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2013 celeron55, Perttu Ahola <celeron55@gmail.com>
// Copyright (C) 2017 nerzhul, Loic Blot <loic.blot@unix-experience.fr>

#pragma once

#include <cassert>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpp_api/s_base.h"
#include "cpp_api/s_client.h"
#include "cpp_api/s_client_common.h"
#include "cpp_api/s_modchannels.h"
#include "cpp_api/s_security.h"

class Client;
class LocalPlayer;
class Camera;
class Minimap;
struct UiInstrumentEvent;

class ClientScripting:
	virtual public ScriptApiBase,
	public ScriptApiSecurity,
	public ScriptApiClientCommon,
	public ScriptApiClient,
	public ScriptApiModChannels
{
public:
	ClientScripting(Client *client);
	void on_client_ready(LocalPlayer *localplayer);
	void on_camera_ready(Camera *camera);
	void on_minimap_ready(Minimap *minimap);

	// Per-surface button handler refs for client-authored declarative UI.
	void releaseRmlUiServerButtonRefs(const std::string &surface_id);
	void setRmlUiServerButtonRefs(const std::string &surface_id, std::vector<int> &&refs);
	void releaseAllRmlUiServerButtonRefs();

	// Global instrument handler (client-authored policy hook).
	void setRmlUiInstrumentHandler(int lua_registry_ref);
	void clearRmlUiInstrumentHandler();
	bool invokeRmlUiInstrumentEvent(const UiInstrumentEvent &ev);

protected:
	// from ScriptApiSecurity:
	bool checkPathInternal(const std::string &abs_path, bool write_required,
		bool *write_allowed) override {
		warningstream << "IO API called in client scripting" << std::endl;
		assert(0);
		return false;
	}
	bool modNamesAreTrusted() override { return true; }

private:
	virtual void InitializeModApi(lua_State *L, int top);

	std::unordered_map<std::string, std::vector<int>> m_rmlui_server_button_refs;
	int m_rmlui_instrument_handler_ref = LUA_NOREF;
};
