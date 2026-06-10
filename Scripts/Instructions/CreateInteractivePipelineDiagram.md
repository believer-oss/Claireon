Do not use instructions from this file unless asked.

# Create Interactive Pipeline Diagram

Generate an interactive, self-contained HTML diagram documenting a sequential pipeline (processing stages, data transformations, request lifecycles). Each stage is rendered as a three-column card showing its configuration inputs, core logic, and runtime data effects. The diagram is data-driven: a JavaScript array defines stages and JS renders all HTML at load time.

## Overview

This workflow produces a single `.html` file that visually documents a pipeline:
- **Vertical stage flow** — numbered stage cards connected by arrows
- **Three-column stage cards** — Config Inputs (left, green) | Stage Core (center, blue) | Runtime Data (right, orange)
- **Callback pills** below each card showing interface hooks (Will/Did/Per-entity pattern)
- **Clickable stages** with a detail sidebar showing full field tables and notes
- **Filter buttons** to dim/highlight config, runtime data, or callbacks independently
- **Conditional stage indicators** (cyan) for stages gated by boolean flags
- **Terminal node** for the pipeline completion signal
- **Data-driven rendering** — all stage content lives in a JS `STAGES` array; the rendering functions generate HTML from it

This complements `CreateInteractiveArchitecture.md` (node-graph diagrams for class relationships). Use **architecture diagrams** for "what are the classes and how do they connect." Use **pipeline diagrams** for "what are the stages and how does data flow through them."

## Prerequisites

- Source documentation describing the pipeline stages, their inputs/outputs, and lifecycle hooks
- The pipeline must be sequential (stages execute in order, possibly with conditional skips)
- Knowledge of the config object, runtime/state object, and callback interface for the system

## Options

- `--pipeline <name>`: Target pipeline name (prompted interactively if not provided)
- `--source <paths>`: Documentation files or source headers to analyze
- `--output <path>`: Output file path (default: `<source-dir>/<pipeline>_pipeline_stages_diagram.html`)

## Steps

### 1. Gather Requirements

If `--pipeline` was not provided, ask:

> "Which pipeline would you like to diagram? Provide a name and point me to the documentation or source files that describe the stages."

Confirm the scope: how many stages, whether there is a config object, a runtime state object, and an owner/callback interface.

### 2. Research Phase

Read the source documentation (design docs, header files, or markdown specs) and extract:

1. **Stage inventory** — ordered list of stages with numbers, names, and any conditional gates
2. **Config object fields** — for each stage, which fields on the config object are consumed
3. **Runtime data reads/writes** — for each stage, which fields on the runtime state are read from prior stages and which are written
4. **Callbacks** — for each stage, the interface methods called (Will/Did pattern, per-entity events)
5. **Special behaviors** — conditional reordering, async/sync branching, wait/bypass mechanisms, mutual exclusivity rules
6. **Terminal event** — what fires when the full pipeline completes

For each config field, capture: name, type, and a one-line description.
For each runtime field, capture: name, type, which stage writes it, and which stages read it.
For each callback, capture: name, kind (will/did/per-entity), and what the owner can do in it.

### 3. Build Stage Catalog

Structure each stage as a JavaScript object matching this schema:

```javascript
{
  id: 'sN',                    // Unique ID: 's1', 's2', 's3h' (for 3.5), etc.
  num: 'N',                    // Display number: '1', '1.5', '2', etc.
  name: 'STAGE NAME',          // ALL-CAPS short name
  conditional: null,           // null if always runs, or string describing the gate condition
  isSpecial: false,            // true if stage needs custom rendering (e.g., sub-modes)
  desc: 'HTML string...',      // Stage description. May contain <code>, <strong>, <em> tags.
  logicNote: 'HTML string...', // Implementation note. May contain <code> tags. null if none.
  config: [                    // Config fields consumed by this stage
    { n: 'FieldName', t: 'FieldType', d: 'One-line description.' },
  ],
  rtReads: [                   // Runtime data fields READ from prior stages
    { n: 'FieldName', t: 'FieldType', d: 'Which stage wrote it and what it represents.' },
  ],
  rtWrites: [                  // Runtime data fields WRITTEN by this stage
    { n: 'FieldName', t: 'FieldType', d: 'What it represents and which later stages read it.' },
  ],
  callbacks: [                 // Interface hooks called during this stage
    { n: 'CallbackName', k: 'will|did|per|none', d: 'What the owner can do in this callback.' },
  ],
  notes: [                     // Special behavior notes shown in the sidebar
    { k: 'cyan|blue|orange|red|purple|pink|green', t: 'Plain text note content.' },
  ],
  // Optional: custom arrays for stages with sub-modes (e.g., arrival modes)
  subModes: [
    { key: 'mode_id', label: 'Display Label', cls: 'css-class', desc: 'Mode description.' },
  ],
}
```

