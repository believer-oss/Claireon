#Requires -Version 5.1

<#
.SYNOPSIS
    Common utilities shared across all Claireon PowerShell scripts.

.DESCRIPTION
    Provides core shared functionality for Claireon utility scripts including
    Unreal project detection, custom engine detection, progress logging,
    commandlet execution, path conversion, and toast notifications.
#>

function Find-UnrealProject {
    <#
    .SYNOPSIS
        Locates a .uproject file by searching upward from the given path.

    .PARAMETER StartPath
        Path to start searching from. Defaults to current directory.

    .OUTPUTS
        System.String - Absolute path to the first .uproject file found
    #>
    param(
        [string]$StartPath = $PWD
    )

    $MaxDepth = 5
    $CurrentPath = Resolve-Path $StartPath

    for ($i = 0; $i -lt $MaxDepth; $i++) {
        $ProjectFiles = Get-ChildItem -Path $CurrentPath -Filter "*.uproject" -File -ErrorAction SilentlyContinue
        if ($ProjectFiles -and $ProjectFiles.Count -gt 0) {
            return $ProjectFiles[0].FullName
        }

        $ParentPath = Split-Path $CurrentPath -Parent
        if ($null -eq $ParentPath -or $ParentPath -eq $CurrentPath) {
            break
        }
        $CurrentPath = $ParentPath
    }

    throw ".uproject file not found. Please run from within an Unreal project directory or specify -ProjectPath."
}

function Write-StepProgress {
    <#
    .SYNOPSIS
        Writes consistent progress messages to console.

    .DESCRIPTION
        Outputs timestamped messages to the console with optional color formatting.

    .PARAMETER Message
        Message to display

    .PARAMETER Color
        Console color (default: Cyan)

    .EXAMPLE
        Write-StepProgress "Starting build..." -Color Green
    #>
    param(
        [Parameter(Mandatory=$true)]
        [string]$Message,

        [ConsoleColor]$Color = [ConsoleColor]::Cyan
    )

    $Timestamp = Get-Date -Format "HH:mm:ss"
    Write-Host "[$Timestamp] $Message" -ForegroundColor $Color
}

function Show-ToastNotification {
    <#
    .SYNOPSIS
        Displays a Windows toast notification.

    .DESCRIPTION
        Shows a native Windows 10/11 toast notification using the Windows.UI.Notifications API.
        Useful for alerting users when long-running scripts complete.

    .PARAMETER Title
        The notification title (first line of text)

    .PARAMETER Message
        The notification message (second line of text)

    .PARAMETER Sound
        Whether to play a notification sound (default: $true)

    .EXAMPLE
        Show-ToastNotification -Title "Build Complete" -Message "Editor built successfully"

    .NOTES
        Requires Windows 10 or later.
        Uses PowerShell as the app identifier for notifications.
    #>
    param(
        [Parameter(Mandatory=$true)]
        [string]$Title,

        [Parameter(Mandatory=$true)]
        [string]$Message,

        [bool]$Sound = $true
    )

    try {
        # Load Windows.UI.Notifications types
        [Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType = WindowsRuntime] | Out-Null
        [Windows.Data.Xml.Dom.XmlDocument, Windows.Data.Xml.Dom.XmlDocument, ContentType = WindowsRuntime] | Out-Null

        # PowerShell app identifier
        $AppId = '{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\WindowsPowerShell\v1.0\powershell.exe'

        # Build toast XML
        $SoundXml = if ($Sound) { '<audio src="ms-winsoundevent:Notification.Default"/>' } else { '<audio silent="true"/>' }

        $ToastXml = @"
<toast>
    <visual>
        <binding template="ToastText02">
            <text id="1">$([System.Security.SecurityElement]::Escape($Title))</text>
            <text id="2">$([System.Security.SecurityElement]::Escape($Message))</text>
        </binding>
    </visual>
    $SoundXml
</toast>
"@

        # Create and show notification
        $XmlDocument = [Windows.Data.Xml.Dom.XmlDocument]::new()
        $XmlDocument.LoadXml($ToastXml)
        $Toast = [Windows.UI.Notifications.ToastNotification]::new($XmlDocument)
        [Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier($AppId).Show($Toast)

        Write-Verbose "Toast notification displayed: $Title"
    }
    catch {
        Write-Warning "Failed to display toast notification: $($_.Exception.Message)"
        # Don't throw - notifications are non-critical
    }
}

function Get-EngineGitSha {
    <#
    .SYNOPSIS
        Returns the 8-char short git HEAD SHA for an engine directory.

    .PARAMETER EnginePath
        Root path of the Unreal Engine checkout.

    .OUTPUTS
        System.String - 8-char short SHA, or $null if not a git repo.
    #>
    param(
        [Parameter(Mandatory=$true)]
        [string]$EnginePath
    )

    $GitDir = Join-Path $EnginePath ".git"
    if (-not (Test-Path $GitDir)) {
        return $null
    }

    try {
        $Output = & git -C $EnginePath rev-parse --short=8 HEAD 2>$null
        if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($Output)) {
            return $Output.Trim()
        }
    } catch {
        # git not available or failed
    }

    return $null
}

