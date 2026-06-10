<#
.SYNOPSIS
    Starts (or attaches to) the Claireon MCP proxy singleton -- no editor required.

.DESCRIPTION
    Spawns the always-on Claireon proxy (Content/Python/claireon_proxy.py) as a
    detached process and binds the given worktree's deterministic MCP port via
    /admin/ensure_worktree, so MCP clients can connect without launching the
    Unreal editor. Useful for testing the proxy itself and for serving the
    editor-less MCP surface (the proxy meta-tool and file-backed prompts such
    as `workflow`; tool_search/python_execute still require a running editor).

    The proxy is a per-machine singleton (registration port 43017 is the
    lock). If one is already running, this script attaches to it instead of
    spawning a second copy, then binds the worktree as usual. When an editor
    is launched later for the same worktree, it auto-promotes behind the proxy
    and the same port keeps serving -- no client reconfiguration.

    Python resolution order:
      1. -PythonExe parameter
      2. Unreal's vendored Python (Engine/Binaries/ThirdParty/Python3/Win64),
         when an engine can be located via ClaireonCommon.psm1
      3. python.exe on PATH

.PARAMETER WorktreeRoot
    Worktree/project root to bind. Defaults to the current directory.

.PARAMETER PythonExe
    Explicit path to a Python 3 interpreter. Overrides auto-detection.

.OUTPUTS
    Exit code 0 on success; non-zero on failure. Prints the bound MCP port
    and a ready-to-paste .mcp.json snippet.

.EXAMPLE
    .\Start-MCPProxy.ps1

.EXAMPLE
    .\Start-MCPProxy.ps1 -WorktreeRoot D:\git\MyProject
#>
[CmdletBinding()]
param(
    [string]$WorktreeRoot,
    [string]$PythonExe
)

$ErrorActionPreference = 'Stop'
$ProxyRegPort = 43017

function Write-Progress2 {
    param([string]$Message, [string]$Color = 'White')
    Write-Host "[Start-MCPProxy] $Message" -ForegroundColor $Color
}

# =============================================================================
# STEP 0: resolve worktree root and the proxy script
# =============================================================================

if ([string]::IsNullOrWhiteSpace($WorktreeRoot)) {
    $WorktreeRoot = (Get-Location).Path
}
$WorktreeRoot = [System.IO.Path]::GetFullPath($WorktreeRoot)

# The script ships next to the plugin: Scripts/Utilities -> <plugin root> ->
# Content/Python. The same relative path holds whether the plugin is a repo
# root checkout or vendored at Plugins/Claireon.
$ProxyScript = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\Content\Python\claireon_proxy.py'))
if (-not (Test-Path -Path $ProxyScript -PathType Leaf)) {
    Write-Progress2 "claireon_proxy.py not found at $ProxyScript" -Color Red
    exit 1
}

# =============================================================================
# STEP 1: resolve a Python interpreter
# =============================================================================

if ([string]::IsNullOrWhiteSpace($PythonExe)) {
    # Prefer Unreal's vendored Python when an engine is locatable.
    $commonModule = Join-Path $PSScriptRoot 'ClaireonCommon.psm1'
    if (Test-Path -Path $commonModule -PathType Leaf) {
        try {
            Import-Module $commonModule -Force -ErrorAction Stop
            $project = $null
            try { $project = Find-UnrealProject -StartPath $WorktreeRoot } catch {}
            if ($null -ne $project) {
                $engine = Find-UnrealEngine -ProjectPath $project
                if ($null -ne $engine -and -not [string]::IsNullOrWhiteSpace($engine.EnginePath)) {
                    $candidate = Join-Path $engine.EnginePath 'Engine\Binaries\ThirdParty\Python3\Win64\python.exe'
                    if (Test-Path -Path $candidate -PathType Leaf) {
                        $PythonExe = $candidate
                    }
                }
            }
        } catch {
            Write-Progress2 "Engine python detection skipped: $($_.Exception.Message)" -Color Yellow
        }
    }
}
if ([string]::IsNullOrWhiteSpace($PythonExe)) {
    $cmd = Get-Command python -ErrorAction SilentlyContinue
    if ($null -ne $cmd) { $PythonExe = $cmd.Source }
}
if ([string]::IsNullOrWhiteSpace($PythonExe) -or -not (Test-Path -Path $PythonExe -PathType Leaf)) {
    Write-Progress2 'No Python interpreter found. Pass -PythonExe (any Python 3; the proxy is stdlib-only).' -Color Red
    exit 2
}
Write-Progress2 "Python: $PythonExe"

# =============================================================================
# STEP 2: spawn the singleton if 43017 is not already listening
# =============================================================================

function Test-ProxyRegPortListening {
    param([int]$TimeoutMs = 500)
    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $iar = $client.BeginConnect('127.0.0.1', $ProxyRegPort, $null, $null)
        $ok = $iar.AsyncWaitHandle.WaitOne([TimeSpan]::FromMilliseconds($TimeoutMs))
        if ($ok -and $client.Connected) {
            $client.EndConnect($iar)
            return $true
        }
        return $false
    } catch {
        return $false
    } finally {
        $client.Close()
    }
}