Also define a terminal object:

```javascript
{
  id: 'terminal',
  name: 'PIPELINE COMPLETE SIGNAL NAME',
  desc: 'When this fires and what it means.',
  callbacks: [ { n: 'CallbackName', k: 'did|per', d: 'Description.' } ],
  useCases: [ 'Use case 1', 'Use case 2' ],
  notes: [ { k: 'red', t: 'Important caveats.' } ],
}
```

### 4. Generate HTML Diagram

Using the stage catalog, generate a self-contained HTML file following the template specification below. The entire diagram is a single `.html` file with inline CSS and JavaScript.

### 5. Output & Verify

1. Write the HTML file to the output path
2. Verify stage count matches expectations
3. Report to user: file path, stage count, and how to open it

---

## HTML Template Specification

### Document Structure

```html
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title><Pipeline Name> — Stage Reference</title>
  <style>/* Inline CSS */</style>
</head>
<body>
  <header><!-- Title, badge, filter buttons --></header>
  <div class="layout">
    <div class="sidebar"><!-- Default overview panel + JS-injected stage details --></div>
    <div class="canvas-area">
      <div class="pipeline" id="pipeline-root"><!-- JS-rendered stage cards --></div>
    </div>
  </div>
  <div class="legend"><!-- Color legend --></div>
  <script>
    const STAGES = [ /* stage objects */ ];
    const TERMINAL = { /* terminal object */ };
    /* rendering functions + interactivity */
  </script>
</body>
</html>
```

### CSS Design System

Use the same dark-theme color variables as `CreateInteractiveArchitecture.md`:

```css
:root {
  --bg: #0d1117;       --surface: #161b22;    --surface2: #1c2333;
  --border: #30363d;   --text: #c9d1d9;       --text-dim: #8b949e;
  --text-bright: #f0f6fc;
  --blue: #58a6ff;     --green: #3fb950;       --purple: #bc8cff;
  --orange: #d29922;   --red: #f85149;         --cyan: #39d2c0;
  --pink: #f778ba;     --yellow: #e3b341;
}
```

**Layout**: Full-height body with flex column. Header (surface bg) at top. Below: flex row with `.sidebar` (360px fixed, surface bg, scrollable) on left and `.canvas-area` (flex:1, scrollable both axes) on right.

**Header**: `h1` (18px), `.badge` pill (purple bg, black text, 11px), `.controls` div (margin-left:auto, flex row of filter buttons matching architecture diagram styling).

### Three-Column Stage Card

Each stage renders as a `.stage-block` div containing a `.stage-panels` CSS grid with three columns:

```
+-------------------+-----------------------+-------------------+
|  CONFIG INPUTS     |   (1) STAGE NAME        |  RUNTIME DATA   |
|  (green border)  |   Description text    |  (orange border) |
|                  |   Logic note          |                  |
|  FieldName       |   [CONDITIONAL]       |  READS:          |
|   FieldType      |   [SYNC/ASYNC]        |   FieldName      |
|   Description    |                       |                  |
|                  |                       |  WRITES:         |
|  FieldName       |                       |   FieldName      |
|   FieldType      |                       |                  |
|   Description    |                       |                  |
+-------------------+-----------------------+-------------------+
  Callbacks: [ReceiveWill...] [ReceiveDid...] [PerEntityCb...]
```

**Grid**: `grid-template-columns: 340px 1fr 340px` — fixed side panels, flexible center.

#### Config Panel (Left)

- Background: `rgba(63,185,80,0.05)` with `rgba(63,185,80,0.2)` border
- Border radius: `7px 0 0 7px` (rounded left corners only)
- Header: "CONFIG INPUTS" in green, 9px, uppercase, letter-spacing 1.5px
- Each field entry: `.field-name` (cyan, monospace, 10.5px) + `.field-type` (purple, monospace, 9.5px) + `.field-note` (dim text, 10px)

#### Stage Center

