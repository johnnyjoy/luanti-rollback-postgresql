// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// RmlUi UI font helpers: load faces from the same client settings as the main GUI
// (font_path, font_path_bold, …, fallback_font_path) and bundled assets.
// Tracks the filesystem path used to derive default font-family for generated RML
// after LoadFontFace succeeds.

#pragma once

#include <string>

/// Derive a font-family name from a font file path (basename stem, or segment before
/// '-' as in Arimo-Regular.ttf). Returns empty if \p font_path is empty.
std::string ui_font_family_from_font_path(const std::string &font_path);

/// CSS font-family value for inline styles: quote when needed (spaces, punctuation, etc.).
std::string ui_font_family_css_token(const std::string &family);

/// After RmlUi loads faces, set the path used to derive default font-family for generated RML
/// (first successful primary face, or fallback path if that was the only load).
void ui_font_set_resolved_primary_path(const std::string &path_used_for_family);

/// Empty until \ref ui_font_set_resolved_primary_path runs after a successful load.
const std::string &ui_font_get_resolved_primary_path();

/// CSS token for declarative panel: uses resolved path after successful font load only (empty before then).
std::string ui_font_declarative_default_family_css();

/// Rebuild cached builtin:test_overlay RML; call only after fonts are loaded into RmlUi.
void ui_font_refresh_builtin_overlay_rml_from_path(const std::string &path_used_for_family);

/// Cached builtin:test_overlay document; valid after successful font init + refresh.
const char *ui_font_get_builtin_overlay_rml_cached();

/// Clear resolved path and builtin cache when RmlUi shuts down so a later session can reload.
void ui_font_clear_resolution_state();

/// Inline style for generated RML {@code <body>}: fill the context. Descendants with
/// {@code position:fixed} do not expand the body in layout; without width/height the document
/// box stays 0×0 while geometry may still render.
const char *ui_rml_body_viewport_style();
