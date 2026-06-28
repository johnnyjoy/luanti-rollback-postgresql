// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "ui_manager.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <unordered_map>
#include <vector>

#include "client/ui_font.h"
#include "client/inputhandler.h"
#include "client/keys.h"
#include "client/renderingengine.h"
#include "client/rmlui_irrlicht_render_interface.h"
#include "log.h"
#include "settings.h"
#include "util/numeric.h"

#include <cstdlib>
#include <cstring>
#include <string>

struct SurfaceEntry {
	std::string id;
	UiLayer layer = UiLayer::OVERLAY;
	int priority = 0;
	bool visible = true;
	/// Passed to ElementDocument::Show when toggling visibility; matches initial mount.
	bool modal_document = false;
	UiDismissPolicy dismiss_policy = UiDismissPolicy::None;
	std::optional<UiSurfacePositioning> positioning;
	std::optional<UiSurfaceLayout> layout;
	/// Last applied absolute position (for sticky HUDs when the viewport resizes).
	s32 last_viewport_w = 0;
	s32 last_viewport_h = 0;
	s32 last_applied_left = -1;
	s32 last_applied_top = -1;
	Rml::ElementDocument *document = nullptr;
	struct FocusIsolationBackup {
		bool had_focus = false;
		Rml::String focus;
		bool had_tab_index = false;
		Rml::String tab_index;
		bool had_nav = false;
		Rml::String nav;
	};
	/// Document-level focus isolation backup: original property presence + values before overriding.
	/// Keyed by element pointer; valid only for the lifetime of the mounted document.
	std::unordered_map<Rml::Element *, FocusIsolationBackup> focus_isolation_backup;
	/// From declarative compile (`luaui_btn_0` …); used to validate hover-based button dispatch.
	int declarative_button_count = 0;
	/// Populated when mounting declarative UI with core.ui.bind; drives setSurfaceState patching.
	std::vector<UiDeclarativeBindingEntry> binding_targets;
};

namespace {

static int rml_key_modifiers_from(const UiKeyEvent &e)
{
	int mod = 0;
	if (e.shift)
		mod |= Rml::Input::KM_SHIFT;
	if (e.ctrl)
		mod |= Rml::Input::KM_CTRL;
	return mod;
}

static Rml::Input::KeyIdentifier rml_key_identifier_from_irr(EKEY_CODE k)
{
	using Rml::Input::KeyIdentifier;
	using namespace Rml::Input;
	switch (k) {
	case KEY_BACK: return KI_BACK;
	case KEY_TAB: return KI_TAB;
	case KEY_RETURN: return KI_RETURN;
	case KEY_ESCAPE: return KI_ESCAPE;
	case KEY_SPACE: return KI_SPACE;

	case KEY_PRIOR: return KI_PRIOR;
	case KEY_NEXT: return KI_NEXT;
	case KEY_END: return KI_END;
	case KEY_HOME: return KI_HOME;
	case KEY_LEFT: return KI_LEFT;
	case KEY_UP: return KI_UP;
	case KEY_RIGHT: return KI_RIGHT;
	case KEY_DOWN: return KI_DOWN;
	case KEY_INSERT: return KI_INSERT;
	case KEY_DELETE: return KI_DELETE;

	case KEY_LSHIFT: return KI_LSHIFT;
	case KEY_RSHIFT: return KI_RSHIFT;
	case KEY_LCONTROL: return KI_LCONTROL;
	case KEY_RCONTROL: return KI_RCONTROL;
	case KEY_LMENU: return KI_LMENU;
	case KEY_RMENU: return KI_RMENU;

	case KEY_F1: return KI_F1;
	case KEY_F2: return KI_F2;
	case KEY_F3: return KI_F3;
	case KEY_F4: return KI_F4;
	case KEY_F5: return KI_F5;
	case KEY_F6: return KI_F6;
	case KEY_F7: return KI_F7;
	case KEY_F8: return KI_F8;
	case KEY_F9: return KI_F9;
	case KEY_F10: return KI_F10;
	case KEY_F11: return KI_F11;
	case KEY_F12: return KI_F12;

	case KEY_KEY_0: return KI_0;
	case KEY_KEY_1: return KI_1;
	case KEY_KEY_2: return KI_2;
	case KEY_KEY_3: return KI_3;
	case KEY_KEY_4: return KI_4;
	case KEY_KEY_5: return KI_5;
	case KEY_KEY_6: return KI_6;
	case KEY_KEY_7: return KI_7;
	case KEY_KEY_8: return KI_8;
	case KEY_KEY_9: return KI_9;

	case KEY_KEY_A: return KI_A;
	case KEY_KEY_B: return KI_B;
	case KEY_KEY_C: return KI_C;
	case KEY_KEY_D: return KI_D;
	case KEY_KEY_E: return KI_E;
	case KEY_KEY_F: return KI_F;
	case KEY_KEY_G: return KI_G;
	case KEY_KEY_H: return KI_H;
	case KEY_KEY_I: return KI_I;
	case KEY_KEY_J: return KI_J;
	case KEY_KEY_K: return KI_K;
	case KEY_KEY_L: return KI_L;
	case KEY_KEY_M: return KI_M;
	case KEY_KEY_N: return KI_N;
	case KEY_KEY_O: return KI_O;
	case KEY_KEY_P: return KI_P;
	case KEY_KEY_Q: return KI_Q;
	case KEY_KEY_R: return KI_R;
	case KEY_KEY_S: return KI_S;
	case KEY_KEY_T: return KI_T;
	case KEY_KEY_U: return KI_U;
	case KEY_KEY_V: return KI_V;
	case KEY_KEY_W: return KI_W;
	case KEY_KEY_X: return KI_X;
	case KEY_KEY_Y: return KI_Y;
	case KEY_KEY_Z: return KI_Z;

	default:
		return KI_UNKNOWN;
	}
}

static void collect_elements_depth_first(Rml::Element *root, std::vector<Rml::Element *> &out);
static Rml::ElementDocument *find_topmost_visible_modal_document(UiManager::Impl *impl);
static void update_modal_focus_trap(UiManager::Impl *impl);

static std::string escape_for_rml_inner_text(const std::string &s)
{
	std::string out;
	out.reserve(s.size());
	for (unsigned char uc : s) {
		char c = static_cast<char>(uc);
		switch (c) {
		case '&':
			out += "&amp;";
			break;
		case '<':
			out += "&lt;";
			break;
		case '>':
			out += "&gt;";
			break;
		case '"':
			out += "&quot;";
			break;
		default:
			out += c;
			break;
		}
	}
	return out;
}

static bool g_ui_font_faces_loaded = false;
/// Rml::Initialise/Shutdown are global; release builds disable the double-Init assert. Refcount so
/// nested failure paths and multiple Game sessions stay paired without UB.
static int g_rmlui_init_refcount = 0;

static void rmlui_release_one_init_ref()
{
	assert(g_rmlui_init_refcount > 0);
	g_rmlui_init_refcount--;
	if (g_rmlui_init_refcount == 0) {
		Rml::Shutdown();
		g_ui_font_faces_loaded = false;
	}
}

static bool load_rmlui_font_faces(std::string &error_message)
{
	if (g_ui_font_faces_loaded)
		return true;
	if (!g_settings) {
		error_message = "RmlUi: settings not available for font loading";
		return false;
	}
	static const char *const keys[] = {
		"font_path",
		"font_path_bold",
		"font_path_italic",
		"font_path_bold_italic",
	};
	// Path of the first primary-tier face LoadFontFace actually accepted; this (not settings
	// order alone) must drive default font-family for generated RML.
	std::string first_loaded_primary_path;
	bool any = false;
	bool fallback_loaded = false;
	for (const char *key : keys) {
		const std::string path = g_settings->get(key);
		if (path.empty())
			continue;
		infostream << "[RmlUi] RmlUi attempting primary font: " << key << " -> " << path
				<< std::endl;
		if (Rml::LoadFontFace(Rml::String(path), false)) {
			any = true;
			if (first_loaded_primary_path.empty())
				first_loaded_primary_path = path;
			infostream << "[RmlUi] RmlUi loaded primary font face: " << key << std::endl;
		} else {
			warningstream << "[RmlUi] RmlUi failed to load primary font: " << key << " -> "
					<< path << std::endl;
		}
	}
	const std::string fallback = g_settings->get("fallback_font_path");
	if (!fallback.empty()) {
		infostream << "[RmlUi] RmlUi attempting fallback font: " << fallback << std::endl;
		if (Rml::LoadFontFace(Rml::String(fallback), true)) {
			any = true;
			fallback_loaded = true;
			infostream << "[RmlUi] RmlUi loaded fallback font face (broad glyph coverage)"
					<< std::endl;
		} else {
			warningstream << "[RmlUi] RmlUi failed to load fallback font: " << fallback
					<< std::endl;
		}
	}
	if (!any) {
		error_message = "RmlUi: could not load any font face (check font_path settings)";
		return false;
	}

	std::string path_for_default_family = first_loaded_primary_path;
	if (path_for_default_family.empty() && fallback_loaded)
		path_for_default_family = fallback;

	ui_font_set_resolved_primary_path(path_for_default_family);
	const std::string resolved_family = ui_font_family_from_font_path(path_for_default_family);
	ui_font_refresh_builtin_overlay_rml_from_path(path_for_default_family);

	infostream << "[RmlUi] font init summary: default_family=\"" << resolved_family
			<< "\" from path=\"" << path_for_default_family << "\""
			<< "; primary_face_loaded_first=" << (first_loaded_primary_path.empty() ? "no" : "yes")
			<< "; fallback_loaded=" << (fallback_loaded ? "yes" : "no") << std::endl;

	g_ui_font_faces_loaded = true;
	return true;
}

} // namespace

struct UiManager::Impl {
	std::unique_ptr<RmlUiIrrlichtRenderInterface> rml_render;
	Rml::Context *rml_context = nullptr;
	bool rml_library_initialized = false;
	std::unordered_map<std::string, SurfaceEntry> surfaces;
	/// Order of successful mounts (unordered_map iteration is not stable).
	std::vector<std::string> surface_mount_sequence;
};