- Background: `rgba(88,166,255,0.06)` with top/bottom border `rgba(88,166,255,0.2)`
- For conditional stages: background `rgba(57,210,192,0.05)` with cyan borders
- Stage header row: numbered badge (26px circle, blue bg or cyan for conditional) + stage name (13px bold) + optional badges
- Description: 11px, may contain `<code>` tags (styled yellow on subtle yellow bg)
- Logic note: 10px dim text on surface2 bg, rounded corners

#### Runtime Panel (Right)

- Background: `rgba(210,153,34,0.05)` with `rgba(210,153,34,0.2)` border
- Border radius: `0 7px 7px 0` (rounded right corners only)
- Two sections: **READS** (dim field names, dim types — data from prior stages) and **WRITES** (yellow field names — data this stage produces)
- Same field-entry format as config panel but with read/write visual distinction

#### Callback Row

- Below the three-column grid, flex-wrap row of callback pills
- "Callbacks:" label in dim uppercase
- Each pill: monospace 10px, rounded, colored by kind:
  - `.will` — blue (pre-stage hook)
  - `.did` — green (post-stage hook)
  - `.per` — purple (per-entity event)
  - `.none` — dim italic (no callbacks for this stage)

### Stage Connector

Between stage blocks, a `.stage-connector` div:
- Centered vertical line (2px, border color)
- Downward arrow (CSS triangle)
- Optional label pill (9px, dim text, border, rounded, positioned absolute center) showing the condition for the next stage if it is conditional

### Terminal Block

The final node uses a distinct style:
- Yellow-tinted background and border (`rgba(227,179,65,0.06)`)
- Checkmark icon in yellow circle
- Two-column body: use cases on the left, callbacks + notes on the right

### Sidebar Detail Panels

#### Default Panel

Visible when no stage is selected. Contains:
1. **Title and instructions** — "Click any stage to inspect..."
2. **Reading the diagram** — explains the three columns and color coding
3. **Pipeline-specific patterns** — key architectural patterns of this pipeline (e.g., owner override mechanism, async branching, wait/bypass, mutual exclusivity)

Use `.sb-note` blocks with colored left borders for pattern descriptions.

#### Per-Stage Panels

Generated by JavaScript from the STAGES data. Each panel contains:
1. **Stage title** with number and conditional/async badges
2. **Description** (HTML, may contain `<code>` tags)
3. **Logic note** in a `.sb-note.blue` block
4. **Config Inputs** table (`.dtable` — 3-column: name, type, description)
5. **Runtime Data — Reads** table
6. **Runtime Data — Writes** table
7. **Sub-modes** section (if the stage has them, e.g., arrival modes)
8. **Owner Callbacks** with description per callback
9. **Notes & Special Behaviors** as `.sb-note` blocks

### Filter Buttons

Minimum set: **All**, **Config Inputs**, **Runtime Data**, **Callbacks**.

Filters work by toggling `.hide-config`, `.hide-runtime`, `.hide-callbacks` CSS classes on the respective panel elements. These classes set `opacity: 0.08` to dim without removing — the layout stays stable.

### JavaScript Architecture

The diagram is **data-driven**. All pipeline content lives in `const STAGES = [...]` and `const TERMINAL = {...}`. Rendering functions generate HTML from the data:

```javascript
// Core data
const STAGES = [ /* stage objects per schema above */ ];
const TERMINAL = { /* terminal object */ };

// Rendering functions (generate HTML strings from data)
function escHtml(s) { /* escape & < > for safe insertion */ }
function renderFieldList(fields) { /* -> field-entry divs */ }
function renderCallbackPills(cbs) { /* -> callback pill spans */ }
function renderStage(stage, index) { /* -> full stage-block div */ }
function renderConnector(label) { /* -> stage-connector div */ }
function renderTerminal() { /* -> terminal-block div */ }

// Pipeline assembly
function buildPipeline() {
  // Iterate STAGES, call renderStage + renderConnector for each
  // Append renderTerminal at the end
  // Set innerHTML on #pipeline-root
}

// Sidebar detail generation
function buildSidebarDetail(stage) { /* -> detail panel HTML for one stage */ }
function buildTerminalDetail() { /* -> detail panel HTML for terminal */ }
function buildAllSidebarDetails() {
  // Iterate STAGES, call buildSidebarDetail for each
  // Append buildTerminalDetail
  // Set innerHTML on #sidebar-detail-content
}

// Interactivity
function selectStage(id) {
  // Toggle selection on stage block (CSS box-shadow)
  // Toggle detail panel visibility in sidebar
  // Clicking already-selected stage deselects -> shows default panel
}

function setFilter(filterKey) {
  // Toggle active class on filter buttons
  // Toggle hide-* classes on config/runtime/callback elements
}

// Init
buildPipeline();
buildAllSidebarDetails();
```

