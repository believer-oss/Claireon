---
name: Create Interactive Architecture Diagram
description: Generate an interactive self-contained HTML architecture diagram for any system, with clickable nodes, sidebar details, layer filters, and animated data flow
type: resource
uri: claireon://instructions/architecture-viz
---

<!-- claude-hint:
model: sonnet
effort: high
rationale: Architecture HTML generation from codebase research + asset inspection
-->

Do not use instructions from this file unless asked.

> **Token placeholders**: This document uses `{{TOKEN}}` placeholders. `{{PROJECT_NAME}}` is project context (resolve from your environment); the diagram-template tokens (`{{NODE_ID}}`, `{{LAYER_KEY}}`, `{{COLOR}}`, etc.) are per-element fill-ins you assign while generating the diagram. See the legend at `claireon://instructions/token-legend` (fetch via `resources/read`).

# Create Interactive Architecture Diagram

Generate an interactive, self-contained HTML architecture diagram for any {{PROJECT_NAME}} system. The output is a single `.html` file with an SVG diagram, clickable nodes with a detail sidebar, layer filter buttons, connection highlighting, and animated data flow.

**This is the default document format for architecture documentation in `Docs/`.** All architecture diagrams must use this interactive SVG style — clickable nodes, sidebar detail panels, layer filtering, and animated data flow.

## Overview

The diagram consists of:
- **Header** with title, badge, and layer filter buttons
- **Sidebar** (left, 340px) with detail panels that swap when nodes are clicked
- **Canvas** (right, flex) with an SVG diagram containing:
  - Clickable node groups arranged in horizontal layers
  - Color-coded connection lines between nodes
  - Layer filter support (fading non-matching nodes)
  - Animated flow dot overlay
- **Legend** (bottom-left of canvas) showing color meanings

## Prerequisites

- Codebase search tools: Grep, Glob, Read
- Optionally MCP editor tools for blueprint inspection: `search_assets`, `bp_get_properties`, `bp_get_graph`

## Options

- `--system <name>`: Target system name (prompted interactively if not provided)
- `--scope <narrow|broad>`: How many related systems to include (default: narrow = 5-12 nodes, broad = 12-20 nodes)
- `--output <path>`: Output file path (default: `Docs/<system>.html`)

## Steps

### 1. Gather Requirements

If `--system` was not provided, ask the user:

> "Which system would you like to diagram? (e.g., Loot, Inventory, Targeting, Progression, Missions, Encounters)"

Confirm the scope: narrow (just the core system, 5-12 nodes) or broad (includes adjacent systems, 12-20 nodes).

### 2. Research Phase

Execute these sub-phases sequentially. The goal is to discover all architecturally significant classes, their properties, relationships, and source files.

#### 2a. C++ Source Search

Use Grep/Glob to find headers and source files:

```
Grep: "class U<ClassName>" or "class A<ClassName>" in *.h files
Glob: **/<ClassName>.h, **/<ClassName>.cpp
```

From each header, extract:
- `UPROPERTY` declarations (especially `FieldNotify`, `BindWidget`, `Replicated`, `BlueprintReadOnly`)
- `UFUNCTION` declarations (`BlueprintCallable`, `BlueprintImplementableEvent`, `BlueprintPure`, `Server`)
- Delegate declarations (`DECLARE_DYNAMIC_MULTICAST_DELEGATE`, `DECLARE_MULTICAST_DELEGATE`)
- Class hierarchy (parent class, interfaces)

#### 2b. Asset Discovery (if MCP available)

Use `search_assets` with the system keyword:

```
search_assets(query="<system>", class_filter="Blueprint", max_results=20)
search_assets(query="<system>", class_filter="WidgetBlueprint", max_results=20)
search_assets(query="WBP_<system>", max_results=10)
search_assets(query="VM<system>", class_filter="Blueprint", max_results=10)
```

#### 2c. Blueprint Inspection (if MCP available)

For each widget/blueprint found:
1. `bp_get_properties` — Properties, functions, event dispatchers
2. `bp_get_graph` with `format="json"` and `detail_level="full"` — Graph nodes and connections

