// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "irrlichttypes_bloated.h"
#include <IVideoDriver.h>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "client/ui_declarative_binding.h"

class InputHandler;

class Client;

namespace Rml {
class Context;
}

/**
 * Logical stacking order for UI surfaces within the single RmlUi context.
 * Higher values draw later (closer to the user). Used for registry ordering;
 * full per-surface compositing may evolve with the same enum.
 */
enum class UiLayer : u8 {
	HUD = 0,
	OVERLAY,
	MENU,
	DEBUG,
};

/// Dismiss policy for modal surfaces.
enum class UiDismissPolicy : u8 {
	None = 0,
	Escape,
	OutsideOrEscape,
};

/// Minimal declarative positioning (v1): anchor + pixel offset + optional viewport clamping.
enum class UiAnchor : u8 {
	Center = 0,
	TopLeft,
	TopRight,
	BottomLeft,
	BottomRight,
	Top,
	Bottom,
	Left,
	Right,
};

struct UiSurfacePositioning {
	UiAnchor anchor = UiAnchor::Center;
	s32 x = 0;
	s32 y = 0;
	bool keep_in_view = true;
};

/// Instrument edit eligibility for HUD-style surfaces (v1).
struct UiSurfaceLayout {
	bool movable = false;
	bool resizable = false;
	bool sticky = true;
	/// When empty, all anchors are allowed.
	std::vector<UiAnchor> allowed_anchors;
	/// Explicit drag handle id (hover chain must contain this id to start a drag).
	/// When empty, defaults to "instrument_drag".
	std::string drag_handle_id;
};

/// Instrument-mode pointer / drag / resize event emitted by the client.
/// The engine supplies hit-testing and capture mechanics; Lua (typically server-side)
/// decides behavior and returns explicit apply patches.
struct UiInstrumentEvent {
	/// Phase of the instrument interaction.
	std::string phase; // "pointer_move", "drag_start", "drag_move", "drag_end", "resize_start", ...

	/// Kind of interaction ("drag" or "resize").
	std::string kind;

	/// Handle element id that initiated the interaction (drag_handle / resize_handle).
	std::string element_id;

	/// For resize interactions: "n|s|e|w|ne|nw|se|sw".
	std::string resize_handle;

	/// Target surface id (HUD surface).
	std::string surface_id;

	/// Pointer position in window pixels.
	s32 mouse_x = 0;
	s32 mouse_y = 0;

	/// Viewport size in pixels.
	s32 viewport_w = 0;
	s32 viewport_h = 0;

	/// Absolute rect of the instrument box at time of event.
	/// (Measured as the border box of the first content root element under `exp_pos`.)
	s32 abs_x = 0;
	s32 abs_y = 0;
	s32 rect_w = 0;
	s32 rect_h = 0;

	/// Initial rect at capture start (drag_start / resize_start).
	s32 initial_abs_x = 0;
	s32 initial_abs_y = 0;
	s32 initial_rect_w = 0;
	s32 initial_rect_h = 0;

	/// Hover chain element ids (topmost first). Empty ids are omitted.
	std::vector<std::string> id_path;

	/// Current placement record (if any) for the surface.
	bool has_placement = false;
	std::string placement_anchor; // token like "top-left"
	s32 placement_x = 0;
	s32 placement_y = 0;
	bool placement_keep_in_view = true;

	/// Instrument contract (if present) supplied by the mount metadata.
	bool has_instrument = false;
	bool instrument_movable = false;
	bool instrument_resizable = false;
	bool instrument_sticky = true;
	std::vector<std::string> instrument_anchors;

	/// Drag mechanics: populated for drag_* phases.
	s32 drag_start_mouse_x = 0;
	s32 drag_start_mouse_y = 0;
	s32 drag_dx = 0;
	s32 drag_dy = 0;
};

/**
 * Owns the client UI runtime: one RmlUi context per window, named surfaces
 * (mounted documents), layer metadata, update/render/shutdown.
 *
 * Limitations (first implementation):
 * - Exactly one RmlUi::Context for the main drawable.
 * - Context::Render() draws the whole context in one pass; surface/layer data
 *   is stored for future ordering and tooling; RmlUi stacks multiple documents
 *   according to its own rules unless we add per-document z-order later.
 * - Pointer input: primary button only (left mouse / dig key), no keyboard focus
 *   or full event model; see @ref processRmlUiInput.
 */
