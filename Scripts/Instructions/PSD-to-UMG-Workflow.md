Do not use instructions from this file unless asked.

# PSD-to-UMG Widget Workflow

AI-assisted pipeline for converting Photoshop PSD designs into Unreal Engine UMG widget blueprints using `psd-rs` for analysis/export and Claireon MCP for widget construction.

## Overview

This workflow takes a `.psd` file as input and produces a fully constructed Widget Blueprint in Unreal Engine. The process has seven phases:

1. **Analysis** — Inspect the PSD to get a JSON manifest of all layers, bounds, opacity, and hierarchy
2. **Structure Planning** — Map PSD layers to UMG widget types and identify dynamic/data-driven patterns
3. **User Confirmation** — Present the proposed widget structure for approval before building
4. **Asset Export** — Export individual layers as PNG files using psd-rs
5. **Asset Import** — Import the PNGs into Unreal as Texture2D assets via Claireon MCP
6. **Widget Construction** — Build the Widget Blueprint using Claireon MCP session-based editing
7. **Iteration** — Apply adjustments based on user feedback

## Prerequisites

### psd-rs Installation

Check whether `psd-rs` is available:

```bash
which psd-rs || psd-rs --help
```

If the command is not found, install it:

```bash
# If the repo already exists locally:
cargo install --path ~/code/believer-oss/psd-rs

# Otherwise, clone and install:
git clone https://github.com/believer-oss/psd-rs.git
cd psd-rs
cargo install --path .
```

After installation, verify with `psd-rs --help`. You should see the `inspect`, `export`, and `interactive` subcommands.

### Claireon MCP

The Unreal Editor must be running with the Claireon MCP server active on port 8017. Verify by checking that `claireon.widgetbp_edit` and `claireon.asset.import_file` tools are available. If the tools are not visible, follow the instructions in `ConnectToUnrealEditorMCP.md`.

---

## Phase 1: Analysis

Run `psd-rs inspect` to produce a JSON manifest of the PSD file:

```bash
psd-rs inspect /path/to/design.psd
```

This outputs JSON to stdout with this structure:

```json
{
  "file": {
    "width": 1920,
    "height": 1080,
    "depth": 8,
    "color_mode": "RGB"
  },
  "layers": [
    {
      "id": 0,
      "name": "Background",
      "kind": "pixel",
      "visible": true,
      "opacity": 255,
      "blend_mode": "Normal",
      "clipping": false,
      "bounds": { "top": 0, "left": 0, "bottom": 1080, "right": 1920 },
      "size": { "width": 1920, "height": 1080 },
      "parent_id": null,
      "children": [],
      "depth": 0,
      "z_index": 0
    }
  ]
}
```

To save the manifest to a file for reference:

```bash
psd-rs inspect /path/to/design.psd -o manifest.json
```

Use the `--flat` flag if you only need a flat layer list without tree hierarchy information:

```bash
psd-rs inspect /path/to/design.psd --flat
```

**What to look for in the manifest:**

- **`file.width` / `file.height`**: The canvas resolution. All layer bounds are relative to this.
- **`kind`**: Layer type — `"pixel"` for raster content, `"group"` for folders, `"text"` for text layers.
- **`bounds`**: The bounding box `{top, left, bottom, right}` in canvas coordinates. Use these for positioning.
- **`opacity`**: 0-255 range. Convert to UMG's 0.0-1.0 scale (see Reference section).
- **`visible`**: Whether the layer is visible in the PSD. Invisible layers are usually hidden design variants.
- **`parent_id` / `children`**: The layer hierarchy. Groups contain children; this maps to UMG's panel/child relationship.
- **`depth`**: Nesting level in the layer tree.
- **`z_index`**: Paint order. Higher z_index layers render on top.

---

## Phase 2: Structure Planning

After analyzing the manifest, plan the UMG widget structure before building anything.

### Layer-to-Widget Mapping Table

