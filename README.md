<p align="center">
  <h1 align="center">Claireon</h1>
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

Claireon is an Unreal Engine editor plugin that runs a [Model Context Protocol](https://modelcontextprotocol.io/) (MCP) server inside the editor, exposing hundreds of editor automation tools for AI-assisted development workflows behind a deliberately small MCP surface (`tool_search` + `python_execute`). It enables AI assistants like [Claude Code](https://docs.anthropic.com/en/docs/claude-code) to read, search, inspect, and edit Unreal assets directly.

## Features

- **Hybrid Tool Discovery** — `tool_search` ranks the full tool catalog with SQLite FTS5 + a vendored embedding model, so agents find the right tool without a giant tool list
- **Blueprint Editing** — Read and modify Blueprint graphs, properties, components, and connections
- **Animation Authoring** — Anim sequences, montages, notifies, blend spaces/aim offsets, and AnimBP graphs
- **State Tree Editing** — Inspect and edit State Tree assets, nodes, and runtime state
- **Behavior Tree & EQS** — Inspect and edit Behavior Trees, Blackboards, and Environment Query Systems
- **Widget Blueprint Editing** — Read and modify UMG widget hierarchies and animations
- **Niagara & PCG** — Inspect and edit Niagara particle systems and PCG graphs
- **Data Tables** — Full CRUD operations on Data Table assets with CSV/JSON import/export
- **Audio & MetaSounds** — Sound placement, attenuation, audio components, and MetaSound inspection
- **Sequences, Cameras & More** — Level Sequences (incl. actor rebinding), camera assets, chooser/proxy tables, gameplay tags, Enhanced Input, landscape/terrain
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

1. Clone this repository directly into your project's `Plugins/` folder (the plugin lives at
   the repository root): `git clone <repo-url> Plugins/Claireon`
2. Regenerate project files and build

```
YourProject/
  Plugins/
    Claireon/
      Claireon.uplugin
      Source/
      Content/
      ...
```

3. Enable the plugin in the editor under **Edit > Plugins > Claireon**

### Starting the Server

The MCP server can be started in two ways:

- **Command line**: Launch the editor with `-StartMCPServer` to auto-start on load
- **Toolbar button**: Click the **Claireon** button in the editor toolbar to open the Claireon panel, which also starts the server

The server binds a deterministic per-worktree port: SHA-256 of the canonicalized project root, folded into the ephemeral range `49152-65535`. Every checkout gets a stable, collision-free port with no configuration, and multiple worktrees on one machine never fight over a shared default. The live port and PID are written to `Saved/Claireon/MCPServer.json` on startup. `-MCPServerPort=<port>` remains as a developer escape hatch to force a specific port.

#### Optional MCP proxy

