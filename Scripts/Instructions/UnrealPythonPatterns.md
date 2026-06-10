# Unreal Python Patterns

This document consolidates correct Python patterns, known anti-patterns, and deprecated API replacements discovered through the PythonAuditLog analysis. Use it as a quick reference before writing any Unreal Python automation — find the right pattern in under 30 seconds.

---

## Asset Registry — Querying Assets

**Problem**: `ARFilter` instance properties are read-only in Python. Assigning `class_paths`, `recursive_paths`, or any other filter field after construction raises an `AttributeError`.

```python
# WRONG — ARFilter instance properties are read-only:
registry = unreal.AssetRegistryHelpers.get_asset_registry()
filter = unreal.ARFilter()
filter.class_paths = [unreal.TopLevelAssetPath("/Script/Engine", "Blueprint")]  # ERROR
filter.recursive_paths = True  # ERROR
assets = registry.get_assets(filter)

# CORRECT — use get_assets_by_class():
registry = unreal.AssetRegistryHelpers.get_asset_registry()
assets = registry.get_assets_by_class(
    unreal.TopLevelAssetPath("/Script/Engine", "Blueprint"),
    True  # recursive
)
# Filter to /Game path manually if needed:
assets = [a for a in assets if str(a.package_name).startswith("/Game")]
```

**Notes**: `get_assets_by_class` returns all matching assets engine-wide. Always apply a path prefix filter afterward when you only care about project content.

---

## search_all_assets() — Status and Workaround

**Problem**: `search_all_assets()` is broken in the current Python binding. Positional arguments raise a `TypeError` about bool conversion; keyword arguments raise a `TypeError` about argument count. Avoid it entirely.

```python
# BROKEN — positional arg gives TypeError about bool conversion:
# registry.search_all_assets("MyAsset")

# BROKEN — keyword arg gives TypeError about argument count:
# registry.search_all_assets(query_str="MyAsset", synchronous_search=True)

# WORKAROUND — search by name substring after get_assets_by_class:
assets = registry.get_assets_by_class(unreal.TopLevelAssetPath("/Script/Engine", "Blueprint"), True)
matching_assets = [a for a in assets if "MyAsset" in str(a.package_name)]
```

**Notes**: The substring filter on `package_name` is the only reliable workaround until the binding is fixed upstream.

---

## World Access — Deprecated vs. Current

**Problem**: `EditorLevelLibrary.get_editor_world()` is deprecated and emits a `DeprecationWarning` in UE 5.5+. It may be removed in future engine versions.

```python
# WRONG (deprecated):
world = unreal.EditorLevelLibrary.get_editor_world()  # DeprecationWarning

# CORRECT:
subsystem = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
world = subsystem.get_game_world()
```

**Notes**: `get_game_world()` returns the PIE world when in-editor play is active, and the editor world otherwise. For most automation scripts this is the correct behavior.

---

## Blueprint-Generated Class Loading

**Problem**: `unreal.load_class()` returns `None` for Blueprint-generated classes when passed the `_C` suffix path. The function works for native C++ classes but silently fails for Blueprint assets.

```python
# WRONG — returns None for Blueprint-generated classes:
cls = unreal.load_class(None, '/Game/Path/BP_Foo.BP_Foo_C')  # returns None

# CORRECT:
bp = unreal.load_asset('/Game/Path/BP_Foo')
cls = bp.generated_class()
cdo = unreal.get_default_object(cls)
```

**Notes**: Always load the Blueprint asset (without `_C`) via `load_asset`, then call `generated_class()` to get the class object. From there `get_default_object(cls)` gives you the CDO for reading or setting default property values.

---

## GameplayTagContainer — Clearing Tags

**Problem**: `GameplayTagContainer` exposes no `add_tag()` method from Python, and the `gameplay_tags` property is read-only. There is no direct Python API to surgically add or remove individual tags.

```python
# WRONG — these methods don't exist or are read-only:
container.add_tag(tag)        # AttributeError
container.set_editor_property('gameplay_tags', [...])  # read-only

# WORKAROUND (Python only) — clear the entire container:
empty = unreal.GameplayTagContainer()
empty.import_text('()')
req.set_editor_property('require_tags', empty)

# PREFERRED — use the MCP tool for surgical add/remove:
# editor.blueprint.edit { operation: "set_gameplay_tags", property_path: "...", tags_to_remove: [...] }
```

**Notes**: The `import_text('()')` trick resets the container to empty by deserializing an empty tag array. It is the only way to clear tags purely from Python. For any operation more precise than a full clear (add one tag, remove one tag), use the `editor.blueprint.edit` MCP tool with `set_gameplay_tags` — it reads and writes the underlying `FGameplayTagContainer` directly via the C++ API.

---

## Blueprint Compilation Status

**Problem**: `BlueprintEditorLibrary.compile_blueprint()` returns `None`. There is no Python-accessible property to check whether compilation succeeded or what errors were produced.