| PSD Layer Type | PSD Indicators | UMG Widget |
|---|---|---|
| Pixel layer | `kind: "pixel"` | `Image` (with imported texture as Brush) |
| Group/folder | `kind: "group"` | Panel widget (see container detection below) |
| Text layer | `kind: "text"` | `TextBlock` |
| Background fill | Full-canvas pixel layer at z_index 0 | `Image` or `Border` with solid brush |
| Icon/button art | Small pixel layer with descriptive name | `Image` inside a `Button` if interactive |
| Progress indicator | Named "bar", "fill", "progress" | `ProgressBar` or `Image` with material |

### Detecting Dynamic Containers

When a group layer contains children, analyze their bounds to determine the best container widget:

**Vertical stack** — Children have similar `left` values but incrementing `top` values with consistent spacing:
- Use `VerticalBox`
- Each child becomes a `VerticalBoxSlot`

**Horizontal row** — Children have similar `top` values but incrementing `left` values with consistent spacing:
- Use `HorizontalBox`
- Each child becomes a `HorizontalBoxSlot`

**Grid pattern** — Children arranged in rows and columns with uniform cell sizes:
- Use `UniformGridPanel` for equal-sized cells
- Use `WrapBox` if items should flow and wrap

**Overlapping layers** — Multiple children with similar or identical bounds stacked on top of each other:
- Use `Overlay`
- Children layer on top of each other in slot order

**Freeform layout** — Children at arbitrary positions with no discernible pattern:
- Use `CanvasPanel`
- Each child gets explicit `CanvasPanelSlot` positioning via `OffsetLeft`, `OffsetTop`, `OffsetRight`, `OffsetBottom`

**Detection algorithm:**

1. Collect all direct child bounds within the group
2. Sort children by `top` coordinate
3. Check if `left` values are within 5% tolerance of each other — if yes, vertical stack
4. Sort children by `left` coordinate
5. Check if `top` values are within 5% tolerance of each other — if yes, horizontal row
6. Check if children form a grid (consistent row/column spacing)
7. Check if children overlap significantly (>50% area overlap) — if yes, overlay
8. Default to CanvasPanel for freeform layouts

### Identifying Data-Driven Patterns

Scan the manifest for patterns that suggest dynamic content requiring data binding rather than static construction.

**Repeated groups:** Three or more sibling groups with naming patterns like `card_1`, `card_2`, `card_3` (or `item_01`, `item_02`, etc.) that share similar internal structure. These indicate a list or grid of templated items:
- Create an entry widget from one instance of the repeated group
- Use `ListView` for vertical scrolling lists
- Use `TileView` for grid/tile layouts
- Bind to an array data source in the ViewModel

**Text as data:** Layers named with semantic identifiers like `player_name`, `score`, `health`, `timer`, `level_label`. These represent values that change at runtime:
- Use `TextBlock` for each
- Bind `Text` property to the corresponding ViewModel field
- Note the font size and style from the PSD for the `Font` property

**Dynamic images:** Layers named `portrait`, `avatar`, `icon`, `thumbnail`, or any layer that represents content loaded at runtime:
- Use `Image` widget
- Bind the `Brush` property to a `SlateBrush` or `Texture2D` ViewModel field
- The PSD layer serves as a placeholder — export it as a fallback texture

**Static vs. dynamic determination:**
- **Static**: Backgrounds, decorative frames, borders, separators, fixed icons (e.g., a gear icon that always looks the same). Import these as textures and set directly.
- **Dynamic**: Any element that represents data — player info, scores, inventory items, health bars, portraits. These should be bound to ViewModel properties.

### ViewModel Discovery

Before creating new ViewModels, check whether the project already has ones that match:

1. Search for existing ViewModels:
   - Use `claireon.asset_search` with query terms like "ViewModel", "VM", or domain-specific names
   - Use `claireon.asset_list` to browse `/Game/UI/ViewModels/` or similar paths

2. Inspect matching ViewModels:
   - Use `claireon.widgetbp_get_tree` with `include_mvvm_bindings: true` on existing widgets that use them
   - Check if the ViewModel's properties cover the fields you need

3. If an existing ViewModel matches:
   - Recommend binding to it
   - Document which ViewModel properties map to which widget properties

4. If no suitable ViewModel exists:
   - Recommend creating a new one with a descriptive name (e.g., `VM_PlayerHUD`, `VM_InventoryPanel`)
   - List the required fields with types: `FText` for display strings, `FSlateBrush` for dynamic images, `float` for progress bars, `int32` for counts, `TArray<UObject*>` for list data sources