For workflows where the editor restarts often (rebuilds, crashes, live-coding reloads), an optional always-on proxy (`Content/Python/claireon_proxy.py`, run with UE's vendored Python) can front the editor: Claude Code stays connected to the worktree's deterministic port while editors come and go behind it. Opt in with `-EnableMCPProxy` on the command line, or via **Editor Preferences > Plugins > Claireon > MCP Proxy > Enable MCP Proxy** (off by default). When enabled, the proxy holds the worktree port, and the editor binds an ephemeral port and registers with the proxy; editor ingress is then gated by a per-session bearer token so the proxy is the sole entry point. A single proxy instance serves all worktrees on the machine (registration port `43017`). Even with the setting off, an editor that finds its port already held by a Claireon proxy auto-promotes into proxy-attached mode.

### Connecting Claude Code

Once the server is running, configure Claude Code to connect. Take the port from `Saved/Claireon/MCPServer.json` -- it is stable for a given checkout path, so the config can be committed to your project's `.mcp.json` once and forgotten:

```json
{
  "mcpServers": {
    "claireon": {
      "type": "http",
      "url": "http://127.0.0.1:<port>/mcp"
    }
  }
}
```

The MCP surface is intentionally two tools: `tool_search` to discover what is available, and
`python_execute` to run editor Python (where every catalog tool is callable as
`claireon.<tool_name>(...)`). Discover tools with:

```
> tool_search blueprint
> tool_search state tree
> tool_search(tool_name="bp_apply_delta")   # full detail for one tool
```

#### One-click launch from the editor

Once the plugin is loaded, open the **Claireon** panel (toolbar button, or **Window > General > Miscellaneous > Claireon**); its status strip exposes a **Claude Code** launch button (toggleable via the `Show Claude Code Button` setting). Clicking it:

1. Resolves the live MCP server port (from the in-process server, falling back to `Saved/Claireon/MCPServer.json`).
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
| [Untested](https://github.com/believer-oss/untested) | Unit test framework for Claireon's test suite | Optional plugin dependency |
| BlueprintAssist | Enhanced Blueprint editor integration | Optional plugin dependency |
| LyraGame | Lyra framework init state checking in PIE tools | Auto-detected via filesystem |

## Architecture

```
<repository root>/
  Source/Claireon/
    Public/
      ClaireonModule.h          # Module entry point, server lifecycle
      ClaireonServer.h          # HTTP server, JSON-RPC dispatch
      ClaireonSessionManager.h  # Per-asset exclusive locking
      IClaireonToolProvider.h   # Modular-feature interface for external tool providers
      Tools/
        IClaireonTool.h         # Tool interface — implement this to add tools
    Private/
      Tools/                    # Tool implementations (hundreds of operations)
      Tests/                    # Unit tests (requires Untested)
  Content/
    Python/                     # Multi-worktree MCP proxy (claireon_proxy.py)
    MCP/Instructions/           # Instruction documents served as MCP resources
  Resources/
    Models/                     # Vendored bge-small-en-v1.5-int8 embedding model (MIT)
```

### Registering External Tools

Other modules contribute tools without including server internals by implementing the
modular-feature provider interface:

```cpp
#include "IClaireonToolProvider.h"
#include "Tools/IClaireonTool.h"

class FMyToolProvider : public IClaireonToolProvider
{
public:
    TArray<TSharedPtr<IClaireonTool>> GetTools() const override;
    FName GetProviderName() const override { return TEXT("MyPlugin"); }
};

// In your module's StartupModule():
IModularFeatures::Get().RegisterModularFeature(IClaireonToolProvider::FeatureName, &MyProvider);
```

Providers are discovered at server start and dynamically as they register/unregister at
runtime. A provider can also extend bare-name resolution with project-specific module names
and class-prefix conventions via the optional `GetKnownModules()` / `GetClassPrefixMap()`
overrides. For one-off registrations, `FClaireonModule::Get().RegisterExternalTool()` also
works; tools registered before the server starts are queued and flushed on startup.

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
2. Implement `GetCategory()`, `GetOperation()`, `GetDescription()`, `GetInputSchema()`, and
   `Execute()` — the tool name is always `<category>_<operation>` (`GetName()` is sealed)
3. Register it with the built-in provider in `ClaireonModule.cpp` (or via your own
   `IClaireonToolProvider` if it lives in another module)

```cpp
class MyTool : public IClaireonTool
{
public:
    FString GetCategory() const override { return TEXT("custom"); }
    FString GetOperation() const override { return TEXT("my_tool"); }   // name: "custom_my_tool"
    FString GetDescription() const override { return TEXT("Does something useful"); }

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
- `ResetWorktreeToMain.md` — Park current branch and reset to latest main

**Unreal Engine Workflows:**
- `ConnectToUnrealEditorMCP.md` — Connect Claude Code to the MCP server in the editor
- `FixDeprecatedBlueprintAPIs.md` — Parse editor logs for deprecated API warnings
- `FixNaniteMaterialCompatibility.md` — Identify Nanite/material incompatibility issues
- `FixDuplicateGameplayCueTags.md` — Find duplicate GameplayCue tag registrations
- `ResaveAssetsWithLoadErrors.md` — Resave assets with broken references
- `UnrealPythonPatterns.md` — Quick reference for correct Unreal Python API patterns
- `PSD-to-UMG-Workflow.md` — Convert Photoshop PSD designs into UMG widget blueprints

**Development Workflows:**
- `BreakDownWorkProposal.md` — Break a proposal into skeleton-test-implement stages
- `CheckDocumentationStaleness.md` — Scan HTML docs for staleness vs. source files
- `VerifyTestBehavior.md` — Walk through test cases and verify intended behavior

**Documentation Generation:**
- `CreateInteractiveArchitecture.md` — Generate interactive SVG architecture diagrams
- `CreateInteractivePipelineDiagram.md` — Generate interactive pipeline stage diagrams

## Security Considerations

Claireon runs an **unauthenticated HTTP server on localhost** (`127.0.0.1`). This is by design for seamless local AI assistant integration, but you should be aware of the implications:

- **Never expose the MCP port externally.** Do not use port forwarding, reverse proxies, or tunnels to make the server (or the Claireon proxy, ports `43017` and `49152-65535`) accessible from other machines.
- **Python execution is unrestricted.** The `python_execute` tool has full access to the filesystem, network, and editor APIs — equivalent to the editor's built-in Python console.
- **Execution timeout is best-effort.** A watchdog injects a `TimeoutError` between Python bytecodes after `Python Execution Timeout` (default 60 s, settable in Editor Preferences); a blocking native call is not interrupted mid-call, and the error surfaces only when it returns to Python.
- **No authentication.** Any process on localhost can call the MCP server. (In proxy mode the editor itself only accepts proxy traffic carrying a per-session bearer token, but the proxy's own MCP endpoint is equally unauthenticated on localhost.)

For full details, see [SECURITY.md](SECURITY.md).

## License

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.

Third-party dependencies retain their own licenses. The vendored embedding model ships with its license at `Resources/Models/bge-small-en-v1.5-int8/LICENSE.txt`.
