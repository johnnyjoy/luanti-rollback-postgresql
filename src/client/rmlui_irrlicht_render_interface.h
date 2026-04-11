// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Irrlicht video driver implementation of RmlUi::RenderInterface.

#pragma once

#include <RmlUi/Core/RenderInterface.h>

#include <IVideoDriver.h>

class RmlUiIrrlichtRenderInterface final : public Rml::RenderInterface
{
public:
	explicit RmlUiIrrlichtRenderInterface(video::IVideoDriver *driver);
	~RmlUiIrrlichtRenderInterface() override;

	Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
			Rml::Span<const int> indices) override;
	void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
			Rml::TextureHandle texture) override;
	void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;

	Rml::TextureHandle LoadTexture(Rml::Vector2i &texture_dimensions,
			const Rml::String &source) override;
	Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source,
			Rml::Vector2i source_dimensions) override;
	void ReleaseTexture(Rml::TextureHandle texture) override;

	void EnableScissorRegion(bool enable) override;
	void SetScissorRegion(Rml::Rectanglei region) override;

private:
	video::IVideoDriver *m_driver = nullptr;
	bool m_is_scissor_region_enabled = false;
};
