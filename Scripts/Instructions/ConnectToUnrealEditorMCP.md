Do not use instructions from this file unless asked.

# Connect to Unreal Editor via MCP

This instruction describes how to connect to a running Unreal Editor instance using the MCP (Model Context Protocol) server, and how to use the available tools to query assets, search the project, and execute Python scripts inside the editor.

## Overview

The Unreal Editor can host an MCP server that exposes editor functionality over HTTP. Once connected, you can:
- Search for assets by name, class, or path (like Ctrl+P in the editor)
- Query asset dependency and referencer hierarchies
- Execute arbitrary Python scripts with full access to the `unreal` module

## Prerequisites

- The project must be built (editor binaries exist)
- Windows with PowerShell 5.1+
- The `.mcp.json` file must be configured (already present at project root)

## Step 1: Check if the Editor is Already Running with MCP

Before launching a new editor, check whether one is already running with MCP enabled by looking for the port file:

```powershell
$ProjectDir = Get-Location
$PortFile = Join-Path $ProjectDir "Saved\MCPServer.json"

if (Test-Path $PortFile) {
    $PortInfo = Get-Content $PortFile -Raw | ConvertFrom-Json
    Write-Output "MCP server already running on port $($PortInfo.port) (PID: $($PortInfo.pid))"

    # Verify the process is still alive
    $Process = Get-Process -Id $PortInfo.pid -ErrorAction SilentlyContinue
    if ($null -eq $Process) {
        Write-Output "Stale port file (process not running). Cleaning up."
        Remove-Item $PortFile -Force
    }
}
```

If the port file exists and the process is alive, skip to Step 3.

## Step 2: Launch the Editor with MCP Enabled

If no editor is running with MCP, launch one. Use the project's existing build/launch infrastructure:

```powershell
# Import common utilities
Import-Module (Join-Path $ProjectDir "Scripts\Utilities\ClaireonCommon.psm1") -Force

# Find the engine
$Engine = Find-UnrealEngine -ProjectPath $ProjectPath

# Launch with MCP server enabled
$LaunchArgs = @(
    "`"$ProjectPath`"",
    "-StartMCPServer",
    "-MCPServerPort=8017",
    "-log"
)

$EditorProcess = Start-Process -FilePath $Engine.EditorExe -ArgumentList $LaunchArgs -PassThru
Write-Output "Editor launched (PID: $($EditorProcess.Id)). Waiting for MCP server..."
```

Then wait for the MCP server to become ready (the port file appears):

```powershell
$PortFile = Join-Path $ProjectDir "Saved\MCPServer.json"
$Timeout = 180  # seconds
$Elapsed = 0

while ($Elapsed -lt $Timeout) {
    if (Test-Path $PortFile) {
        $PortInfo = Get-Content $PortFile -Raw | ConvertFrom-Json
        Write-Output "MCP server ready on port $($PortInfo.port)"
        break
    }
    Start-Sleep -Seconds 3
    $Elapsed += 3
}
```

**Important**: The editor takes 1-3 minutes to fully start. The MCP server begins accepting connections only after the editor has initialized. Do not send requests before the port file appears.

**Note for Claude Code**: The Bash tool runs commands through bash, not PowerShell directly. PowerShell `$` variables will be consumed by bash before reaching PowerShell. To avoid this, write the launch script to a temporary `.ps1` file (e.g. `Saved/launch_mcp.ps1`) and execute it with `powershell -NoProfile -ExecutionPolicy Bypass -File <path>`. Run this as a background task since the script blocks while waiting for the MCP server.

## Step 3: Reconnect Claude Code to the MCP Server

If the editor was launched after Claude Code started, the MCP tools will not be available yet. Claude Code attempts to connect to MCP servers at startup, and does not automatically retry if the server was unavailable.

Run the `/mcp` slash command in Claude Code to reconnect. You should see output like:

```
Reconnected to unreal-editor.
```

If the tools are still not appearing after `/mcp`, check the troubleshooting section below.

## Step 4: Use MCP Tools

Once the MCP server is running and Claude Code is connected, the tools are available as `mcp__unreal-editor__<tool_name>` (configured via the project's `.mcp.json`).

If the tools are not appearing, verify the `.mcp.json` at the project root contains:

```json
{
  "mcpServers": {
    "unreal-editor": {
      "type": "http",
      "url": "http://127.0.0.1:8017/mcp"
    }
  }
}
```

If the server is on a different port (e.g. because 8017 was busy), update the URL to match the port in `Saved/MCPServer.json`.

---

## Available MCP Tools

### Tool 1: `search_assets`

Search for assets in the Unreal project by name. Works like the editor's Ctrl+P dialog. Results are ranked by match quality (exact > prefix > contains > path contains).

**MCP tool name**: `mcp__unreal-editor__search_assets`

**Parameters**:

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `query` | string | Yes | — | Search string to match against asset names |
| `class_filter` | string | No | — | Restrict to a class: `Blueprint`, `StaticMesh`, `Material`, `SkeletalMesh`, `World`, `DataAsset`, `Texture2D`, etc. |
| `path_filter` | string | No | — | Restrict to assets under a path, e.g. `/Game/Characters` |
| `max_results` | integer | No | 50 | Maximum results (1-500) |

**Example uses**:

Find a level/map by name:
```json
{ "query": "TestMap", "class_filter": "World" }
```

Find all blueprints related to targeting:
```json
{ "query": "Targeting", "class_filter": "Blueprint", "max_results": 20 }
```

Find assets under a specific folder:
```json
{ "query": "Player", "path_filter": "/Game/Characters" }
```

**Output format**: Numbered list of matching assets with package path, class name, and disk size.

```
Found 3 assets matching "TestMap":