namespace {
// (investigation-only logging / verification harness removed)

static void collect_elements_depth_first(Rml::Element *root, std::vector<Rml::Element *> &out)
{
	if (!root)
		return;
	out.push_back(root);
	const int n = root->GetNumChildren();
	for (int i = 0; i < n; ++i)
		collect_elements_depth_first(root->GetChild(i), out);
}

static Rml::Element *find_first_autofocus_element(Rml::ElementDocument *doc)
{
	if (!doc)
		return nullptr;
	std::vector<Rml::Element *> els;
	els.reserve(256);
	collect_elements_depth_first(doc, els);
	for (Rml::Element *e : els) {
		if (!e)
			continue;
		if (e->HasAttribute(Rml::String("autofocus")))
			return e;
	}
	return nullptr;
}

static Rml::ElementDocument *find_topmost_visible_modal_document(UiManager::Impl *impl)
{
	if (!impl || !impl->rml_context)
		return nullptr;
	const int ndoc = impl->rml_context->GetNumDocuments();
	for (int i = ndoc - 1; i >= 0; --i) {
		Rml::ElementDocument *doc = impl->rml_context->GetDocument(i);
		if (!doc)
			continue;
		for (const auto &p : impl->surfaces) {
			const SurfaceEntry &se = p.second;
			if (!se.visible || !se.document || se.document != doc)
				continue;
			if (se.modal_document)
				return doc;
			break;
		}
	}
	return nullptr;
}

static Rml::ElementDocument *find_topmost_visible_non_overlay_document(UiManager::Impl *impl)
{
	if (!impl || !impl->rml_context)
		return nullptr;
	Rml::Context *ctx = impl->rml_context;
	const int ndoc = ctx->GetNumDocuments();
	for (int i = ndoc - 1; i >= 0; --i) {
		Rml::ElementDocument *doc = ctx->GetDocument(i);
		if (!doc)
			continue;
		for (const auto &p : impl->surfaces) {
			const SurfaceEntry &se = p.second;
			if (!se.visible || !se.document || se.document != doc)
				continue;
			// Skip the built-in overlay surface (diagnostic HUD). It should never become the focus owner.
			if (p.first == UiManager::OVERLAY_SURFACE_ID)
				break;
			return doc;
		}
	}
	return nullptr;
}

static const SurfaceEntry *find_topmost_visible_surface_entry(UiManager::Impl *impl, std::string *out_surface_id)
{
	if (!impl || !impl->rml_context)
		return nullptr;
	const int ndoc = impl->rml_context->GetNumDocuments();
	for (int i = ndoc - 1; i >= 0; --i) {
		Rml::ElementDocument *doc = impl->rml_context->GetDocument(i);
		if (!doc)
			continue;
		for (const auto &p : impl->surfaces) {
			const SurfaceEntry &se = p.second;
			if (!se.visible || !se.document || se.document != doc)
				continue;
			if (out_surface_id)
				*out_surface_id = p.first;
			return &se;
		}
	}
	return nullptr;
}

static void update_modal_focus_trap(UiManager::Impl *impl)
{
	if (!impl)
		return;
	Rml::ElementDocument *top_modal = find_topmost_visible_modal_document(impl);
	for (auto &p : impl->surfaces) {
		SurfaceEntry &se = p.second;
		if (!se.visible || !se.document)
			continue;

		// Document-level focus isolation:
		// - When a modal is active: all other documents must be excluded from focus/tab navigation.
		// - When no modal: restore original focus/tab-index computed values for any previously isolated documents.
		if (!top_modal) {
			// Restore if previously isolated.
			if (!se.focus_isolation_backup.empty()) {
				for (auto &bp : se.focus_isolation_backup) {
					Rml::Element *el = bp.first;
					if (!el)
						continue;
					const SurfaceEntry::FocusIsolationBackup &bak = bp.second;
					if (bak.had_focus)
						el->SetProperty(Rml::String("focus"), bak.focus);
					else
						el->RemoveProperty(Rml::String("focus"));
					if (bak.had_tab_index)
						el->SetProperty(Rml::String("tab-index"), bak.tab_index);
					else
						el->RemoveProperty(Rml::String("tab-index"));
					if (bak.had_nav)
						el->SetProperty(Rml::String("nav"), bak.nav);
					else
						el->RemoveProperty(Rml::String("nav"));
				}
				se.focus_isolation_backup.clear();
			}
			continue;
		}

		if (se.document == top_modal) {
			// Ensure the modal document remains focusable; restore if it was previously isolated.
			if (!se.focus_isolation_backup.empty()) {
				for (auto &bp : se.focus_isolation_backup) {
					Rml::Element *el = bp.first;
					if (!el)
						continue;
					const SurfaceEntry::FocusIsolationBackup &bak = bp.second;
					if (bak.had_focus)
						el->SetProperty(Rml::String("focus"), bak.focus);
					else
						el->RemoveProperty(Rml::String("focus"));
					if (bak.had_tab_index)
						el->SetProperty(Rml::String("tab-index"), bak.tab_index);
					else
						el->RemoveProperty(Rml::String("tab-index"));
					if (bak.had_nav)
						el->SetProperty(Rml::String("nav"), bak.nav);
					else
						el->RemoveProperty(Rml::String("nav"));
				}
				se.focus_isolation_backup.clear();
			}
			continue;
		}

		// Lock focus on all elements in non-modal documents (raw RML, widgets, etc).
		std::vector<Rml::Element *> els;
		els.reserve(256);
		collect_elements_depth_first(se.document, els);
		for (Rml::Element *el : els) {
			if (!el)
				continue;
			// Save computed properties before overriding.
			if (se.focus_isolation_backup.find(el) == se.focus_isolation_backup.end()) {
				const Rml::Property *pfocus = el->GetProperty(Rml::String("focus"));
				const Rml::Property *ptab = el->GetProperty(Rml::String("tab-index"));
				const Rml::Property *pnav = el->GetProperty(Rml::String("nav"));
				SurfaceEntry::FocusIsolationBackup bak;
				bak.had_focus = (pfocus != nullptr);
				if (pfocus)
					bak.focus = pfocus->ToString();
				bak.had_tab_index = (ptab != nullptr);
				if (ptab)
					bak.tab_index = ptab->ToString();
				bak.had_nav = (pnav != nullptr);
				if (pnav)
					bak.nav = pnav->ToString();
				se.focus_isolation_backup.emplace(el, std::move(bak));
			}
			el->SetProperty(Rml::String("focus"), Rml::String("none"));
			el->SetProperty(Rml::String("tab-index"), Rml::String("none"));
			el->SetProperty(Rml::String("nav"), Rml::String("none"));
		}
	}
}

} // namespace

static constexpr const char RMLUI_CONTEXT_NAME[] = "main";

// (investigation-only DOM / mount diagnostics removed)

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

/// Classify instrument position into a stable 9-way region using viewport thirds of the rect center.
/// Deterministic: same rect + viewport → same labels (used for adaptation; not tied to anchor string).
static void classify_placement_from_geometry(s32 abs_x, s32 abs_y, s32 w, s32 h, s32 vw, s32 vh,
		std::string *out_region, std::string *out_kind)
{
	if (!out_region || !out_kind)
		return;
	if (w <= 0 || h <= 0 || vw <= 0 || vh <= 0) {
		*out_region = "center";
		*out_kind = "center";
		return;
	}

	// Touch margins: large HUDs whose *center* sits in the middle third still "dock" visually to a
	// viewport edge/corner — improves corner/edge adaptation (TEST_021 E/F).
	const s32 cap = std::max(1, std::min(vw, vh) / 8);
	const s32 m = std::min(24, std::max(8, cap));
	const bool touch_l = abs_x <= m;
	const bool touch_r = (abs_x + w) >= vw - m;
	const bool touch_t = abs_y <= m;
	const bool touch_b = (abs_y + h) >= vh - m;

	const s32 cx = abs_x + w / 2;
	const s32 cy = abs_y + h / 2;
	const s32 t1x = vw / 3;
	const s32 t2x = (vw * 2) / 3;
	const s32 t1y = vh / 3;
	const s32 t2y = (vh * 2) / 3;

	int col = 1;
	if (cx < t1x)
		col = 0;
	else if (cx < t2x)
		col = 1;
	else
		col = 2;

	int row = 1;
	if (cy < t1y)
		row = 0;
	else if (cy < t2y)
		row = 1;
	else
		row = 2;

	static const char *const kRegions[3][3] = {
			{"top-left", "top", "top-right"},
			{"left", "center", "right"},
			{"bottom-left", "bottom", "bottom-right"},
	};
	*out_region = kRegions[row][col];

	const bool span_h = touch_l && touch_r;
	const bool span_v = touch_t && touch_b;

	// Edge-touch overrides are conservative: also require centroid column/row to match, so a wide
	// HUD moved slightly left does not become "left" (TEST_021 D premature vertical).
	if (touch_t && touch_l && !span_h)
		*out_region = "top-left";
	else if (touch_t && touch_r && !span_h)
		*out_region = "top-right";
	else if (touch_b && touch_l && !span_h)
		*out_region = "bottom-left";
	else if (touch_b && touch_r && !span_h)
		*out_region = "bottom-right";
	else if (touch_t && span_h)
		*out_region = "top";
	else if (touch_b && span_h)
		*out_region = "bottom";
	else if (touch_l && !touch_r && !span_h && col == 0)
		*out_region = "left";
	else if (touch_r && !touch_l && !span_h && col == 2)
		*out_region = "right";

	const std::string &r = *out_region;
	if (r == "center")
		*out_kind = "center";
	else if (r == "top" || r == "bottom" || r == "left" || r == "right")
		*out_kind = "edge";
	else
		*out_kind = "corner";
}

static void classify_placement_from_anchor_token(const std::string &anchor, std::string *out_region,
		std::string *out_kind)
{
	if (!out_region || !out_kind)
		return;
	*out_region = anchor;
	if (anchor == "center")
		*out_kind = "center";
	else if (anchor == "top" || anchor == "bottom" || anchor == "left" || anchor == "right")
		*out_kind = "edge";
	else
		*out_kind = "corner";
}

