# Contributing to Claireon

Thank you for your interest in contributing to Claireon! This document provides guidelines and information for contributors.

## Getting Started

1. Fork the repository
2. Clone your fork and set up the plugin in an Unreal Engine project (see [README.md](README.md#installation))
3. Create a feature branch from `main`
4. Make your changes
5. Submit a pull request

## Development Setup

1. Copy the `Claireon/` directory into a UE5 project's `Plugins/` folder
2. Generate project files and build
3. Launch the editor with `-StartMCPServer` to test your changes
4. Use any MCP client (e.g., Claude Code) to exercise the tools

## Pull Request Process

1. **One concern per PR** — Keep pull requests focused on a single change
2. **Describe what and why** — Explain the motivation, not just the mechanics
3. **Include test evidence** — Show that the tools work (MCP client output, screenshots, test results)
4. **Update documentation** — If you add or change tools, update descriptions and schemas

### Commit Messages

Follow the conventional commit format:

```
<type>(<scope>): <description>

[optional body]
```

Types: `feat`, `fix`, `refactor`, `docs`, `test`, `chore`

Examples:
```
feat(tools): add DataAsset inspection tool
fix(server): handle malformed JSON-RPC requests gracefully
refactor(session): simplify lock expiry cleanup
```

## Adding New Tools

New tools are the most common contribution. To add one:

1. Create a new class implementing `IClaireonTool` in `Source/Claireon/Private/Tools/`
2. Add a corresponding header in `Source/Claireon/Public/Tools/` (if the tool needs to be referenced externally)
3. Register the tool in `ClaireonModule.cpp`'s `StartServer()` method
4. Follow the file naming convention: `ClaireonTool_<CamelCaseName>.h/.cpp`, where the name
   combines the category and action (e.g. `ClaireonTool_DataTableAddRow`,
   `ClaireonTool_AnimGraphAnalyze`)

Note: the MCP surface itself exposes only two meta-tools: `tool_search` (discovery) and
`python_execute` (execution). Your new tool is reachable through them — discovered via
`tool_search` and invoked as `claireon.<tool_name>(...)` inside `python_execute` — not as a
top-level MCP tool.

### Tool Naming Convention

A tool's wire name is `<category>_<operation>`, composed automatically from `GetCategory()`
and `GetOperation()` (`IClaireonTool::GetName()` is sealed so the two cannot disagree —
implement the parts, never the whole):

```
bp_get_graph
statetree_inspect
datatable_add_row
asset_search
```

### Tool Design Guidelines

- **Return structured JSON** — Use `FToolResult::Data` for machine-readable output and `FToolResult::Summary` for human-readable summaries
- **Validate inputs early** — Return clear error messages via `MakeErrorResult()` for invalid arguments
- **Respect sessions** — Use `FClaireonSessionManager` for any tool that modifies assets
- **Keep execution fast** — Tools run on the game thread. Avoid blocking operations.
- **Write descriptions for AI** — Tool descriptions are read by AI assistants, so be precise about what the tool does, what arguments it accepts, and what it returns

## Writing Tests

### Assert against `Data`, not `Summary`

`FToolResult::GetContentAsString()` is exactly `bIsError ? ErrorMessage : Summary`. It
**never** reads `Data`. Tool families disagree about what goes in `Summary`, with no
naming signal to tell them apart: some put the whole payload there (`BuildStateResponse`
on the blueprint / widget / anim edit bases, the log tools, niagara-compile, datatable
`export_json` / `export_csv`), while the inspect tools and most decomposed per-operation
tools put only a one-line count there and everything else in `Data`.

The consequence is a test that passes while verifying nothing:
`GetContentAsString().Contains("some_id")` can never match for the second family, so it
holds just as well on an empty result as on a correct one. That single ambiguity defeated
roughly 170 assertions before it was noticed.

The rule:

- Assert **structured facts** against `Result.Data`, unconditionally.
- Reserve `Summary` / `GetContentAsString()` for asserting **human-readable text shape** --
  "the error names the parameter", "the CSV header lists every column".

Use the guarded macros in
`Source/Claireon/Private/Tests/ClaireonTestDataAssertions.h`
(`UNTEST_CLAIREON_DATA_STR` / `_INT` / `_BOOL` / `_ARRAY` / `_OBJECT`). They are shorter
than the hand-rolled `TryGet*Field` form, and when a field is absent they name the field,
say whether the tool errored / returned no `Data` at all / carried a different key, and
dump the keys `Data` did contain. That header is also where the full convention is
written down.

### Never let a missing precondition score as a pass

Untest has no skip primitive. An early `co_return` is recorded as a **PASS**, so a test
that bails when its fixture is missing is indistinguishable from one that verified
everything. Prefer a hard failure whenever the precondition is something the test
controls -- a registry it can populate via `FClaireonModule::EnsureServerForTest()`, a
fixture it can create, a committed asset it can rely on. Reserve skips for genuinely
external configuration (no Python interpreter, no ONNX runtime), log them at `Warning` or
higher, and say in the message that the coverage did not run.

### Shipping-asset fixtures must be registered

If a test hard-codes a `/Game` path it expects to already exist, add a row to the table in
`Source/Claireon/Private/Tests/ClaireonShippingFixtureGuardTests.cpp`. That guard fails
loudly when a fixture is renamed, deleted, or replaced by an `ObjectRedirector` stub --
the rot that silently emptied every attenuation test.

## Code Style

- Follow Unreal Engine coding standards (see [UE Coding Standard](https://dev.epicgames.com/documentation/en-us/unreal-engine/epic-cplusplus-coding-standard-for-unreal-engine))
- Use tabs for indentation
- Prefix classes per UE convention: `F` for structs, `U` for UObjects, `I` for interfaces, `E` for enums
- Copyright header on all new files:
  ```cpp
  // Copyright (c) 2026 The Claireon Contributors
  // SPDX-License-Identifier: MIT
  ```

## Reporting Issues

When reporting bugs, please include:

- Unreal Engine version
- Steps to reproduce
- MCP client and tool name used
- Relevant log output (filter for `LogClaireon` in the Output Log)

## License

By contributing, you agree that your contributions will be licensed under the [MIT License](LICENSE).
