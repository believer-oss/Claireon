<p align="center">
  <h1 align="center">Claireon MCP</h1>
  <p align="center">
    Model Context Protocol server for Unreal Editor automation
  </p>
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="License: MIT"></a>
  <img src="https://img.shields.io/badge/Unreal%20Engine-5.x-black?logo=unrealengine" alt="Unreal Engine 5">
  <img src="https://img.shields.io/badge/platform-Win64-lightgrey" alt="Platform: Win64">
  <img src="https://img.shields.io/badge/MCP-Streamable%20HTTP-green" alt="MCP: Streamable HTTP">
  <img src="https://img.shields.io/badge/status-beta-orange" alt="Status: Beta">
</p>

---

Claireon is an Unreal Engine editor plugin that runs a [Model Context Protocol](https://modelcontextprotocol.io/) (MCP) server inside the editor, exposing 80+ tools for AI-assisted development workflows. It enables AI assistants like [Claude Code](https://docs.anthropic.com/en/docs/claude-code) to read, search, inspect, and edit Unreal assets directly.

## Features

- **Blueprint Editing** — Read and modify Blueprint graphs, properties, components, and connections
- **State Tree Editing** — Inspect and edit State Tree assets, nodes, and runtime state
- **Behavior Tree & EQS** — Inspect and edit Behavior Trees, Blackboards, and Environment Query Systems
- **Widget Blueprint Editing** — Read and modify UMG widget hierarchies and animations
- **Niagara & PCG** — Inspect and edit Niagara particle systems and PCG graphs
- **Data Tables** — Full CRUD operations on Data Table assets with CSV/JSON import/export
- **Asset Management** — Search, list, validate, resave, cook, and fix up redirectors
- **PIE Integration** — Start/stop Play-In-Editor, query actors, spawn enemies, test abilities, take screenshots
- **Trace Analysis** — Open and analyze Unreal Insights trace files for performance profiling
- **Blueprint Diffing** — Compare Blueprint and State Tree assets between revisions
- **Python Execution** — Run arbitrary Python scripts in the editor with audit logging
- **Built-in REPL** — In-editor AI chat assistant with Claude integration (optional)
- **Session Management** — Exclusive-per-asset locking with automatic timeout cleanup

## Requirements

- Unreal Engine 5.x (tested with 5.5+)
- Windows (Win64) — other platforms may work but are untested
- Python 3 plugin enabled in the editor
- [Claude Code](https://docs.anthropic.com/en/docs/claude-code) or any MCP-compatible client

## Installation

### As a Plugin (recommended)

1. Clone or download this repository
2. Copy the `Claireon/` directory into your project's `Plugins/` folder
3. Regenerate project files and build

```
YourProject/
  Plugins/
    Claireon/
      Claireon.uplugin
      Source/
      Content/
      ...
```

4. Enable the plugin in the editor under **Edit > Plugins > Claireon MCP**

### Starting the Server

The MCP server can be started in two ways:

- **Command line**: Launch the editor with `-StartMCPServer` to auto-start on load
- **Toolbar button**: Click the **AI Chat** button in the editor toolbar to open the diagnostics panel, which also starts the server

The server listens on port `8017` by default. Override with `-MCPServerPort=<port>`.

### Connecting Claude Code

Once the server is running, configure Claude Code to connect:

```json
{
  "mcpServers": {
    "unreal-editor": {
      "type": "streamable-http",
      "url": "http://localhost:8017/mcp"
    }
  }
}
```

Then use `search_tools` to discover available tools:

```
> search_tools blueprint
> search_tools state tree
> search_tools niagara
```

#### One-click launch from the editor

Once the plugin is loaded, the level editor toolbar exposes a **Claude Code** button next to **AI Chat** (also available under **Window > General > Miscellaneous > Launch Claude Code**). Clicking it:

1. Resolves the live MCP server port (handles auto-incremented ports if `8017` was busy).
2. Writes a per-launch MCP config to `Saved/Claireon/claude-code-mcp.json` so the committed `.mcp.json` stays untouched.
3. Spawns a PowerShell window at the project root and runs `claude --mcp-config <path>` against that config. On Windows 11 with Windows Terminal as the default console host, the new window automatically opens in Terminal.

Two optional settings under **Editor Preferences > Plugins > Claireon > Claude Code Launch** let teams customize the flow:

- **Initial Prompt** — string sent to Claude Code on launch (typically a slash command like `/mcp-connect-claireon` to auto-fire a project-specific init skill). Empty by default.
- **Skip Permission Prompts on Launch** — when true, passes `--dangerously-skip-permissions` so the initial-prompt skill can run unattended. Off by default.

Project-wide defaults can be committed via `Config/DefaultEditorPerProjectUserSettings.ini`:

```ini
[/Script/Claireon.ClaireonSettings]
LaunchInitialPrompt=/mcp-connect-claireon
bLaunchSkipPermissions=True
```

**First-run requirements (per machine):**

- Install Claude Code: <https://docs.anthropic.com/en/docs/claude-code/quickstart>
- Run `claude /login` once to authenticate against your Claude.ai subscription. Inference then bills against your Pro/Max/Team seat instead of the Anthropic API.

## Optional Dependencies

Claireon works standalone, but can integrate with these plugins when present:

| Plugin | Purpose | Detection |
|--------|---------|-----------|
| [Untested](https://github.com/believer-oss/untested) | Unit test framework for Claireon's test suite (coming soon) | Auto-detected via filesystem |
| BlueprintAssist | Enhanced Blueprint editor integration | Auto-detected via filesystem |
| LyraGame | Lyra framework init state checking in PIE tools | Auto-detected via filesystem |

## Architecture

```
Claireon/
  Source/Claireon/
    Public/
      ClaireonModule.h          # Module entry point, server lifecycle
      ClaireonServer.h          # HTTP server, JSON-RPC dispatch
      ClaireonSessionManager.h     # Per-asset exclusive locking
      Tools/
        IClaireonTool.h            # Tool interface — implement this to add tools
    Private/
      Tools/                     # 80+ tool implementations
      Tests/                     # Unit tests (requires Untested)
  Content/
    Python/                      # Python bridge scripts
```

### Registering External Tools

Other modules can register tools with Claireon without including server internals:

```cpp
#include "ClaireonModule.h"
#include "Tools/IClaireonTool.h"

// In your module's startup:
FClaireonModule& Claireon = FClaireonModule::Get();
Claireon.RegisterExternalTool(MakeShared<MyCustomTool>());
```

Tools registered before the server starts are automatically queued and flushed on startup.

## Development

### Building

Claireon builds as a standard Unreal Engine editor plugin. No special build steps are required beyond the usual:

```bash
# Generate project files (if using a .uproject)
UnrealBuildTool -projectfiles -project="YourProject.uproject" -game -engine

# Build
UnrealBuildTool YourProjectEditor Win64 Development -project="YourProject.uproject"
```

### Adding a New Tool

1. Create a class implementing `IClaireonTool` (see `Tools/IClaireonTool.h`)
2. Implement `GetName()`, `GetDescription()`, `GetInputSchema()`, `GetCategory()`, and `Execute()`
3. Register it in `ClaireonModule.cpp`'s `StartServer()` method

```cpp
class MyTool : public IClaireonTool
{
public:
    FString GetName() const override { return TEXT("editor.my_tool"); }
    FString GetDescription() const override { return TEXT("Does something useful"); }
    FString GetCategory() const override { return TEXT("custom"); }

    TSharedPtr<FJsonObject> GetInputSchema() const override
    {
        auto Schema = MakeShared<FJsonObject>();
        Schema->SetStringField(TEXT("type"), TEXT("object"));
        // ... define properties
        return Schema;
    }

    FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override
    {
        // Your implementation here
        auto Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("result"), TEXT("success"));
        return MakeSuccessResult(Data, TEXT("Tool completed successfully"));
    }
};
```

## Scripts

The `Scripts/` directory contains PowerShell utilities and Claude Code instruction documents for working with Unreal Engine projects.

### Utility Scripts (`Scripts/Utilities/`)

PowerShell scripts for common Unreal Engine development tasks. All scripts auto-detect the project and engine location — run them from anywhere inside your project directory.

| Script | Purpose |
|--------|---------|
| `Invoke-EditorBuild.ps1` | Build the editor (auto-detects target from .uproject) |
| `Invoke-GenerateProjectFiles.ps1` | Generate VS project files and clang compilation database |
| `Invoke-ValidateAssets.ps1` | Validate asset integrity via ResavePackages |
| `Invoke-ResaveAssets.ps1` | Resave specific assets via editor Python automation |
| `Invoke-FixupRedirectors.ps1` | Fix up asset redirectors after moves/renames |
| `Invoke-CleanProject.ps1` | Clean build artifacts (Binaries, Intermediate, etc.) |
| `Get-GitUsername.ps1` | Extract git username from email for branch naming |
| `Wait-UBT.ps1` | Wait for UnrealBuildTool to finish before proceeding |

The shared module `ClaireonCommon.psm1` provides core functions used by all scripts: `Find-UnrealProject`, `Find-UnrealEngine` (SHA-aware custom engine detection), `Get-EditorBuildTarget`, `Invoke-UnrealCommandlet`, and more.

### Instruction Documents (`Scripts/Instructions/`)

Markdown documents that serve as structured prompts for AI assistants (e.g., Claude Code). Each document describes a multi-step workflow that an AI agent can follow autonomously. Invoke them by asking your AI assistant to follow the instructions in a specific file.

**Git Workflows:**
- `AutoPushChangesAsNewBranch.md` — Move local changes to a new branch with standardized naming
- `CleanupBranchesAfterMerge.md` — Rebase all local branches onto main after merges
- `SynchronizeWithSharedWork.md` — Integrate work from other worktrees
- `ResetWorktreeToMain.md` — Park current branch and reset to latest main

**Unreal Engine Workflows:**
- `ConnectToUnrealEditorMCP.md` — Connect Claude Code to the MCP server in the editor
- `FixDeprecatedBlueprintAPIs.md` — Parse editor logs for deprecated API warnings
- `FixNaniteMaterialCompatibility.md` — Identify Nanite/material incompatibility issues
- `FixDuplicateGameplayCueTags.md` — Find duplicate GameplayCue tag registrations
- `ResaveAssetsWithLoadErrors.md` — Resave assets with broken references
- `UnrealPythonPatterns.md` — Quick reference for correct Unreal Python API patterns

**Development Workflows:**
- `BreakDownWorkProposal.md` — Break a proposal into skeleton-test-implement stages
- `CheckDocumentationStaleness.md` — Scan HTML docs for staleness vs. source files
- `VerifyTestBehavior.md` — Walk through test cases and verify intended behavior

**Documentation Generation:**
- `CreateInteractiveArchitecture.md` — Generate interactive SVG architecture diagrams
- `CreateInteractivePipelineDiagram.md` — Generate interactive pipeline stage diagrams

## Security Considerations

Claireon runs an **unauthenticated HTTP server on localhost** (`127.0.0.1`). This is by design for seamless local AI assistant integration, but you should be aware of the implications:

- **Never expose port 8017 externally.** Do not use port forwarding, reverse proxies, or tunnels to make the server accessible from other machines.
- **Python execution is unrestricted.** The `execute_python_script` tool has full access to the filesystem, network, and editor APIs — equivalent to the editor's built-in Python console.
- **Execution timeout is advisory only.** The `timeout_seconds` parameter is not currently enforced at the process level.
- **No authentication.** Any process on localhost can call the MCP server.

For full details, see [SECURITY.md](SECURITY.md).

## License

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.

Third-party dependencies (e.g., `sqlite-vec`) retain their own licenses. See `Claireon/Binaries/ThirdParty/` for details.