static void apply_surface_positioning(UiManager::Impl *impl, SurfaceEntry &se, int vw, int vh)
{
	if (!impl || !impl->rml_context || !se.document || !se.positioning)
		return;
	if (vw <= 0 || vh <= 0)
		return;

	Rml::Element *pos = se.document->GetElementById(Rml::String("exp_pos"));
	if (!pos)
		return;

	// Layout must have happened for size to be meaningful.
	const Rml::Vector2f sz = pos->GetBox().GetSize();
	const float wf = sz.x;
	const float hf = sz.y;
	if (!(wf > 1.f) || !(hf > 1.f))
		return;

	const int w = static_cast<int>(std::floor(wf + 0.5f));
	const int h = static_cast<int>(std::floor(hf + 0.5f));

	const UiSurfacePositioning p = *se.positioning;

	auto base_x = [&]() -> int {
		switch (p.anchor) {
		case UiAnchor::Center: return (vw - w) / 2;
		case UiAnchor::TopLeft: return 0;
		case UiAnchor::TopRight: return vw - w;
		case UiAnchor::BottomLeft: return 0;
		case UiAnchor::BottomRight: return vw - w;
		case UiAnchor::Top: return (vw - w) / 2;
		case UiAnchor::Bottom: return (vw - w) / 2;
		case UiAnchor::Left: return 0;
		case UiAnchor::Right: return vw - w;
		}
		return (vw - w) / 2;
	}();

	auto base_y = [&]() -> int {
		switch (p.anchor) {
		case UiAnchor::Center: return (vh - h) / 2;
		case UiAnchor::TopLeft: return 0;
		case UiAnchor::TopRight: return 0;
		case UiAnchor::BottomLeft: return vh - h;
		case UiAnchor::BottomRight: return vh - h;
		case UiAnchor::Top: return 0;
		case UiAnchor::Bottom: return vh - h;
		case UiAnchor::Left: return (vh - h) / 2;
		case UiAnchor::Right: return (vh - h) / 2;
		}
		return (vh - h) / 2;
	}();

	int x = base_x + static_cast<int>(p.x);
	int y = base_y + static_cast<int>(p.y);

	const bool viewport_changed = se.last_viewport_w > 0 && se.last_viewport_h > 0 &&
			(se.last_viewport_w != vw || se.last_viewport_h != vh);
	if (viewport_changed && se.layout && se.layout->sticky && se.last_applied_left >= 0 &&
			se.last_applied_top >= 0) {
		// Keep the HUD visually anchored while the window resizes instead of re-deriving
		// from anchor+offset (which jumps when the box size changes).
		x = se.last_applied_left;
		y = se.last_applied_top;
	}

	if (p.keep_in_view) {
		const int max_x = std::max(0, vw - w);
		const int max_y = std::max(0, vh - h);
		x = std::max(0, std::min(max_x, x));
		y = std::max(0, std::min(max_y, y));
	}

	pos->SetProperty(Rml::String("position"), Rml::String("fixed"));
	pos->SetProperty(Rml::String("left"), Rml::String(std::to_string(x) + "px"));
	pos->SetProperty(Rml::String("top"), Rml::String(std::to_string(y) + "px"));

	se.last_applied_left = x;
	se.last_applied_top = y;
	se.last_viewport_w = vw;
	se.last_viewport_h = vh;
}

bool mount_surface_impl(UiManager::Impl *impl, const UiMountSurfaceDesc &desc,
		std::string &error_message)
{
	if (!impl || !impl->rml_context) {
		error_message = "RmlUi: no context";
		return false;
	}
	if (impl->surfaces.count(desc.surface_id) != 0) {
		error_message = "RmlUi: surface id already mounted";
		return false;
	}

	Rml::ElementDocument *doc = impl->rml_context->LoadDocumentFromMemory(desc.rml_memory,
			desc.document_url);
	if (!doc) {
		error_message = "RmlUi: LoadDocumentFromMemory failed";
		return false;
	}

	const UiMountOptions &opt = desc.options;
	SurfaceEntry entry;
	entry.id = desc.surface_id;
	entry.layer = desc.layer;
	entry.priority = desc.priority;
	entry.visible = true;
	entry.modal_document = opt.modal_document;
	entry.dismiss_policy = opt.dismiss_policy;
	entry.document = doc;
	entry.declarative_button_count = opt.lua_button_count;
	if (opt.positioning)
		entry.positioning = *opt.positioning;
	if (opt.layout)
		entry.layout = *opt.layout;
	if (opt.bindings)
		entry.binding_targets = *opt.bindings;
	doc->Show(opt.modal_document ? Rml::ModalFlag::Modal : Rml::ModalFlag::None);

	impl->surfaces.emplace(desc.surface_id, std::move(entry));
	impl->surface_mount_sequence.push_back(desc.surface_id);
	return true;
}

void close_surface_documents_impl(UiManager::Impl *impl)
{
	if (!impl)
		return;
	for (auto &p : impl->surfaces) {
		if (p.second.document) {
			p.second.document->Close();
			p.second.document = nullptr;
		}
	}
	impl->surfaces.clear();
	impl->surface_mount_sequence.clear();
}

bool any_surface_visible_for_render_impl(const UiManager::Impl *impl)
{
	if (!impl)
		return false;
	for (const auto &p : impl->surfaces) {
		if (p.second.visible && p.second.document)
			return true;
	}
	return false;
}

size_t count_surfaces_total(const UiManager::Impl *impl)
{
	return impl ? impl->surfaces.size() : 0;
}

size_t count_surfaces_visible_docs(const UiManager::Impl *impl)
{
	if (!impl)
		return 0;
	size_t n = 0;
	for (const auto &p : impl->surfaces) {
		if (p.second.visible && p.second.document)
			n++;
	}
	return n;
}

const char *UiManager::getBuiltinOverlayTestRmlDocument()
{
	return ui_font_get_builtin_overlay_rml_cached();
}

UiManager::UiManager() :
		m_impl(std::make_unique<Impl>())
{
}

UiManager::~UiManager()
{
	shutdown();
}

bool UiManager::initialize(video::IVideoDriver *driver, std::string &error_message)
{
	if (!driver || !m_impl) {
		error_message = "RmlUi: invalid driver or internal state";
		return false;
	}

	if (m_ready) {
		error_message = "RmlUi: already initialized (repeat UiManager::initialize)";
		return false;
	}
	if (m_impl->rml_context != nullptr) {
		error_message = "RmlUi: internal state error (context already set)";
		return false;
	}

	m_impl->rml_render = std::make_unique<RmlUiIrrlichtRenderInterface>(driver);
	Rml::SetRenderInterface(m_impl->rml_render.get());

	if (g_rmlui_init_refcount == 0) {
		if (!Rml::Initialise()) {
			error_message = "RmlUi: Rml::Initialise() failed";
			Rml::SetRenderInterface(nullptr);
			m_impl->rml_render.reset();
			return false;
		}
	}
	g_rmlui_init_refcount++;
	m_impl->rml_library_initialized = true;

	if (!load_rmlui_font_faces(error_message)) {
		rmlui_release_one_init_ref();
		m_impl->rml_library_initialized = false;
		Rml::SetRenderInterface(nullptr);
		m_impl->rml_render.reset();
		return false;
	}

	{
		const auto ss = driver->getScreenSize();
		int w = (ss.Width > 0) ? static_cast<int>(ss.Width)
				: MYMAX(1, static_cast<int>(g_settings->getU16("screen_w")));
		int h = (ss.Height > 0) ? static_cast<int>(ss.Height)
				: MYMAX(1, static_cast<int>(g_settings->getU16("screen_h")));
		if (w <= 0)
			w = 800;
		if (h <= 0)
			h = 600;
		m_impl->rml_context = Rml::CreateContext(RMLUI_CONTEXT_NAME, Rml::Vector2i(w, h));
	}
	if (!m_impl->rml_context) {
		error_message = "RmlUi: Rml::CreateContext failed";
		rmlui_release_one_init_ref();
		m_impl->rml_library_initialized = false;
		ui_font_clear_resolution_state();
		Rml::SetRenderInterface(nullptr);
		m_impl->rml_render.reset();
		return false;
	}

	{
		UiMountSurfaceDesc od;
		od.surface_id = OVERLAY_SURFACE_ID;
		od.layer = UiLayer::OVERLAY;
		od.priority = 0;
		od.rml_memory = ui_font_get_builtin_overlay_rml_cached();
		od.document_url = "rmlui://surface/overlay";
		if (!mount_surface_impl(m_impl.get(), od, error_message)) {
			close_surface_documents_impl(m_impl.get());
			Rml::RemoveContext(RMLUI_CONTEXT_NAME);
			m_impl->rml_context = nullptr;
			rmlui_release_one_init_ref();
			m_impl->rml_library_initialized = false;
			ui_font_clear_resolution_state();
			Rml::SetRenderInterface(nullptr);
			m_impl->rml_render.reset();
			return false;
		}
	}

	m_ready = true;
	infostream << "[RmlUi] UiManager::initialize ok" << std::endl;
	return true;
}

void UiManager::shutdown()
{
	if (!m_impl)
		return;

	infostream << "[RmlUi] UiManager::shutdown" << std::endl;
	// Disable dispatch first so any stray listener callback sees a dead switch before documents close.
	m_ui_click_dispatch_enabled = false;
	m_ui_click_dispatch = {};
	close_surface_documents_impl(m_impl.get());

	if (m_impl->rml_context) {
		m_impl->rml_context->Update();
		Rml::RemoveContext(RMLUI_CONTEXT_NAME);
		m_impl->rml_context = nullptr;
	}
	if (m_impl->rml_library_initialized) {
		rmlui_release_one_init_ref();
		m_impl->rml_library_initialized = false;
	}
	ui_font_clear_resolution_state();
	m_impl->rml_render.reset();
	m_ready = false;
}

