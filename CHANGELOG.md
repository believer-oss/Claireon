# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

Two rounds of tooling-feedback work. Both rounds ship together.

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
