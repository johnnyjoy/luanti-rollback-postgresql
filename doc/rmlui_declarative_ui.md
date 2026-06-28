## Declarative RmlUi (experimental)

> **Documentation moved.** The authoritative doc set is under [`docs/`](../docs/ui-index.md):
> start at [`docs/ui-index.md`](../docs/ui-index.md) (architecture, API contract, bridge, usage, validation).
> Long-range product vision: [`memory-bank/engine-proposal-unified-ui-next.md`](../memory-bank/engine-proposal-unified-ui-next.md).
> This file is kept for the **shadow / `box-shadow`** note below.

This branch contains an experimental declarative UI bridge built on top of RmlUi surfaces (`core.ui.panel`, `core.ui.modal`).

### Shadow support (important)

Native RmlUi `box-shadow` requires advanced render-interface features (layers, clip masks, save-to-texture, compositing, filters).
Those features are **not implemented** in this branch’s render bridge, and native `box-shadow` can render as opaque slabs.

To keep UI visuals stable, the declarative compiler treats `box-shadow` as **quarantined**:

- it is **stripped** from emitted RCSS
- it must **not** reach the renderer

#### Preferred replacement: Lua helper `ui.shadow_box`

Luanti provides a Lua-level composition helper:

```lua
local ui = core.ui

ui.shadow_box({
  shadow = { dx = 0, dy = 10, softness = 2, color = "#000000", opacities = { 0.26, 0.14, 0.08 } },
  style = {
    position = "fixed",
    top = "50%",
    left = "50%",
    transform = "translate(-50%, -50%)",
    width = "440px",
    padding = "16px",
    background_color = "#22304a",
    border = "2px solid #4f6aa0",
    border_radius = "12px",
    box_sizing = "border-box",
  },
  children = {
    ui.text("Shadowed content"),
  },
})
```

This helper renders a deterministic “soft shadow” using 1–3 plain rectangles behind the content.
It does **not** use native RmlUi `box-shadow` and requires no advanced RenderInterface features.