void UiManager::setUiClickDispatcher(
		std::function<void(const std::string &surface_id, int button_index)> fn)
{
	m_ui_click_dispatch = std::move(fn);
	// Empty dispatcher (e.g. Game teardown) must silence listeners without relying on document order.
	m_ui_click_dispatch_enabled = static_cast<bool>(m_ui_click_dispatch);
}

void UiManager::setInstrumentEventDispatcher(std::function<bool(const UiInstrumentEvent &ev)> fn)
{
	m_instrument_event_dispatch = std::move(fn);
	m_instrument_event_dispatch_enabled = static_cast<bool>(m_instrument_event_dispatch);
}

bool UiManager::tryUiClickDispatch(const std::string &surface_id, int button_index)
{
	if (!m_ready || !m_ui_click_dispatch_enabled)
		return false;
	if (!m_ui_click_dispatch) {
		warningstream << "[RmlUi] tryUiClickDispatch: no click dispatcher (setUiClickDispatcher) sid="
				<< surface_id << " btn=" << button_index << std::endl;
		return false;
	}
	verbosestream << "[RmlUi] click dispatch sid=" << surface_id
			<< " btn=" << button_index << std::endl;
	m_ui_click_dispatch(surface_id, button_index);
	return true;
}

std::optional<UiManager::DeclarativeButtonRef> UiManager::findDeclarativeButtonFromHover(
		Rml::Context *ctx) const
{
	if (!m_impl || !ctx)
		return std::nullopt;
	Rml::Element *e = ctx->GetHoverElement();

	static constexpr u32 kUiActionInventorySlotFlag = 0x80000000u;
	static constexpr u32 kUiActionInventorySlotMask = 0x7FFFFFFFu;

	auto try_parse_prefixed_index = [](const std::string &id, const char *prefix, long min_v,
			long max_v) -> std::optional<long> {
		const size_t plen = std::strlen(prefix);
		if (id.size() < plen + 1 || id.compare(0, plen, prefix) != 0)
			return std::nullopt;
		const std::string sub = id.substr(plen);
		char *endptr = nullptr;
		const long v = std::strtol(sub.c_str(), &endptr, 10);
		if (endptr == sub.c_str() || (endptr && *endptr != '\0'))
			return std::nullopt;
		if (v < min_v || v > max_v)
			return std::nullopt;
		return v;
	};

	auto encode_slot_action = [](long slot_index_1based) -> int {
		const u32 idx = static_cast<u32>(slot_index_1based) & kUiActionInventorySlotMask;
		const u32 code = kUiActionInventorySlotFlag | idx;
		return static_cast<int>(code);
	};

	while (e) {
		const Rml::String &rid = e->GetId();
		const std::string id(rid.c_str());

		bool is_btn = false;
		int action_code = -1;

		if (auto v = try_parse_prefixed_index(id, "luaui_btn_", 0, 1024)) {
			is_btn = true;
			action_code = static_cast<int>(*v);
		} else if (auto v = try_parse_prefixed_index(id, "luaui_slot_", 1, 4096)) {
			is_btn = false;
			action_code = encode_slot_action(*v);
		} else {
			e = e->GetParentNode();
			continue;
		}

		Rml::ElementDocument *owner_doc = e->GetOwnerDocument();
		if (!owner_doc)
			return std::nullopt;
		for (const auto &p : m_impl->surfaces) {
			if (p.second.document != owner_doc)
				continue;
			if (is_btn) {
				if (p.second.declarative_button_count <= 0)
					return std::nullopt;
				if (action_code < 0 || action_code >= p.second.declarative_button_count)
					return std::nullopt;
			}
			DeclarativeButtonRef ref;
			ref.surface_id = p.first;
			ref.button_index = action_code;
			return ref;
		}
		return std::nullopt;
	}
	return std::nullopt;
}