function Test-IsEngineRoot {
    <#
    .SYNOPSIS
        Returns $true if the given path looks like an Unreal Engine root.

    .PARAMETER Path
        Directory to check.

    .OUTPUTS
        System.Boolean
    #>
    param(
        [Parameter(Mandatory=$true)]
        [string]$Path
    )

    $BuildVersion = Join-Path $Path "Engine\Build\Build.version"
    return (Test-Path -Path $BuildVersion -PathType Leaf)
}

function Find-UnrealEngine {
    <#
    .SYNOPSIS
        Detects the custom Unreal Engine location for the project.

    .DESCRIPTION
        Uses a multi-stage detection strategy:
        1. Parse EngineAssociation from .uproject to get prefix + target SHA
        2. Registry scan: check registered prefix-* paths, verify git SHA on disk
        3. Filesystem scan: check sibling directories of project root
        4. Auto-register: write registry entry for future fast lookup
        5. Legacy fallback: any valid registered prefix-* path (warns about mismatch)
        6. Validate that the engine is built (UnrealEditor.exe exists)

    .PARAMETER ProjectPath
        Path to the .uproject file.

    .OUTPUTS
        System.Collections.Hashtable - Contains EnginePath, EditorExe, EditorCmdExe,
        BuildBat, UnrealBuildTool, GenerateProjectFiles
    #>
    param(
        [Parameter(Mandatory=$true)]
        [string]$ProjectPath
    )

    if (-not (Test-Path -Path $ProjectPath -PathType Leaf)) {
        throw "Project file not found: $ProjectPath"
    }

    # --- 1. Parse EngineAssociation ---
    $UProject = Get-Content $ProjectPath -Raw | ConvertFrom-Json
    $EngineAssociation = $UProject.EngineAssociation

    if ([string]::IsNullOrWhiteSpace($EngineAssociation)) {
        throw ".uproject does not contain EngineAssociation field"
    }

    # Extract prefix and target SHA (e.g. "custom-fcb017c3" -> prefix="custom", sha="fcb017c3")
    $TargetSha = $null
    $Prefix = $EngineAssociation
    if ($EngineAssociation -match '^(.+)-([0-9a-f]{8})$') {
        $Prefix = $Matches[1]
        $TargetSha = $Matches[2]
    }

    Write-Verbose "Looking for engine: $EngineAssociation (prefix=$Prefix, sha=$TargetSha)"

    $RegPath = "HKCU:\Software\Epic Games\Unreal Engine\Builds"
    $EnginePath = $null

    # --- 2. Registry scan with SHA verification ---
    if (Test-Path $RegPath) {
        $EngineBuilds = Get-ItemProperty -Path $RegPath

        # Try exact registry key first
        if ($EngineBuilds.PSObject.Properties.Name -contains $EngineAssociation) {
            $CandidatePath = $EngineBuilds.$EngineAssociation
            if (-not [string]::IsNullOrWhiteSpace($CandidatePath) -and (Test-Path -Path $CandidatePath -PathType Container)) {
                if ($null -eq $TargetSha) {
                    # No SHA to verify, trust the registry
                    $EnginePath = $CandidatePath
                } else {
                    $DiskSha = Get-EngineGitSha -EnginePath $CandidatePath
                    if ($DiskSha -eq $TargetSha) {
                        $EnginePath = $CandidatePath
                        Write-Verbose "Registry exact match verified: SHA $DiskSha"
                    } else {
                        Write-Warning "Registry key '$EngineAssociation' exists but SHA mismatch (disk=$DiskSha, expected=$TargetSha)"
                    }
                }
            }
        }

        # Try other prefix-* registry entries
        if ($null -eq $EnginePath -and $null -ne $TargetSha) {
            $PrefixEngines = $EngineBuilds.PSObject.Properties |
                Where-Object { $_.Name -like "$Prefix-*" }

            foreach ($Candidate in $PrefixEngines) {
                $CandidatePath = $Candidate.Value
                if ([string]::IsNullOrWhiteSpace($CandidatePath) -or -not (Test-Path -Path $CandidatePath -PathType Container)) {
                    continue
                }
                $DiskSha = Get-EngineGitSha -EnginePath $CandidatePath
                if ($DiskSha -eq $TargetSha) {
                    $EnginePath = $CandidatePath
                    Write-Verbose "Registry prefix match: $($Candidate.Name) -> SHA $DiskSha verified"
                    break
                }
            }
        }
    }

    # --- 3. Filesystem scan: sibling directories of project root ---
    if ($null -eq $EnginePath -and $null -ne $TargetSha) {
        $ProjectDir = Split-Path $ProjectPath -Parent
        $ParentDir = Split-Path $ProjectDir -Parent

        Write-Verbose "Scanning sibling directories of $ParentDir for engine with SHA $TargetSha"

        if (Test-Path $ParentDir) {
            $SiblingDirs = Get-ChildItem -Path $ParentDir -Directory -ErrorAction SilentlyContinue
            foreach ($Dir in $SiblingDirs) {
                if (-not (Test-IsEngineRoot -Path $Dir.FullName)) {
                    continue
                }
                $DiskSha = Get-EngineGitSha -EnginePath $Dir.FullName
                if ($DiskSha -eq $TargetSha) {
                    $EnginePath = $Dir.FullName
                    Write-StepProgress "Engine found via filesystem scan: $EnginePath (SHA $DiskSha)" -Color Green

                    # --- 4. Auto-register for fast future lookup ---
                    try {
                        if (-not (Test-Path $RegPath)) {
                            New-Item -Path $RegPath -Force | Out-Null
                        }
                        New-ItemProperty -Path $RegPath -Name $EngineAssociation -Value $EnginePath -PropertyType String -Force | Out-Null
                        Write-StepProgress "Auto-registered engine as '$EngineAssociation' in registry" -Color Green
                    } catch {
                        Write-Warning "Failed to auto-register engine in registry: $($_.Exception.Message)"
                    }

                    break
                }
            }
        }
    }

    # --- 5. Legacy fallback: any valid registered prefix-* path ---
    if ($null -eq $EnginePath -and (Test-Path $RegPath)) {
        Write-Warning "No SHA-verified engine found for $EngineAssociation, trying legacy fallback..."

        $EngineBuilds = Get-ItemProperty -Path $RegPath
        $PrefixEngines = $EngineBuilds.PSObject.Properties |
            Where-Object { $_.Name -like "$Prefix-*" }

        foreach ($Candidate in $PrefixEngines) {
            $CandidatePath = $Candidate.Value
            if (-not [string]::IsNullOrWhiteSpace($CandidatePath) -and (Test-Path -Path $CandidatePath -PathType Container)) {
                $EnginePath = $CandidatePath
                $DiskSha = Get-EngineGitSha -EnginePath $CandidatePath
                Write-Warning "Using fallback engine: $($Candidate.Name) at $EnginePath (SHA on disk: $DiskSha, expected: $TargetSha)"
                break
            }
        }
    }

    if ($null -eq $EnginePath) {
        throw "No custom Unreal Engine found. Expected $EngineAssociation in registry or as sibling directory."
    }

    # --- 6. Validate engine is built ---
    $EditorExe = Join-Path $EnginePath "Engine\Binaries\Win64\UnrealEditor.exe"
    if (-not (Test-Path -Path $EditorExe -PathType Leaf)) {
        throw "UnrealEditor.exe not found at: $EditorExe. Engine at $EnginePath may not be built."
    }

    # Build and return full hashtable
    $EditorCmdExe = Join-Path $EnginePath "Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
    $BuildBat = Join-Path $EnginePath "Engine\Build\BatchFiles\Build.bat"
    $UnrealBuildTool = Join-Path $EnginePath "Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe"
    $GenerateProjectFiles = Join-Path $EnginePath "GenerateProjectFiles.bat"

    return @{
        EnginePath           = $EnginePath
        EditorExe            = $EditorExe
        EditorCmdExe         = $EditorCmdExe
        BuildBat             = $BuildBat
        UnrealBuildTool      = $UnrealBuildTool
        GenerateProjectFiles = $GenerateProjectFiles
    }
}