class UiManager {
public:
	UiManager();
	~UiManager();

	UiManager(const UiManager &) = delete;
	UiManager &operator=(const UiManager &) = delete;

	/// Creates the RmlUi context and mounts the default test surface on @ref OVERLAY_SURFACE_ID.
	bool initialize(video::IVideoDriver *driver, std::string &error_message);
	void shutdown();

	void update(f32 dtime, video::IVideoDriver *driver);
	void render(video::IVideoDriver *driver, const v2u32 &screensize);

	/// Mount a document from memory. `document_url` must be unique (e.g. `rmlui://overlay`).
	/// @param lua_button_count Number of `luaui_btn_N` ids with Lua `on_click` refs
	///        (from declarative compile); used to attach internal RmlUi click listeners.
	/// @param bindings Optional compile-time binding targets (core.ui.bind); used by set_state
	///        to patch live elements without remounting the surface.
	bool mount(const std::string &surface_id, UiLayer layer, int priority,
			const char *rml_memory, const char *document_url, std::string &error_message,
			int lua_button_count = 0,
			const std::vector<UiDeclarativeBindingEntry> *bindings = nullptr,
			bool modal_document = false,
			UiDismissPolicy dismiss_policy = UiDismissPolicy::None,
			const UiSurfacePositioning *positioning = nullptr,
			const UiSurfaceLayout *layout = nullptr);

	/// Apply surface-local string state to bound props. Does not reload RML or remount the
	/// document. Use compile with core.ui.bind for targets; structural changes use a new mount
	/// via core.ui.panel (C++ internal remount).
	///
	/// Returns false if @p state is non-empty but no bound element was updated (missing keys,
	/// typos, or patch misses). Returns true if @p state is empty, or at least one binding was
	/// applied. Patch misses are logged; partial success still returns true.
	bool setSurfaceState(const std::string &surface_id,
			const std::map<std::string, std::string> &state, std::string &error_message);

	/// Apply a new positioning record to an already-mounted surface (engine mechanics only).
	bool applySurfacePositioning(const std::string &surface_id, const UiSurfacePositioning &pos,
			std::string &error_message);

	/// Apply a set of RCSS properties to one element by id within a mounted surface.
	bool setElementProperties(const std::string &surface_id, const std::string &element_id,
			const std::map<std::string, std::string> &props, std::string &error_message);

	/// Routes declarative `on_click` from RmlUi to Lua (set from Game::startup).
	void setUiClickDispatcher(std::function<void(const std::string &surface_id, int button_index)> fn);

	/// Routes instrument-mode events to the game layer (set from Game::startup).
	/// Current implementation ignores the return value; capture decisions are made in C++.
	void setInstrumentEventDispatcher(std::function<bool(const UiInstrumentEvent &ev)> fn);

	/// Feed mouse position and primary button into RmlUi before game dig/punch logic.
	/// If the pointer hits interactive RmlUi, suppresses dig key "pressed"/"released"
	/// for this frame via @ref InputHandler::suppressRmlUiPrimaryPress/Release.
	void processRmlUiInput(InputHandler *input);
	void unmount(const std::string &surface_id);
	void setSurfaceVisible(const std::string &surface_id, bool visible);
	bool surfaceVisible(const std::string &surface_id) const;

	/// Returns the new visibility, or false if the surface does not exist.
	bool toggleSurfaceVisible(const std::string &surface_id);

	/// Same as @ref toggleSurfaceVisible(OVERLAY_SURFACE_ID).
	bool toggleTestOverlayVisible();

	bool isReady() const { return m_ready; }

	/// True if a surface id is currently registered (mounted), including hidden documents.
	bool hasSurface(const std::string &surface_id) const;

	/// True if any mounted visible surface was shown with @ref ModalFlag::Modal (game must use free mouse).
	bool hasVisibleModalSurface() const;

	/// True if a visible modal surface is configured to dismiss via ESC and/or outside click.
	bool hasVisibleDismissableModalSurface() const;

	/// Dismiss the top-most visible modal surface (if it allows ESC dismissal).
	/// Returns true if a surface was dismissed.
	bool dismissTopmostModalByEscape();