void UiManager::processRmlUiInput(InputHandler *input)
{
	if (!m_ready || !m_impl || !m_impl->rml_context || !input)
		return;

	Rml::Context *ctx = m_impl->rml_context;

	// Keyboard + text input: forward raw events to RmlUi. This enables basic focus navigation
	// (Tab) and text field editing when the mounted document contains inputs.
	{
		std::vector<UiKeyEvent> key_events;
		std::u32string text_input;
		input->takeUiEvents(key_events, text_input);
		// Always drain UI events to avoid delivering stale input after a surface mounts later.
		if (!any_surface_visible_for_render_impl(m_impl.get()))
			return;
		// If a modal is visible, enforce document-level focus isolation immediately before
		// processing keyboard events. This must hold even for raw RML or future widgets.
		update_modal_focus_trap(m_impl.get());
		if (Rml::ElementDocument *top_modal = find_topmost_visible_modal_document(m_impl.get())) {
			// Ensure focus is inside the modal document before we process any key/text events.
			if (Rml::Element *fe = ctx->GetFocusElement()) {
				Rml::ElementDocument *owner = fe->GetOwnerDocument();
				if (!owner || owner != top_modal)
					top_modal->Focus();
			} else {
				top_modal->Focus();
			}
		}
		for (const UiKeyEvent &e : key_events) {
			const int mod = rml_key_modifiers_from(e);
			const Rml::Input::KeyIdentifier kid = rml_key_identifier_from_irr(e.key);
			if (e.pressed_down)
				ctx->ProcessKeyDown(kid, mod);
			else
				ctx->ProcessKeyUp(kid, mod);
		}

		// Printable text input is delivered separately in many systems; we forward the buffered
		// text stream (collected at input layer) and filter control codes defensively.
		for (char32_t cp : text_input) {
			if (cp == 0)
				continue;
			// Skip ASCII control codes (incl. Tab, Enter, Backspace). Editing/navigation is via key events.
			if (cp < 0x20u || cp == 0x7Fu)
				continue;
			ctx->ProcessTextInput(static_cast<Rml::Character>(cp));
		}
		// After processing navigation, ensure focus still cannot escape the topmost modal.
		if (Rml::ElementDocument *top_modal = find_topmost_visible_modal_document(m_impl.get())) {
			if (Rml::Element *fe = ctx->GetFocusElement()) {
				Rml::ElementDocument *owner = fe->GetOwnerDocument();
				if (!owner || owner != top_modal)
					top_modal->Focus();
			} else {
				top_modal->Focus();
			}
		}
	}

	const v2s32 mpos = input->getMousePos();
	Rml::Vector2i dims = ctx->GetDimensions();
	v2s32 mpos_clamped = mpos;
	if (m_instrument_mode && m_instrument_capture_kind != InstrumentCaptureKind::None) {
		// While dragging/resizing, confine the pointer so it cannot drift beyond what the
		// captured rect can do. This prevents "cursor outruns the instrument" at viewport edges.
		if (m_instrument_capture_kind == InstrumentCaptureKind::Drag &&
				m_instrument_capture_start_rect_w > 0 && m_instrument_capture_start_rect_h > 0 &&
				dims.x > 0 && dims.y > 0) {
			const s32 dx = mpos_clamped.X - m_instrument_capture_start_mouse_x;
			const s32 dy = mpos_clamped.Y - m_instrument_capture_start_mouse_y;

			const s32 min_dx = -m_instrument_capture_start_abs_x;
			const s32 max_dx = (dims.x - m_instrument_capture_start_rect_w) - m_instrument_capture_start_abs_x;
			const s32 min_dy = -m_instrument_capture_start_abs_y;
			const s32 max_dy = (dims.y - m_instrument_capture_start_rect_h) - m_instrument_capture_start_abs_y;

			const s32 cdx = std::clamp(dx, min_dx, max_dx);
			const s32 cdy = std::clamp(dy, min_dy, max_dy);
			mpos_clamped.X = m_instrument_capture_start_mouse_x + cdx;
			mpos_clamped.Y = m_instrument_capture_start_mouse_y + cdy;
		}

		const s32 max_x = std::max<s32>(0, dims.x - 1);
		const s32 max_y = std::max<s32>(0, dims.y - 1);
		mpos_clamped.X = std::clamp(mpos_clamped.X, 0, max_x);
		mpos_clamped.Y = std::clamp(mpos_clamped.Y, 0, max_y);
		if (mpos_clamped != mpos)
			input->setMousePos(mpos_clamped.X, mpos_clamped.Y);
	}

	const int mod = 0;
	ctx->ProcessMouseMove(mpos_clamped.X, mpos_clamped.Y, mod);

	auto find_surface_from_owner_doc = [&](Rml::ElementDocument *doc) -> std::pair<std::string, SurfaceEntry *> {
		if (!doc)
			return {"", nullptr};
		for (auto &p : m_impl->surfaces) {
			if (p.second.document == doc)
				return {p.first, &p.second};
		}
		return {"", nullptr};
	};

	auto build_id_path_from = [](Rml::Element *e) -> std::vector<std::string> {
		std::vector<std::string> out;
		while (e) {
			const Rml::String &rid = e->GetId();
			if (!rid.empty())
				out.emplace_back(rid.c_str());
			e = e->GetParentNode();
		}
		return out;
	};

	auto fill_event_common = [&](UiInstrumentEvent &ev, const std::string &sid, SurfaceEntry *se, Rml::Element *hover) {
		ev.surface_id = sid;
		ev.mouse_x = mpos_clamped.X;
		ev.mouse_y = mpos_clamped.Y;
		ev.viewport_w = dims.x;
		ev.viewport_h = dims.y;
		ev.id_path = build_id_path_from(hover);

		// Placement snapshot (if any).
		if (se && se->positioning) {
			ev.has_placement = true;
			ev.placement_x = se->positioning->x;
			ev.placement_y = se->positioning->y;
			ev.placement_keep_in_view = se->positioning->keep_in_view;
			ev.placement_anchor = anchor_to_token(se->positioning->anchor);
		}

		// Resizable HUD layout contract (shared by instruments and other HUD surfaces).
		if (se && se->layout) {
			ev.has_instrument = true;
			ev.instrument_movable = se->layout->movable;
			ev.instrument_resizable = se->layout->resizable;
			ev.instrument_sticky = se->layout->sticky;
			ev.instrument_keep_aspect = se->layout->keep_aspect;
			ev.instrument_aspect_ratio_set = se->layout->aspect_ratio_set;
			ev.instrument_aspect_ratio = se->layout->aspect_ratio;
			for (UiAnchor a : se->layout->allowed_anchors)
				ev.instrument_anchors.emplace_back(anchor_to_token(a));
		}

		// Root rect info (for snapping/resizing math in Lua).
		// Use the first content root element under exp_pos when present; this corresponds
		// to the visually sized box, not the full-viewport wrapper.
		if (se && se->document) {
			if (Rml::Element *pos = se->document->GetElementById(Rml::String("exp_pos"))) {
				Rml::Element *box_el = nullptr;
				const int n = pos->GetNumChildren();
				for (int i = 0; i < n; ++i) {
					Rml::Element *ch = pos->GetChild(i);
					if (ch) {
						box_el = ch;
						break;
					}
				}
				if (!box_el)
					box_el = pos;

				const Rml::Vector2f off = box_el->GetAbsoluteOffset(Rml::BoxArea::Border);
				const Rml::Vector2f sz = box_el->GetBox().GetSize(Rml::BoxArea::Border);
				ev.abs_x = static_cast<s32>(std::floor(off.x + 0.5f));
				ev.abs_y = static_cast<s32>(std::floor(off.y + 0.5f));
				ev.rect_w = static_cast<s32>(std::floor(sz.x + 0.5f));
				ev.rect_h = static_cast<s32>(std::floor(sz.y + 0.5f));
			}
		}

		// Nine-way region + kind: geometry when the measured rect is valid; else anchor tokens.
		if (ev.rect_w > 0 && ev.rect_h > 0 && ev.viewport_w > 0 && ev.viewport_h > 0) {
			classify_placement_from_geometry(ev.abs_x, ev.abs_y, ev.rect_w, ev.rect_h, ev.viewport_w,
					ev.viewport_h, &ev.placement_region, &ev.placement_kind);
		} else if (ev.has_placement) {
			classify_placement_from_anchor_token(ev.placement_anchor, &ev.placement_region, &ev.placement_kind);
		} else {
			ev.placement_region.clear();
			ev.placement_kind.clear();
		}
	};

	auto dispatch_instrument = [&](const UiInstrumentEvent &ev) -> bool {
		if (!m_instrument_event_dispatch_enabled || !m_instrument_event_dispatch)
			return false;
		return m_instrument_event_dispatch(ev);
	};

	auto infer_resize_dir = [&](s32 rx, s32 ry, s32 rw, s32 rh, s32 mx, s32 my,
					    s32 border) -> std::string {
		if (rw <= 0 || rh <= 0)
			return "";
		const s32 left = rx;
		const s32 top = ry;
		const s32 right = rx + rw;
		const s32 bottom = ry + rh;

		const bool within_y = (my >= top - border) && (my <= bottom + border);
		const bool within_x = (mx >= left - border) && (mx <= right + border);

		const bool near_l = within_y && (std::abs(mx - left) <= border);
		const bool near_r = within_y && (std::abs(mx - right) <= border);
		const bool near_t = within_x && (std::abs(my - top) <= border);
		const bool near_b = within_x && (std::abs(my - bottom) <= border);

		char h = 0;
		char v = 0;
		if (near_l && !near_r)
			h = 'w';
		else if (near_r && !near_l)
			h = 'e';
		if (near_t && !near_b)
			v = 'n';
		else if (near_b && !near_t)
			v = 's';

		std::string dir;
		if (v)
			dir.push_back(v);
		if (h)
			dir.push_back(h);
		return dir;
	};

	// When no explicit drag-handle element is hit, allow dragging from non-interactive
	// chrome inside the HUD content box (declarative buttons/slots and form controls still win).
	auto implicit_instrument_drag_element = [&](SurfaceEntry &se, s32 mx, s32 my) -> Rml::Element * {
		if (!se.document)
			return nullptr;
		// Per-document hit test: GetHoverElement() is global topmost across all stacked documents,
		// so lower HUDs would never see a matching hover. Query this surface's document only.
		const Rml::Vector2f mp(static_cast<float>(mx), static_cast<float>(my));
		Rml::Element *hover = ctx->GetElementAtPoint(mp, nullptr, se.document);
		if (!hover || hover->GetOwnerDocument() != se.document)
			return nullptr;
		Rml::Element *pos = se.document->GetElementById(Rml::String("exp_pos"));
		if (!pos)
			return nullptr;
		bool under_exp_pos = false;
		for (Rml::Element *x = hover; x; x = x->GetParentNode()) {
			if (x == pos) {
				under_exp_pos = true;
				break;
			}
		}
		if (!under_exp_pos)
			return nullptr;
		for (Rml::Element *x = hover; x; x = x->GetParentNode()) {
			if (x->HasAttribute(Rml::String("data-luui-resize-handle")))
				return nullptr;
			if (dynamic_cast<Rml::ElementFormControl *>(x))
				return nullptr;
			const Rml::String &rid = x->GetId();
			const std::string id(rid.c_str());
			if (id.size() >= 10 && id.compare(0, 10, "luaui_btn_") == 0)
				return nullptr;
			if (id.size() >= 11 && id.compare(0, 11, "luaui_slot_") == 0)
				return nullptr;
			if (x == pos)
				break;
		}

		Rml::Element *box_el = nullptr;
		const int nch = pos->GetNumChildren();
		for (int i = 0; i < nch; ++i) {
			if (Rml::Element *ch = pos->GetChild(i)) {
				box_el = ch;
				break;
			}
		}
		if (!box_el)
			box_el = pos;
		const Rml::Vector2f off = box_el->GetAbsoluteOffset(Rml::BoxArea::Border);
		const Rml::Vector2f sz = box_el->GetBox().GetSize(Rml::BoxArea::Border);
		const s32 rx = static_cast<s32>(std::floor(off.x + 0.5f));
		const s32 ry = static_cast<s32>(std::floor(off.y + 0.5f));
		const s32 rw = static_cast<s32>(std::floor(sz.x + 0.5f));
		const s32 rh = static_cast<s32>(std::floor(sz.y + 0.5f));
		if (rw <= 0 || rh <= 0)
			return nullptr;
		if (mx < rx || my < ry || mx >= rx + rw || my >= ry + rh)
			return nullptr;
		if (se.layout && se.layout->resizable) {
			if (!infer_resize_dir(rx, ry, rw, rh, mx, my, 10).empty())
				return nullptr;
		}
		return pos;
	};

	// Instrument mode: Lua decides drag/resize behavior; C++ only forwards events.
	if (m_instrument_mode) {
		// Cursor feedback for implicit resizing (no visible handle).
		if (m_instrument_capture_kind == InstrumentCaptureKind::None) {
			gui::ICursorControl *cc = nullptr;
			if (IrrlichtDevice *dev = RenderingEngine::get_raw_device())
				cc = dev->getCursorControl();
			if (cc) {
				gui::ECURSOR_ICON want = gui::ECI_NORMAL;
				// Pick the topmost resizable HUD surface under the pointer.
				for (auto it = m_impl->surface_mount_sequence.rbegin();
						it != m_impl->surface_mount_sequence.rend(); ++it) {
					auto sit = m_impl->surfaces.find(*it);
					if (sit == m_impl->surfaces.end())
						continue;
					SurfaceEntry &se = sit->second;
					if (!se.visible || !se.document || se.layer != UiLayer::HUD || !se.layout ||
							!se.layout->resizable)
						continue;
					UiInstrumentEvent ev;
					fill_event_common(ev, *it, &se, ctx->GetHoverElement());
					const std::string dir = infer_resize_dir(ev.abs_x, ev.abs_y, ev.rect_w, ev.rect_h,
							mpos_clamped.X, mpos_clamped.Y, 10);
					if (dir.empty())
						continue;
					if (dir == "n" || dir == "s")
						want = gui::ECI_SIZENS;
					else if (dir == "e" || dir == "w")
						want = gui::ECI_SIZEWE;
					else if (dir == "ne" || dir == "sw")
						want = gui::ECI_SIZENESW;
					else if (dir == "nw" || dir == "se")
						want = gui::ECI_SIZENWSE;
					break;
				}
				if (cc->getActiveIcon() != want)
					cc->setActiveIcon(want);
			}
		}

		// Pointer move (non-captured): provide hover chain / surface for Lua UX/policy.
		if (m_instrument_capture_kind == InstrumentCaptureKind::None) {
			Rml::Element *hover = ctx->GetHoverElement();
			Rml::ElementDocument *doc = hover ? hover->GetOwnerDocument() : nullptr;
			auto [sid, se] = find_surface_from_owner_doc(doc);
			if (se && !sid.empty()) {
				UiInstrumentEvent ev;
				ev.phase = "pointer_move";
				fill_event_common(ev, sid, se, hover);
				(void)dispatch_instrument(ev);
			}
		}

		// If captured, send drag_move/resize_move each frame (even if the pointer is over other UI).
		if (m_instrument_capture_kind != InstrumentCaptureKind::None &&
				!m_instrument_capture_surface_id.empty()) {
			auto it = m_impl->surfaces.find(m_instrument_capture_surface_id);
			if (it != m_impl->surfaces.end()) {
				UiInstrumentEvent ev;
				if (m_instrument_capture_kind == InstrumentCaptureKind::Resize) {
					ev.kind = "resize";
					ev.phase = "resize_move";
					ev.resize_handle = m_instrument_capture_resize_handle;
				} else {
					ev.kind = "drag";
					ev.phase = "drag_move";
				}
				ev.element_id = m_instrument_capture_element_id;
				ev.drag_start_mouse_x = m_instrument_capture_start_mouse_x;
				ev.drag_start_mouse_y = m_instrument_capture_start_mouse_y;
				ev.drag_dx = mpos_clamped.X - m_instrument_capture_start_mouse_x;
				ev.drag_dy = mpos_clamped.Y - m_instrument_capture_start_mouse_y;
				ev.initial_abs_x = m_instrument_capture_start_abs_x;
				ev.initial_abs_y = m_instrument_capture_start_abs_y;
				ev.initial_rect_w = m_instrument_capture_start_rect_w;
				ev.initial_rect_h = m_instrument_capture_start_rect_h;
				fill_event_common(ev, m_instrument_capture_surface_id, &it->second, ctx->GetHoverElement());
				(void)dispatch_instrument(ev);
			}
		}
	}

	/*
	 * Primary-click bridge only (not a general input system):
	 * - Feed the same logical "primary action" as gameplay dig: KeyType::DIG (default: left mouse).
	 * - RmlUi mouse button index 0 = left; ProcessMouse* return false when the pointer is
	 *   interacting with RmlUi elements, which we treat as "consume" for this frame: clear DIG
	 *   pressed/released so world dig/punch does not run (see suppressRmlUiPrimary*).
	 * - After primary release, @ref dispatchDeclarativeButtonFromHover walks the hover chain
	 *   for `luaui_btn_N` (RmlUi event listeners are not relied on for routing).
	 */
	if (input->wasKeyPressed(KeyType::DIG)) {
		// Instrument mode: resolve drag/resize start by hit-testing declarative handles.
		if (m_instrument_mode) {
			auto hit = [&](Rml::Element *e) -> bool {
				if (!e)
					return false;
				const Rml::Vector2f off = e->GetAbsoluteOffset(Rml::BoxArea::Border);
				const Rml::Vector2f sz = e->GetBox().GetSize(Rml::BoxArea::Border);
				const float x = static_cast<float>(mpos_clamped.X);
				const float y = static_cast<float>(mpos_clamped.Y);
				return x >= off.x && y >= off.y && x < (off.x + sz.x) && y < (off.y + sz.y);
			};

			auto choose_smallest_hit = [&](const std::vector<Rml::Element *> &els) -> Rml::Element * {
				Rml::Element *best = nullptr;
				float best_area = 0.f;
				for (Rml::Element *e : els) {
					if (!hit(e))
						continue;
					const Rml::Vector2f sz = e->GetBox().GetSize();
					const float area = std::max(1.f, sz.x) * std::max(1.f, sz.y);
					if (!best || area < best_area) {
						best = e;
						best_area = area;
					}
				}
				return best;
			};

			// Find the topmost HUD surface with a handle containing the pointer.
			for (auto it = m_impl->surface_mount_sequence.rbegin();
					it != m_impl->surface_mount_sequence.rend(); ++it) {
				const std::string &sid = *it;
				auto sit = m_impl->surfaces.find(sid);
				if (sit == m_impl->surfaces.end())
					continue;
				SurfaceEntry &se = sit->second;
				if (!se.visible || !se.document)
					continue;
				if (se.layer != UiLayer::HUD)
					continue;
				if (!se.layout)
					continue;

				std::vector<Rml::Element *> all;
				collect_elements_depth_first(se.document, all);

				// Prefer resize handles (more specific) over drag handles.
				std::vector<Rml::Element *> resize_hits;
				for (Rml::Element *e : all) {
					if (e && e->HasAttribute(Rml::String("data-luui-resize-handle")))
						resize_hits.push_back(e);
				}
				Rml::Element *resize_handle = choose_smallest_hit(resize_hits);
				if (resize_handle) {
					const Rml::String eid = resize_handle->GetId();
					if (eid.empty())
						continue;

					UiInstrumentEvent ev;
					ev.kind = "resize";
					ev.phase = "resize_start";
					ev.element_id = eid.c_str();
					ev.resize_handle = resize_handle->GetAttribute<Rml::String>(
							Rml::String("data-luui-resize-handle"), Rml::String()).c_str();
					fill_event_common(ev, sid, &se, ctx->GetHoverElement());

					m_instrument_capture_kind = InstrumentCaptureKind::Resize;
					m_instrument_capture_surface_id = sid;
					m_instrument_capture_element_id = ev.element_id;
					m_instrument_capture_resize_handle = ev.resize_handle;
				m_instrument_capture_start_mouse_x = mpos_clamped.X;
				m_instrument_capture_start_mouse_y = mpos_clamped.Y;
					m_instrument_capture_start_abs_x = ev.abs_x;
					m_instrument_capture_start_abs_y = ev.abs_y;
					m_instrument_capture_start_rect_w = ev.rect_w;
					m_instrument_capture_start_rect_h = ev.rect_h;
					ev.initial_abs_x = m_instrument_capture_start_abs_x;
					ev.initial_abs_y = m_instrument_capture_start_abs_y;
					ev.initial_rect_w = m_instrument_capture_start_rect_w;
					ev.initial_rect_h = m_instrument_capture_start_rect_h;
					ev.drag_start_mouse_x = m_instrument_capture_start_mouse_x;
					ev.drag_start_mouse_y = m_instrument_capture_start_mouse_y;
					ev.drag_dx = 0;
					ev.drag_dy = 0;
					(void)dispatch_instrument(ev);

					input->suppressRmlUiPrimaryPress();
					m_primary_click_armed_button.reset();
					goto after_primary_down;
				}

				// Implicit resize hit zone: edges/corners of exp_pos (no visible handle element required).
				if (se.layout->resizable) {
					UiInstrumentEvent ev;
					ev.kind = "resize";
					ev.phase = "resize_start";
					ev.element_id = "exp_pos";
					fill_event_common(ev, sid, &se, ctx->GetHoverElement());
					const s32 x = mpos_clamped.X;
					const s32 y = mpos_clamped.Y;
					const s32 rx = ev.abs_x;
					const s32 ry = ev.abs_y;
					const s32 rw = ev.rect_w;
					const s32 rh = ev.rect_h;
					const std::string dir = infer_resize_dir(rx, ry, rw, rh, x, y, 10);
					if (!dir.empty()) {
						ev.resize_handle = dir;
							m_instrument_capture_kind = InstrumentCaptureKind::Resize;
							m_instrument_capture_surface_id = sid;
							m_instrument_capture_element_id = ev.element_id;
							m_instrument_capture_resize_handle = ev.resize_handle;
							m_instrument_capture_start_mouse_x = mpos_clamped.X;
							m_instrument_capture_start_mouse_y = mpos_clamped.Y;
							m_instrument_capture_start_abs_x = ev.abs_x;
							m_instrument_capture_start_abs_y = ev.abs_y;
							m_instrument_capture_start_rect_w = ev.rect_w;
							m_instrument_capture_start_rect_h = ev.rect_h;
							ev.initial_abs_x = m_instrument_capture_start_abs_x;
							ev.initial_abs_y = m_instrument_capture_start_abs_y;
							ev.initial_rect_w = m_instrument_capture_start_rect_w;
							ev.initial_rect_h = m_instrument_capture_start_rect_h;
							ev.drag_start_mouse_x = m_instrument_capture_start_mouse_x;
							ev.drag_start_mouse_y = m_instrument_capture_start_mouse_y;
							ev.drag_dx = 0;
							ev.drag_dy = 0;
							(void)dispatch_instrument(ev);

							input->suppressRmlUiPrimaryPress();
							m_primary_click_armed_button.reset();
							goto after_primary_down;
					}
				}

				// Drag handles: declarative marker or fallback to legacy drag_handle_id.
				if (!se.layout->movable)
					continue;

				std::vector<Rml::Element *> drag_hits;
				for (Rml::Element *e : all) {
					if (e && e->HasAttribute(Rml::String("data-luui-drag-handle")))
						drag_hits.push_back(e);
				}

				// Back-compat: explicit id handle.
				const std::string legacy_id =
						(se.layout->drag_handle_id.empty() ? std::string("instrument_drag") : se.layout->drag_handle_id);
				if (Rml::Element *legacy = se.document->GetElementById(Rml::String(legacy_id.c_str())))
					drag_hits.push_back(legacy);

				Rml::Element *drag_handle = choose_smallest_hit(drag_hits);
				if (!drag_handle)
					drag_handle = implicit_instrument_drag_element(se, mpos_clamped.X, mpos_clamped.Y);
				if (!drag_handle)
					continue;
				const Rml::String eid = drag_handle->GetId();
				if (eid.empty())
					continue;

				UiInstrumentEvent ev;
				ev.kind = "drag";
				ev.phase = "drag_start";
				ev.element_id = eid.c_str();
				fill_event_common(ev, sid, &se, ctx->GetHoverElement());

				m_instrument_capture_kind = InstrumentCaptureKind::Drag;
				m_instrument_capture_surface_id = sid;
				m_instrument_capture_element_id = ev.element_id;
				m_instrument_capture_resize_handle.clear();
				m_instrument_capture_start_mouse_x = mpos_clamped.X;
				m_instrument_capture_start_mouse_y = mpos_clamped.Y;
				m_instrument_capture_start_abs_x = ev.abs_x;
				m_instrument_capture_start_abs_y = ev.abs_y;
				m_instrument_capture_start_rect_w = ev.rect_w;
				m_instrument_capture_start_rect_h = ev.rect_h;
				ev.initial_abs_x = m_instrument_capture_start_abs_x;
				ev.initial_abs_y = m_instrument_capture_start_abs_y;
				ev.initial_rect_w = m_instrument_capture_start_rect_w;
				ev.initial_rect_h = m_instrument_capture_start_rect_h;
				ev.drag_start_mouse_x = m_instrument_capture_start_mouse_x;
				ev.drag_start_mouse_y = m_instrument_capture_start_mouse_y;
				ev.drag_dx = 0;
				ev.drag_dy = 0;
				(void)dispatch_instrument(ev);

				input->suppressRmlUiPrimaryPress();
				m_primary_click_armed_button.reset();
				goto after_primary_down;
			}
		}

		// Default path: forward press to RmlUi and arm declarative button dispatch if needed.
		{
			const bool not_interacting = ctx->ProcessMouseButtonDown(0, mod);
			if (!not_interacting) {
				input->suppressRmlUiPrimaryPress();
				// Arm only when the press began on a declarative button id.
				m_primary_click_armed_button = findDeclarativeButtonFromHover(ctx);
			} else {
				m_primary_click_armed_button.reset();
			}
		}
	}
after_primary_down:
	if (input->wasKeyReleased(KeyType::DIG)) {
		// If an instrument capture is active, do not forward release to RmlUi; deliver end event.
		if (m_instrument_mode && m_instrument_capture_kind != InstrumentCaptureKind::None &&
				!m_instrument_capture_surface_id.empty()) {
			input->suppressRmlUiPrimaryRelease();
			auto it = m_impl->surfaces.find(m_instrument_capture_surface_id);
			if (it != m_impl->surfaces.end()) {
				UiInstrumentEvent ev;
				if (m_instrument_capture_kind == InstrumentCaptureKind::Resize) {
					ev.kind = "resize";
					ev.phase = "resize_end";
					ev.resize_handle = m_instrument_capture_resize_handle;
				} else {
					ev.kind = "drag";
					ev.phase = "drag_end";
				}
				ev.element_id = m_instrument_capture_element_id;
				ev.drag_start_mouse_x = m_instrument_capture_start_mouse_x;
				ev.drag_start_mouse_y = m_instrument_capture_start_mouse_y;
				ev.drag_dx = mpos_clamped.X - m_instrument_capture_start_mouse_x;
				ev.drag_dy = mpos_clamped.Y - m_instrument_capture_start_mouse_y;
				ev.initial_abs_x = m_instrument_capture_start_abs_x;
				ev.initial_abs_y = m_instrument_capture_start_abs_y;
				ev.initial_rect_w = m_instrument_capture_start_rect_w;
				ev.initial_rect_h = m_instrument_capture_start_rect_h;
				fill_event_common(ev, m_instrument_capture_surface_id, &it->second, ctx->GetHoverElement());
				(void)dispatch_instrument(ev);
			}
			m_instrument_capture_kind = InstrumentCaptureKind::None;
			m_instrument_capture_surface_id.clear();
			m_instrument_capture_element_id.clear();
			m_instrument_capture_resize_handle.clear();
			m_primary_click_armed_button.reset();
			return;
		}

		const bool not_interacting = ctx->ProcessMouseButtonUp(0, mod);
		if (!not_interacting)
			input->suppressRmlUiPrimaryRelease();

		if (m_instrument_mode) {
			Rml::Element *hover = ctx->GetHoverElement();
			Rml::ElementDocument *doc = hover ? hover->GetOwnerDocument() : nullptr;
			auto [sid, se] = find_surface_from_owner_doc(doc);
			if (se && !sid.empty()) {
				UiInstrumentEvent ev;
				ev.phase = "pointer_up";
				fill_event_common(ev, sid, se, hover);
				(void)dispatch_instrument(ev);
			}
		}

		// Modal dismiss policy (outside click): on primary release outside the modal content
		// bounds, close the top-most dismissable modal and consume the click.
		if (m_impl && m_impl->rml_context) {
			std::string top_sid;
			const SurfaceEntry *top = find_topmost_visible_surface_entry(m_impl.get(), &top_sid);
			if (top && top->modal_document && top->dismiss_policy == UiDismissPolicy::OutsideOrEscape &&
					top->document) {
				Rml::ElementDocument *doc = top->document;
				Rml::ElementList body_list;
				doc->GetElementsByTagName(body_list, Rml::String("body"));
				Rml::Element *body = body_list.empty() ? nullptr : body_list.front();
				Rml::Element *content_root = nullptr;
				// Declarative compiler wraps user content inside a full-viewport div#exp_panel.
				// For outside-click dismiss we must hit-test the *user* root node, not exp_panel.
				Rml::Element *panel = doc->GetElementById(Rml::String("exp_panel"));
				if (panel) {
					const int n = panel->GetNumChildren();
					for (int ci = 0; ci < n; ++ci) {
						Rml::Element *ch = panel->GetChild(ci);
						if (ch) {
							content_root = ch;
							break;
						}
					}
				}
				// Fallback: if exp_panel isn't present (non-declarative doc), use first child of body.
				if (!content_root && body) {
					const int n = body->GetNumChildren();
					for (int ci = 0; ci < n; ++ci) {
						Rml::Element *ch = body->GetChild(ci);
						if (ch) {
							content_root = ch;
							break;
						}
					}
				}
				if (content_root) {
					const Rml::Vector2f pos = content_root->GetAbsoluteOffset();
					const Rml::Vector2f sz = content_root->GetBox().GetSize();
					const float x = static_cast<float>(mpos.X);
					const float y = static_cast<float>(mpos.Y);
					const bool inside = x >= pos.x && y >= pos.y && x < (pos.x + sz.x) && y < (pos.y + sz.y);
					if (!inside) {
						infostream << "[RmlUi] dismiss modal (outside click) sid=" << top_sid << std::endl;
						// Notify server/client Lua via the existing click dispatch path (sentinel index).
						// -2 becomes 0xFFFFFFFE when serialized as u32.
						tryUiClickDispatch(top_sid, -2);
						unmount(top_sid);
						if (Rml::ElementDocument *next_modal = find_topmost_visible_modal_document(m_impl.get()))
							next_modal->Focus();
						m_primary_click_armed_button.reset();
						return;
					}
				}
			}
		}
		// Single-fire: dispatch once for a press→release on the same declarative button.
		if (m_primary_click_armed_button) {
			const auto released = findDeclarativeButtonFromHover(ctx);
			if (released && released->surface_id == m_primary_click_armed_button->surface_id &&
					released->button_index == m_primary_click_armed_button->button_index) {
				tryUiClickDispatch(released->surface_id, released->button_index);
			}
		}
		m_primary_click_armed_button.reset();
	}
}