if (Test-ProxyRegPortListening) {
    Write-Progress2 "Proxy already listening on 127.0.0.1:$ProxyRegPort -- attaching." -Color Green
} else {
    Write-Progress2 "Spawning proxy singleton (detached): $PythonExe $ProxyScript" -Color Cyan
    # cwd = the script's own directory so no handle pins the worktree root.
    Start-Process `
        -FilePath $PythonExe `
        -ArgumentList @("`"$ProxyScript`"") `
        -WorkingDirectory (Split-Path $ProxyScript -Parent) `
        -WindowStyle Hidden | Out-Null

    $deadline = (Get-Date).AddSeconds(5)
    while ((Get-Date) -lt $deadline) {
        if (Test-ProxyRegPortListening -TimeoutMs 200) { break }
        Start-Sleep -Milliseconds 200
    }
    if (-not (Test-ProxyRegPortListening -TimeoutMs 200)) {
        Write-Progress2 "Proxy did not start listening on $ProxyRegPort within 5s. Check %LOCALAPPDATA% proxy.log via: python `"$ProxyScript`" --log-level DEBUG (foreground)." -Color Red
        exit 3
    }
}

$health = Invoke-RestMethod -Uri "http://127.0.0.1:$ProxyRegPort/admin/health" -Method Get -TimeoutSec 5
$shortVersion = $health.version_hash
if ($null -ne $shortVersion -and $shortVersion.Length -gt 8) { $shortVersion = $shortVersion.Substring(0, 8) }
Write-Progress2 "Proxy healthy (pid=$($health.pid) version=$shortVersion)" -Color Green

# =============================================================================
# STEP 3: bind this worktree's deterministic port
# =============================================================================

$ResolvedPort = $null
try {
    $body = @{ worktree_root = $WorktreeRoot } | ConvertTo-Json -Compress
    $ensure = Invoke-RestMethod -Uri "http://127.0.0.1:$ProxyRegPort/admin/ensure_worktree" `
        -Method Post -ContentType 'application/json' -Body $body -TimeoutSec 10
    if ($null -ne $ensure.mcp_port) {
        $ResolvedPort = [int]$ensure.mcp_port
        Write-Progress2 "Worktree bound: $WorktreeRoot -> port $ResolvedPort" -Color Green
    }
} catch {
    $resp = $_.Exception.Response
    $errJson = $null
    if ($null -ne $resp) {
        try {
            $reader = New-Object System.IO.StreamReader($resp.GetResponseStream())
            $errJson = $reader.ReadToEnd() | ConvertFrom-Json
        } catch {}
    }
    if ($null -ne $errJson -and $errJson.reason -eq 'port_held_by_editor') {
        # A direct-connect editor already owns the port; clients use it as-is.
        $ResolvedPort = [int]$errJson.port
        Write-Progress2 "Port $ResolvedPort is held by a direct-connect editor (pid $($errJson.editor_pid)); clients connect to it directly." -Color Yellow
    } else {
        if ($null -ne $errJson) {
            Write-Progress2 "ensure_worktree refused: reason=$($errJson.reason)" -Color Red
        } else {
            Write-Progress2 "ensure_worktree failed: $($_.Exception.Message)" -Color Red
        }
        exit 4
    }
}
if ($null -eq $ResolvedPort) {
    Write-Progress2 'ensure_worktree returned no port.' -Color Red
    exit 5
}

# =============================================================================
# STEP 4: verify /mcp answers, then print connection info
# =============================================================================

$initBody = '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}'
$probeOk = $false
$probeDeadline = (Get-Date).AddSeconds(5)
while ((Get-Date) -lt $probeDeadline) {
    try {
        $init = Invoke-RestMethod -Uri "http://127.0.0.1:$ResolvedPort/mcp" `
            -Method Post -ContentType 'application/json' -Body $initBody -TimeoutSec 2
        if ($null -ne $init.result) { $probeOk = $true; break }
    } catch {}
    Start-Sleep -Milliseconds 250
}
if ($probeOk) {
    Write-Progress2 "MCP initialize OK on port $ResolvedPort (serverInfo: $($init.result.serverInfo.name))" -Color Green
} else {
    Write-Progress2 "MCP did not answer on port $ResolvedPort within 5s." -Color Red
    exit 6
}

Write-Host ''
Write-Host 'Connect an MCP client (e.g. .mcp.json):'
Write-Host @"
{
  "mcpServers": {
    "claireon": {
      "type": "http",
      "url": "http://127.0.0.1:$ResolvedPort/mcp"
    }
  }
}
"@
Write-Host 'Editor-less surface: proxy meta-tool (status/launch_editor/read_log/...) and file-backed'
Write-Host 'prompts (prompts/list, prompts/get). tool_search and python_execute need a running editor;'
Write-Host "start one later with proxy(command='launch_editor') or Invoke-EditorBuildAndLaunch.ps1."
exit 0
