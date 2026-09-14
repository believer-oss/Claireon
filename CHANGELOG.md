# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [2.2.0] - 2026-09-14

Blueprint authoring tools review response: nine review findings (R1-R8, C1) fixed
with executed regression coverage, plus the follow-up round's transaction-identity
and recovery-accounting corrections. Unified with the PCG authoring follow-up work
(generation, data inspection, parameter binding) and the three carried-forward
mutation-safety items (scoped editor ownership, local-get cleanup, atomic PCG editor
reconstruction).

### Added

- **`instructions_list` / `instructions_read`** expose the served instruction documents
  (topic slug, title, summary, wire address; full text by slug) as plain tools, so a caller
  holding only tool access reads the same bytes the `resources/read` and `prompts/get`
  transports serve.
- **`mcp_reload_content`** rescans the plugin's `Content/MCP` tree and refreshes the prompt
  and resource registries without an editor restart; a document that fails to parse keeps
  its previously loaded copy.
- **`level_build_brush`** creates box brush geometry on a placed Volume actor. A volume
  spawned by `level_place_actor` or `audio_place_audio_volume` has no brush, so its bounds
  were a zero-extent point and PCG generation domains, reverb and blocking saw nothing.
- **Side-band advisories.** Hints, warnings and per-call summaries raised by tools invoked
  from inside one `python_execute` request are captured, de-duplicated (occurrence counts
  preserved, invariant divergence reported) and rendered as one rollup on the request's
  result instead of being lost in stdout. Repeated hints are rate-limited per hint key
  within the request; direct tool calls are unaffected.
