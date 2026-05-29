# ApplySpecCatalog.json -- Regeneration Checklist

This catalog describes the per-tool apply_spec calling convention and
gotchas for the Claireon tools that support declarative spec application.
Each entry is derived from the tool's GetInputSchema plus its applicator
code. Update it whenever one of those tools changes its spec entry shape,
calling convention, or asset-creation behavior.

## Keying convention

Top-level keys are the bare GetCategory() value for each apply_spec tool.
The lookup pivot is:

    tool_search(name="<family>_apply_spec", detail="full")

which surfaces the matching entry. The historical apply_spec_help tool has
been retired; tool_search returns entries for each registered family, and
the catalog now contains 17 entries (one per registered apply_spec-supporting
tool).

## The 17 catalogued families

The catalog has exactly 17 entries (asserted by ClaireonToolSearchExecuteTests).
The tool field on each entry must match the registered Claireon MCP tool name
composed from GetCategory() + "_" + GetOperation():

1. attenuation -> claireon.attenuation_apply_spec
2. behaviortree -> claireon.behaviortree_apply_spec
3. blackboard -> claireon.blackboard_apply_spec
4. bp -> claireon.bp_apply_spec
5. concurrency -> claireon.concurrency_apply_spec
6. eqs -> claireon.eqs_apply_spec
7. level_sequence -> claireon.level_sequence_apply_spec
8. material -> claireon.material_apply_spec
9. material_instance -> claireon.material_instance_instance_apply_spec
   (doubled "instance" is intentional under current naming)
10. metasound -> claireon.metasound_apply_spec
11. niagara -> claireon.niagara_apply_spec
12. pcg -> claireon.pcg_apply_spec
13. soundclass -> claireon.soundclass_apply_spec
14. soundcue -> claireon.soundcue_apply_spec
15. soundmix -> claireon.soundmix_apply_spec
16. statetree -> claireon.statetree_apply_spec
17. widgetbp -> claireon.widgetbp_apply_spec

If an 18th apply_spec-supporting tool ships, add a row here AND a
corresponding entry in ApplySpecCatalog.json.

## Steps

1. Confirm the registered tool list still matches the 17 entries above.
2. Update each entry's gotchas[] array to reflect new fuzzy-resolution
   rules, auto-wrap behaviour for struct properties, new
   creates_asset_if_missing tools, or new limitations.
3. For each entry, verify the calling_convention string still matches the
   tool's actual schema (params-nested vs flat vs single-asset).
4. For each entry, verify creates_asset_if_missing is correct.
5. Bump _meta.entry_count if the entry count changed.
6. Run the acceptance checks below.

## Acceptance checks

- python -m json.tool Claireon/Content/ApplySpecCatalog.json > /dev/null exits 0.
- _meta.entry_count equals the actual number of non-meta top-level keys
  (and equals 17 today).
- Every entry has all six keys populated: tool, spec_entry_types,
  calling_convention, creates_asset_if_missing, id_mapping_pattern, gotchas
  (gotchas may be empty [] but the key must be present).
- tool values are unique across entries.
- Catalog-tools-match-registered invariant: each top-level key matches the
  GetCategory() of some registered Claireon MCP tool whose GetOperation() is
  apply_spec or instance_apply_spec, and every such registered tool has a
  catalog entry (asserted bidirectionally by ClaireonToolSearchExecuteTests).