bool UiManager::mount(const std::string &surface_id, UiLayer layer, int priority,
		const char *rml_memory, const char *document_url, std::string &error_message,
		const UiMountOptions &options)
{
	if (!m_ready || !m_impl || !m_impl->rml_context) {
		error_message = "RmlUi: UiManager not initialized";
		return false;
	}
	UiMountSurfaceDesc desc;
	desc.surface_id = surface_id;
	desc.layer = layer;
	desc.priority = priority;
	desc.rml_memory = rml_memory;
	desc.document_url = document_url;
	desc.options = options;
	if (!mount_surface_impl(m_impl.get(), desc, error_message))
		return false;
	{
		auto it = m_impl->surfaces.find(surface_id);
		// UX: when mounting a modal surface, establish an initial keyboard focus target so
		// Tab navigation and text input behave predictably for keyboard users.
		if (options.modal_document && it != m_impl->surfaces.end() && it->second.document)
			it->second.document->Focus();
	}
	update_modal_focus_trap(m_impl.get());
	return true;
}

bool UiManager::setSurfaceState(const std::string &surface_id,
		const std::map<std::string, std::string> &state, std::string &error_message)
{
	if (!m_ready || !m_impl) {
		error_message = "RmlUi: UiManager not initialized";
		return false;
	}
	auto it = m_impl->surfaces.find(surface_id);
	if (it == m_impl->surfaces.end() || !it->second.document) {
		error_message = "RmlUi: surface not mounted";
		return false;
	}
	Rml::ElementDocument *doc = it->second.document;
	const std::vector<UiDeclarativeBindingEntry> &targets = it->second.binding_targets;

	if (!targets.empty() && !state.empty()) {
		bool any_state_key_matches = false;
		for (const auto &be : targets) {
			if (state.find(be.state_key) != state.end()) {
				any_state_key_matches = true;
				break;
			}
		}
		if (!any_state_key_matches)
			warningstream << "UI binding: no state keys matched any binding on surface \""
					<< surface_id << "\"" << std::endl;
	}

	size_t patched_count = 0;
	for (const auto &be : targets) {
		auto sit = state.find(be.state_key);
		if (sit == state.end())
			continue;
		Rml::Element *el = doc->GetElementById(Rml::String(be.element_id.c_str()));
		if (!el) {
			warningstream << "UI binding patch miss: id=\"" << be.element_id
					<< "\" not found for key=\"" << be.state_key << "\"" << std::endl;
			continue;
		}
		switch (be.kind) {
		case UiDeclarativeBindingEntry::Kind::TextValue: {
			const std::string escaped = escape_for_rml_inner_text(sit->second);
			el->SetInnerRML(escaped);
			++patched_count;
			break;
		}
		default:
			break;
		}
	}

	if (!state.empty() && patched_count == 0) {
		error_message = "RmlUi: set_state applied no updates";
		return false;
	}
	return true;
}

