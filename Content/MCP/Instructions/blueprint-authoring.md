---
name: blueprint-authoring
description: Judgement-based principles for authoring readable Blueprint graphs -- decomposition, execution topology, data locality, layout intent, and variable categorization. Read this before making non-trivial edits to a Blueprint graph.
type: resource
uri: claireon://instructions/blueprint-authoring
---

<!-- claude-hint:
Reference material, read inline during a Blueprint editing task. Not a prompt and
not a workflow -- do not escalate model or effort on account of pulling it. Pair it
with `bp_lint` for the mechanical half of the same rule set.
-->

# Authoring Readable Blueprint Graphs

`bp_lint` finds what can be measured. This document covers what cannot: the calls that
need judgement about intent. Read it once per task, before you start restructuring.

The rules below were derived by diffing a machine-authored ability Blueprint against a
hand-cleaned version of the same asset, then verified by re-measuring both. On that asset:
total wire length 295,447 -> 165,588, longest wire 9,772 -> 1,836, wires over 2,000 units
18 -> 0, backward wires 3 -> 0, widest event island 22,272 -> 10,368, and reroute nodes
136 -> 73.

**These are defaults derived from one ability graph, not universal invariants.** Read every
numeric threshold below as "what worked on the measured case", and every "should" as a
default to depart from with a reason. Two clarifications worth carrying:

- Execution joins went from **13 to 1**, not 12 to 0. The lower pair counts only joins
  arriving directly at a node; counting them the way `bp_lint` does -- resolving through
  reroutes, because a reroute is presentation and not execution semantics -- the cleaned
  asset still contains one join hidden behind a reroute. Even a carefully hand-cleaned graph
  kept one, which is the argument for the rule rather than against it.
- The event rail on the cleaned asset spans x 512..1,104, not the tighter range an earlier
  draft claimed. Events line up, but not to a single column.

## The one sentence version

A graph is readable when a person can follow any single execution path left to right
without their eye leaving a horizontal rail, and can see every value a node consumes
without scrolling.

Everything below follows from that.

---

## 1. Decomposition: when to make a new graph

Extraction is the highest-leverage edit available to you, and it is almost always what
a wide, tangled graph actually needs. Reach for it before you reach for layout.

**Extract when any of these is true:**

- An event's island is wider than roughly two screens (about 11,000 graph units). Width
  is the symptom; the cause is a chain that should have been several graphs.
- The same exec chain appears twice anywhere in the Blueprint. Duplicated subgraphs are
  the single most common cause of long-distance wires, because the second copy inevitably
  gets wired back into the first copy's continuation.
- A latent node's completion lane (`EventReceived`, `Completed`, `OnSuccess`) runs more
  than about three nodes. Latent nodes are natural graph boundaries; treat a long
  completion lane as a missing custom event.
- A cluster of six or more pure nodes (Get / Break / Make / math / Normalize) feeds a
  single consumer. That is a function with a return value that has not been written yet.
- Two different execution paths need the same tail. Extract the tail and call it from
  both, rather than wiring both into it. See section 2.

**Do not extract** a chain that is used exactly once, is under about six nodes, and reads
fine in place. Extraction has a cost: the reader has to jump graphs. Pay it to remove a
duplicate, a join, or a screen of width, not to hit a node count.

### Function or custom event

This choice is semantic, not stylistic, and it is yours to make:

- A **function** call blocks the caller's execution until it returns. Use it for
  synchronous work. Functions may not contain latent or async nodes at all, so a
  selection holding an ability task or a `Delay` cannot become one.
- A **custom event** does not block the caller. Use it when the extracted work contains
  latent nodes (ability tasks, `Delay`, `Wait Gameplay Event`, timelines, async loads),
  or when it is genuinely a separate entry point rather than a subroutine.

The two are not interchangeable: swapping a blocking call for a non-blocking one changes
when the caller's next node runs. Decide which one you want before you start moving nodes,
because the move is not reversible in one step.

`bp_extract_function` does the move for you, running the same code as the right-click
'Collapse to Function' -- so data pins crossing the selection boundary become parameters,
and a selection the menu would refuse is refused here too, with the boundary reported as
evidence. `bp_extract_macro` and `bp_extract_composite` are the other two menu collapses:
a macro inlines rather than being called and so may hold latent nodes, and a composite is
the most permissive, which makes it the fallback when the function form is refused.

