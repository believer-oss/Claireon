<!-- claude-hint:
Reference for the Claireon "fuzz baseline" Blueprints: feature-dense fixtures used
to validate bp_lint / bp_format (auto-format) behavior. Read before regenerating
or extending the fixtures.
-->

# Claireon Fuzz-Baseline Blueprints

Deliberately feature-dense "fuzz" Blueprints that exercise as many Blueprint
features as possible, each with **deterministic, Claireon-observable runtime
behavior** a few seconds after a PIE world loads. They are a baseline for
validating Blueprint reformatting: mangle the formatting (strip variable
categories, stack nodes, add extraneous vars/events, long reroute chains, ...),
then re-run the PIE verifier. If the trace still matches, the reformat preserved
behavior; a mismatch is a functional regression.

Assets live under `/Claireon/FuzzBaseline/`. Generators + verifier live here, in
`Plugins/Claireon/Scripts/FuzzBaseline/`, and run inside the editor's Python:

```python
import sys; sys.path.insert(0, r"<Project>/Plugins/Claireon/Scripts/FuzzBaseline")
import fuzzlib, fuzztrace, gen_conductor        # etc.
```

## The FUZZ trace protocol

Every observable step prints one line via each fixture's `FuzzLog(Tag, Value)`:

```
FUZZ|<AssetName>|<Stream>|<seq>|<Tag>|<Value>
```

and each fixture ends with `FuzzDone()`:

```
FUZZ|<AssetName>|DONE|<checksum>
```

`FuzzLog` increments `FuzzSeq` and folds `Value` into a checksum accumulator
(`Acc = (Acc % 60000) * 31 + Value`). Any wrong branch, dropped delegate, or
reordered latent completion changes the trace and the final checksum. All values
are integers, so the trace is frame-rate independent. Every fixture waits
`BeginPlay + Delay(2s)` before starting so the world has settled.

The trace scaffold (`FuzzName`/`FuzzStream`/`FuzzSeq`/`FuzzAcc` vars +
`FuzzLog`/`FuzzDone` functions) is authored by `fuzztrace.py` so every fixture
shares it. `fuzzlib.py` provides the `Delta` batch-authoring helper over
`bp_apply_delta`.

## Fixtures

| Asset | Stream | Golden checksum | Coverage |
|---|---|---|---|
| `BP_Fuzz_Conductor` | CONDUCTOR | 1397108 | control-flow + latent-node torture test |
| `BP_Fuzz_ComponentChorus` (+ `AC_Fuzz_Beacon`, `BP_Fuzz_ChorusTarget`) | CHORUS | 1100841 | delegates, component-bound events, interfaces, latent motion |
| `BP_Fuzz_LineageGrandchild` (+ `_Child`, `_Base`) | LINEAGE | 871167 | inheritance, overrides, CallParent, construction script |
| `BP_Fuzz_RepRelay` (+ `BP_Fuzz_RepChild`) | SV / CL | SV 101899 | client/server replication: RepNotify, RPC matrix, ExposeOnSpawn |

`BPI_Fuzz` is the shared interface implemented by several fixtures.

### 4. BP_Fuzz_RepRelay / BP_Fuzz_RepChild (`gen_reprelay.py`)