bool UiManager::applySurfacePositioning(const std::string &surface_id, const UiSurfacePositioning &pos,
		std::string &error_message)
{
	if (!m_ready || !m_impl || !m_impl->rml_context) {
		error_message = "RmlUi: UiManager not initialized";
		return false;
	}
	auto it = m_impl->surfaces.find(surface_id);
	if (it == m_impl->surfaces.end() || !it->second.document) {
		error_message = "RmlUi: surface not mounted";
		return false;
	}

	SurfaceEntry &se = it->second;
	se.positioning = pos;

	const Rml::Vector2i dims = m_impl->rml_context->GetDimensions();
	apply_surface_positioning(m_impl.get(), se, dims.x, dims.y);
	return true;
}

bool UiManager::setElementProperties(const std::string &surface_id, const std::string &element_id,
		const std::map<std::string, std::string> &props, std::string &error_message)
{
	if (!m_ready || !m_impl) {
		error_message = "RmlUi: UiManager not initialized";
		return false;
	}
	auto it = m_impl->surfaces.find(surface_id);
	if (it == m_impl->surfaces.end() || !it->second.document) {
		error_message = "RmlUi: surface not mounted";
		return false;
	}
	Rml::ElementDocument *doc = it->second.document;
	Rml::Element *el = doc->GetElementById(Rml::String(element_id.c_str()));
	if (!el) {
		error_message = "RmlUi: element not found";
		return false;
	}
	for (const auto &kv : props) {
		if (!kv.first.empty())
			el->SetProperty(Rml::String(kv.first.c_str()), Rml::String(kv.second.c_str()));
	}
	return true;
}

