// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "rmlui_server_apply.h"

#include "client/client.h"
#include "client/clientevent.h"
#include "client/ui_declarative_compile.h"
#include "client/ui_manager.h"
#include "log.h"

#include <json/json.h>
#include <cctype>
#include <cmath>
#include <map>
#include <memory>
#include <vector>

void apply_rmlui_server_network_event(Client *client, u8 op,
		std::unique_ptr<std::string> surface_id, std::unique_ptr<std::string> payload)
{
	if (!surface_id)
		return;

	UiManager *ui = client->getUiManager();
	if (!ui || !ui->isReady()) {
		client->enqueuePendingRmlUiServerEvent(op, std::move(surface_id), std::move(payload));
		return;
	}

	const std::string &sid = *surface_id;

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

	// op=3: instrument mode control (server -> client). Surface id is ignored.
	// Payload is JSON: { "active": true|false }.
	if (op == 3) {
		if (!payload)
			return;
		Json::CharReaderBuilder builder;
		const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
		Json::Value root;
		std::string errs;
		const std::string &j = *payload;
		if (!reader->parse(j.c_str(), j.c_str() + j.size(), &root, &errs)) {
			warningstream << "[RmlUi] instrument_mode JSON parse failed: " << errs << std::endl;
			return;
		}
		if (!root.isObject() || !root.isMember("active") || !root["active"].isBool()) {
			warningstream << "[RmlUi] instrument_mode JSON must be {active:bool}" << std::endl;
			return;
		}
		if (root["active"].asBool())
			ui->enterInstrumentMode();
		else
			ui->exitInstrumentMode();
		return;
	}

	// op=4: instrument apply patch (server -> client).
	// Payload is JSON object with optional:
	// - placement: { anchor, x, y, keep_in_view }
	// - styles: { "<element_id>": { "<prop>": "<value>", ... }, ... }
	if (op == 4) {
		if (!payload)
			return;
		Json::CharReaderBuilder builder;
		const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
		Json::Value root;
		std::string errs;
		const std::string &j = *payload;
		if (!reader->parse(j.c_str(), j.c_str() + j.size(), &root, &errs)) {
			warningstream << "[RmlUi] instrument_apply JSON parse failed: " << errs << std::endl;
			return;
		}
		if (!root.isObject()) {
			warningstream << "[RmlUi] instrument_apply JSON must be an object" << std::endl;
			return;
		}

		if (root.isMember("placement") && root["placement"].isObject()) {
			const Json::Value &p = root["placement"];
			UiSurfacePositioning pos;
			bool ok = true;
			if (p.isMember("anchor") && p["anchor"].isString()) {
				if (auto a = parse_anchor(p["anchor"].asString()))
					pos.anchor = *a;
				else
					ok = false;
			}
			if (p.isMember("x") && p["x"].isNumeric())
				pos.x = static_cast<s32>(std::floor(p["x"].asDouble() + 0.5));
			if (p.isMember("y") && p["y"].isNumeric())
				pos.y = static_cast<s32>(std::floor(p["y"].asDouble() + 0.5));
			if (p.isMember("keep_in_view") && p["keep_in_view"].isBool())
				pos.keep_in_view = p["keep_in_view"].asBool();
			if (ok) {
				std::string em;
				(void)ui->applySurfacePositioning(sid, pos, em);
			}
		}

		if (root.isMember("styles") && root["styles"].isObject()) {
			const Json::Value &styles = root["styles"];
			for (const auto &eid : styles.getMemberNames()) {
				const Json::Value &props = styles[eid];
				if (!props.isObject())
					continue;
				std::map<std::string, std::string> m;
				for (const auto &k : props.getMemberNames()) {
					const Json::Value &v = props[k];
					if (v.isString())
						m[k] = v.asString();
					else if (v.isInt())
						m[k] = std::to_string(v.asLargestInt());
					else if (v.isUInt())
						m[k] = std::to_string(v.asLargestUInt());
					else if (v.isDouble())
						m[k] = std::to_string(v.asDouble());
					else if (v.isBool())
						m[k] = v.asBool() ? "true" : "false";
				}
				if (!m.empty()) {
					std::string em;
					(void)ui->setElementProperties(sid, eid, m, em);
				}
			}
		}
		return;
	}

	if (op == 2) {
		ui->unmount(sid);
		return;
	}

	if (op == 1) {
		if (!payload)
			return;
		Json::CharReaderBuilder builder;
		const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
		Json::Value root;
		std::string errs;
		const std::string &j = *payload;
		if (!reader->parse(j.c_str(), j.c_str() + j.size(), &root, &errs)) {
			warningstream << "[RmlUi] set_state JSON parse failed: " << errs
					<< std::endl;
			return;
		}
		if (!root.isObject()) {
			warningstream << "[RmlUi] set_state JSON must be an object" << std::endl;
			return;
		}
		std::map<std::string, std::string> state;
		for (const auto &name : root.getMemberNames()) {
			const Json::Value &v = root[name];
			if (v.isString())
				state[name] = v.asString();
			else if (v.isInt())
				state[name] = std::to_string(v.asLargestInt());
			else if (v.isUInt())
				state[name] = std::to_string(v.asLargestUInt());
			else if (v.isDouble())
				state[name] = std::to_string(v.asDouble());
			else if (v.isBool())
				state[name] = v.asBool() ? "true" : "false";
		}
		std::string em;
		ui->setSurfaceState(sid, state, em);
		return;
	}

	if (op != 0 || !payload)
		return;

	Json::CharReaderBuilder b;
	const std::unique_ptr<Json::CharReader> r(b.newCharReader());
	Json::Value root;
	std::string errs;
	const std::string &j = *payload;
	if (!r->parse(j.c_str(), j.c_str() + j.size(), &root, &errs)) {
		warningstream << "[RmlUi] mount JSON parse failed: " << errs << std::endl;
		return;
	}

	auto has_author_positioning = [](const Json::Value &tree) -> bool {
		if (!tree.isObject() || !tree.isMember("style"))
			return false;
		const Json::Value &st = tree["style"];
		if (!st.isObject())
			return false;
		static const char *keys[] = {"position", "top", "left", "right", "bottom", "transform",
				"margin_left", "margin_top", "margin_right", "margin_bottom"};
		for (const char *k : keys) {
			if (st.isMember(k))
				return true;
		}
		return false;
	};

	std::vector<UiDeclarativeBindingEntry> bindings;
	std::vector<UiDeclarativeBindingEntry> *bind_ptr = nullptr;
	std::string rml, compile_err;
	int max_btn = 0;
	bool modal_mount = false;
	UiDismissPolicy dismiss_policy = UiDismissPolicy::None;
	if (!compile_declarative_ui_from_json(root, rml, compile_err, &bindings, &max_btn, &modal_mount,
			    &dismiss_policy)) {
		warningstream << "[RmlUi] mount compile failed: " << compile_err
				<< std::endl;
		return;
	}
	if (!bindings.empty())
		bind_ptr = &bindings;

	ui->unmount(sid);

	// Minimal positioning spec (surface-level):
	// - If the mount wrapper provides explicit anchor/x/y/keep_in_view, honor it.
	// - Else if this is a modal surface and the author did not specify their own position,
	//   default to centered + keep_in_view.
	UiSurfacePositioning pos_spec;
	const UiSurfacePositioning *pos_ptr = nullptr;
	UiSurfaceLayout layout_spec;
	const UiSurfaceLayout *layout_ptr = nullptr;
	UiLayer layer = UiLayer::OVERLAY;
	bool explicit_pos = false;
	Json::Value tree = root;
	if (root.isMember("body") && root["body"].isObject())
		tree = root["body"];

	if (root.isMember("layer") && root["layer"].isString()) {
		const std::string lt = root["layer"].asString();
		if (lt == "hud")
			layer = UiLayer::HUD;
	}

	if (root.isMember("anchor") && root["anchor"].isString()) {
		const std::string tok = root["anchor"].asString();
		// Keep parsing minimal; unknown tokens simply disable explicit positioning.
		// (We intentionally do not error for unknown anchors to keep server-driven UI robust.)
		if (auto a = parse_anchor(tok)) {
			pos_spec.anchor = *a;
			explicit_pos = true;
		}
	}
	if (root.isMember("x") && root["x"].isNumeric()) {
		pos_spec.x = static_cast<s32>(std::floor(root["x"].asDouble() + 0.5));
		explicit_pos = true;
	}
	if (root.isMember("y") && root["y"].isNumeric()) {
		pos_spec.y = static_cast<s32>(std::floor(root["y"].asDouble() + 0.5));
		explicit_pos = true;
	}
	if (root.isMember("keep_in_view") && root["keep_in_view"].isBool()) {
		pos_spec.keep_in_view = root["keep_in_view"].asBool();
		explicit_pos = true;
	}

	if (root.isMember("instrument") && root["instrument"].isObject()) {
		const Json::Value &lo = root["instrument"];
		if (lo.isMember("movable") && lo["movable"].isBool())
			layout_spec.movable = lo["movable"].asBool();
		if (lo.isMember("resizable") && lo["resizable"].isBool())
			layout_spec.resizable = lo["resizable"].asBool();
		if (lo.isMember("sticky") && lo["sticky"].isBool())
			layout_spec.sticky = lo["sticky"].asBool();
		if (lo.isMember("keep_aspect") && lo["keep_aspect"].isBool())
			layout_spec.keep_aspect = lo["keep_aspect"].asBool();
		if (lo.isMember("aspect_ratio") && lo["aspect_ratio"].isNumeric()) {
			layout_spec.aspect_ratio = static_cast<float>(lo["aspect_ratio"].asDouble());
			layout_spec.aspect_ratio_set = true;
		}
		if (lo.isMember("drag_handle") && lo["drag_handle"].isString())
			layout_spec.drag_handle_id = lo["drag_handle"].asString();
		if (lo.isMember("anchors") && lo["anchors"].isArray()) {
			for (Json::ArrayIndex i = 0; i < lo["anchors"].size(); ++i) {
				if (!lo["anchors"][i].isString())
					continue;
				if (auto a = parse_anchor(lo["anchors"][i].asString()))
					layout_spec.allowed_anchors.push_back(*a);
			}
		}
		layout_ptr = &layout_spec;
	}

	const bool author_pos = has_author_positioning(tree);
	if (explicit_pos) {
		pos_ptr = &pos_spec;
	} else if (modal_mount && !author_pos) {
		// Default modal behavior for deterministic, reachable UI.
		pos_spec.anchor = UiAnchor::Center;
		pos_spec.x = 0;
		pos_spec.y = 0;
		pos_spec.keep_in_view = true;
		pos_ptr = &pos_spec;
	}

	std::string doc_url = std::string("rmlui://srv_ui/") + sid;
	std::string em;
	const std::vector<UiDeclarativeBindingEntry> *bind_arg =
			bind_ptr && !bind_ptr->empty() ? bind_ptr : nullptr;
	const int lua_btn_count = max_btn;
	UiMountOptions mount_opts;
	mount_opts.lua_button_count = lua_btn_count;
	mount_opts.bindings = bind_arg;
	mount_opts.modal_document = modal_mount;
	mount_opts.dismiss_policy = dismiss_policy;
	mount_opts.positioning = pos_ptr;
	mount_opts.layout = layout_ptr;
	if (!ui->mount(sid, layer, 0, rml.c_str(), doc_url.c_str(), em, mount_opts)) {
		warningstream << "[RmlUi] mount failed: " << em << std::endl;
		return;
	}
}

void apply_rmlui_server_network_event_from_client_event(Client *client, ClientEvent *event)
{
	std::unique_ptr<std::string> surface_id(event->rmlui_server.surface_id);
	std::unique_ptr<std::string> payload(event->rmlui_server.payload);
	event->rmlui_server.surface_id = nullptr;
	event->rmlui_server.payload = nullptr;
	const u8 op = event->rmlui_server.op;

	if (!surface_id)
		return;

	apply_rmlui_server_network_event(client, op, std::move(surface_id), std::move(payload));
}
