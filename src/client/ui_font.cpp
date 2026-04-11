// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "ui_font.h"

#include <cctype>
#include <string>

static std::string g_builtin_overlay_rml_cache;
static std::string g_resolved_primary_font_path;

std::string ui_font_family_from_font_path(const std::string &font_path)
{
	if (font_path.empty())
		return {};

	size_t pos = font_path.find_last_of("/\\");
	std::string base = (pos == std::string::npos) ? font_path : font_path.substr(pos + 1);

	size_t dot = base.rfind('.');
	if (dot != std::string::npos)
		base = base.substr(0, dot);

	// Bundled fonts often use Family-Variant.ttf; family matches the first segment.
	const size_t sep = base.find('-');
	if (sep != std::string::npos)
		return base.substr(0, sep);

	return base;
}

static bool ui_font_family_needs_quotes(const std::string &family)
{
	if (family.empty())
		return true;
	for (unsigned char c : family) {
		if (c <= 32)
			return true;
		switch (c) {
		case ',':
		case '(':
		case ')':
		case '\'':
		case '"':
		case '\\':
			return true;
		default:
			break;
		}
		if (!(std::isalnum(c) || c == '-' || c == '_'))
			return true;
	}
	return false;
}

std::string ui_font_family_css_token(const std::string &family)
{
	if (family.empty())
		return family;
	if (!ui_font_family_needs_quotes(family))
		return family;

	std::string out;
	out.reserve(family.size() + 2);
	out.push_back('\'');
	for (char c : family) {
		if (c == '\'')
			out += "''";
		else
			out.push_back(c);
	}
	out.push_back('\'');
	return out;
}

void ui_font_set_resolved_primary_path(const std::string &path_used_for_family)
{
	g_resolved_primary_font_path = path_used_for_family;
}

const std::string &ui_font_get_resolved_primary_path()
{
	return g_resolved_primary_font_path;
}

std::string ui_font_declarative_default_family_css()
{
	// Only the path recorded after LoadFontFace succeeds (see load_rmlui_font_faces) may drive
	// font-family — never a settings-order heuristic, which can disagree with the first face that
	// actually loaded (e.g. font_path fails, bold succeeds).
	if (g_resolved_primary_font_path.empty())
		return {};
	return ui_font_family_css_token(
			ui_font_family_from_font_path(g_resolved_primary_font_path));
}

void ui_font_refresh_builtin_overlay_rml_from_path(const std::string &path_used_for_family)
{
	const std::string raw_family = ui_font_family_from_font_path(path_used_for_family);
	const std::string fam_css = ui_font_family_css_token(raw_family);

	// Transparent full-viewport shell (same idea as declarative compile); no placeholder copy.
	std::string panel = "display:block;position:absolute;left:0;top:0;width:100%;height:100%;"
			"margin:0;padding:0;box-sizing:border-box;overflow:visible;background-color:rgba(0,0,0,0);"
			"color:#e8e8f0;";
	if (!fam_css.empty()) {
		panel += "font-family:";
		panel += fam_css;
		panel += ";";
	}
	panel += "font-size:14px;";

	g_builtin_overlay_rml_cache = "<rml>\n<head></head>\n<body style=\"";
	g_builtin_overlay_rml_cache += ui_rml_body_viewport_style();
	g_builtin_overlay_rml_cache += "\">\n<div id=\"exp_panel\" style=\"";
	g_builtin_overlay_rml_cache += panel;
	g_builtin_overlay_rml_cache += "\"></div>\n</body>\n</rml>";
}

const char *ui_font_get_builtin_overlay_rml_cached()
{
	if (g_builtin_overlay_rml_cache.empty() && !g_resolved_primary_font_path.empty())
		ui_font_refresh_builtin_overlay_rml_from_path(g_resolved_primary_font_path);
	/* Defensive: before any successful font init, avoid feeding empty RML to the loader. */
	static const char kEmptyFallback[] =
			"<rml><head></head><body style=\"margin:0\"></body></rml>";
	if (g_builtin_overlay_rml_cache.empty())
		return kEmptyFallback;
	return g_builtin_overlay_rml_cache.c_str();
}

void ui_font_clear_resolution_state()
{
	g_builtin_overlay_rml_cache.clear();
	g_resolved_primary_font_path.clear();
}

const char *ui_rml_body_viewport_style()
{
	return "margin:0;padding:0;width:100%;height:100%;box-sizing:border-box;"
			"position:absolute;left:0;top:0;";
}