Look for:
- ViewModel references (`UVM*` types)
- Delegate bindings, OnRep callbacks
- Child widget relationships (`BindWidget`)
- TileView/ListView patterns (`IUserObjectListEntry`)
- RPC calls (`Server*` patterns)

#### 2d. Backend/Service Investigation

If the system involves backend services:
- Search for Backend SDK usage: `BackendSDK`, `ServerRPC`, `Backend::` references
- Look for PlayerController RPCs: `Server_` or `ServerX` UFUNCTION patterns
- Find service request/response types

Skip this sub-phase if the system is purely client-side.

### 3. Build Node Catalog

Create an intermediate representation. For each node:

| Field | Description |
|-------|-------------|
| **id** | Unique identifier used in `data-detail` and `detail-<id>` (e.g., `lootmenu`) |
| **name** | Display name (e.g., `WBP_LootableMenu`) |
| **cppClass** | C++ class name (e.g., `UFSLootableMenuWidget`) |
| **parentClass** | Parent class (e.g., `UFSMenu`) |
| **assetPath** | Unreal content path if applicable |
| **layer** | One of: `widgets`, `viewmodels`, `gamelogic`, `data`, `subsystems`, `pragma` |
| **tags** | Array from: `cpp`, `bp`, `vm`, `struct`, `replicated`, `delegate`, `subsystem`, `pragma`, `rpc` |
| **properties** | Array of `{name, type, note, isFieldNotify, isDerived}` |
| **methods** | Array of `{name, note}` |
| **connections** | Array of `{targetNode, color, label, isDashed}` |
| **sourceFiles** | Paths for the detail panel |
| **description** | 1-2 sentence summary |

### 4. Choose Layers

Select 3-5 layers from this table. Layers are arranged top-to-bottom from user-facing to backend:

| Layer | Key | Stripe Color | Node BG | When to Use |
|-------|-----|-------------|---------|-------------|
| Widget | `widgets` | `#bc8cff` (purple) | `#1a2233` | Blueprint widgets, UMG components (WBP_*) |
| ViewModel | `viewmodels` | `#3fb950` (green) | `#172217` | MVVM view models, data binding (UVM*) |
| Game Logic | `gamelogic` | `#58a6ff` (blue) | `#1a2233` | Gameplay components, abilities, controllers |
| Data / Networking | `data` | `#d29922` (orange) | `#221a17` | Replicated components, structs, configs, data assets |
| Subsystem | `subsystems` | `#39d2c0` (cyan) | `#172227` | World/game subsystems, registries, singletons |
| Backend / Services | `pragma` | `#f85149` (red) | `#221722` | Backend service calls, Backend SDK, server RPCs |

**Layer combination examples:**
- **MVVM UI system** (loot, inventory): Widget → ViewModel → Data/Networking
- **GAS combat**: Widget → Game Logic (Abilities) → Data (Attributes/Effects)
- **Progression with Backend**: Widget → ViewModel → Game Logic → Backend/Services
- **Full-stack feature**: Widget → ViewModel → Game Logic → Data/Networking → Backend/Services

### 5. Generate HTML

Produce a single self-contained `.html` file following the exact template below. **Do not deviate from the design system.** The CSS, HTML structure, SVG patterns, and JavaScript must follow this specification.

### 6. Output & Verify

1. Write the HTML file to the output path
2. Read the file back to verify it was written correctly
3. If the file is in `Docs/`, also:
   - Follow all `Docs/` conventions (shared stylesheet link, nav link, DOC-TRACKING comment)
   - Add an entry to the `docs` array in `Docs/index.html`
4. Report to the user: file path, node count, layer count, and how to open it

---

## Complete HTML Template

The generated file MUST follow this exact structure. Everything below is the canonical template — copy it verbatim and populate the marked sections with system-specific content.

### Document Skeleton