---

## Phase 3: User Confirmation

Before building anything, present the proposed structure to the user for approval. Format it as a clear summary:

```
Proposed Widget Structure for: WBP_PlayerHUD
Source PSD: player_hud.psd (1920x1080)

Root: CanvasPanel
  +-- IMG_Background (Image) — "background" layer, full-screen
  +-- VB_StatsPanel (VerticalBox) — "stats_panel" group
  |     +-- TB_PlayerName (TextBlock) — "player_name" [BOUND: VM.PlayerName]
  |     +-- TB_Score (TextBlock) — "score" [BOUND: VM.Score]
  +-- OVL_Portrait (Overlay) — "portrait_frame" group
        +-- IMG_PortraitFrame (Image) — "frame" layer [STATIC]
        +-- IMG_Portrait (Image) — "portrait" layer [BOUND: VM.Portrait]

Textures to import: 3 (background, frame, portrait_placeholder)
ViewModel: VM_PlayerHUD (new)
  - PlayerName: FText
  - Score: FText
  - Portrait: FSlateBrush
```

Wait for the user to confirm or request changes before proceeding to Phase 4.

---

## Phase 4: Asset Export

Export the layers that will become textures in Unreal. Use `psd-rs export` with the appropriate flags.

### Export individual layers as separate PNGs

```bash
psd-rs export /path/to/design.psd --layers "background,frame,portrait" --each-layer --layer-relative -o /path/to/export/
```

**Flags explained:**

| Flag | Purpose |
|---|---|
| `--layers <NAMES>` | Comma-separated layer names to export. Supports glob patterns (e.g., `icon_*`). |
| `--each-layer` | Export each matching layer as a separate PNG file (one file per layer). |
| `--layer-relative` | Crop the output to the layer's own bounds instead of the full canvas size. This produces tightly cropped textures. |
| `-o <DIR>` | Output directory for exported PNGs. |
| `--group <NAME>` | Export a specific group as a single flattened PNG (composites all children). |

### Export a group as a flattened composite

For groups where you want one combined image (e.g., a button with multiple visual layers):

```bash
psd-rs export /path/to/design.psd --group "button_normal" --layer-relative -o /path/to/export/
```

### Naming convention

Name exported files to match the Unreal texture naming convention. After export, rename files if needed:

```
T_{WidgetBlueprintName}_{LayerName}.png
```

Examples:
- `T_PlayerHUD_Background.png`
- `T_PlayerHUD_PortraitFrame.png`
- `T_PlayerHUD_IconHealth.png`

---

## Phase 5: Asset Import

Import each exported PNG into Unreal using the Claireon MCP `claireon.asset.import_file` tool.

For each texture file, call:

```json
{
  "source_path": "/absolute/path/to/T_PlayerHUD_Background.png",
  "destination_path": "/Game/UI/Textures/PlayerHUD/",
  "asset_name": "T_PlayerHUD_Background",
  "overwrite": true
}
```

**Parameters:**

| Parameter | Required | Description |
|---|---|---|
| `source_path` | Yes | Absolute filesystem path to the PNG file |
| `destination_path` | Yes | Unreal content path for the destination folder |
| `asset_name` | No | Name for the asset (defaults to filename without extension) |
| `overwrite` | No | Replace existing asset if present (default: false) |

The import tool automatically configures Texture2D assets with UI-friendly settings: no mipmaps, `TEXTUREGROUP_UI` texture group, and sRGB enabled.

Import all textures before starting widget construction. Note the resulting asset paths — you will reference them when setting Image widget Brush properties.

---

## Phase 6: Widget Construction

Build the Widget Blueprint step-by-step using the `claireon.widgetbp_edit` tool.

### Step 1: Create or open the Widget Blueprint

To create a new Widget Blueprint:

```json
{
  "operation": "create",
  "asset_path": "/Game/UI/Widgets/WBP_PlayerHUD"
}
```

To open an existing one:

```json
{
  "operation": "open",
  "asset_path": "/Game/UI/Widgets/WBP_PlayerHUD"
}
```

Both return a `session_id`. Use this session_id in all subsequent operations.

