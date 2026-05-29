# ApplySpecCatalog.json -- Regeneration Checklist

This catalog is derived from each
registered `*_apply_spec` / `*_instance_apply_spec` tool's GetInputSchema
+ applicator code. When that markdown or any apply_spec tool changes,
the owning author runs the steps below and commits the refreshed
`ApplySpecCatalog.json` alongside.

Source of truth: each tool's GetInputSchema + applicator.

## Keying convention

Top-level keys are the bare `GetCategory()` value for each apply_spec
tool. The lookup pivot is now:

```
tool_search(name="<family>_apply_spec", detail="full")
```

which surfaces the matching entry under a `spec_shape` field. The
historical `claireon.apply_spec_help` tool has been deleted; `tool_search`
returns entries for each registered family, and the catalog now contains
17 entries (one per registered apply_spec-supporting tool).

## The 17 catalogued families

The catalog has exactly 17 entries (asserted by tests in
`ClaireonToolSearchExecuteTests`). The `tool` field on each entry must
match the registered Claireon MCP tool name composed from
`GetCategory()` + `_` + `GetOperation()`:

1. `attenuation` -> `attenuation_apply_spec`
2. `behaviortree` -> `behaviortree_apply_spec`
3. `blackboard` -> `blackboard_apply_spec`
4. `bp` -> `bp_apply_spec`
5. `concurrency` -> `concurrency_apply_spec`
6. `eqs` -> `eqs_apply_spec`
7. `level_sequence` -> `level_sequence_apply_spec`
8. `material` -> `material_apply_spec`
9. `material_instance` -> `material_instance_instance_apply_spec`
   (doubled `instance` is intentional under current naming)
10. `metasound` -> `metasound_apply_spec`
11. `niagara` -> `niagara_apply_spec`
12. `pcg` -> `pcg_apply_spec`
13. `soundclass` -> `soundclass_apply_spec`
14. `soundcue` -> `soundcue_apply_spec`
15. `soundmix` -> `soundmix_apply_spec`
16. `statetree` -> `statetree_apply_spec`
17. `widgetbp` -> `widgetbp_apply_spec`

If an 18th apply_spec-supporting tool ships, add a row here AND a
corresponding entry in `ApplySpecCatalog.json`.

## Steps

1. Confirm the registered tool list still
   matches the 17 entries above. If the markdown drops or adds a tool,
   mirror the change in this regen.md and in `ApplySpecCatalog.json`.

2. Read the per-tool gotchas / limitations and update each entry's
   `gotchas[]` array. Common patterns to watch for:
   - new fuzzy-resolution rules
   - new auto-wrap behaviour for struct properties
   - new "creates_asset_if_missing" tools
   - new top-level vs nested form options for spec entries
   - new explicit limitations (NOT yet implemented features)

3. For each entry, verify the `calling_convention` string still matches
   the tool's actual schema (params-nested vs flat vs single-asset).

4. For each entry, verify `creates_asset_if_missing` is correct.

5. Bump `_meta.entry_count` if the entry count changed.

6. Run the acceptance checks below.

## `apply_delta` field (schema_version >= 3)

Each entry carries an `apply_delta` object documenting whether the family
exposes the imperative-batch `*_apply_delta` MCP operation introduced in
work item #0000:

  - `supported: bool` -- required.
  - `tool: string` -- required when `supported == true`. The wire name of
    the registered `<family>_apply_delta` (or `bp_apply_delta`) tool.
  - `supported_phases: string[]` -- subset of
    `["disconnect", "remove_nodes", "nodes", "connect"]`. Required when
    `supported == true`. Phases omitted from this list MUST be empty in
    delta payloads; the base-class validator enforces this at call time.
  - `notes: string` -- optional human-readable note (per-family
    semantics, e.g. "connect = reparent").
  - `reason: string` -- required when `supported == false`.

The 8 in-scope `apply_delta` families shipped with #0000:

| Catalog key | Registered tool | Supported phases |
|---|---|---|
| `behaviortree` | `behaviortree_apply_delta` | all |
| `eqs` | `eqs_apply_delta` | remove_nodes, nodes |
| `level_sequence` | `level_sequence_apply_delta` | remove_nodes, nodes |
| `material` | `material_apply_delta` | all |
| `niagara` | `niagara_apply_delta` | remove_nodes, nodes |
| `pcg` | `pcg_apply_delta` | all |
| `statetree` | `statetree_apply_delta` | all |
| `widgetbp` | `widgetbp_apply_delta` | remove_nodes, nodes, connect (connect == reparent) |

`bp_apply_delta` is the existing `ClaireonTool_ApplyBlueprintDelta` (post
#0000 rename); it is registered independently of the per-family
applicators in this PR but the catalog row mentions it for completeness.

The remaining 8 catalog entries (`attenuation`, `blackboard`,
`concurrency`, `material_instance`, `metasound`, `soundclass`,
`soundcue`, `soundmix`) carry `apply_delta.supported: false` with a
one-line `reason` (flat property set / out of scope this PR / etc.).

## Acceptance checks

- `ConvertFrom-Json` (PowerShell) or `python -m json.tool` exits 0 on
  `Claireon/Content/ApplySpecCatalog.json`.
- `_meta.entry_count` equals the actual number of non-meta top-level
  keys (and equals 17 today).
- `_meta.schema_version` is `3` (bumped by #0000 when the `apply_delta`
  field was added to every entry).
- Every entry has all six original keys populated: `tool`,
  `spec_entry_types`, `calling_convention`, `creates_asset_if_missing`,
  `id_mapping_pattern`, `gotchas` (gotchas may be empty `[]` but the key
  must be present), plus the new `apply_delta` object.
- `tool` values are unique across entries.
- Catalog-tools-match-registered invariant: each top-level key matches
  the GetCategory() of some registered Claireon MCP tool whose
  GetOperation() is `apply_spec` or `instance_apply_spec`, and every
  such registered tool has a catalog entry (asserted bidirectionally by
  `ClaireonToolSearchExecuteTests`).
- Catalog-apply_delta-matches-registered invariant: each entry with
  `apply_delta.supported == true` MUST name a registered Claireon MCP
  tool whose `GetOperation()` is `apply_delta` and whose `GetCategory()`
  equals the catalog key. Conversely, every registered `*_apply_delta`
  tool MUST appear as `apply_delta.tool` on exactly one catalog entry
  whose key matches its category (asserted by
  `ClaireonApplyDeltaCatalogTests`).