```html
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>{{PROJECT_NAME}} {{SYSTEM_NAME}} — Interactive Architecture Diagram</title>
<style>
  /* PASTE FULL CSS FROM SECTION BELOW */
</style>
</head>
<body>

<header>
  <h1>{{PROJECT_NAME}} {{SYSTEM_NAME}}</h1>
  <span class="badge">{{BADGE_TEXT}}</span>
  <div class="controls">
    <button id="btn-all" class="active" onclick="showLayer('all')">All</button>
    <!-- One button per layer -->
    <button id="btn-{{LAYER_KEY}}" onclick="showLayer('{{LAYER_KEY}}')">{{LAYER_LABEL}}</button>
    <!-- Always include Data Flow last -->
    <button id="btn-flow" onclick="showLayer('flow')">Data Flow</button>
  </div>
</header>

<div class="layout">
  <div class="sidebar">
    <!-- Default panel (always present) -->
    <div id="detail-default" class="detail-panel active">
      <!-- Overview, Layer Filters, Key Patterns sections -->
    </div>
    <!-- One detail panel per node -->
    <div id="detail-{{NODE_ID}}" class="detail-panel">
      <!-- Node detail content -->
    </div>
  </div>

  <div class="canvas-area" id="canvas-area">
    <svg class="diagram" id="diagram" viewBox="0 0 1100 {{SVG_HEIGHT}}">
      <defs>
        <!-- Arrow markers + glow filter -->
      </defs>
      <!-- Background grid -->
      <!-- Layer labels + divider lines -->
      <!-- Connections group (BEFORE nodes) -->
      <g id="connections">
        <!-- Connection paths -->
      </g>
      <!-- Node groups -->
      <!-- Flow overlay (hidden) -->
      <g id="flow-overlay" style="display:none">
        <!-- Animated dots -->
      </g>
    </svg>

    <div class="legend">
      <!-- Legend items -->
    </div>
  </div>
</div>

<div class="tooltip" id="tooltip"></div>

<script>
  /* PASTE FULL JS FROM SECTION BELOW */
</script>
</body>
</html>
```

### Full CSS (copy verbatim)