function Get-EditorBuildTarget {
    <#
    .SYNOPSIS
        Derives the editor build target name from a .uproject file path.

    .DESCRIPTION
        Extracts the project name from the .uproject filename and appends "Editor"
        (e.g., "MyGame.uproject" -> "MyGameEditor").

    .PARAMETER ProjectPath
        Path to the .uproject file.

    .OUTPUTS
        System.String - Editor build target name
    #>
    param(
        [Parameter(Mandatory=$true)]
        [string]$ProjectPath
    )

    $ProjectName = [System.IO.Path]::GetFileNameWithoutExtension($ProjectPath)
    return "${ProjectName}Editor"
}

function Get-MCPPort {
    <#
    .SYNOPSIS
        Computes a deterministic MCP server port from the worktree name.

    .DESCRIPTION
        Uses a hash of the worktree directory name to produce a port in the range
        8100-9099. This avoids the default MCP port (8017) and provides 1000 unique
        slots, making collisions unlikely.

    .PARAMETER WorktreeName
        Name of the worktree directory. Defaults to the current directory name.

    .OUTPUTS
        System.Int32 - Port number in the range 8100-9099
    #>
    param(
        [string]$WorktreeName
    )
    if ([string]::IsNullOrWhiteSpace($WorktreeName)) {
        $WorktreeName = (Split-Path -Leaf (Get-Location)).ToLower()
    }
    $hash = 0
    foreach ($char in $WorktreeName.ToCharArray()) {
        $hash = ($hash * 31 + [int]$char) -band 0x7FFFFFFF
    }
    return 8100 + ($hash % 1000)
}

