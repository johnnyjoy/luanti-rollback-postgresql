// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "rmlui_irrlicht_render_interface.h"
#include <EPrimitiveTypes.h>
#include <IImage.h>
#include <ITexture.h>
#include <S3DVertex.h>
#include <dimension2d.h>
#include <vector2d.h>
#include <vector3d.h>
#include <mt_opengl.h>
#include "log.h"
#include "settings.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace {

struct CompiledGeometry {
	std::vector<video::S3DVertex> vertices;
	std::vector<u16> indices;
};

/// RmlUi supplies premultiplied RGB; Irrlicht `EMT_TRANSPARENT_VERTEX_ALPHA` expects straight RGB
/// with standard src-alpha blending.
static video::SColor rmlColourPremulToStraightSColor(const Rml::ColourbPremultiplied &c)
{
	const u32 a = c.alpha;
	if (a == 0)
		return video::SColor(0, 0, 0, 0);
	const u32 r = (static_cast<u32>(c.red) * 255u + a / 2u) / a;
	const u32 g = (static_cast<u32>(c.green) * 255u + a / 2u) / a;
	const u32 b = (static_cast<u32>(c.blue) * 255u + a / 2u) / a;
	return video::SColor(a, core::min_(r, 255u), core::min_(g, 255u), core::min_(b, 255u));
}

} // namespace

RmlUiIrrlichtRenderInterface::RmlUiIrrlichtRenderInterface(video::IVideoDriver *driver) :
		m_driver(driver)
{}

RmlUiIrrlichtRenderInterface::~RmlUiIrrlichtRenderInterface()
{
	m_driver = nullptr;
}

Rml::CompiledGeometryHandle RmlUiIrrlichtRenderInterface::CompileGeometry(
		Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices)
{
	auto *geom = new CompiledGeometry();

	geom->vertices.reserve(vertices.size());
	for (const Rml::Vertex &v : vertices) {
		video::S3DVertex sv;
		sv.Pos.X = v.position.x;
		sv.Pos.Y = v.position.y;
		sv.Pos.Z = 0.f;
		sv.Normal = core::vector3df(0.f, 0.f, -1.f);
		sv.Color = rmlColourPremulToStraightSColor(v.colour);
		sv.TCoords.X = v.tex_coord.x;
		sv.TCoords.Y = v.tex_coord.y;
		geom->vertices.push_back(sv);
	}
	geom->indices.reserve(indices.size());
	for (int idx : indices) {
		if (idx < 0 || idx > 65535) {
			delete geom;
			return {};
		}
		geom->indices.push_back(static_cast<u16>(idx));
	}
	return reinterpret_cast<Rml::CompiledGeometryHandle>(geom);
}

void RmlUiIrrlichtRenderInterface::RenderGeometry(Rml::CompiledGeometryHandle geometry,
		Rml::Vector2f translation, Rml::TextureHandle texture)
{
	if (!m_driver || !geometry)
		return;

	auto *geom = reinterpret_cast<CompiledGeometry *>(geometry);
	if (geom->vertices.empty() || geom->indices.empty())
		return;

	// Coordinate assumptions (RmlUi 2D vs Irrlicht `draw2DVertexPrimitiveList`):
	// Rml vertex positions are document pixels with origin at the top-left of the context; +Y goes down.
	// Irrlicht 2D here uses the same pixel convention (see driver viewport).
	// TODO: VERIFY: if geometry is invisible despite non-zero counts, check driver Y flip / ortho.
	std::vector<video::S3DVertex> draw_verts = geom->vertices;
	for (video::S3DVertex &v : draw_verts) {
		v.Pos.X += translation.x;
		v.Pos.Y += translation.y;
	}
	const u32 tri_count = static_cast<u32>(geom->indices.size() / 3);

	video::SMaterial mat;
	mat.ZBuffer = video::ECFN_DISABLED;
	mat.ZWriteEnable = video::EZW_OFF;
	// Default SMaterial() sets BackfaceCulling=true; 2D draw2DVertexPrimitiveList winding can
	// leave every triangle back-facing and fully culled (no visible pixels).
	mat.BackfaceCulling = false;
	mat.FogEnable = false;

	if (texture) {
		mat.TextureLayers[0].Texture = reinterpret_cast<video::ITexture *>(texture);
		mat.MaterialType = video::EMT_TRANSPARENT_ALPHA_CHANNEL;
	} else {
		mat.TextureLayers[0].Texture = nullptr;
		mat.MaterialType = video::EMT_TRANSPARENT_VERTEX_ALPHA;
	}

	m_driver->setMaterial(mat);
	m_driver->draw2DVertexPrimitiveList(draw_verts.data(), draw_verts.size(),
			geom->indices.data(), tri_count, video::EVT_STANDARD, scene::EPT_TRIANGLES,
			video::EIT_16BIT);
}

void RmlUiIrrlichtRenderInterface::ReleaseGeometry(Rml::CompiledGeometryHandle geometry)
{
	delete reinterpret_cast<CompiledGeometry *>(geometry);
}

Rml::TextureHandle RmlUiIrrlichtRenderInterface::LoadTexture(
		Rml::Vector2i &texture_dimensions, const Rml::String &source)
{
	(void)texture_dimensions;
	(void)source;
	return {};
}

Rml::TextureHandle RmlUiIrrlichtRenderInterface::GenerateTexture(
		Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions)
{
	if (!m_driver || source_dimensions.x < 1 || source_dimensions.y < 1)
		return {};

	const size_t need =
			static_cast<size_t>(source_dimensions.x) * static_cast<size_t>(source_dimensions.y) * 4u;
	if (source.size() < need)
		return {};

	video::IImage *img = m_driver->createImage(video::ECF_A8R8G8B8,
			core::dimension2du(static_cast<u32>(source_dimensions.x),
					static_cast<u32>(source_dimensions.y)));
	if (!img)
		return {};

	for (int y = 0; y < source_dimensions.y; ++y) {
		for (int x = 0; x < source_dimensions.x; ++x) {
			const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(source_dimensions.x) +
						 static_cast<size_t>(x)) *
					4u;
			const Rml::byte r = source[i];
			const Rml::byte g = source[i + 1];
			const Rml::byte b = source[i + 2];
			const Rml::byte a = source[i + 3];
			img->setPixel(static_cast<u32>(x), static_cast<u32>(y), video::SColor(a, r, g, b));
		}
	}

	video::ITexture *tex = m_driver->addTexture("rmlui_generated", img);
	img->drop();
	return reinterpret_cast<Rml::TextureHandle>(tex);
}

void RmlUiIrrlichtRenderInterface::ReleaseTexture(Rml::TextureHandle texture)
{
	if (!m_driver || !texture)
		return;
	m_driver->removeTexture(reinterpret_cast<video::ITexture *>(texture));
}

void RmlUiIrrlichtRenderInterface::EnableScissorRegion(bool enable)
{
	m_is_scissor_region_enabled = enable;
}

void RmlUiIrrlichtRenderInterface::SetScissorRegion(Rml::Rectanglei region)
{
	(void)region;
}

// (diagnostic-only triangle probe removed)
