# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