function Test-PortAvailable {
    <#
    .SYNOPSIS
        Tests whether a TCP port is available for binding.

    .PARAMETER Port
        The port number to test.

    .OUTPUTS
        System.Boolean - $true if the port is free, $false if in use.
    #>
    param(
        [Parameter(Mandatory)]
        [int]$Port
    )
    try {
        $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $Port)
        $listener.Start()
        $listener.Stop()
        return $true
    } catch {
        return $false
    }
}

function Invoke-UnrealCommandlet {
    <#
    .SYNOPSIS
        Invokes an Unreal Engine commandlet.

    .PARAMETER EditorCmdExe
        Path to UnrealEditor-Cmd.exe

    .PARAMETER ProjectPath
        Path to the .uproject file

    .PARAMETER CommandletName
        Name of the commandlet to run (e.g., "ResavePackages")

    .PARAMETER CommandletArgs
        Additional arguments to pass to the commandlet

    .PARAMETER LogFile
        Optional path to write log output

    .PARAMETER TimeoutSeconds
        Timeout in seconds (default: 3600 = 1 hour)

    .OUTPUTS
        System.Int32 - Exit code (0 = success)
    #>
    param(
        [Parameter(Mandatory=$true)]
        [string]$EditorCmdExe,

        [Parameter(Mandatory=$true)]
        [string]$ProjectPath,

        [Parameter(Mandatory=$true)]
        [string]$CommandletName,

        [string[]]$CommandletArgs = @(),

        [string]$LogFile,

        [int]$TimeoutSeconds = 3600
    )

    if (-not (Test-Path $EditorCmdExe)) {
        throw "UnrealEditor-Cmd.exe not found: $EditorCmdExe"
    }

    $AllArgs = @(
        "`"$ProjectPath`"",
        "-run=$CommandletName"
    ) + $CommandletArgs + @(
        "-unattended",
        "-nopause",
        "-nosplash",
        "-stdout"
    )

    if (-not [string]::IsNullOrWhiteSpace($LogFile)) {
        $AllArgs += "-log=`"$LogFile`""
    }

    Write-Verbose "Executing: $EditorCmdExe $($AllArgs -join ' ')"

    try {
        $Process = Start-Process -FilePath $EditorCmdExe -ArgumentList $AllArgs `
            -Wait -PassThru -NoNewWindow

        return $Process.ExitCode
    } catch {
        Write-Error "Commandlet execution failed: $($_.Exception.Message)"
        return 1
    }
}

function Invoke-UnrealEditorPython {
    <#
    .SYNOPSIS
        Runs a Python script in the full Unreal Editor.

    .DESCRIPTION
        Launches the full Unreal Editor (not commandlet mode) and executes a Python script.
        This ensures proper engine initialization including correct version handling.

    .PARAMETER EditorExe
        Path to UnrealEditor.exe

    .PARAMETER ProjectPath
        Path to the .uproject file

    .PARAMETER PythonScript
        Path to the Python script to execute

    .PARAMETER TimeoutSeconds
        Timeout in seconds (default: 3600 = 1 hour)

    .OUTPUTS
        System.Int32 - Exit code (0 = success)
    #>
    param(
        [Parameter(Mandatory=$true)]
        [string]$EditorExe,

        [Parameter(Mandatory=$true)]
        [string]$ProjectPath,

        [Parameter(Mandatory=$true)]
        [string]$PythonScript,

        [int]$TimeoutSeconds = 3600
    )

    if (-not (Test-Path $EditorExe)) {
        throw "UnrealEditor.exe not found: $EditorExe"
    }

    if (-not (Test-Path $PythonScript)) {
        throw "Python script not found: $PythonScript"
    }

    $AllArgs = @(
        "`"$ProjectPath`"",
        "-ExecutePythonScript=`"$PythonScript`"",
        "-unattended",
        "-nopause",
        "-nosplash",
        "-stdout"
    )

    Write-Verbose "Executing: $EditorExe $($AllArgs -join ' ')"

    try {
        $Process = Start-Process -FilePath $EditorExe -ArgumentList $AllArgs `
            -Wait -PassThru -NoNewWindow

        return $Process.ExitCode
    } catch {
        Write-Error "Editor execution failed: $($_.Exception.Message)"
        return 1
    }
}