`bp_extract_event` exists but is deliberately narrow, because the editor has no
collapse-to-event and the general case is not sound. It requires the selection to be
TERMINAL -- no exec edge leaving it -- and no data pin to cross its boundary. That is the
same reasoning as above from the other side: since the call does not block, there is
nowhere correct to put a continuation, so a region that has one is refused rather than
quietly reordered. Use the function form when the region is not terminal; its gateway has
a completion exec pin, which is exactly what expresses "resume after this finishes".

### Never name a Custom Event after something that already exists

A Custom Event is a NEW entry point. It cannot override anything, and it cannot share its
name with anything the class already has. Get this wrong and the failure is close to
invisible: the node has the name you asked for, the call reports success, and the handler
never runs.

The reported case: asked to fix a widget, an author created a Custom Event named
`Construct` next to the widget's real `Event Construct`, and reported it as the correct
event. Nothing ran, and the Blueprint did not compile.

Before authoring a Custom Event, know which of these you are actually doing:

- **Implementing a parent event** (`Construct`, `Tick`, `BeginPlay`, `OnPaint`, any
  BlueprintImplementableEvent or BlueprintNativeEvent): create the OVERRIDE.
  `node_type='EventOverride'` with `function_name='<the real function name>'`, or the
  `add_function_override` operation for a BlueprintNativeEvent. Never a Custom Event.
- **Adding to an event that already exists in the graph**: find the existing node and wire
  to it. A second entry point with the same name is not a second chance to run.
- **Implementing an interface function**: implement the interface function itself. A Custom
  Event beside it is a different function, and no interface call will ever reach it.
- **Genuinely a new entry point**: then the name is yours -- and it must not collide with a
  parent function, an interface function, a function graph in this Blueprint, or another
  event.

`bp_add_node`, `bp_apply_delta` and `bp_apply_spec` all refuse a colliding Custom Event
name now, and the error names the call to make instead. `bp_lint`'s
`custom-event-shadows-function` reports graphs that already carry one.

### Calling an interface function: name the interface

The same failure shape on the call side -- right name, wrong function. With no
`function_class`, a `CallFunction` binds to THIS Blueprint's own implementation on Self.
That is a real, working call, so nothing errors; it is just not an interface message to
another object. When you mean to call an interface on someone else, pass
`function_class='<Interface>'` and wire the target pin.

### Naming

Verb first, and name the effect rather than the position in the flow. `UpdateCurrentShotDirection`,
`PrepareAnimationReposition`, `ComputeDashDestinationForSelectedTarget` all tell the
reader what happens without opening the graph. `Finish`, `Handle`, `DoStuff`, `Step2` do
not. A name that only makes sense to someone who already knows the surrounding flow has
failed.

Prefer `Compute*` / `Calculate*` / `Get*` for pure functions with a return value, and
`Start*` / `Update*` / `Prepare*` / `Apply*` for functions with side effects. The prefix
is a free hint about whether calling it twice is safe.

---

## 2. Execution must never join

A **join** is two or more exec output pins wired into the same input pin. Joins are the
root cause of the worst wires in any graph, because the second path has to travel back
across everything the first path already covered. In the reference asset, a single
branch's `else` pin reached its own `then` continuation via a reroute pair spanning
6,410 units.

Treat every join as a defect. There are three fixes. Prefer them in this order.

### Fix 1: Sequence lanes (safe only when no lane is latent)

Replace `Branch -> optional work -> rejoin tail` with a `Sequence` node, one lane per
step. Each lane gets its own guard `Branch` whose failing pin is **left unconnected**.

Sequence lanes run top to bottom, so for ordinary synchronous work the resulting execution
order is identical to the nested-branch version. That makes this the default fix, and it is
what `bp_lint` suggests.

**Hard exclusion: no lane may contain latent or async work.** A Sequence advances to the
next lane when the current lane *starts* its latent operation, not when that operation
completes. So if a lane holds a `Delay`, an ability task, an async load, a timeline, or any
node that completes through an output delegate, converting a join into lanes reorders
execution -- the exact bug the conversion was supposed to avoid. The same applies to any
early-exit or other control-flow construct inside a lane.

`bp_lint` enforces this: every `exec-join` finding carries `sequence_conversion_safe` plus
the reason, and it reports `false` when any origin is latent. Detection is by function
metadata (`MD_Latent`) and async-task node type, not by pin shape -- counting exec output
pins would flag every `Branch`, since multiple exec outputs mean branching rather than
latency.