1. /Game/Maps/TestMap/TestMap_Main [World] (45.2 MB)
2. /Game/Maps/TestMap/TestMap_Lighting [World] (12.1 MB)
3. /Game/Data/TestMap_Config [DataAsset] (2.3 KB)
```

### Tool 2: `get_asset_references`

Query the full dependency hierarchy for an asset — both what it depends on (forward references) and what depends on it (reverse references / referencers).

**MCP tool name**: `mcp__unreal-editor__get_asset_references`

**Parameters**:

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `asset_path` | string | Yes | — | Unreal content path, e.g. `/Game/Maps/TestMap/TestMap_Main` |
| `include_soft_references` | boolean | No | true | Include soft references (TSoftObjectPtr, etc.) in addition to hard references |
| `recursive` | boolean | No | false | Recursively follow dependencies (builds full tree) |
| `depth_limit` | integer | No | 5 | Max recursion depth when `recursive=true` (1-20) |

**Example uses**:

Get immediate references for a level:
```json
{ "asset_path": "/Game/Maps/TestMap/TestMap_Main" }
```

Get the full recursive dependency tree (what this asset pulls in):
```json
{ "asset_path": "/Game/Maps/TestMap/TestMap_Main", "recursive": true, "depth_limit": 3 }
```

Get only hard references (no soft/lazy references):
```json
{ "asset_path": "/Game/Maps/TestMap/TestMap_Main", "include_soft_references": false }
```

**Output format**: Two sections — dependencies and referencers — each listing package path, asset class, and disk size.

```
Asset: /Game/Maps/TestMap/TestMap_Main
==========

== Dependencies (what this asset uses) ==
  /Game/Characters/Player/BP_PlayerCharacter [Blueprint] (1.2 MB)
  /Game/Environment/Meshes/SM_Wall [StaticMesh] (8.4 MB)
  ...

Total dependencies: 47

== Referencers (what uses this asset) ==
  /Game/Maps/TestMap/TestMap_Persistent [World] (2.1 MB)

Total referencers: 1
```

**Important notes on asset paths**:
- Asset paths always start with `/Game/` for project content
- Use `search_assets` first if you don't know the exact path
- Engine/plugin content uses `/Engine/` or `/PluginName/` prefixes
- The tool validates that the asset exists in the registry before querying

### Tool 3: `execute_python_script`

Execute a Python script inside the running editor's embedded Python environment. The script has full access to the `unreal` module and all editor Python APIs.

**MCP tool name**: `mcp__unreal-editor__execute_python_script`

**Parameters**:

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `script` | string | Yes | — | Python source code to execute |
| `timeout_seconds` | integer | No | 30 | Advisory timeout (not enforced in v1) |

**Constraints**:
- Maximum script size: 64 KB
- Scripts run on the game thread (blocks the editor briefly)
- Full `unreal` module access: `unreal.EditorAssetLibrary`, `unreal.EditorLevelLibrary`, etc.
- Output from `print()` and `unreal.log()` is captured and returned
- Python exceptions are fully surfaced in the response

**Example uses**:

List assets in a directory:
```json
{
  "script": "import unreal\nassets = unreal.EditorAssetLibrary.list_assets('/Game/Characters', recursive=True)\nfor a in assets[:20]:\n    print(a)"
}
```

Get properties from a loaded asset:
```json
{
  "script": "import unreal\nasset = unreal.load_asset('/Game/Data/DT_WeaponStats')\nprint(type(asset))\nprint(asset.get_name())"
}
```

Query actors in the current level:
```json
{
  "script": "import unreal\nactors = unreal.EditorLevelLibrary.get_all_level_actors()\nfor a in actors[:10]:\n    print(f'{a.get_name()} ({a.get_class().get_name()})')"
}
```

Run a bulk operation:
```json
{
  "script": "import unreal\nasset_reg = unreal.AssetRegistryHelpers.get_asset_registry()\nfilter = unreal.ARFilter()\nfilter.class_paths = [unreal.TopLevelAssetPath('/Script/Engine', 'World')]\nassets = asset_reg.get_assets(filter)\nfor a in assets:\n    print(a.package_name)"
}
```

**Output format**: Captured stdout/stderr as plain text. Errors include the full Python traceback.

```
/Game/Characters/Player/BP_PlayerCharacter
/Game/Characters/Player/ABP_PlayerCharacter
/Game/Characters/Enemies/BP_EnemyBase
...
```

---

## Common Workflows

### Find an Asset and Get Its References

1. Search for the asset by name:
   - Use `search_assets` with the name or a keyword
2. Get the full path from the search results
3. Query references with `get_asset_references` using that path

**Example**: "Find the TestMap level and get its reference graph"

```
Step 1: search_assets(query="TestMap", class_filter="World")
  -> /Game/Maps/TestMap/TestMap_Main

