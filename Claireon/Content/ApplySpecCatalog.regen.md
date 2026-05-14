# ApplySpecCatalog.json -- Regeneration Checklist

This catalog describes the per-tool apply_spec calling convention and
gotchas for the 8 Claireon tools that support declarative spec application.
Update it whenever one of those tools changes its spec entry shape,
calling convention, or asset-creation behavior.

## The 8 tools whose spec entries are catalogued

The catalog has exactly 8 entries (asserted by ClaireonApplySpecHelpTests):

1. claireon.behaviortree_edit
2. claireon.blackboard_edit
3. claireon.blueprint_edit_graph
4. claireon.eqs_edit
5. claireon.niagara_edit
6. claireon.pcg_edit
7. claireon.statetree_edit
8. claireon.widgetbp_edit

If a 9th apply_spec-supporting tool ships, add a row here AND a corresponding
entry in ApplySpecCatalog.json.

## Steps

1. Confirm the registered tool list still matches the 8 entries above.
2. Update the gotchas[] array for each tool entry to reflect new fuzzy-
   resolution rules, auto-wrap behaviour for struct properties, new
   creates_asset_if_missing tools, or new limitations.
3. For each entry, verify the calling_convention string still matches
   the tool's actual schema (params-nested vs flat).
4. For each entry, verify creates_asset_if_missing is correct.
5. Bump _meta.entry_count if the entry count changed.
6. Run the acceptance checks below.

## Acceptance checks

- python -m json.tool Claireon/Content/ApplySpecCatalog.json > /dev/null exits 0.
- _meta.entry_count equals the actual number of entries (and equals 8 today).
- Every entry has all six keys populated: tool, spec_entry_types,
  calling_convention, creates_asset_if_missing, id_mapping_pattern, gotchas.
- tool values are unique across entries.
- apply_spec_help returns entry_count == 8 (asserted by ClaireonApplySpecHelpTests).