A dangling exec pin is correct here, not an oversight. A guard that does nothing on
failure should look like it does nothing on failure.

### Fix 2: Extract the shared tail (mechanically safe)

If two genuinely different paths need the same tail, extract the tail into a function or
custom event and call it from each path. Order is preserved, and the duplicate wire
disappears.

Choose this over Fix 1 when the shared tail is substantial (more than a few nodes), or
when the two paths are far apart in the graph, or when the tail is also needed from a
third place.

### Fix 3: Hoist order-independent work above the branch (judgement required)

If the work after the branch does not depend on which way the branch went, move it
before the branch. The rejoin disappears because there is nothing left to rejoin.

**This changes execution order, so it is never safe to apply mechanically.** You must
establish that the hoisted work is independent of everything you moved it past: no shared
mutable state, no ordering contract with a spawn or a timer, no gameplay-visible
sequencing. If you cannot establish that, use Fix 1 instead. If a tool proposes a hoist,
it is a suggestion for you to verify, not a transform to accept.

### Sequence is not free

A `Sequence` with one connected output pin is pure noise; delete it. Sequence earns its
place only when it is removing a join or separating concerns into lanes.

---

## 3. Diagnostics belong in their own lane

Debug output (`Print String`, `VisLog Text`, gated on a diagnostics flag) should
terminate its own Sequence lane, not sit inline in the main execution chain.

Inline diagnostics cost twice: they interrupt the reader's scan of the real logic, and
their guard `Branch` creates a join when the false path has to rejoin the main line. A
diagnostics lane has no continuation, so it cannot create a join.

Put the diagnostics lane first (`then_0`) when it reports on entry, last when it reports
on completion.

---

## 4. Data locality: re-fetch, never drag

**Place one `VariableGet` per consumer, immediately to its left.** Do not fan a single
Get out to several distant consumers, and never wire a function entry's parameter pin
across the graph.

Duplicating Get nodes is correct and expected. In the reference asset the same variable
is fetched nine times, and that is the clean version.

The trade is readability against a negligible access cost, not against zero. A variable
read is cheap but not free, and re-fetching *re-reads*: if the value can change between the
two reads, two Gets are not interchangeable with one. Two exceptions to the default:

- **Large copied values** (big structs, arrays) -- prefer one Get and a short wire, or pass
  by reference into a function.
- **Timing-sensitive mutable state** -- if a value may change mid-frame and the graph
  depends on both consumers seeing the *same* value, fetch once and route it.

Everywhere else, duplicate freely: a 2,000-unit data wire costs the reader every time they
trace it, which is a real and recurring cost.

Target geometry: a data provider sits within about 500 units to the left of its consumer.

Claireon's authoring tools default to emitting local-scope gets, so in most cases this
happens without your intervention. The judgement calls that remain:

- **Function parameters** are re-fetchable as local-scope Get nodes. Use them. A function
  entry node with wires leaving its parameter pins is a smell, and fixing it in one graph
  of the reference asset removed 27 reroute nodes and halved that graph's wire length.
- **Promoting a reused intermediate to a function-local variable** is a judgement call you
  make, not something the tools do for you. It converts a pure expression from
  evaluate-on-demand into a stored value written at one point, which changes results if the
  inputs can change between consumers, if the consumers sit on different branches, or if
  either sits inside a loop. Claireon deliberately does not do this automatically; `bp_lint`
  may suggest it, and you decide.
- **The event graph has no local variables.** When you need the same treatment there, the
  value has to be a member variable -- which is why the `Transient` category in section 6
  exists. Do not let the absence of locals push you back into long wires.

  **But a member used as scratch is shared object state, and that is only safe single-flight.**
  If the flow can overlap itself -- a re-entered event, two latent operations in progress,
  the same ability activating twice -- a second pass overwrites the member before the first
  continuation reads it, and the bug looks like corrupted data rather than a wiring mistake.
  Object-reference members also keep the referent alive longer than the flow that set it.
  Prefer extraction that carries the value as a parameter; reach for a transient member only
  once you have established single-flight and lifetime safety.
- **Local variables inside a pure function are always fine.** A function-local is scoped to
  one evaluation; nothing outside can observe the write, so it is not a side effect and
  `pure-function-has-side-effects` does not count it. Only writes to MEMBER variables and
  calls to impure functions are side effects in a pure function. Do not un-pure a function,
  and do not move its locals out, on the strength of local Sets.