```css
:root {
  --bg: #0d1117;
  --surface: #161b22;
  --surface2: #1c2333;
  --border: #30363d;
  --border-hl: #58a6ff;
  --text: #c9d1d9;
  --text-dim: #8b949e;
  --text-bright: #f0f6fc;
  --blue: #58a6ff;
  --green: #3fb950;
  --purple: #bc8cff;
  --orange: #d29922;
  --red: #f85149;
  --cyan: #39d2c0;
  --pink: #f778ba;
  --yellow: #e3b341;
}
* { box-sizing: border-box; margin: 0; padding: 0; }
body {
  font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
  background: var(--bg);
  color: var(--text);
  min-height: 100vh;
  overflow-x: hidden;
}
header {
  background: var(--surface);
  border-bottom: 1px solid var(--border);
  padding: 20px 32px;
  display: flex;
  align-items: center;
  gap: 16px;
}
header h1 {
  font-size: 20px;
  color: var(--text-bright);
  font-weight: 600;
}
header .badge {
  background: var(--blue);
  color: #000;
  padding: 2px 10px;
  border-radius: 12px;
  font-size: 12px;
  font-weight: 600;
}
.controls {
  margin-left: auto;
  display: flex;
  gap: 8px;
}
.controls button {
  background: var(--surface2);
  border: 1px solid var(--border);
  color: var(--text);
  padding: 6px 14px;
  border-radius: 6px;
  cursor: pointer;
  font-size: 13px;
  transition: all 0.15s;
}
.controls button:hover { border-color: var(--blue); color: var(--blue); }
.controls button.active { background: var(--blue); color: #000; border-color: var(--blue); }

.layout {
  display: flex;
  height: calc(100vh - 65px);
}
.sidebar {
  width: 340px;
  min-width: 340px;
  background: var(--surface);
  border-right: 1px solid var(--border);
  overflow-y: auto;
  padding: 16px;
}
.sidebar h2 {
  font-size: 13px;
  text-transform: uppercase;
  letter-spacing: 1px;
  color: var(--text-dim);
  margin-bottom: 12px;
}
.sidebar .section { margin-bottom: 24px; }

.prop-table {
  width: 100%;
  border-collapse: collapse;
  font-size: 12px;
}
.prop-table th {
  text-align: left;
  padding: 4px 8px;
  color: var(--text-dim);
  border-bottom: 1px solid var(--border);
  font-weight: 500;
}
.prop-table td {
  padding: 4px 8px;
  border-bottom: 1px solid rgba(48,54,61,0.5);
  vertical-align: top;
}
.prop-table .prop-name { color: var(--cyan); font-family: 'Cascadia Code', 'Consolas', monospace; }
.prop-table .prop-type { color: var(--purple); font-family: 'Cascadia Code', 'Consolas', monospace; font-size: 11px; }
.prop-table .prop-note { color: var(--text-dim); font-size: 11px; }
.prop-table .field-notify { color: var(--green); font-size: 10px; font-weight: 600; }
.prop-table .derived { color: var(--orange); font-size: 10px; font-weight: 600; }

.detail-panel {
  display: none;
  animation: fadeIn 0.2s ease;
}
.detail-panel.active { display: block; }
.detail-title {
  font-size: 16px;
  font-weight: 600;
  color: var(--text-bright);
  margin-bottom: 4px;
}
.detail-subtitle {
  font-size: 12px;
  color: var(--text-dim);
  margin-bottom: 12px;
  font-family: 'Cascadia Code', 'Consolas', monospace;
}
.detail-desc {
  font-size: 13px;
  color: var(--text);
  line-height: 1.5;
  margin-bottom: 16px;
}
.tag {
  display: inline-block;
  padding: 2px 8px;
  border-radius: 4px;
  font-size: 11px;
  font-weight: 600;
  margin-right: 4px;
  margin-bottom: 4px;
}
.tag-cpp { background: rgba(88,166,255,0.15); color: var(--blue); }
.tag-bp { background: rgba(188,140,255,0.15); color: var(--purple); }
.tag-vm { background: rgba(63,185,80,0.15); color: var(--green); }
.tag-struct { background: rgba(210,153,34,0.15); color: var(--orange); }
.tag-replicated { background: rgba(248,81,73,0.15); color: var(--red); }
.tag-delegate { background: rgba(247,120,186,0.15); color: var(--pink); }
.tag-subsystem { background: rgba(57,210,192,0.15); color: var(--cyan); }

.canvas-area {
  flex: 1;
  position: relative;
  overflow: hidden;
}
svg.diagram {
  width: 100%;
  height: 100%;
}

/* SVG node styling */
.node-group { cursor: pointer; }
.node-group:hover .node-bg { filter: brightness(1.2); }
.node-group.selected .node-bg { stroke: var(--blue); stroke-width: 2; }
.node-bg { rx: 8; ry: 8; transition: filter 0.15s; }
.node-title { font-size: 13px; font-weight: 600; fill: var(--text-bright); font-family: 'Segoe UI', system-ui, sans-serif; }
.node-subtitle { font-size: 10px; fill: var(--text-dim); font-family: 'Cascadia Code', 'Consolas', monospace; }
.node-badge { font-size: 9px; font-weight: 700; font-family: 'Segoe UI', system-ui, sans-serif; }

/* Connection lines */
.conn { stroke-width: 1.5; fill: none; opacity: 0.5; transition: opacity 0.2s; }
.conn.highlighted { opacity: 1; stroke-width: 2.5; }
.conn-label { font-size: 10px; fill: var(--text-dim); font-family: 'Segoe UI', system-ui, sans-serif; }

/* Legend */
.legend {
  position: absolute;
  bottom: 16px;
  left: 16px;
  background: var(--surface);
  border: 1px solid var(--border);
  border-radius: 8px;
  padding: 12px 16px;
  font-size: 11px;
  display: flex;
  gap: 16px;
  flex-wrap: wrap;
}
.legend-item { display: flex; align-items: center; gap: 6px; }
.legend-swatch {
  width: 12px; height: 12px; border-radius: 3px;
}

@keyframes fadeIn { from { opacity: 0; } to { opacity: 1; } }

/* Tooltip */
.tooltip {
  position: absolute;
  background: var(--surface2);
  border: 1px solid var(--border);
  border-radius: 6px;
  padding: 8px 12px;
  font-size: 12px;
  color: var(--text);
  pointer-events: none;
  opacity: 0;
  transition: opacity 0.15s;
  z-index: 100;
  max-width: 260px;
}
.tooltip.visible { opacity: 1; }

/* Flow indicator */
.flow-label {
  position: absolute;
  background: rgba(88,166,255,0.1);
  border: 1px dashed var(--blue);
  border-radius: 6px;
  padding: 4px 10px;
  font-size: 11px;
  color: var(--blue);
  pointer-events: none;
}
```