### Step 2: Add widgets to build the hierarchy

Add widgets top-down, starting with the root panel and working inward. Each `add_widget` call requires the session_id and returns the created widget's name.

```json
{
  "operation": "add_widget",
  "session_id": "<session_id>",
  "widget_class": "CanvasPanel",
  "widget_name": "RootCanvas"
}
```

Add children by specifying `parent_name`:

```json
{
  "operation": "add_widget",
  "session_id": "<session_id>",
  "widget_class": "Image",
  "parent_name": "RootCanvas",
  "widget_name": "IMG_Background"
}
```

Common widget classes:
- `CanvasPanel`, `VerticalBox`, `HorizontalBox`, `Overlay`, `GridPanel`, `UniformGridPanel`, `WrapBox`
- `Image`, `TextBlock`, `RichTextBlock`, `ProgressBar`, `Spacer`
- `Button`, `CheckBox`, `Slider`, `EditableText`
- `SizeBox`, `ScaleBox`, `Border`, `NamedSlot`
- `ListView`, `TileView`

Use the `index` parameter to control insertion order (which affects rendering order within panels):

```json
{
  "operation": "add_widget",
  "session_id": "<session_id>",
  "widget_class": "Image",
  "parent_name": "RootCanvas",
  "widget_name": "IMG_Foreground",
  "index": 1
}
```

### Step 3: Set slot properties (positioning)

Slot properties control how a widget is positioned within its parent panel. The property names depend on the parent panel type.

**CanvasPanelSlot** (parent is CanvasPanel):

```json
{
  "operation": "set_slot_property",
  "session_id": "<session_id>",
  "widget_name": "IMG_Background",
  "property_name": "OffsetLeft",
  "value": "0"
}
```

Key CanvasPanelSlot properties: `OffsetLeft`, `OffsetTop`, `OffsetRight`, `OffsetBottom`, `AnchorMinX`, `AnchorMinY`, `AnchorMaxX`, `AnchorMaxY`, `AlignmentX`, `AlignmentY`, `SizeToContent`.

**VerticalBoxSlot / HorizontalBoxSlot**:

Key properties: `PaddingLeft`, `PaddingTop`, `PaddingRight`, `PaddingBottom`, `SizeRule` (Auto or Fill), `FillWeight`, `HorizontalAlignment` (HAlign_Fill, HAlign_Left, HAlign_Center, HAlign_Right), `VerticalAlignment` (VAlign_Fill, VAlign_Top, VAlign_Center, VAlign_Bottom).

**OverlaySlot**:

Key properties: `PaddingLeft`, `PaddingTop`, `PaddingRight`, `PaddingBottom`, `HorizontalAlignment`, `VerticalAlignment`.

### Step 4: Set widget properties

Set visual properties on individual widgets.

**Image widget — set the Brush texture:**

```json
{
  "operation": "set_widget_property",
  "session_id": "<session_id>",
  "widget_name": "IMG_Background",
  "property_name": "Brush.ResourceObject",
  "value": "/Game/UI/Textures/PlayerHUD/T_PlayerHUD_Background"
}
```

**Image widget — set image size to match the texture:**

```json
{
  "operation": "set_widget_property",
  "session_id": "<session_id>",
  "widget_name": "IMG_Background",
  "property_name": "Brush.ImageSize",
  "value": "(X=1920.0, Y=1080.0)"
}
```

**TextBlock — set text content:**

```json
{
  "operation": "set_widget_property",
  "session_id": "<session_id>",
  "widget_name": "TB_PlayerName",
  "property_name": "Text",
  "value": "Player Name"
}
```

**Any widget — set render opacity:**

```json
{
  "operation": "set_widget_property",
  "session_id": "<session_id>",
  "widget_name": "IMG_Background",
  "property_name": "RenderOpacity",
  "value": "0.8"
}
```

### Step 5: Compile and save

After all widgets are added and configured:

```json
{
  "operation": "compile",
  "session_id": "<session_id>"
}
```

```json
{
  "operation": "save",
  "session_id": "<session_id>"
}
```

### Step 6: Verify the result

Use `claireon.widgetbp_get_tree` to inspect the final widget tree:

```json
{
  "asset_path": "/Game/UI/Widgets/WBP_PlayerHUD",
  "include_properties": true,
  "include_bindings": true,
  "include_mvvm_bindings": true
}
```

Close the session when done:

```json
{
  "operation": "close",
  "session_id": "<session_id>"
}
```

---

## Phase 7: Iteration

After the initial build, common adjustments include:

- **Repositioning**: Adjust slot offsets if elements are slightly misaligned. Use `set_slot_property` to tweak `OffsetLeft`, `OffsetTop`, etc.
- **Swapping containers**: If a VerticalBox should have been an Overlay, use `replace_widget` with `preserve_children: true` to swap the container type without losing children.
- **Adding missing elements**: Use `add_widget` to insert additional widgets. Use `move_widget` to reparent widgets if the hierarchy needs restructuring.
- **Adjusting anchors**: If the widget needs to scale with screen size, update anchor properties on CanvasPanelSlots. See the Anchoring Strategies section below.
- **Renaming widgets**: Use `rename_widget` to fix names that do not follow the project's naming convention.
- **Re-exporting layers**: If the PSD is updated, re-run `psd-rs export` for changed layers and re-import with `overwrite: true`.

---

## Reference

### Positioning: PSD Bounds to Canvas Slot Properties

PSD bounds use canvas-relative coordinates `{top, left, bottom, right}`. Convert to UMG CanvasPanelSlot properties:

```
OffsetLeft   = bounds.left
OffsetTop    = bounds.top
OffsetRight  = bounds.right - bounds.left    (width, when using size mode)
OffsetBottom = bounds.bottom - bounds.top     (height, when using size mode)
```

When using anchors at (0,0)-(0,0) (top-left anchor), the offset values represent position and size directly:
- `OffsetLeft` = X position
- `OffsetTop` = Y position
- `OffsetRight` = Width
- `OffsetBottom` = Height

### Relative Positioning Within Groups

When a layer is inside a group, calculate its position relative to the group's bounds:

```
relative_left = child.bounds.left - group.bounds.left
relative_top  = child.bounds.top - group.bounds.top
```

This is important when the group maps to a sub-panel. The child's position within that panel should be relative to the panel's own origin, not the canvas origin.

### Opacity Mapping

PSD stores opacity as an integer from 0 to 255. UMG uses a float from 0.0 to 1.0:

```
RenderOpacity = psd_opacity / 255.0
```

Examples:
- PSD 255 (fully opaque) = UMG `1.0`
- PSD 191 (75%) = UMG `0.749`
- PSD 128 (50%) = UMG `0.502`
- PSD 0 (invisible) = UMG `0.0`

### Visibility Mapping

| PSD State | UMG Behavior |
|---|---|
| `visible: true` | Widget is visible — include it in the tree |
| `visible: false` | Set `Visibility` to `Collapsed` (hidden and takes no space) or `Hidden` (hidden but reserves space) |
| Layer exists but has 0x0 bounds | Skip — this is an empty layer |

Layers with `visible: false` are typically design alternatives or hidden states. Ask the user whether to include them as collapsed widgets or skip them entirely.

### DPI / Resolution Scaling

If the PSD canvas size differs from the target UI resolution, apply a uniform scale factor:

```
scale = target_width / psd_width
```

Apply this scale to all position and size values:

```
scaled_left   = bounds.left * scale
scaled_top    = bounds.top * scale
scaled_width  = (bounds.right - bounds.left) * scale
scaled_height = (bounds.bottom - bounds.top) * scale
```

**Common scenarios:**

| PSD Resolution | Target Resolution | Scale Factor |
|---|---|---|
| 1920x1080 | 1920x1080 | 1.0 (no scaling) |
| 3840x2160 | 1920x1080 | 0.5 (4K mockup to 1080p UI) |
| 2560x1440 | 1920x1080 | 0.75 |
| 1920x1080 | 3840x2160 | 2.0 (1080p mockup to 4K UI) |

If the PSD matches the target resolution, no scaling is needed. Always ask the user for the target resolution if it is not obvious from context.

### Anchoring Strategies

Anchors determine how a widget repositions when the screen size changes. Set anchors on CanvasPanelSlots using `AnchorMinX`, `AnchorMinY`, `AnchorMaxX`, `AnchorMaxY`.