- **Extract a pure data island only when it earns a function.** `pure-data-island` reports
  every cluster past its threshold; the threshold is a default measured on one ability
  graph, not a rule. Extract when EITHER (1) the island has several outputs, is expensive to
  compute, and the extraction becomes a NON-pure function that stores the result so it is
  computed once, OR (2) the island is very large and sits on the event graph -- AND in both
  cases (3) it is not already inside a pure function, where it is already isolated. A
  six-node Get/Break/Make cluster feeding one setter, or a nineteen-node island that is the
  body of a pure function, meets none of these: leave it and let the finding stand.

---

## 5. Layout intent

Run `bp_format` (BlueprintAssist) rather than positioning nodes by hand. What follows is
the intent a formatter cannot infer, plus the checks worth making after it runs.

### The event rail

Every `Event` and `CustomEvent` node in the event graph sits in **one left column**, at
the same x. Islands stack top to bottom with at least 256 units of vertical clearance,
and no two islands' bounding boxes may overlap.

**One event per island.** If following an event's exec chain leads you into a second
event node, the two islands have been fused and should be split. Fused islands are how a
graph ends up 22,000 units wide.

### The callback exception

A custom event bound as a callback from a single in-graph site -- an EQS query finishing,
a delegate bound one line above, an async load completing -- reads as a *continuation* of
the code that bound it, not as a new entry point. Putting it on the far-left rail is
technically consistent and actively confusing, because its one and only caller is
immediately above it.

Place such an event one indent right of the rail (roughly 500 to 650 units) and just
below its binding site. Keep it on the rail if it has more than one binder, or if its
binder is in a different graph.

### Exec chains ride a horizontal rail

Consecutive exec nodes step 256 to 512 units right with almost no vertical change. Any
vertical movement happens through a reroute, deliberately, at a branch or a Sequence
lane. If tracing a chain makes your eye jump vertically mid-line, something is wrong
upstream.

### Reroutes are short local jogs

A reroute exists to drop a wire into a lane, not to ferry it across the graph. A reroute
whose endpoints are more than about 1,000 units apart on exec, or 2,000 on data, is
telling you to restructure -- extract a function, or add a variable -- not to route
better. Chains of two or more reroutes in series are the same signal, louder.

Formatting a hand-placed graph for the first time will typically **raise**
`reroute-chain`, `long-reroute` and `long-wire` counts. The formatter is making
pre-existing structural debt visible, not adding it. Those findings are the input to
the next round -- fix `entry-param-wire` and extract pure data islands, and the
knots fall out with them (section 4 measured 27 reroutes removed and wire length
halved from one such fix). Do not read the delta as a regression, and do not revert
a format pass on account of it.

Do not reach for a different number to judge it by, either. Total wire length can
rise for a good reason -- separating overlapping runs can increase total wire length
while improving legibility -- so it is no better an acceptance gate than the finding count.
**Check the fixed point instead.** `bp_format` converges in one call: run it again and
nothing moves. So after you fix the structural cause, re-run it -- if the knots are
still there, the fix did not address the cause, and that is a conclusion you can reach
without any metric moving the right way.

### Do not chase coordinates

Node coordinates carry no meaning beyond the structure they express. In the reference
diff, grid alignment was statistically identical before and after; every real improvement
was structural. Do not spend edits nudging nodes toward round numbers, and do not treat a
coordinate diff against a reference asset as a defect. Fix the structure and let the
formatter place things.

---

## 6. Variable categorization

Derive the split mechanically, then apply judgement to the sub-buckets.

### The mechanical part

**Written by a graph in this Blueprint** is *evidence* that a variable is derived state
rather than authored configuration -- not proof. Where it holds, the variable should be:

- marked `Transient` (recomputed at runtime, so saving it is at best wasted and at worst a
  stale-value bug),
- **not** `InstanceEditable` (a designer-facing default overwritten on the first frame is a
  trap),
- categorized under `Transient`.

**Several legitimate patterns are graph-written and not derived state**, and none of them
should be relabelled:

| Pattern | Why the write is legitimate |
|---|---|
| `SaveGame` | persisted deliberately; Transient would defeat it |
| `Config` | authored default the graph normalises |
| Replicated / RepNotify | written by the network layer's own path |
| `ExposeOnSpawn` | supplied at spawn, then updated at runtime |
| Construction-script write | a default computed at construction time |
| Externally editable | something outside this Blueprint may set it |