	/// HUD instrument mode (client-only): disables gameplay input and enables draggable HUD editing.
	void enterInstrumentMode();
	void exitInstrumentMode();
	bool isInstrumentMode() const;

	/// Snapshot HUD surface placement (for modder persistence).
	/// Returns entries for mounted HUD surfaces that have a positioning record.
	std::map<std::string, UiSurfacePositioning> getHudPlacement() const;
	/// Apply placement snapshot. Unknown ids are ignored.
	void setHudPlacement(const std::map<std::string, UiSurfacePositioning> &placement);

	/// Default in-game test surface id (layer OVERLAY).
	static constexpr const char *OVERLAY_SURFACE_ID = "overlay";

	/**
	 * Temporary bridge helper: same RML as the default overlay test document.
	 * Used by the Lua `template = "builtin:test_overlay"` mount
	 * spec; not a stable public content API.
	 */
	static const char *getBuiltinOverlayTestRmlDocument();

	/// Opaque; full definition in ui_manager.cpp (PIMPL). Public so friend helpers can name the type.
	struct Impl;

private:
	enum class InstrumentCaptureKind : u8 {
		None = 0,
		Drag,
		Resize,
	};

	std::unique_ptr<Impl> m_impl;
	bool m_ready = false;
	std::function<void(const std::string &surface_id, int button_index)> m_ui_click_dispatch;
	/// When false, RmlUi listeners do not invoke Lua (shutdown / dispatcher cleared).
	bool m_ui_click_dispatch_enabled = true;
	std::function<bool(const UiInstrumentEvent &ev)> m_instrument_event_dispatch;
	bool m_instrument_event_dispatch_enabled = false;
	bool m_instrument_mode = false;
	InstrumentCaptureKind m_instrument_capture_kind = InstrumentCaptureKind::None;
	std::string m_instrument_capture_surface_id;
	std::string m_instrument_capture_element_id;
	std::string m_instrument_capture_resize_handle;
	s32 m_instrument_capture_start_mouse_x = 0;
	s32 m_instrument_capture_start_mouse_y = 0;
	s32 m_instrument_capture_start_abs_x = 0;
	s32 m_instrument_capture_start_abs_y = 0;
	s32 m_instrument_capture_start_rect_w = 0;
	s32 m_instrument_capture_start_rect_h = 0;

	struct DeclarativeButtonRef {
		std::string surface_id;
		// Unified UI action code:
		// - buttons: 0..N-1
		// - modal dismiss: -1 escape, -2 outside (serialized as u32 sentinels)
		// - inventory slots: encoded with high bit set (see UiManager::encodeInventorySlotAction)
		int button_index = -1;
	};
	/// Press→release latch for declarative button clicks (single-fire per physical click).
	std::optional<DeclarativeButtonRef> m_primary_click_armed_button;

	/// Invoked from @ref processRmlUiInput after primary release (hover chain walk to `luaui_btn_*`).
	bool tryUiClickDispatch(const std::string &surface_id, int button_index);
	std::optional<DeclarativeButtonRef> findDeclarativeButtonFromHover(Rml::Context *ctx) const;

	// ui_manager.cpp helpers (friend so they may use Impl)
	friend bool mount_surface_impl(Impl *impl, const std::string &surface_id, UiLayer layer,
			int priority, const char *rml_memory, const char *document_url,
			std::string &error_message,
			const std::vector<UiDeclarativeBindingEntry> *bindings, bool modal_document,
			UiDismissPolicy dismiss_policy,
			int declarative_button_count,
			const UiSurfacePositioning *positioning,
			const UiSurfaceLayout *layout);
	friend void close_surface_documents_impl(Impl *impl);
	friend bool any_surface_visible_for_render_impl(const Impl *impl);
	friend size_t count_surfaces_total(const Impl *impl);
	friend size_t count_surfaces_visible_docs(const Impl *impl);
	friend void log_rmlui_context_stack_and_mount_order(Impl *impl);
	friend void log_rmlui_post_layout_surface_diagnostics(Impl *impl);
	friend void log_rmlui_placement_lab_resize(Impl *impl, const core::dimension2du &ss);

};
