# Contributing to Claireon MCP

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
4. Follow the naming convention: `ClaireonTool_<Category><Action>.h/.cpp`

### Tool Naming Convention

Tool names follow the pattern `editor.<category>.<action>`:

```
editor.blueprint.get_properties
editor.state_tree.inspect
editor.data_table.add_row
editor.asset.search
```

### Tool Design Guidelines

- **Return structured JSON** — Use `FToolResult::Data` for machine-readable output and `FToolResult::Summary` for human-readable summaries
- **Validate inputs early** — Return clear error messages via `MakeErrorResult()` for invalid arguments
- **Respect sessions** — Use `FClaireonSessionManager` for any tool that modifies assets
- **Keep execution fast** — Tools run on the game thread. Avoid blocking operations.
- **Write descriptions for AI** — Tool descriptions are read by AI assistants, so be precise about what the tool does, what arguments it accepts, and what it returns

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