function ConvertTo-UnrealPath {
    <#
    .SYNOPSIS
        Converts a Windows file path to an Unreal content path.

    .PARAMETER FilePath
        Windows file path (e.g., "D:\project\Content\Characters\Player.uasset")

    .PARAMETER ProjectPath
        Path to the .uproject file

    .OUTPUTS
        System.String - Unreal content path (e.g., "/Game/Characters/Player")
    #>
    param(
        [Parameter(Mandatory=$true)]
        [string]$FilePath,

        [Parameter(Mandatory=$true)]
        [string]$ProjectPath
    )

    $ProjectDir = Split-Path $ProjectPath -Parent
    $ContentDir = Join-Path $ProjectDir "Content"

    $NormalizedFilePath = $FilePath.Replace("/", "\")
    $NormalizedContentDir = $ContentDir.Replace("/", "\")

    if ($NormalizedFilePath.StartsWith($NormalizedContentDir, [System.StringComparison]::OrdinalIgnoreCase)) {
        $RelativePath = $NormalizedFilePath.Substring($NormalizedContentDir.Length).TrimStart("\")
        $RelativePath = $RelativePath -replace "\.uasset$", "" -replace "\.umap$", ""
        $RelativePath = $RelativePath.Replace("\", "/")
        return "/Game/$RelativePath"
    }

    # Check for plugin content
    $PluginsDir = Join-Path $ProjectDir "Plugins"
    $NormalizedPluginsDir = $PluginsDir.Replace("/", "\")

    if ($NormalizedFilePath.StartsWith($NormalizedPluginsDir, [System.StringComparison]::OrdinalIgnoreCase)) {
        $RelativeToPlugins = $NormalizedFilePath.Substring($NormalizedPluginsDir.Length).TrimStart("\")
        $Parts = $RelativeToPlugins -split "\\"
        if ($Parts.Length -ge 2 -and $Parts[1] -eq "Content") {
            $PluginName = $Parts[0]
            $ContentRelative = ($Parts | Select-Object -Skip 2) -join "/"
            $ContentRelative = $ContentRelative -replace "\.uasset$", "" -replace "\.umap$", ""
            return "/$PluginName/$ContentRelative"
        }
    }

    throw "Unable to convert path to Unreal content path: $FilePath"
}

function ConvertTo-WindowsPath {
    <#
    .SYNOPSIS
        Converts an Unreal content path to a Windows file path.

    .PARAMETER UnrealPath
        Unreal content path (e.g., "/Game/Characters/Player")

    .PARAMETER ProjectPath
        Path to the .uproject file

    .PARAMETER Extension
        File extension to append (default: ".uasset")

    .OUTPUTS
        System.String - Windows file path
    #>
    param(
        [Parameter(Mandatory=$true)]
        [string]$UnrealPath,

        [Parameter(Mandatory=$true)]
        [string]$ProjectPath,

        [string]$Extension = ".uasset"
    )

    $ProjectDir = Split-Path $ProjectPath -Parent

    if ($UnrealPath.StartsWith("/Game/")) {
        $RelativePath = $UnrealPath.Substring(6)  # Remove "/Game/"
        $ContentDir = Join-Path $ProjectDir "Content"
        return Join-Path $ContentDir ($RelativePath.Replace("/", "\") + $Extension)
    }

    # Plugin content (e.g., "/PluginName/...")
    if ($UnrealPath.StartsWith("/")) {
        $Parts = $UnrealPath.TrimStart("/").Split("/")
        if ($Parts.Length -ge 1) {
            $PluginName = $Parts[0]
            $RelativePath = ($Parts | Select-Object -Skip 1) -join "\"
            $PluginContentDir = Join-Path $ProjectDir "Plugins\$PluginName\Content"
            return Join-Path $PluginContentDir ($RelativePath + $Extension)
        }
    }

    throw "Unable to convert Unreal path to Windows path: $UnrealPath"
}

# Export module members
Export-ModuleMember -Function `
    Find-UnrealProject, `
    Write-StepProgress, `
    Show-ToastNotification, `
    Get-EngineGitSha, `
    Test-IsEngineRoot, `
    Find-UnrealEngine, `
    Get-EditorBuildTarget, `
    Get-MCPPort, `
    Test-PortAvailable, `
    Invoke-UnrealCommandlet, `
    Invoke-UnrealEditorPython, `
    ConvertTo-UnrealPath, `
    ConvertTo-WindowsPath
