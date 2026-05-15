# BPAuthoringPatterns.json -- Regeneration Checklist

This catalog is hand-mined from the BP-to-C++ translator source. When any of
the source files below change, the owning author runs the six steps in order
and commits the refreshed `BPAuthoringPatterns.json` alongside the source change.

Source files (must all be covered by `_meta.source_files`):

- `Claireon/Source/Claireon/Private/ClaireonBPNodeMapper.cpp`
- `Claireon/Source/Claireon/Private/ClaireonBPMacroHandler.cpp`
- `Claireon/Source/Claireon/Private/Tools/ClaireonTool_BlueprintTranslateScaffold.cpp`
- `Scripts/Testing/BP_CPP_TRANSLATOR/REFINEMENT_LOG.md`

## Steps

1. Capture the new commit:
   - `git rev-parse HEAD` -> becomes `_meta.source_commit_sha`.

2. Diff `ClaireonBPNodeMapper.cpp` against the previous SHA and review for:
   - new `BPMathMappings[]` rows (add one `CallFunction` entry per row;
     `function_class` is usually `KismetMathLibrary`, confirm per row);
   - new `MapNodeEx` dispatch cases (cast, event, variable get/set, sequence,
     branch, custom event, async task, switch variants, timeline);
   - new names covered by `MapLatentCallFunctionNodeEx` / latent-struct
     detection (anything exposing an `FLatentActionInfo` struct property);
   - new types handled in `SanitizeDefaultValue()` (update
     `pin_default_formats`).

   `git diff <old-sha> HEAD -- Claireon/Source/Claireon/Private/ClaireonBPNodeMapper.cpp`

3. Diff `ClaireonBPMacroHandler.cpp` and review for:
   - new names added to `IsKnownMacro()` (the `MacroInstance` entry set must
     equal this set exactly);
   - new `Handle*` methods (one `MacroInstance` entry each, using the
     `StandardMacros` library path).

   `git diff <old-sha> HEAD -- Claireon/Source/Claireon/Private/ClaireonBPMacroHandler.cpp`

4. Diff `ClaireonTool_BlueprintTranslateScaffold.cpp` and review for orphan
   classification / async-task-proxy promotion rule changes. These fold into
   the `disambiguator` text of the corresponding pattern entries.

   `git diff <old-sha> HEAD -- Claireon/Source/Claireon/Private/Tools/ClaireonTool_BlueprintTranslateScaffold.cpp`

5. Diff `REFINEMENT_LOG.md` and review for any new `V*` iteration entries
   that imply new reverse-authoring rules (e.g. V7-orphans, V7-bpmath,
   V7-unconnected, V7-dupdef, V7-asyncproxy, V8-latent). Fold new rules into
   `disambiguator` text on the affected entries.

   `git diff <old-sha> HEAD -- Scripts/Testing/BP_CPP_TRANSLATOR/REFINEMENT_LOG.md`

6. Update affected entries, bump `_meta.source_commit_sha` and
   `_meta.generated_utc`, run the acceptance checks below, and commit the
   refreshed catalog alongside the source change.

## Acceptance checks

- `python -m json.tool Claireon/Content/BPAuthoringPatterns.json > /dev/null` exits 0.
- `_meta.source_commit_sha` equals `git rev-parse HEAD` at mining time.
- Every entry has all eleven keys populated (`null` when not applicable,
  never missing).
- The set of `macro_name` values where `bp_node_type == "MacroInstance"`
  equals the exact `IsKnownMacro()` set.
- Every `translator_ref` file basename appears in `_meta.source_files`.

## Ownership

The next agent that edits `ClaireonBPNodeMapper.cpp` or
`ClaireonBPMacroHandler.cpp` is responsible for running this checklist before
merging.