- **`pcg_generate`** runs generation on the placed components bound to a graph (or one
  actor's component) and waits for it to finish before returning, reporting per-component
  instance counts. `UPCGComponent::Generate()` only schedules work, so measuring in the
  call that triggered it read the previous generation. A generation that does not finish
  within `timeout_ms` is an error, never a success with a flag.
- **`pcg_inspect_data`** reports the runtime data on a node's output pin after generation:
  point count, density and Z ranges, and min/max/mean per numeric attribute, with
  non-numeric attributes named rather than omitted. Inspection is enabled before the
  generation it reads, because PCG records inspection data only for executions that had
  it on beforehand.
- **`pcg_add_user_parameter` returns `property_guid`.** `UPCGUserParameterGetSettings`
  binds by `PropertyGuid`, not by name, and the graph's parameter bag is not reachable
  through Python reflection, so the response is the only place that value can come from.
  `pcg_set_node_property` on `PropertyGuid` accepts the parameter NAME as well as a GUID,
  resolves it, syncs `PropertyName`, and rebuilds the pins through `SetSettingsInterface`.
- **`pcg_connect` verifies the edge landed** (`AddEdge` returns void and silently no-ops
  on a pair it dislikes) and refuses to link from a Get Graph Parameter node bound to no
  live parameter -- the runtime graph accepted that edge while the editor logged "Could not
  create link" and the graph generated nothing.
- **`pcg_apply_spec` connections resolve the graph's own Input/Output boundary**, falling
  through from spec-local ids to the identifiers `pcg_connect` accepts. A connection the
  spec asked for and did not get is an error, not a warning.
- **`pcg_get_node_properties` expands instanced sub-objects** one level
  (`MeshSelectorParameters.MeshEntries: ...`) so a dotted-path write can be read back in
  the same language.
- **Stale-cache guidance**: `pcg_save` hints at `pcg_refresh` whenever live components are
  bound to the saved graph (they regenerate from the cached compiled graph until it is
  evicted), and `pcg_set_node_property` says so in its description.
- **`level_place_actor` reports brushless volumes** (`volumes_without_brush`) and hints at
  `level_build_brush`: a `SpawnActor`'d volume has zero-extent bounds, so PCG generation
  domains, reverb and blocking silently see nothing.
- **`claireon://instructions/pcg-authoring`** resource: the behaviours that make a correct
  PCG graph look broken over MCP (asynchronous generation, the compiled-graph cache,
  GUID-bound parameters, settings whose values do not mean what their names suggest).

### Fixed

- **The plugin compiles on stock UE 5.5 through 5.8 again.** Every iteration over
  `FJsonObject::Values` uses `const auto&` and reads the key through `FString(*Key)`:
  `FJsonObject::FStringType` exists only from 5.8, a spelled-out `TPair<FString, ...>`
  is a temporary that clang on 5.8 refuses to bind, and `UE::FSharedString` key arrays
  do not exist before 5.8. Verified with a clean plugin-only build against stock 5.5, 5.6,
  5.7 and 5.8 (`WITH_BLUEPRINT_ASSIST=0`, tests excluded).
- **bp_format compiles against BlueprintAssist 4.5.x and 4.9.x from one source tree.**
  4.9 (the UE 5.8 release) moved `OnPostFormatting` into `FBAFormatRequest`, replaced
  `IsCalculatingNodeSize()` with `GetNumberOfPendingNodesToCache()`, renamed
  `FormatAllEvents()` to `RequestFormatAll()`, and made `FBAInputProcessor::NodeActions`
  a `TUniquePtr`. `ClaireonBlueprintAssistCompat.h` wraps those four surfaces, selecting
  by `__has_include("BAGraphHandler/BAFormatRequest.h")` rather than a version number.
  The interactive transaction baselines accept BlueprintAssist 4.9.1 alongside 4.5.2: the
  extraction transaction sequences were re-run against 4.9.1 on UE 5.8.2 and observed
  unchanged, so the version list grew rather than the sequences. DEC-15's canonical
  by-reference case records the engine's `bIsConst` on Array Add's TargetArray instead of
  asserting the 5.5 value (5.8 reports false); the assertions that carry the claim, the
  ArrayParm branch and the by-reference walk, are unchanged.
- **UE 5.7/5.8 behaviour shifts absorbed in the tools, not the tests.** Node-title
  lookups (`target_node_title`, `node_title`) fall back to the normalized spelling when the
  exact one misses, because the engine renders `Print String` or `PrintString` depending on
  whether friendly names apply in the process; two nodes that normalize alike still report
  as ambiguous. Enhanced Input set/remove edit the array `GetMappings()` returns (5.7 moved
  it into `DefaultKeyMappings` and left a deprecated `Mappings` mirror that reflection found
  first). Level-sequence tools read a binding's name from its possessable or spawnable
  (`FMovieSceneBinding::GetName()` is deprecated from 5.7 and empty). `widgetbp_duplicate_animation`
  registers the copy's variable GUID like `create_animation` does, and the GUID bookkeeping
  applies from 5.6 where the API appeared. Test-side: the camera "no director" expectation
  is declared only before 5.7 (add_rig installs one from 5.7), and the trace fixture declares
  5.8's `[MemAlloc] Invalid Tag` analyzer errors expected.

- **A scoped asset-editor open closes exactly the instance it opened.**
  `FClaireonScopedAssetEditor` closed through `CloseAllEditorsForAsset`, which also took
  down any editor somebody else opened on the same asset during the scope. It now closes
  the toolkit it recorded at construction through that toolkit's own `CloseWindow`, only
  while the subsystem still lists it; an instance the user closed in the meantime is left
  alone and never dereferenced. Pre-existing editors were already never closed.
- **Synthesized local gets leave no orphan on failure.** `EmitAdjacentVariableGet` and
  `EmitLocalParameterGet` added their node before knowing whether it had a value pin
  (`UK2Node_VariableGet` creates one only when the reference resolves a property) and
  returned null without removing it; the caller never received the node and could not
  clean it up. Both now destroy their node on every unsuccessful path.
- **PCG editor reconstruction is atomic with respect to the editor graph.**
  `ReconstructOpenEditor` removed orphaned editor nodes before resolving every missing
  counterpart's class, so an `Unavailable` result could follow a partial mutation. Every
  class lookup and every replacement node's construction and link now happen before the
  first removal; `Unavailable` is returned with the view exactly as found, the runtime
  graph untouched, and a later retry able to complete the same rebuild.
- **PCG edge and node edits settle the pending editor rebuild first.** The editor-view
  reconstruct coalesces to the next tick, but a batch of edits in one request (a
  `python_execute` body, `pcg_apply_spec`'s passes) never ticks between them; an `AddEdge`
  after an `add_node` notified the natively built Input/Output editor nodes, whose link
  rebuild looked the new peer up, found nothing, and fired "Could not create link" ensures
  with the asset editor open. `pcg_connect`, `pcg_disconnect`, `pcg_disconnect_all`,
  `pcg_remove_node` and the spec applicator now settle the graph's pending rebuild before
  mutating.
- **`pcg_inspect_data` compiles on UE 5.8**, where `UPCGSubsystem::GetExecutedStacks` is
  gone: executed stacks are read from the component's execution-state inspection record.
- **`pcg_inspect_data` reads every point-data shape and every entry.** From UE 5.6 samplers
  emit `UPCGPointArrayData`, a sibling of `UPCGPointData`; the tool cast to the latter and
  reported zero points as a success. It now reads through `UPCGBasePointData` on 5.6+. A
  point with no metadata entry resolves to the attribute's DEFAULT value and was skipped, so
  a default 5 and an explicit 9 reported mean 9 and an all-default attribute vanished; every
  entry now counts.
- **`pcg_generate` reports each component's own instances**, read from its managed ISM
  resources. Counting the owner's ISM components attributed every component on a shared
  actor (and hand-placed instances) to each of them and summed once per component;
  `owner_ism_instances` / `owner_instances_total` keep the level-visible number, once per
  distinct owner.
- **`pcg_inspect_data`'s actor selector is applied before the 32-component cap**, so a
  requested actor beyond the first 32 is found instead of reported as having no component.
- **`pcg_add_user_parameter` never leaves a declaration behind on a failed readback.** The
  post-mutation missing-descriptor error removes a newly declared parameter before
  cancelling the transaction (cancel is not rollback) and says which of the two states --
  removed again, or pre-existing and unchanged -- the graph is in.
- **Landscape test fixtures unregister from `ULandscapeSubsystem`.** A `SpawnActor`'d
  `ALandscape` has no guid, so `Destroy()` never unregistered it and the subsystem's next
  tick dereferenced a null `ULandscapeInfo` inside whichever unrelated test ran seconds
  later. The scoped fixture assigns the guid and builds the info so register/unregister are
  symmetric.
- **Tests appended after their `WITH_UNTESTED` guard** in the AssetUtils and PCG gap
  suites compiled only where the framework is present; the guard now closes the file.
- **`bp_format` refuses before touching BlueprintAssist-only code** in a build without the
  plugin, instead of referencing helpers that exist only under `WITH_BLUEPRINT_ASSIST`.
- **UE 5.8 key lookups**: two `FJsonObject::Values.Find(FString)` sites (`bp_stack_islands`,
  `bp_lint`) use the TCHAR* spelling that compiles against `UE::FSharedString` keys.

- **`prefer_local_gets` substitution is restricted to pure, receiver-less reads.** A
  distant variable get with a wired or defaulted Target pin (an explicit receiver), or
  a validated (impure) get, now keeps its literal wire: the synthesized adjacent get
  copies only the variable reference and would otherwise silently retarget the read to
  `self` -- or fail to compile.
- **`promote_enclosing_locals` refuses split getters** (`split_pin_unsupported`).
  Promotion rewires only the parent value pin; a struct getter split into field subpins
  would have had its field consumers silently disconnected and left compiling on
  default values.
- **Event extraction preserves internal loopbacks.** A single-entry cyclic body (a
  Branch retried through a Delay) kept its backedge only by luck of shape; the entry
  pin's `BreakAllPinLinks` -- which could only ever sever internal edges, since the
  boundary pairs were already broken from the external side -- is gone.
- **`transaction_rollback_group` gates its undo on the group transaction's identity**
  (`FTransactionContext::TransactionId` captured at `begin_group`), never on the display
  title. An empty group (first mutation refused in preflight) is popped by the engine on
  close; the rollback now declines with `group_not_at_undo_head` instead of undoing
  whatever sits at the head -- including a previous same-label group's work.
- **`bp_format` retention and recovery reporting is measured, not inferred.**
  `mutation_retained`, `undo_record_available`, and `recovery.undo_count` derive from
  the transaction buffer (standing entries above the pre-call undo head) and from a
  positions diff against the pre-mutation snapshot -- never from per-island tallies.
  A settle-failed island's transaction is counted; a rolled-back island's is not; a
  clean island's retained format keeps `mutation_retained` true; and
  `recovery.count_confidence` distinguishes an exact count from one that must be
  verified against `transaction_history`.
- **`is_static` preflight distinguishes member ownership from receiver identity.** A
  helper reading a property through an explicitly wired object parameter is a legal
  static-function shape and converts; only implicit self-context access refuses.
- **`bp_lint` variable usage resolves to the declaration's owner.** Another class's
  same-named property and function-locals no longer fake `written_by_graph` evidence on
  this Blueprint's variable or suppress its unreferenced finding.
- **Synthesized PCG editor nodes self-update on native edits.** Nodes the editor-sync
  creates in an open PCG editor now subscribe to `UPCGNode::OnNodeChangedDelegate`
  (public runtime API) and rebuild their pins when a Details-panel change lands --
  previously that binding existed only for natively constructed nodes. Subscriptions
  are unbound at module shutdown and self-clean when their editor node dies.
- **Stale operational guidance corrected**: the extraction refusal names
  `bp_switch_graph` (not the nonexistent `bp_open_graph`); `test_run` documentation
  states the bridge gate has no autonomous expiry and releases only when a final or
  cancelling `test_poll` observes completion.
- **Fuzz-baseline gameplay tags moved out of `UE_DEFINE_GAMEPLAY_TAG`** (refused in an
  Editor-type module; fired ensures at every DLL load and broke commandlet test
  discovery) into the plugin's `Config/Tags/ClaireonFuzzTags.ini`, registered via
  `AddTagIniSearchPath` at module startup, commandlets included.

### Known limitations (recorded, by design or deferred)

- **Undo cannot restore exact post-format positions.** BlueprintAssist adjusts island
  anchor positions on later settle ticks OUTSIDE any transaction, so undoing every
  transaction a `bp_format` call opened restores structure exactly (nodes, links,
  minted reroutes) but can leave small position residue on island roots. The recovery
  block discloses this; `mutation_retained` counts residue as retained work
  (`retained_is_untransacted_residue`), and re-running `bp_format` converges it.
- **PCG synthesized-node affordances deferred**: the dynamic-pin add/remove control,
  enabled-state visuals, and error badges depend on private, non-reflected editor state
  that `Construct()` alone can set. Pin/link correctness is unaffected. Deferral,
  workaround (editor restart constructs natively), and closing options are recorded in
  [the PCG affordance work item](../../Docs/llm/todo/pcg-synthesized-node-affordances.md).
- **The two real-asset extraction tests are opt-in.** The plugin ships no game content,
  so `Claireon.BPEditor.ExtractionSemantics.PureIslandOnTheRealAssetExtractsPure` and
  `...EnclosingLocalsRefuseOnTheRealAsset` need a project Blueprint to clone. Point them at
  one with `CLAIREON_TEST_BLUEPRINT` (full object path) and `CLAIREON_TEST_BLUEPRINT_FUNCTION`
  (a function graph whose island reads enclosing-graph locals). Unset, both pass carrying a
  warning that names the variable to set -- the automation framework has no Skipped state to
  report -- so a checkout with no game content is not a red suite. Every other fixture in the
  suite is synthetic.
- **Function-properties RPC routing is verified at authoring level only.** The test
  suite's waiver of runtime client/server routing verification stands and is recorded
  in the suite itself; this release makes no claim of runtime-verified RPC behavior.

## [2.1.0] - 2026-08-13

Two rounds of tooling-feedback work, the 2026-08 P0/P1/P2 defect-triage bands,
the runtime GAS tool family, and Unreal Engine 5.6/5.7/5.8 compatibility.

### Changed — BREAKING (behavior)

- **Arguments a tool's schema does not declare are now hard errors.** They were
  silently ignored, so a misspelled optional parameter produced a default-behavior run
  reported as success. Adding a parameter remains a safe widening; renaming or removing
  one is now a visible break. A schema with no `properties` object stays permissive.
- **`level_list_actors` `class_filter` is an is-a test.** The filter resolves the name
  to a class and matches subclasses (`include_subclasses=false` for exact-class); the
  legacy case-insensitive substring match survives only as a fallback for names that do
  not resolve, disclosed by a warning. Result sets can grow (BP subclasses now match)
  and shrink (unrelated substring matches no longer do).
- **A colliding `(category, operation)` tool registration is refused** and logs an
  Error at boot, instead of silently overwriting the earlier tool.
- **`pie_get_player_pawn` returns structured data** instead of a preformatted string.

### Changed — BREAKING (wire format)

- **`bp_compile_batch`, `log_search`, and `log_tail` moved their advisory text from
  `data.hint` to the result-level `hint` block.** These three were the only users of an
  ad-hoc `Data.hint` string convention that ran parallel to `FToolResult::Hint`; there is
  now one channel. Consumers reading `data.hint` on those tools will find it absent —
  read the `<hint>` block (MCP) or `hint` (Python envelope) instead. Two in-tree spec
  readers were repointed in the same change; they would otherwise have passed forever
  against a field nobody sets.
- **The REPL now serializes `hint` on both success and error paths.** It previously
  dropped the field entirely, which is what made the `data.hint` convention look
  necessary. Note the knock-on: `python_execute`'s existing nudges become visible in the
  REPL for the first time. Bounded — the session latch is transport-independent, so each
  still fires at most once per session — but it is a visible behavior change in a channel
  with prior noise complaints.

### Changed

- **Session inactivity timeout default dropped from 60 to 10 minutes** (one shared
  constant, `ClaireonDefaultSessionTimeoutMinutes`). Every operation on a session
  resets the clock via `Touch()`, so only genuinely idle sessions expire; a forgotten
  read session no longer blocks editor-wide tools for the better part of an hour.
  `timeout_minutes` on `bp_open`/`bp_create`/`animgraph_open` still overrides per session.
- **`bp_apply_spec` is now discoverable as the batch-edit primitive.** Search keywords,
  description, and a when-to-use patterns block cover the vocabulary sessions actually
  reach for (`bp_edit_batch`, `bp_apply_graph_diff`, batch/bulk/one-call/atomic), and
  `bp_add_node`'s guidance points multi-node authoring at it. Gated by new
  tool-search corpus rows. The July feedback sessions searched for a batch primitive,
  missed this tool entirely, and burned ~90 round-trips authoring one event body.

### Fixed

- **`bp_set_pin_value` on a SPLIT pin distributes the literal to the sub-pins.**
  Previously the parent-pin serialized string was written and reported success, but
  the compiler reads split defaults from the sub-pins, so the value never applied
  (silent zeros at runtime). Named form `(X=..,Y=..)` maps by member; bare form
  `10,20,30` maps in the struct's own bare-parse order (note FRotator's is
  Pitch,Yaw,Roll -- not the sub-pin display order), so a literal means the same thing
  split or unsplit. All leaf values validate before any write; unknown members error
  naming the valid ones.
- **Latent-task class-pin refresh is now pinned by a test.** Setting the `Class` pin
  on `K2Node_LatentGameplayTaskCall` exposes the spawn class's ExposeOnSpawn pins in
  the same call (no `bp_reconstruct_node`), matching the existing
  `K2Node_SpawnActorFromClass` guarantee.

### Added

- **Unreal Engine 5.6, 5.7, and 5.8 compatibility, version-gated.** The plugin builds
  from clean against stock 5.5 through 5.8 (JSON object keys are spelled portably for
  5.8's `FSharedString`-keyed `FJsonObject`).
- **Runtime GAS tool family (`gas_*`)** — inspect and mutate abilities, effects,
  attributes, and gameplay tags on live PIE actors, defaulting to the server world.
- **`editor_log_search` `since`/`before` time bounds**, so a search can be scoped to
  the window an experiment actually ran in.
- **`bp_get_graph` `resolve_knots` view** — connections reported as if reroute knots
  were transparent, alongside the raw view.
- **`bp_set_pin_value` `pin_direction` hint** (defaults to input), disambiguating pins
  that share a name across directions.
- **`bp_close` accepts `asset_path`** as an alternative to `session_id`.
- **`uobject_inspect` unwraps `FInstancedStruct`**, emitting `_struct` plus the wrapped
  fields instead of an opaque export string; unset wraps emit `_struct: null`.
- **Tool-search ranking: query-domain and operation-token-coverage signals** scored
  outside the FTS5 columns, so a domain-specific tool outranks its
  abbreviation-enriched siblings for its own domain vocabulary.
- **`uobject_set_property`** — the write counterpart to `uobject_inspect`, with the same
  reflection reach: properties with no Blueprint accessor specifier, protected/private
  fields, transient fields, nested struct members, and array elements, on assets, CDOs,
  and live PIE instances. Writing a property the details panel would refuse (no
  `EditAnywhere`, or `EditConst`) requires an explicit `allow_non_editable=true`. Writes
  are transactional and fire change notification on the object that actually owns the
  property, not just the path root.
- **`component_reregister`** — runs the details-panel change cycle on a component so
  engine state derived from its properties is re-established after a raw write that
  bypassed notification. `changed_property='bCanEverAffectNavigation'` is what resyncs the
  navigation octree, since `SetCanEverAffectNavigation` is not a `UFUNCTION` and Python
  can only write the flag raw.
- **`pie_set_paused(bool)`** — sets rather than toggles, so a caller cannot leave PIE
  paused and then misread a frozen world as an idle one.
- **`bp_add_macro`**, **`bp_list_node_types`**, and the **curve asset tool family**
  (`create` / `add_key` / `set_keys` / `clear_keys`).
- **Guidance hints** on `bp_get_graph` and `bp_get_component_details`, naming the
  parameter that would return what a detail level omitted. Latched once per session per
  hint code, so a bulk sweep is not narrated.

### Removed

- **`pie_tick` and `pie_sleep`.** Unfixable by construction: tool calls run on the game
  thread, so neither can ever advance the world from inside `python_execute` — the
  promise in their own descriptions could not be kept in any context. `pie_wait_for` and
  `pie_wait_poll` already implement the correct non-blocking pattern.

### Fixed

- **Trace results tell the truth.** Stat-derived scopes reach `pie_trace_start`
  captures; GPU presence is disclosed from the capture itself rather than the requested
  channel string; non-finite numbers are replaced with `null` at every result boundary
  instead of emitting invalid JSON; capture contents and finite aggregates are
  disclosed on `trace_open`.
- **Silent-success write paths are closed.** Writes that could not be applied
  (unresolved functions, unbound nodes, rejected literals) now error or disclose,
  instead of committing a dead node and reporting `created`; dotted
  `"Class.Function"` spellings in node specs bind correctly.
- **The deferred-action autosave names every package it writes** before and after the
  write, so an implicit save is never invisible.
- **Actor ids are scoped to their world**, and class arguments coerce to the CDO, so a
  stale id cannot silently resolve into the wrong world.
- **`pcg_save` saves freshly created packages**, not just previously saved ones.
- **The world-transition barrier runs for map-less PIE starts**, not only for
  `mapPath=` starts.
- **StateTree runtime components resolve by type, not class-name substring**, so
  `UStateTreeAIComponent` subclasses are found.
- **`console_execute` returns the command's log output** instead of a bare success.
- **World-transition Python purge had never executed.** The barrier compiled its
  multi-statement script under `ExecuteStatement` (CPython `Py_single_input`), so it
  raised `SyntaxError` before running; the return value was discarded and `Unattended`
  suppressed the error. Dark at four call sites since it was written. Now runs, reports
  failures with detail, and logs how many references it nulled.
- **`bp_get_component_details` returned archetype deltas, not values.** Struct properties
  were exported through `ExportTextItem_Direct`, which omits any member equal to the
  struct default, so an explicitly-set-and-persisted `bStartWithTickEnabled=False` simply
  vanished from the output. Absent read as unset and nearly caused correct work to be
  reverted. Values are now expanded member by member, the editable-only filter is gone
  (reach matches `uobject_inspect`), and each property carries `default_value` plus an
  `overridden` flag so the distinction is stated rather than inferred.
- **Read-only Blueprint inspection no longer opens a blocking session.**
  `bp_get_component_details` registered a session for a read, which made the bridge refuse
  every bypass-mode tool until it timed out.
- **The proxy no longer reports "build and launch the editor first" for a running
  editor.** It now distinguishes `starting`, `loading`, `evicted`, and `unresponsive` from
  `no_editor`, and results carry a machine-readable `editorState`.
- **`bp_set_property` accepts nested paths.** It did a flat property lookup while its own
  description claimed dot notation worked. Both property setters now share
  `bp_set_cdo_property`'s resolver.
- **`uobject_inspect` resolves SCS component templates** when the object is a CDO and the
  component property is null (where the authored defaults actually live).
- **`pie_start_async(mapPath=)` drains asset compilation first**, by default, with an
  opt-out. A hard crash is worse than a slow call.
- **`session_release` with no arguments names the open sessions** it could release,
  instead of only naming the parameters it accepts.

## [2.0.0] - 2026-03-11

Initial open-source release.

### Added

- **MCP Server** — Streamable HTTP server with JSON-RPC 2.0 dispatch, session management, and tool registration
- **Blueprint Tools** — Read/edit Blueprint graphs, properties, components, and connections (`get_blueprint_graph`, `edit_blueprint_graph`, `format_blueprint_graph`, `get_blueprint_properties`, `search_in_blueprints`, `compile_blueprints`, `diff_blueprint`)
- **State Tree Tools** — Inspect and edit State Tree assets, nodes, and runtime state (`state_tree_inspect`, `state_tree_edit`, `state_tree_diff`, `state_tree_list_node_types`, `state_tree_runtime_inspect`, `state_tree_runtime_send_event`)
- **Behavior Tree & EQS Tools** — Inspect and edit Behavior Trees, Blackboards, and Environment Query Systems (`behavior_tree_inspect`, `behavior_tree_edit`, `blackboard_edit`, `eqs_inspect`, `eqs_edit`)
- **Widget Blueprint Tools** — Read and modify UMG widget hierarchies and animations (`get_widget_bp_tree`, `edit_widget_bp`)
- **Niagara Tools** — Inspect and edit Niagara particle systems (`niagara_inspect`, `niagara_edit`)
- **PCG Tools** — Inspect and edit Procedural Content Generation graphs (`pcg_graph_inspect`, `pcg_graph_edit`)
- **Data Table Tools** — Full CRUD operations with CSV/JSON import/export (`data_table_get_info`, `data_table_get_row`, `data_table_get_rows`, `data_table_find_rows`, `data_table_add_row`, `data_table_set_row_values`, `data_table_rename_row`, `data_table_move_row`, `data_table_duplicate_row`, `data_table_remove_row`, `data_table_search`, `data_table_export_csv`, `data_table_export_json`, `data_table_import_csv`, `data_table_import_json`)
- **Asset Tools** — Search, list, validate, resave, cook, diff properties, fix up redirectors, and query references (`search_assets`, `list_assets`, `validate_assets`, `resave_assets`, `cook_assets`, `diff_asset_properties`, `fixup_redirectors`, `get_asset_references`)
- **PIE Tools** — Start/stop Play-In-Editor, query actors, spawn enemies, test abilities, take screenshots, manage traces (`pie_start`, `pie_stop`, `pie_status`, `pie_get_player_pawn`, `pie_get_actor`, `pie_get_component`, `pie_list_actors`, `pie_spawn_enemy`, `pie_test_ability`, `pie_screenshot`, `pie_wait_for`, `pie_check_init_state`, `pie_ai_target_info`, `pie_get_damage_events`, `pie_register_damage_listener`, `pie_unregister_damage_listener`, `pie_trace_start`, `pie_trace_stop`)
- **Trace Tools** — Open and analyze Unreal Insights trace files (`trace_open`, `trace_close`, `trace_get_session_info`, `trace_get_threads`, `trace_get_top_scopes`, `trace_get_scope_details`, `trace_get_frame_stats`)
- **Python Execution** — Run arbitrary Python scripts in the editor with audit logging (`execute_python_script`, `python_audit_log`)
- **Utility Tools** — Engine/project info, console commands, log tailing, map management, live coding reload, session management, tool search (`engine_info`, `project_info`, `console_execute`, `log_tail`, `map_open`, `map_status`, `live_coding_reload`, `list_sessions`, `release_sessions`, `search_tools`, `feedback_submit`)
- **Flythrough Tools** — Camera flythrough recording and playback (`flythrough_start`, `flythrough_stop`, `flythrough_status`)
- **Built-in REPL** — In-editor AI chat assistant with Claude integration (optional, requires API key)
- **Session Management** — Exclusive-per-asset locking with automatic timeout cleanup
- **External Tool Registration** — Other modules can register tools via `FClaireonModule::RegisterExternalTool()`
- **PowerShell Utility Scripts** — Build, project file generation, Blueprint compilation, asset validation, resave, redirector fixup, clean, and git utilities
- **Claude Code Instruction Documents** — Structured AI agent prompts for git workflows, UE workflows, development workflows, and documentation generation
- **Third-party: sqlite-vec** — Vendored v0.1.6 loadable extension (MIT/Apache 2.0) for future vector search support