Client/server replication. PIE in `netMode="Client"` runs a listen server + 1
client, so each actor exists as a server instance (stream `SV`) and a client
instance (stream `CL`) -- the stream is chosen at BeginPlay from `HasAuthority`.
A `bReplicates` relay drives a strictly-ordered, timer-spaced server sequence, so
the SV trace and checksum are deterministic; the CL trace receives what the server
sends. Exercises: RepNotify vars (`RepCounter`/`RepFlags` -> `OnRep_*` fire on the
client), a replicated array + a plain replicated int, the RPC matrix as custom
events (Run-on-Server reliable, NetMulticast reliable + unreliable -- set via the
node's `FunctionFlags` bitmask), `SwitchHasAuthority`/`HasAuthority` branching, and
a runtime-spawned replicated `BP_Fuzz_RepChild` whose ExposeOnSpawn `SpawnStamp`
replicates to both server and client. The client correctly never sees the
server-only RPC. Verification asserts the SV stream strictly (deterministic) and
checks the CL stream lines (`reprelay_cl_golden.trace`) in order.

### 1. BP_Fuzz_Conductor (`gen_conductor.py`) -- 290 nodes / 14 graphs

A phase machine (PhaseA..PhaseF) driven from BeginPlay. Exercises: 3 timelines
(multi-track float/vector/event, looping, ping-pong), every Standard macro
(Gate, DoOnce, DoN, FlipFlop, MultiGate, ForEach/ForEachWithBreak, While, ForLoop)
plus a custom macro and a collapsed composite, timers (named + by-event, paused/
resumed/cleared, a retriggerable delay re-triggered mid-flight), switches (int/
string/name/gameplay-tag/enum), Select/Math/promotable operators, split-pin
Make/Break struct, arrays/sets/maps, dynamic casts (success + guaranteed-fail
`CastFailed`), pure functions with locals, a large pure data island, an event
dispatcher bound/unbound/rebound, an async save-load and a soft-object load, an
interface call + event. Deliberate lint bait that must survive formatting: an
`exec-join` whose sibling is latent (Sequence conversion would reorder it), a
shared tail, an entry-param wire, reroute chains, inline diagnostics, a no-op
Sequence.

### 2. BP_Fuzz_ComponentChorus (`gen_chorus.py`) -- ~98 nodes / 7 graphs

Delegates and component metadata -- the surface most fragile under reformatting.
A `BP_Fuzz_ChorusTarget` is spawned on top of `SphereMover` for a synchronous
begin-overlap; `MoveComponentTo` drives the sphere out for end-overlap. Exercises:
`ComponentBoundEvent` nodes (sphere begin/end/hit overlap + the `AC_Fuzz_Beacon`
dispatcher), the full delegate node set (`AddDelegate`/`RemoveDelegate`/
`ClearDelegate`/`CallDelegate`/`CreateDelegate`) with a bind/unbind/rebind dance
(the mid-sequence broadcast lands while unbound and is correctly dropped),
interface calls (a `FuzzQuery` message to the beacon component + a `FuzzNudge`
event on self), and the beacon broadcasting its own dispatcher.

### 3. BP_Fuzz_LineageBase / _Child / _Grandchild (`gen_lineage.py`)

A three-tier inheritance chain. The base owns BeginPlay + the trace scaffold;
each tier overrides `RunTier` (with `CallParentFunction`) and `ComputeTier`
(virtual dispatch), so a grandchild instance's `ComputeTier(1)` resolves through
all three overrides (`((1 + BaseTuning) + 100) + 1000`). Also covers: a
construction script that derives a per-instance `ConstructedValue` from an
InstanceEditable `BaseTuning` (a lint-exempt construction-authored default, NOT
Transient), an inherited event dispatcher bound from the base, and a
legitimately-named custom event beside the overrides (non-shadowing).

## Verifying

`verify.py` opens the fixture's map, PIEs, waits for the trace to settle, and
diffs the captured `Server:` FUZZ lines against `<asset>_golden.trace` (grouping
commas normalized). Golden traces were captured from the editor log in `Saved/Logs/`.
Because the fixtures are reformatting-invariant, the golden trace is the
acceptance oracle for any format/lint change.

Note: a fixture actor with an inline-curve Timeline (Conductor) makes its host
`.umap` unsaveable ("Illegal reference to private object"); PIE runs on the
in-memory level regardless, so the verifier places the actor and plays without
persisting the map.

## Regenerating

Each `gen_*.py` runs its stages in order (`stage_create`, `stage_variables` /
`stage_functions`, `stage_events`, `stage_islands`/`build_*`). They are mostly
idempotent (auto-created overrides and existing nodes are recovered by GUID), but
a clean rebuild is `delete_asset` then re-run.

**Plugin-mount caveat:** `claireon.bp_create` accepts only `/Game/` paths, so the
assets are authored under `/Game/Claireon/FuzzBaseline/` and then relocated to the
`/Claireon/` plugin mount with `EditorAssetLibrary.rename_asset` (which fixes up
inter-asset references) followed by `save_asset`. `gen_reprelay.build_relay()` shows
the one RPC-safe stage order: author the RPC custom events PLAIN, wire every call
site, and only then set their net `FunctionFlags` (a `CallFunction` cannot bind a
net-flagged custom event by name). See
`reference_claireon_fuzz_authoring_gotchas` in the auto-memory for the authoring
gotchas these generators encode (exec pin names, `Array<T>` grammar, string-switch
Default safety, CreateDelegate `SelectedFunctionName`-after-wiring, etc.).

## Portability (adding these to Claireon's own suite)

The generators author every fixture from scratch using only engine classes
(`Actor`, `SceneComponent`, `SphereComponent`, `BoxComponent`,
`StaticMeshComponent`, `TimerHandle`, `Vector`/`IntPoint`, the `Kismet*Library`
functions, `/Engine/BasicShapes/Cube`), one Claireon-owned type
(`ClaireonUObjectInspectMulticast` as the dispatcher signature), and references
among the fixtures themselves -- no game-specific parent classes or content.

The Conductor's `SwitchGameplayTag` coverage uses two Claireon-owned tags,
`Claireon.FuzzTest.Alpha` / `Claireon.FuzzTest.Beta`, declared in
`Plugins/Claireon/Config/Tags/ClaireonFuzzTags.ini` and registered from module
startup via `ClaireonFuzzTestTags::RegisterFuzzTestTags()` (an `AddTagIniSearchPath`
call -- NOT `UE_DEFINE_GAMEPLAY_TAG`, which an Editor-type module may not use), so
the fixtures carry no project tag dependency. To relocate the set into the Claireon plugin's own content,
change the `/Claireon/FuzzBaseline/` asset-path prefix in each generator to the
plugin mount.
