---
name: pcg-authoring
description: Behaviours that make PCG graphs authored over MCP look broken when they are not -- asynchronous generation, the compiled-graph cache, graph-parameter binding, and the settings whose values do not do what their names suggest. Read this before debugging a PCG graph that generates nothing or ignores an edit.
type: resource
uri: claireon://instructions/pcg-authoring
---

<!-- claude-hint:
Reference material, read inline during a PCG authoring task. Not a prompt and not a
workflow -- do not escalate model or effort on account of pulling it. Every item here
came from a graph that was correct and looked broken, or looked correct and was broken.
-->

# Authoring PCG Graphs Through Claireon

The recurring failure in PCG authoring is not a wrong graph. It is a **correct graph read
through a stale or asynchronous surface**, concluded to be wrong, and then "fixed" into
something actually wrong. Most of this document is about telling those apart.

## Generation is asynchronous -- use pcg_generate, do not roll your own wait

`UPCGComponent::Generate()` schedules work. It does not do it. Reading ISM instance transforms
later in the *same* `python_execute` returns the previous generation's output, or nothing at all.
`GenerateLocal(true)` is not reliably synchronous either.

`pcg_generate` dispatches and waits, and reports the resulting instance counts. Use it. It
returns an error rather than a success-with-a-flag when generation does not finish in time,
because a caller who measures after a half-finished generate reaches exactly the wrong
conclusion.

If you are driving generation from raw Python instead, the only correct shape is two MCP calls
-- change and dispatch in the first, measure in the second. Measuring in the call that triggered
it produces a confident, wrong conclusion: "the noise is not driving Z", "the graph is broken".
Both have been reached this way on graphs that were fine.

## Asking what is actually on a pin

`pcg_inspect` reports static structure. `pcg_inspect_data` reports the runtime data on a node's
output pin -- point count, density and Z ranges, and min/max/mean for every numeric attribute.
Reach for it before inferring anything from spawned meshes: measuring ISM transforms to find out
whether a filter kept anything is indirect, asynchronous, and how most of the wrong conclusions
in this document were originally reached.

It enables inspection and regenerates by default, because PCG only records inspection data
during an execution that had inspection switched on beforehand.

## Editing a graph does not invalidate what a placed component generates from

A `UPCGComponent` regenerates from the **cached compiled graph**. Saving the asset does not
evict that cache. So after a real edit -- a density change, a new node -- the component
regenerates and produces *byte-identical* output.

The symptom is unmistakable once you know it: node counts and instance counts that do not
move across a change that certainly should have moved them.

`pcg_refresh` evicts the cache; pass `regenerate_components` to also re-run the placed
components. `pcg_save` emits a hint pointing here whenever live components are bound to the
graph being saved.

## Graph parameters bind by GUID, not by name

`UPCGUserParameterGetSettings` binds through `PropertyGuid`. `PropertyName` only *labels the
output pin* -- it is not the binding.

This matters because of how the failure presents. A getter with no valid `PropertyGuid`
still reports an output pin, named after whatever `PropertyName` holds. Every structural
check passes, the runtime graph accepts the edge, and only the editor refuses it:

```
LogPCGEditor: Could not create link to InputPin <x> from Node <y>
Ensure condition failed: false [PCGEditorGraph.cpp:264]
```

A graph that reads as wired and generates nothing.

The supported sequence:

1. `pcg_add_user_parameter` returns `property_guid`. It is the only way to obtain it -- the
   graph's parameter bag is an `FInstancedPropertyBag` whose descriptors are not reachable
   through Python reflection.
2. `pcg_set_node_property` on `PropertyGuid` accepts either that GUID **or the parameter
   name**, resolves it, syncs `PropertyName`, and rebuilds the pins.
3. `pcg_connect` refuses to link from a getter bound to nothing, naming the fix.

## Settings whose value does not mean what the name suggests

**`SpatialNoise` Brightness and Contrast are a no-op on the output.**
`UPCGSpatialNoiseSettings` always writes normalised `0..1` to `ValueTarget` regardless of
either. Verified by sweeping Contrast `1 -> 3500 -> 100000` with zero change in output
range. For real amplitude, remap downstream:

```
PCGAttributeRemapSettings: InRange 0..1 -> OutRange -4500..4500
```

**`SpatialNoise.Transform` Scale3D is frequency, not size,** and it is violently sensitive.
Over a 240 m region: `0.00015` is near-constant, `0.5` is coherent terrain, `8000` is white
noise. Change it by orders of magnitude, not percentages.

## Forms that work and are worth copying

Weighted mesh entries, as a struct literal:

```
MeshSelectorParameters.MeshEntries =
  ((Descriptor=(StaticMesh="/Game/.../SM_X.SM_X"),Weight=3),(...))
```

Attribute selectors round-trip in a discoverable way: `pcg_get_node_properties` prints
`ValueTarget` as `PCGBegin($Density)PCGEnd`, which is the exact wrapper syntax to write
back. A bare `$Position.Z` is correctly rejected.

The graph Output node's **input** pin is labelled `"Out"`, not `"In"` -- so the closing edge of
every graph is `MyNode."Out" -> Output."Out"`. `Output."In"` is the obvious first guess and it is
wrong for every graph. `pcg_connect` says so precisely when you get it wrong, listing the pins
that do exist.

`pcg_apply_spec` takes inline `properties` per node -- a whole multi-node graph including
mesh tables lands in one call. Its `connections` resolve spec-local ids first and fall
through to the same identifiers `pcg_connect` accepts, so `"Input"` and `"Output"` work and
a spec can wire its own graph boundary.

## A volume with no brush is not an empty volume, it is a point

An `AVolume` spawned by `level_place_actor` or `audio_place_audio_volume` has no brush, so
its bounds are a zero-extent point. PCG generation domains see nothing and generate nothing,
silently. `level_build_brush` gives it real geometry, and `level_place_actor` hints at that tool
whenever it spawns a volume without a brush.

## Not PCG: chained member access off a UE Python struct getter reads zero

```python
comp.get_instance_transform(i, True).translation.z    # 0.0, always
t = comp.get_instance_transform(i, True)
tr = t.translation
float(tr.z)                                            # correct
```

The temporary is collected before the member read. The tell is **repeated struct addresses
across loop iterations**. This makes a working system look completely broken, and it bites
precisely when measuring generated output -- which is most PCG debugging.