**Full-screen element** (backgrounds, full-screen overlays):
```
AnchorMin = (0, 0), AnchorMax = (1, 1)
Offsets = (0, 0, 0, 0)
```

**Centered element** (centered dialog, popup):
```
AnchorMin = (0.5, 0.5), AnchorMax = (0.5, 0.5)
AlignmentX = 0.5, AlignmentY = 0.5
Offsets = (0, 0, width, height)
```

**Bottom bar** (action bar, HUD bar pinned to bottom):
```
AnchorMin = (0, 1), AnchorMax = (1, 1)
AlignmentY = 1.0
OffsetTop = -bar_height, OffsetBottom = bar_height
OffsetLeft = 0, OffsetRight = 0
```

**Top-left corner** (minimap, health display):
```
AnchorMin = (0, 0), AnchorMax = (0, 0)
Offsets = (margin_left, margin_top, width, height)
```

**Top-right corner** (score, currency display):
```
AnchorMin = (1, 0), AnchorMax = (1, 0)
AlignmentX = 1.0
Offsets = (-margin_right, margin_top, width, height)
```

**Bottom-left corner** (chat, ability bar):
```
AnchorMin = (0, 1), AnchorMax = (0, 1)
AlignmentY = 1.0
Offsets = (margin_left, -margin_bottom, width, height)
```

**Bottom-right corner** (minimap alternative):
```
AnchorMin = (1, 1), AnchorMax = (1, 1)
AlignmentX = 1.0, AlignmentY = 1.0
Offsets = (-margin_right, -margin_bottom, width, height)
```

Choose the anchor strategy based on where the element sits in the PSD canvas. Elements near edges should anchor to that edge. Elements in the center should anchor to center. Full-width elements should stretch between left and right anchors.

---

## Troubleshooting

### Texture appears black after import

- The source PNG may have been exported at canvas size instead of layer size. Re-export with `--layer-relative` to crop to actual layer bounds.
- The texture may not have imported correctly. Check the `claireon.asset.import_file` response for errors.
- Verify the Brush.ResourceObject path is correct by searching for the texture with `claireon.asset_search`.

### Widget not visible in the editor

- Check `RenderOpacity` is not 0.0.
- Check `Visibility` is set to `Visible` (not `Collapsed` or `Hidden`).
- Check that the widget has non-zero size. CanvasPanelSlot with OffsetRight=0 and OffsetBottom=0 produces a zero-size widget.
- Check the widget is added as a child of a panel that is itself visible and has size.

### Widget is the wrong size

- Verify the PSD bounds were converted correctly. `OffsetRight` and `OffsetBottom` in size mode represent width and height, not absolute right/bottom coordinates.
- Check whether DPI scaling was applied when it should not have been (or vice versa).
- For Image widgets, check `Brush.ImageSize` matches the texture dimensions.

### Textures appear blurry

- The import tool sets `TEXTUREGROUP_UI` and disables mipmaps by default. If the texture still appears blurry, verify the source PNG resolution matches the display size.
- Avoid scaling up small textures. If the PSD layer is 128x128 but displayed at 256x256, re-export at higher resolution or use a larger source.

### Missing layers in the manifest

- Layers with zero-size bounds (0x0) are empty and will not have exportable content.
- Adjustment layers, smart objects, and layer effects may not appear as separate pixel layers. Use `--flat` mode to see all layers regardless of hierarchy.
- Hidden layers (`visible: false`) are still included in the manifest but will not render on export unless explicitly selected with `--layers`.

### psd-rs export produces blank images

- Verify the layer name passed to `--layers` exactly matches the name in the manifest (case-sensitive).
- Glob patterns in `--layers` use standard glob syntax: `*` matches any characters, `?` matches one character.
- Check that the layer has pixel data (kind is `"pixel"`, not `"group"`). To export a group as a composite, use `--group` instead of `--layers`.

### Claireon MCP session errors

- If you get "session not found", the session may have timed out or been closed. Open a new session with `open` or `create`.
- If `compile` reports errors, use `get_state` to inspect the current widget tree for issues.
- If `add_widget` fails, verify the parent widget name is correct and is a panel type that accepts children.