void UiManager::unmount(const std::string &surface_id)
{
	if (!m_impl || !m_ready)
		return;
	auto it = m_impl->surfaces.find(surface_id);
	if (it == m_impl->surfaces.end())
		return;
	const bool was_visible_modal = it->second.visible && it->second.document && it->second.modal_document;
	if (m_impl->rml_context) {
		// Defensive: Clear internal hover/active chains before closing a document. Otherwise the context
		// may retain pointers to elements that are about to be destroyed, leading to "dead clicks"
		// on the next surface/modal until the mouse state is reset.
		(void)m_impl->rml_context->ProcessMouseLeave();
	}
	if (it->second.document) {
		it->second.document->Close();
		it->second.document = nullptr;
	}
	{
		auto &v = m_impl->surface_mount_sequence;
		v.erase(std::remove(v.begin(), v.end(), surface_id), v.end());
	}
	m_impl->surfaces.erase(it);
	update_modal_focus_trap(m_impl.get());

	// Deterministic unwind: if we just removed the active/top modal, move focus to the next modal (if any).
	if (was_visible_modal) {
		if (Rml::ElementDocument *next_modal = find_topmost_visible_modal_document(m_impl.get())) {
			// RmlUi maintains internal modal state; some builds appear to not restore the previous modal
			// as interaction owner when the top modal closes. Force a reactivation by Hide+Show(Modal).
			next_modal->Hide();
			next_modal->Show(Rml::ModalFlag::Modal, Rml::FocusFlag::Auto);
			// Prefer Auto focus (autofocus attribute or document default) without overriding it.
			// Only fall back to explicit Focus() if nothing is focused after the reactivation.
			if (Rml::Context *ctx = m_impl->rml_context) {
				Rml::Element *fe = ctx->GetFocusElement();
				if (!fe || fe->GetOwnerDocument() != next_modal) {
					// If Auto focus didn't land inside the document, try explicit autofocus target.
					if (Rml::Element *ae = find_first_autofocus_element(next_modal))
						(void)ae->Focus();
					else
						next_modal->Focus();
				} else if (fe == next_modal) {
					// Auto focus fell back to document. Prefer an autofocus element if present.
					if (Rml::Element *ae = find_first_autofocus_element(next_modal))
						(void)ae->Focus();
				}
			} else {
				next_modal->Focus();
			}
			return;
		}
		// No modal remains. Restore interaction ownership to the topmost visible non-overlay surface, if any.
		if (Rml::ElementDocument *next_owner = find_topmost_visible_non_overlay_document(m_impl.get())) {
			// Use Show(..., Auto) as the smallest reliable way to establish focus (element autofocus or document).
			next_owner->Show(Rml::ModalFlag::Keep, Rml::FocusFlag::Auto);
		}
	}
}

void UiManager::setSurfaceVisible(const std::string &surface_id, bool visible)
{
	if (!m_impl || !m_ready)
		return;
	auto it = m_impl->surfaces.find(surface_id);
	if (it == m_impl->surfaces.end() || !it->second.document)
		return;
	it->second.visible = visible;
	if (visible)
		it->second.document->Show(it->second.modal_document ? Rml::ModalFlag::Modal
				: Rml::ModalFlag::None);
	else
		it->second.document->Hide();
	update_modal_focus_trap(m_impl.get());
}

bool UiManager::surfaceVisible(const std::string &surface_id) const
{
	if (!m_ready || !m_impl)
		return false;
	auto it = m_impl->surfaces.find(surface_id);
	if (it == m_impl->surfaces.end())
		return false;
	return it->second.visible;
}

bool UiManager::toggleSurfaceVisible(const std::string &surface_id)
{
	if (!m_ready || !m_impl)
		return false;
	auto it = m_impl->surfaces.find(surface_id);
	if (it == m_impl->surfaces.end() || !it->second.document)
		return false;
	it->second.visible = !it->second.visible;
	if (it->second.visible)
		it->second.document->Show(it->second.modal_document ? Rml::ModalFlag::Modal
				: Rml::ModalFlag::None);
	else
		it->second.document->Hide();
	update_modal_focus_trap(m_impl.get());
	return it->second.visible;
}

bool UiManager::toggleTestOverlayVisible()
{
	return toggleSurfaceVisible(OVERLAY_SURFACE_ID);
}

bool UiManager::hasSurface(const std::string &surface_id) const
{
	if (!m_ready || !m_impl)
		return false;
	return m_impl->surfaces.find(surface_id) != m_impl->surfaces.end();
}

bool UiManager::hasVisibleModalSurface() const
{
	if (!m_ready || !m_impl)
		return false;
	for (const auto &p : m_impl->surfaces) {
		if (p.second.visible && p.second.document && p.second.modal_document)
			return true;
	}
	return false;
}

bool UiManager::hasVisibleDismissableModalSurface() const
{
	if (!m_ready || !m_impl)
		return false;
	for (const auto &p : m_impl->surfaces) {
		if (p.second.visible && p.second.document && p.second.modal_document &&
				p.second.dismiss_policy != UiDismissPolicy::None)
			return true;
	}
	return false;
}

bool UiManager::dismissTopmostModalByEscape()
{
	if (!m_ready || !m_impl || !m_impl->rml_context)
		return false;
	std::string top_sid;
	const SurfaceEntry *top = find_topmost_visible_surface_entry(m_impl.get(), &top_sid);
	if (!top || !top->modal_document)
		return false;
	if (top->dismiss_policy != UiDismissPolicy::Escape &&
			top->dismiss_policy != UiDismissPolicy::OutsideOrEscape)
		return false;
	// -1 becomes 0xFFFFFFFF when serialized as u32.
	tryUiClickDispatch(top_sid, -1);
	unmount(top_sid);
	if (Rml::ElementDocument *next_modal = find_topmost_visible_modal_document(m_impl.get()))
		next_modal->Focus();
	m_primary_click_armed_button.reset();
	return true;
}

void UiManager::enterInstrumentMode()
{
	if (!m_ready || !m_impl)
		return;
	m_instrument_mode = true;

	// Minimal UX cue: a tiny non-interactive overlay label.
	if (!hasSurface("instrument_mode_overlay")) {
		static const char *kRml =
				"<rml>\n"
				"<head><style>"
				"body{pointer-events:none;margin:0;padding:0;background-color:transparent;width:100%;"
				"height:100%;min-width:1px;min-height:1px;}"
				"#lm{pointer-events:none;box-sizing:border-box;position:absolute;left:12px;top:12px;"
				"display:block;width:auto;max-width:420px;"
				"background-color:rgba(20,24,32,220);"
				"border:1px #5a6e8c;"
				"border-radius:6px;padding:8px 10px;"
				"white-space:normal;font-family:Arimo;font-size:13px;line-height:1.35;"
				"color:#eaf2fb;}"
				"</style></head>\n"
				"<body><div id=\"lm\">Instrument mode — drag HUD; ESC exits</div></body>\n"
				"</rml>";
		std::string em;
		(void)mount("instrument_mode_overlay", UiLayer::OVERLAY, 1000, kRml,
				"rmlui://instrument_mode_overlay", em, UiMountOptions());
	}
}

void UiManager::exitInstrumentMode()
{
	if (!m_ready || !m_impl)
		return;
	m_instrument_mode = false;
	m_instrument_capture_kind = InstrumentCaptureKind::None;
	m_instrument_capture_surface_id.clear();
	m_instrument_capture_element_id.clear();
	m_instrument_capture_resize_handle.clear();
	if (IrrlichtDevice *dev = RenderingEngine::get_raw_device()) {
		if (gui::ICursorControl *cc = dev->getCursorControl())
			cc->setActiveIcon(gui::ECI_NORMAL);
	}
	unmount("instrument_mode_overlay");
}

bool UiManager::isInstrumentMode() const
{
	return m_ready && m_instrument_mode;
}

std::map<std::string, UiSurfacePositioning> UiManager::getHudPlacement() const
{
	std::map<std::string, UiSurfacePositioning> out;
	if (!m_ready || !m_impl)
		return out;
	for (const auto &p : m_impl->surfaces) {
		const SurfaceEntry &se = p.second;
		if (!se.visible || !se.document)
			continue;
		if (se.layer != UiLayer::HUD)
			continue;
		if (!se.positioning)
			continue;
		out.emplace(p.first, *se.positioning);
	}
	return out;
}

void UiManager::setHudPlacement(const std::map<std::string, UiSurfacePositioning> &placement)
{
	if (!m_ready || !m_impl || !m_impl->rml_context)
		return;
	const Rml::Vector2i dims = m_impl->rml_context->GetDimensions();
	for (const auto &p : placement) {
		auto it = m_impl->surfaces.find(p.first);
		if (it == m_impl->surfaces.end())
			continue;
		SurfaceEntry &se = it->second;
		if (!se.document || se.layer != UiLayer::HUD)
			continue;
		se.positioning = p.second;
		apply_surface_positioning(m_impl.get(), se, dims.x, dims.y);
	}
}

// (investigation-only placement lab resize logging removed)

void UiManager::update(f32 dtime, video::IVideoDriver *driver)
{
	(void)dtime;
	if (!m_ready || !m_impl || !m_impl->rml_context || !driver)
		return;

	const auto ss = driver->getScreenSize();
	m_impl->rml_context->SetDimensions(Rml::Vector2i(ss.Width, ss.Height));
	m_impl->rml_context->Update();

	// Minimal positioning layer: apply after layout, every frame (resize-stable).
	// This avoids CSS transforms (which can interfere with our hover-based click bridge),
	// and ensures keep_in_view clamping can use actual computed element dimensions.
	for (auto &p : m_impl->surfaces) {
		SurfaceEntry &se = p.second;
		if (!se.visible || !se.document)
			continue;
		apply_surface_positioning(m_impl.get(), se, static_cast<int>(ss.Width), static_cast<int>(ss.Height));
	}
}

void UiManager::render(video::IVideoDriver *driver, const v2u32 &screensize)
{
	if (!m_ready || !m_impl || !m_impl->rml_context || !m_impl->rml_render || !driver)
		return;

	// First version: one Context::Render() for the whole context; surface registry holds
	// layer/priority for future ordering (e.g. per-document passes or z-index) and tooling.
	if (!any_surface_visible_for_render_impl(m_impl.get())) {
		return;
	}
	driver->setViewPort(core::rect<s32>(0, 0, screensize.X, screensize.Y));
	m_impl->rml_context->Render();
}
