# ApplySpecCatalog.json -- Regeneration Checklist

This catalog describes the per-tool `apply_spec` calling convention and
gotchas for the 8 Claireon tools that support declarative spec application.
Update it whenever one of those tools changes its spec entry shape,
calling convention, or asset-creation behavior.

## The 8 tools whose spec entries are catalogued

The catalog has exactly 8 entries (asserted by `ClaireonApplySpecHelpTests`).
The `tool` field on each entry must match one of the registered Claireon
MCP tool names below:

1. `claireon.behaviortree_edit`
2. `claireon.blackboard_edit`
3. `claireon.blueprint_edit_graph`
4. `claireon.eqs_edit`
5. `claireon.niagara_edit`
6. `claireon.pcg_edit`
7. `claireon.statetree_edit`
8. `claireon.widgetbp_edit`

If a 9th apply_spec-supporting tool ships, add a row here AND a corresponding
entry in `ApplySpecCatalog.json`.

## Steps

1. Confirm the registered tool list still matches the 8 entries above. If a
   tool is added or removed, mirror the change in this regen.md and in
   `ApplySpecCatalog.json`.

2. Update the `gotchas[]` array for each tool entry. Common patterns to
   watch for:
   - new fuzzy-resolution rules
   - new auto-wrap behaviour for struct properties
   - new "creates_asset_if_missing" tools
   - new top-level vs nested form options for spec entries
   - new explicit limitations (NOT yet implemented features)

3. For each entry, verify the `calling_convention` string still matches
   the tool's actual schema (params-nested vs flat). Today only
   `claireon.blueprint_edit_graph` uses the flat shape.

4. For each entry, verify `creates_asset_if_missing` is correct. Today
   only `claireon.blueprint_edit_graph` creates the asset; the other
   seven require the target asset to already exist.

5. Bump `_meta.entry_count` if the entry count changed.

6. Run the acceptance checks below.

## Acceptance checks

- `python -m json.tool Claireon/Content/ApplySpecCatalog.json > /dev/null` exits 0.
- `_meta.entry_count` equals the actual number of entries (and equals 8 today).
- Every entry has all six keys populated: `tool`, `spec_entry_types`,
  `calling_convention`, `creates_asset_if_missing`, `id_mapping_pattern`,
  `gotchas` (gotchas may be empty `[]` but the key must be present).
- `tool` values are unique across entries.
- `claireon.apply_spec_help` returns `entry_count == 8` (asserted by
  `ClaireonApplySpecHelpTests`).