**Key rendering rules**:
- `desc` and `logicNote` fields are raw HTML (may contain `<code>`, `<strong>`, `<em>`) — insert directly, do NOT escape
- `config[].d`, `rtReads[].d`, `rtWrites[].d`, `callbacks[].d`, `notes[].t` are plain text — escape with `escHtml()`
- Use `<>` (U+2039/U+203A) instead of `<>` in type strings to avoid HTML escaping issues (e.g., `TArray<FName>` not `TArray<FName>`)

### Legend

Fixed-position at bottom-right corner. Shows swatches for:
- Config input (green)
- Runtime write (orange)
- Callback (yellow)
- Conditional (cyan)
- Async (purple) — if the pipeline has async branching

---

## Adapting for Different Pipelines

The template is designed for the general pattern of a staged pipeline with config inputs, mutable state, and interface callbacks. To adapt:

### Pipelines without a callback interface

Remove the callbacks row from stage cards. Remove the Callbacks filter button. Set `callbacks: []` on each stage. The three-column layout still works with just config and runtime data.

### Pipelines with branching (not purely linear)

For stages with multiple execution paths (e.g., sync vs async):
1. Add a `subModes` array to the stage object
2. Render sub-mode pills or cards inside the center panel
3. Add badges (e.g., `SYNC/ASYNC`) to the stage header

For conditional stages that may be skipped entirely:
1. Set `conditional: 'description of the gate condition'`
2. The rendering automatically applies cyan styling and shows the condition

### Pipelines with nested sub-pipelines

If a stage itself runs a sub-pipeline, represent the outer stage as a single stage card with a note referencing the sub-pipeline. Consider creating a separate diagram for the sub-pipeline rather than nesting.

### Non-UE5 pipelines

The template is not Unreal-specific. Replace "Config" with whatever the static configuration object is, "Runtime Data" with whatever the mutable state is, and "Callbacks" with whatever the extension/hook mechanism is. The three-column pattern works for any system with static config -> processing -> mutable state.

---

## Error Handling

- **No clear stage ordering**: If the pipeline documentation is ambiguous about stage order, ask the user to clarify before proceeding.
- **Too many stages (>15)**: Consider grouping related stages into phases, or splitting into multiple diagrams.
- **No config/runtime distinction**: If the pipeline has a single state object, use the left column for "inputs to this stage" and the right column for "outputs of this stage" — the visual pattern still works.
- **Missing documentation**: For stages with incomplete docs, add a note: `{ k: 'red', t: 'Documentation incomplete — details TBD.' }`.

## Important Notes

- **All output is self-contained**: The HTML file must work when opened directly in a browser with no external dependencies.
- **Data-driven, not hardcoded HTML**: All stage content lives in the `STAGES` JavaScript array. The rendering functions generate HTML. This makes the diagram easy to update — change the data, the rendering adapts.
- **Escape correctly**: `desc` and `logicNote` contain HTML and must NOT be escaped. All other text fields (field descriptions, callback descriptions, note text) must be escaped via `escHtml()`.
- **Type strings use `<>` not `<>`**: This avoids HTML parsing issues in field type displays.
- **Consistent with architecture diagrams**: The color palette, font stack, sidebar pattern, and dark theme match `CreateInteractiveArchitecture.md` so pipeline and architecture diagrams feel like siblings.

## Example Execution

**User request**: "Create a pipeline diagram for the Spawn Pipeline"

**Research results**: 11 stages + terminal from pipeline documentation and source headers.

**Output**: `Docs/spawn_pipeline_stages_diagram.html`
- 11 stages (1, 1.5, 2, 3, 3.5, 4, 5, 6, 7, 8, 9) + Pipeline Complete terminal
- 3 conditional stages (1.5, 3.5, 5, 7)
- 1 sync/async branching stage (6 — Spawn, with 5 arrival modes)
- Config inputs: ~60 UMyPipelineConfig fields distributed across stages
- Runtime data: ~20 UMyPipelineRuntimeData fields with clear read/write provenance
- Callbacks: 22 IMyPipelineOwner interface methods (Will/Did/Per-entity)
- Filter buttons: All, Config Inputs, Runtime Data, Callbacks