### SVG Structure

#### ViewBox sizing

- 3 layers: `viewBox="0 0 1100 720"`
- 4 layers: `viewBox="0 0 1100 920"`
- 5 layers: `viewBox="0 0 1100 1120"`

#### Defs block (always include all)

```xml
<defs>
  <marker id="arrow-blue" markerWidth="8" markerHeight="6" refX="8" refY="3" orient="auto">
    <polygon points="0 0, 8 3, 0 6" fill="#58a6ff"/>
  </marker>
  <marker id="arrow-green" markerWidth="8" markerHeight="6" refX="8" refY="3" orient="auto">
    <polygon points="0 0, 8 3, 0 6" fill="#3fb950"/>
  </marker>
  <marker id="arrow-purple" markerWidth="8" markerHeight="6" refX="8" refY="3" orient="auto">
    <polygon points="0 0, 8 3, 0 6" fill="#bc8cff"/>
  </marker>
  <marker id="arrow-orange" markerWidth="8" markerHeight="6" refX="8" refY="3" orient="auto">
    <polygon points="0 0, 8 3, 0 6" fill="#d29922"/>
  </marker>
  <marker id="arrow-red" markerWidth="8" markerHeight="6" refX="8" refY="3" orient="auto">
    <polygon points="0 0, 8 3, 0 6" fill="#f85149"/>
  </marker>
  <marker id="arrow-cyan" markerWidth="8" markerHeight="6" refX="8" refY="3" orient="auto">
    <polygon points="0 0, 8 3, 0 6" fill="#39d2c0"/>
  </marker>
  <marker id="arrow-pink" markerWidth="8" markerHeight="6" refX="8" refY="3" orient="auto">
    <polygon points="0 0, 8 3, 0 6" fill="#f778ba"/>
  </marker>
  <filter id="glow">
    <feGaussianBlur stdDeviation="3" result="blur"/>
    <feMerge><feMergeNode in="blur"/><feMergeNode in="SourceGraphic"/></feMerge>
  </filter>
</defs>
```

#### Background grid (always include)

```xml
<pattern id="grid" width="20" height="20" patternUnits="userSpaceOnUse">
  <path d="M 20 0 L 0 0 0 20" fill="none" stroke="rgba(48,54,61,0.3)" stroke-width="0.5"/>
</pattern>
<rect width="1100" height="{{SVG_HEIGHT}}" fill="url(#grid)"/>
```

#### Layer labels + dividers

```xml
<text x="48" y="30" font-size="11" fill="#8b949e" font-family="Segoe UI, sans-serif" font-weight="600" letter-spacing="1">{{LAYER_NAME}} LAYER</text>
<line x1="40" y1="38" x2="1060" y2="38" stroke="rgba(48,54,61,0.5)" stroke-width="1" stroke-dasharray="4"/>
```

Distribute layer Y positions evenly across the viewBox height. For 3 layers in a 720px viewBox, use approximately Y=30/38, Y=310/318, Y=500/508.

### Node Anatomy

Every node is a `<g class="node-group">` with these exact attributes and children:

```xml
<g class="node-group" data-layer="{{LAYER_KEY}}" data-detail="{{NODE_ID}}" onclick="selectNode(this)">
  <!-- Background rect -->
  <rect class="node-bg" x="X" y="Y" width="W" height="H" fill="{{LAYER_BG}}" stroke="#30363d"/>
  <!-- Left color stripe (4px wide, matches layer color) -->
  <rect x="X" y="Y" width="4" height="H" rx="2" fill="{{LAYER_COLOR}}"/>
  <!-- Title (offset x+16, y+25 from rect) -->
  <text class="node-title" x="X+16" y="Y+25">{{NODE_NAME}}</text>
  <!-- Subtitle (offset x+16, y+45 from rect) -->
  <text class="node-subtitle" x="X+16" y="Y+45">{{CPP_CLASS}} : {{PARENT}}</text>
  <!-- Optional content lines (offset y+65, y+80, etc.) -->
  <text x="X+16" y="Y+65" font-size="10" fill="{{ACCENT_COLOR}}" font-family="Cascadia Code, Consolas, monospace">{{PROPERTY_LINE}}</text>
</g>
```