`bp_lint` reports `transient-not-marked` at **info** severity and **medium** confidence, and
suppresses it entirely when any of the above is present. Treat a finding as a question, not
an instruction -- and note that clearing `InstanceEditable` on a variable something else
really does set is a functional change, not a tidy-up.

Use `Transient|Handles` for the subset whose only job is to be released later: gameplay
effect handles, timer handles, ability task references, spawned actor references. They read
as a group because they are cleaned up as a group, which makes a missing cleanup visible.
Type alone does not settle this: an actor or task reference may equally be configuration or
an injected dependency.

**Only read, never written** suggests authored configuration. It usually belongs in a config
or tuning category and is usually `InstanceEditable` -- but an external writer is always
possible, so this too is evidence rather than proof.

A variable that is *both* graph-written and `InstanceEditable` is the one case that is a bug
rather than a style issue: the designer's value is overwritten on the first frame, which
reads as the details panel being broken. `bp_lint` reports that one at **warning**.

### The judgement part

Claireon only suggests the generic buckets (`Transient`, `Transient|Handles`, `Config`,
`Debug`, `Deprecated`), because it cannot know what your configuration is *about*. Sub-bucketing
the config variables is yours to do, and it is worth doing once a Blueprint has more than
a handful.

Group by the thing being tuned, not by type. `Tuning|Targeting`, `Tuning|Throw`,
`Tuning|Apex`, `Setup`, `Debug` beat `Floats`, `Booleans`, `Classes` every time, because
a designer opens the details panel looking for a behaviour, not for a type. Follow any
existing category vocabulary in sibling assets rather than inventing a parallel one.

### Unreferenced variables

`bp_lint` can only prove a variable is unreferenced *within its own asset*. Blueprint
variables are reachable from other content -- child Blueprints, level scripts, Sequencer
tracks, data assets, native code touching them by name.

So: **an unreferenced-in-asset finding is never grounds for deletion on its own.** To act
on one, confirm with a corpus-wide search (`bp_search`, plus `asset_references` for
asset-typed variables). Note that a cold Find-in-Blueprints index can take minutes to
build on first search; `bp_search_index_status` tells you whether the next search will
pay that cost, and `bp_lint` returns that status alongside its findings so you know the
confidence ceiling you are working under.

If you cannot afford the search, report the finding and leave the variable alone. Do not
launder "no references in this asset" into "safe to delete" in your summary to the user.

The same reasoning applies to uncalled functions.

---

## 7. Comments

Comment nodes in this codebase are used as **header documentation blocks**, parked above
a graph's top-left node, not as boxes drawn around node groups. Write prose explaining
intent and non-obvious decisions, and reference a design note by path when one exists.

Every island of more than roughly twenty nodes deserves one. This is the most commonly
skipped item on this list, and the cheapest to fix: a 400-node event graph with one
comment on the first island is not documented.

Explain *why*, since the nodes already show *what*. "Back-to-front is what makes the
output ascending" is worth writing down; "spawns the projectile" is not.

---

## 8. Working with `bp_lint`

- Run it before you start, to decide what the graph actually needs, and again after you
  finish, to confirm you did not trade one problem for another.
- Findings carry a `suggested_fix` with a tool name and a complete argument set. That is a
  suggestion, not an instruction. Read the evidence before applying one.
- `bp_lint` never mutates anything. Every change goes through an explicit tool call that
  you make.
- Layout-scope rules will fire on existing content that predates them. A pile of
  layout findings on an asset you are touching for an unrelated reason is not a mandate to
  reformat it. Fix what you are in there to fix.
- Do not report a lint count as if it were work completed. Report what you changed,
  and do not use the lint total as an acceptance gate for a layout pass. "Confirm
  you did not trade one problem for another" is about semantics -- execution order,
  data freshness, extraction correctness -- not arithmetic on finding counts.

## Anti-patterns, collected

- A `Branch` whose `else` reroutes forward to rejoin its own `then` chain.
- A `Sequence` with one connected output.
- A Custom Event named after a parent event, an interface function, or an event already in the graph.
- A reroute pair spanning thousands of units.
- A wire leaving a function entry's parameter pin toward a distant consumer.
- Two events chained into one island.
- A callback event parked on the far-left rail, thousands of units from its only binder.
- The same exec chain present twice in one Blueprint.
- `Print String` inline in the main execution chain.
- A member variable that is both graph-written and `InstanceEditable`.
- Deleting a variable because it looked unreferenced inside one asset.