Step 2: get_asset_references(asset_path="/Game/Maps/TestMap/TestMap_Main")
  -> Lists all dependencies and referencers

Step 3 (optional): get_asset_references(asset_path="/Game/Maps/TestMap/TestMap_Main", recursive=true, depth_limit=2)
  -> Full recursive dependency tree
```

### Explore What Depends on an Asset

Use `get_asset_references` and look at the "Referencers" section to see what assets reference a given asset. This is useful for:
- Understanding the impact of changing or deleting an asset
- Finding all levels that use a particular blueprint
- Tracing how a material or texture is used across the project

### Run Complex Queries with Python

When `search_assets` or `get_asset_references` are not flexible enough, use `execute_python_script` to run arbitrary queries against the editor's API:

```python
import unreal

# Find all blueprints that inherit from a specific class
registry = unreal.AssetRegistryHelpers.get_asset_registry()
filter = unreal.ARFilter()
filter.class_paths = [unreal.TopLevelAssetPath('/Script/Engine', 'Blueprint')]
filter.recursive_paths = True
filter.package_paths = ['/Game/Characters']

assets = registry.get_assets(filter)
for asset in assets:
    print(f"{asset.asset_name} -> {asset.package_name}")
```

### Build a Reference Graph

To build a full graph of references for visualization or analysis:

1. Start with a root asset
2. Get its references (both dependencies and referencers)
3. For each referenced asset, recursively get its references
4. Use `recursive=true` with `get_asset_references` to get the dependency tree in one call

Or for custom graph building, use Python:

```python
import unreal

def get_deps(package_name):
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    deps = registry.get_dependencies(package_name)
    return [str(d) for d in deps]

root = '/Game/Maps/TestMap/TestMap_Main'
visited = set()
queue = [root]
edges = []

while queue:
    current = queue.pop(0)
    if current in visited:
        continue
    visited.add(current)
    deps = get_deps(current)
    for dep in deps:
        if dep.startswith('/Game/'):
            edges.append((current, dep))
            if dep not in visited:
                queue.append(dep)

print(f"Nodes: {len(visited)}")
print(f"Edges: {len(edges)}")
for src, dst in edges[:50]:
    print(f"  {src} -> {dst}")
```

## Troubleshooting

### MCP tools not appearing in Claude Code

- Verify `.mcp.json` exists at the project root with the correct URL
- Check that the editor is running with MCP enabled (look for `Saved/MCPServer.json`)
- If the port changed, update the URL in `.mcp.json` to match
- **If the editor was not running when Claude Code started**, the MCP connection attempt during startup will fail silently and the tools will not be available. Claude Code does not automatically retry failed MCP connections. Two workarounds:
  1. **Preferred**: Run the `/mcp` slash command in Claude Code to manually reconnect. This re-attempts the connection without restarting the session.
  2. Restart Claude Code after the editor and MCP server are confirmed running.
- If you edited `.mcp.json` itself (not just started the server), restart Claude Code to pick up the config change

### "Asset not found in registry"

- The asset path must be the **package path**, not the file path. Use `/Game/...` not `Content/...`
- Use `search_assets` to find the correct path
- The asset registry may not have finished scanning yet if the editor just started — wait a moment and retry

### Python script errors

- Python exceptions are returned in the response with full tracebacks — read them to debug
- The `unreal` module documentation is at: `Help > Python API Reference` in the editor
- Scripts run in an isolated scope — variables do not persist between calls
- Use `print()` for output; return values are not captured

### Editor startup is slow

- First launch after a build takes 2-3 minutes as shaders compile and the asset registry scans
- Subsequent launches with warm caches take ~60 seconds
- The MCP server starts accepting connections only after the editor's module initialization completes

### Port conflict

- If port 8017 is busy, the server auto-increments (8018, 8019, ...) up to 10 retries
- The actual port is always in `Saved/MCPServer.json`
- Use `-MCPServerPort=NNNN` when launching the editor to specify a different starting port