**Sizing guidelines:**
- Small node (name + subtitle only): 170-220px wide, 55-70px tall
- Medium node (+ 1-2 content lines): 220-305px wide, 80-110px tall
- Large node (+ 3-5 content lines): 250-305px wide, 105-140px tall

**Content line colors by layer:**
- Widgets: `#8b949e` (dim) or `#3fb950` (green, for VM creation notes)
- ViewModels: `#39d2c0` (cyan, for properties), `#8b949e` (dim, for counts)
- Data: `#f778ba` (pink, for delegates), `#f85149` (red, for replication), `#d29922` (orange, for config)

**Positioning rules:**
- Nodes in the same layer share similar Y positions within that layer's band
- Space nodes horizontally with ~40-80px gaps
- Start the leftmost node at x=60
- Keep nodes within x=60 to x=1060

### Connection Patterns

Connections are `<path>` or `<line>` elements inside `<g id="connections">`, drawn BEFORE node groups in SVG source so nodes render on top.

```xml
<path class="conn" id="conn-{{SOURCE}}-{{TARGET}}" d="M x1 y1 L x2 y2" stroke="{{COLOR}}" marker-end="url(#arrow-{{COLOR_NAME}})"/>
<text class="conn-label" x="labelX" y="labelY">{{LABEL}}</text>
```

**Connection ID format:** `conn-{{source_short}}-{{target_short}}` (e.g., `conn-menu-display`, `conn-comp-config`)

**Path types:**
- Straight: `M x1 y1 L x2 y2` — short, same-layer connections
- Curved: `M x1 y1 Q cx cy x2 y2` — cross-layer or long connections
- Multi-segment: `M x1 y1 L mx my Q cx cy x2 y2` — complex routing

**Stroke styles:**
- Solid: normal relationships (binds, creates, wraps, delegates)
- Dashed (`stroke-dasharray="4"`): RPC calls, async operations, soft references

**Color by relationship type:**

| Relationship | Color | Marker | Example |
|-------------|-------|--------|---------|
| BindWidget / child widget | `#bc8cff` purple | `arrow-purple` | Menu → Display |
| Creates / wraps VM | `#3fb950` green | `arrow-green` | Display → VMLootSlot |
| Data flow / FieldNotify | `#58a6ff` blue | `arrow-blue` | VM → Widget binding |
| Config / data reference | `#d29922` orange | `arrow-orange` | Component → Config |
| Delegate / event | `#f778ba` pink | `arrow-pink` | Component → Display |
| Server RPC | `#f85149` red | `arrow-red` | Widget → PlayerController |
| Subsystem lookup | `#39d2c0` cyan | `arrow-cyan` | Registry → VM |

### Sidebar Detail Panels

#### Default panel (always present, shown when no node selected)

```html
<div id="detail-default" class="detail-panel active">
  <div class="section">
    <h2>Overview</h2>
    <p class="detail-desc">Click any node in the diagram to inspect its properties, view model bindings, and relationships.</p>
    <p class="detail-desc">{{SYSTEM_OVERVIEW_TEXT}}</p>
  </div>
  <div class="section">
    <h2>Layer Filters</h2>
    <p class="detail-desc" style="font-size:12px;">
      <strong>{{Layer1}}</strong> — {{description}}<br>
      <strong>{{Layer2}}</strong> — {{description}}<br>
      <strong>Data Flow</strong> — Animated data flow sequence
    </p>
  </div>
  <div class="section">
    <h2>Key Patterns</h2>
    <p class="detail-desc" style="font-size:12px;">{{NOTABLE_PATTERNS}}</p>
  </div>
</div>
```

#### Node detail panel