```python
# compile_blueprint() returns None — no status check possible from Python:
unreal.BlueprintEditorLibrary.compile_blueprint(bp)  # returns None
# bp.get_editor_property('status')  # ERROR: protected

# PREFERRED — use MCP tool which reads Blueprint->Status directly:
# editor.blueprint.compile { contentPath: "/Game/Path" }
# Returns PASSED/FAILED with error details per blueprint.
```

**Notes**: The MCP `editor.blueprint.compile` tool compiles the blueprint and returns structured pass/fail output with per-error detail. Use it whenever you need to verify compilation result or surface errors to the user.

---

## remove_unused_nodes() Returns None

**Problem**: `remove_unused_nodes` and `remove_unused_variables` are asymmetric. `remove_unused_variables` returns an `int32` count of removed variables, but `remove_unused_nodes` returns `void` (None in Python). Do not attempt to check its return value.

```python
# Asymmetry: remove_unused_nodes returns void, remove_unused_variables returns int32
removed_nodes = unreal.BlueprintEditorLibrary.remove_unused_nodes(bp)    # None
removed_vars  = unreal.BlueprintEditorLibrary.remove_unused_variables(bp) # int

# Don't check removed_nodes — just call it and accept no count:
unreal.BlueprintEditorLibrary.remove_unused_nodes(bp)
vars_removed = unreal.BlueprintEditorLibrary.remove_unused_variables(bp)
print(f"Removed {vars_removed} variables (node count unavailable)")

# PREFERRED — use MCP compile with remove_unused: true
# which reports both through the C++ API.
```

**Notes**: If you need a node removal count for reporting purposes, use `editor.blueprint.compile` with `remove_unused: true`. The MCP tool surfaces both counts from the C++ side.

---

## SimpleConstructionScript is Protected — Use ObjectIterator

**Problem**: `Blueprint.SimpleConstructionScript` is a protected property. `get_editor_property('SimpleConstructionScript')` raises an error. You cannot walk the SCS tree directly from Python.

```python
# WRONG — SimpleConstructionScript is protected:
scs = bp.get_editor_property('SimpleConstructionScript')  # ERROR: protected

# CORRECT — use ObjectIterator to find component templates:
bp_path = '/Game/Path/BP_MyBlueprint'
bp = unreal.load_asset(bp_path)
for comp in unreal.ObjectIterator(unreal.SkeletalMeshComponent):
    if 'BP_MyBlueprint' in comp.get_path_name():
        comp.modify()
        comp.set_editor_property('SkinnedAsset', None)
        break
unreal.EditorAssetLibrary.save_asset(bp_path, only_if_is_dirty=False)
```

**Notes**: `ObjectIterator` walks all live UObjects of a given class. Filtering by `get_path_name()` containing the Blueprint name isolates the component templates owned by that Blueprint. Always call `modify()` before changing a property to ensure the transaction is recorded for undo.

---

## Package.mark_package_dirty() Doesn't Exist

**Problem**: The Python `Package` object does not expose `mark_package_dirty()`. Calling it raises an `AttributeError`.

```python
# WRONG — Package has no mark_package_dirty() in Python:
pkg = asset.get_package()
pkg.mark_package_dirty()  # AttributeError

# CORRECT — save unconditionally:
unreal.EditorAssetLibrary.save_asset(asset_path, only_if_is_dirty=False)
```

**Notes**: Passing `only_if_is_dirty=False` to `save_asset` forces a save regardless of dirty state, which is the correct substitute for marking-then-saving. If you genuinely need to dirty an asset without saving immediately, call `asset.modify()` on the asset object instead.

---

## Deprecated Properties — Known List

| Deprecated Property | Deprecation Warning | Replacement |
|---|---|---|
| `GameplayEffect.RemovalTagRequirements` | "Property is deprecated" | Unknown in UE 5.5 — use `editor.blueprint.edit set_gameplay_tags` MCP tool |
| `GameplayEffect.ApplicationTagRequirements` | "Property is deprecated" | Unknown in UE 5.5 — check `FGameplayEffectQuery` |
| `SkeletalMeshComponent.SkeletalMesh` | "Use GetSkeletalMeshAsset() or GetSkinnedAsset()" | Use `SkinnedAsset` property in Python |
| `EditorLevelLibrary.get_editor_world()` | "Use UnrealEditorSubsystem" | `get_editor_subsystem(UnrealEditorSubsystem).get_game_world()` |

---

## When to Use MCP Tools Instead

Stop writing Python and use an MCP tool when you need to:

| Task | MCP Tool |
|---|---|
| Remove specific nodes by GUID | `editor.blueprint.edit` → `remove_node` (stateless) |
| Fix stale pins on a node | `editor.blueprint.edit` → `reconstruct_node` |
| Modify GameplayTagContainer fields | `editor.blueprint.edit` → `set_gameplay_tags` |
| Enumerate graphs in a blueprint | `editor.blueprint.edit` → `list_graphs` |
| Batch compile + check for errors | `editor.blueprint.compile` |
| Compile + remove unused | `editor.blueprint.compile` with `remove_unused: true` |
| Inspect StateTree runtime state | `statetree.runtime_inspect` |