```html
<div id="detail-{{NODE_ID}}" class="detail-panel">
  <div class="detail-title">{{NODE_NAME}}</div>
  <div class="detail-subtitle">{{CPP_CLASS}} : {{PARENT}}</div>
  <div style="margin-bottom:8px">
    <span class="tag tag-bp">Blueprint</span>
    <span class="tag tag-cpp">C++ Base</span>
    <!-- Add tags as applicable: tag-vm, tag-struct, tag-replicated, tag-delegate, tag-subsystem -->
  </div>
  <p class="detail-desc">{{1-2 sentence description}}</p>

  <!-- Repeat sections as needed -->
  <div class="section">
    <h2>{{Section Title}}</h2>
    <table class="prop-table">
      <tr><th>Property</th><th>Type</th><th></th></tr>
      <tr>
        <td class="prop-name">{{Name}}</td>
        <td class="prop-type">{{Type}}</td>
        <td><span class="field-notify">FieldNotify</span></td>
      </tr>
    </table>
  </div>

  <div class="section">
    <h2>Source Files</h2>
    <p style="font-size:12px; color:var(--text-dim); font-family:monospace;">
      {{ClassName}}.h / .cpp<br>
      /Game/Path/To/Asset
    </p>
  </div>
</div>
```

**Common section types (use as applicable):**
- `BindWidget Properties` — for widgets with C++ BindWidget members
- `FieldNotify Properties` — for VMs with FieldNotify UPROPERTY
- `Derived Properties` — for VM computed properties (use `<span class="derived">Derived</span>`)
- `Key Methods` — notable functions
- `Blueprint Events` — BlueprintImplementableEvent functions
- `Delegate Bindings` — which delegates the class binds to and handlers
- `Delegates` — delegates the class declares
- `Replicated State` — replicated properties
- `Blueprint Graph Flow` — step-by-step BP graph description
- `Child Widgets` — nested widget references
- `VM Binding (Blueprint)` — how BP binds widget elements to VM properties
- `Configuration` — for data assets / config classes
- `Structs` — for struct collection nodes
- `Loot RPCs` / `RPCs` — for controller RPC endpoints

### Flow Animation Overlay

A hidden `<g>` containing animated circles that trace key data paths. Include 2-4 flow dots.

```xml
<g id="flow-overlay" style="display:none">
  <circle r="5" fill="{{PATH_COLOR}}" filter="url(#glow)" opacity="0">
    <animateMotion dur="{{2-4s}}" repeatCount="indefinite" path="M x1 y1 L x2 y2 ..."/>
    <animate attributeName="opacity" values="0;1;1;0" dur="{{SAME_DUR}}" repeatCount="indefinite"/>
  </circle>
  <!-- Stagger begin times: 0s, 0.5s, 1s, 1.5s -->
  <circle r="5" fill="{{PATH_COLOR}}" filter="url(#glow)" opacity="0">
    <animateMotion dur="{{DUR}}" repeatCount="indefinite" begin="1s" path="M ..."/>
    <animate attributeName="opacity" values="0;1;1;0" dur="{{DUR}}" repeatCount="indefinite" begin="1s"/>
  </circle>
</g>
```

Pick 2-4 key data flow paths (e.g., server→component→VM→widget, widget→RPC→server). Each dot follows the path coordinates of the corresponding connection lines.

### Legend

```html
<div class="legend">
  <div class="legend-item"><div class="legend-swatch" style="background:{{COLOR}}"></div>{{LABEL}}</div>
  <!-- One entry per color used in the diagram -->
</div>
```

### Full JavaScript (copy verbatim, populate the `related` lookup)

```javascript
const nodeGroups = document.querySelectorAll('.node-group');
const connections = document.querySelectorAll('.conn');
const detailPanels = document.querySelectorAll('.detail-panel');
const flowOverlay = document.getElementById('flow-overlay');

let selectedNode = null;
let currentLayer = 'all';

function selectNode(el) {
  if (selectedNode) selectedNode.classList.remove('selected');
  selectedNode = el;
  el.classList.add('selected');

  const detailId = el.dataset.detail;
  detailPanels.forEach(p => p.classList.remove('active'));
  const panel = document.getElementById('detail-' + detailId);
  if (panel) panel.classList.add('active');

  highlightConnections(detailId);
}

function highlightConnections(nodeId) {
  // POPULATE: map each nodeId to its related connection element IDs
  const related = {
    '{{nodeId}}': ['conn-{{src}}-{{tgt}}', ...],
    // ... one entry per node
  };

  const ids = related[nodeId] || [];
  connections.forEach(c => {
    c.classList.toggle('highlighted', ids.includes(c.id));
  });
}

function showLayer(layer) {
  currentLayer = layer;

  document.querySelectorAll('.controls button').forEach(b => b.classList.remove('active'));
  document.getElementById('btn-' + layer).classList.add('active');

  flowOverlay.style.display = layer === 'flow' ? 'block' : 'none';

  nodeGroups.forEach(g => {
    if (layer === 'all' || layer === 'flow') {
      g.style.opacity = '1';
    } else {
      g.style.opacity = g.dataset.layer === layer ? '1' : '0.15';
    }
  });

  // POPULATE: map each layer key to its relevant connection IDs
  const connLayers = {
    '{{LAYER_KEY}}': ['conn-...', ...],
    // ... one entry per layer
  };

  connections.forEach(c => {
    if (layer === 'all' || layer === 'flow') {
      c.style.opacity = '';
    } else {
      const layerConns = connLayers[layer] || [];
      c.style.opacity = layerConns.includes(c.id) ? '' : '0.08';
    }
  });

  if (selectedNode) {
    selectedNode.classList.remove('selected');
    selectedNode = null;
    detailPanels.forEach(p => p.classList.remove('active'));
    document.getElementById('detail-default').classList.add('active');
    connections.forEach(c => c.classList.remove('highlighted'));
  }
}

// Click empty space to deselect
document.getElementById('diagram').addEventListener('click', function(e) {
  if (e.target === this || e.target.tagName === 'rect' && e.target.getAttribute('fill') === 'url(#grid)') {
    if (selectedNode) {
      selectedNode.classList.remove('selected');
      selectedNode = null;
      detailPanels.forEach(p => p.classList.remove('active'));
      document.getElementById('detail-default').classList.add('active');
      connections.forEach(c => c.classList.remove('highlighted'));
    }
  }
});
```

---

## Error Handling

- **MCP tools unavailable**: Fall back to C++ source analysis only. Note any BPs as "inspection unavailable" in the node catalog.
- **No assets found**: Try broader search terms. Ask user for alternative keywords or asset paths.
- **System too large (>20 nodes)**: Ask user to narrow scope. Suggest splitting into sub-diagrams.
- **System too small (<3 nodes)**: Expand scope to include adjacent systems.

## Important Notes

- **Self-contained**: The HTML file must work when opened directly in a browser with zero external dependencies
- **Connections before nodes**: Put `<g id="connections">` before node groups in SVG so nodes render on top
- **Connection IDs must match**: The `highlightConnections` lookup table must reference the exact `id` attributes on connection path elements
- **Layer keys must match**: Every `data-layer` on nodes, every filter button `id`, and every entry in `connLayers` must use the same key strings
- **Keep detail panels focused**: Highlight FieldNotify, Replicated, BindWidget, and key methods. Don't list every UPROPERTY.
- **Node positioning is manual**: Calculate reasonable X/Y positions. Nodes in the same layer should be at similar Y positions.
- **Test mentally**: Before writing, verify all node IDs, connection IDs, and lookup tables are consistent
- **Docs/ conventions**: If placed in `Docs/`, also include `<link rel="stylesheet" href="styles/shared.css">`, `<a href="index.html" class="doc-nav-home">` nav link, and a `DOC-TRACKING` JSON comment. Add an entry to the `docs` array in `Docs/index.html`.

## Example

The {{PROJECT_NAME}} Loot System diagram is an example of this style in practice. It demonstrates:
- 11 nodes across 3 layers (Widgets, ViewModels, Data/Networking)
- 13 connections with color-coded relationship labels
- Full sidebar with detail panels per node
- Layer filtering with opacity transitions
- 3 animated flow dots tracing data paths through the system
- Click-to-select with connection highlighting
- Click-empty-space to deselect
