#Requires -Version 5.1

<#
.SYNOPSIS
    Builds the Unreal Editor for the detected project.

.DESCRIPTION
    Generates project files and compiles the editor target. Optionally
    cleans build artifacts before building. The build target name is derived
    automatically from the .uproject filename.

.PARAMETER ProjectPath
    Path to the .uproject file. Auto-detected if not provided.

.PARAMETER Configuration
    Build configuration: Development, DebugGame, or Shipping. Default: Development

.PARAMETER Clean
    Clean build artifacts before building. Calls Invoke-CleanProject.ps1 with
    -IncludePlugins to ensure a full rebuild.

.PARAMETER SkipGenerateProjectFiles
    Skip generating Visual Studio project files and compilation database.

.PARAMETER SkipWaitForUBT
    Skip waiting for UnrealBuildTool to be available before building.
    By default, the script waits for UBT to be free.

.OUTPUTS
    Exit code 0 for success, non-zero for failure

.EXAMPLE
    .\Invoke-EditorBuild.ps1

.EXAMPLE
    .\Invoke-EditorBuild.ps1 -Configuration DebugGame

.EXAMPLE
    .\Invoke-EditorBuild.ps1 -Clean
#>

param(
    [string]$ProjectPath,
    [ValidateSet("Development", "DebugGame", "Shipping")]
    [string]$Configuration = "Development",
    [switch]$Clean,
    [switch]$SkipGenerateProjectFiles,
    [switch]$SkipWaitForUBT
)

$ErrorActionPreference = "Stop"

# Import common module
$ModulePath = Join-Path $PSScriptRoot "ClaireonCommon.psm1"
Import-Module $ModulePath -Force

# =============================================================================
# INITIALIZE
# =============================================================================

Write-StepProgress "=== Build Editor ===" -Color Yellow
Write-Output ""

# Find project
if ([string]::IsNullOrWhiteSpace($ProjectPath)) {
    Write-StepProgress "Detecting project..." -Color Cyan
    $ProjectPath = Find-UnrealProject
}

if (-not (Test-Path $ProjectPath)) {
    Write-Error "Project file not found: $ProjectPath"
    exit 1
}

$ProjectDir = Split-Path $ProjectPath -Parent
$BuildTarget = Get-EditorBuildTarget -ProjectPath $ProjectPath
Write-Output "Project: $ProjectPath"
Write-Output "Build target: $BuildTarget"

# Find engine
Write-StepProgress "Detecting Unreal Engine..." -Color Cyan
try {
    $Engine = Find-UnrealEngine -ProjectPath $ProjectPath
} catch {
    Write-Error "Failed to find Unreal Engine: $($_.Exception.Message)"
    exit 1
}

Write-Output "Engine: $($Engine.EnginePath)"
Write-Output "Configuration: $Configuration"
Write-Output ""

# Build paths
$BuildBat = Join-Path $Engine.EnginePath "Engine\Build\BatchFiles\Build.bat"
if (-not (Test-Path $BuildBat)) {
    Write-Error "Build.bat not found: $BuildBat"
    exit 1
}

# =============================================================================
# CLEAN PHASE (optional)
# =============================================================================

if ($Clean) {
    Write-StepProgress "Cleaning build artifacts..." -Color Yellow

    $CleanScript = Join-Path $PSScriptRoot "Invoke-CleanProject.ps1"
    if (-not (Test-Path $CleanScript)) {
        Write-Error "Clean script not found: $CleanScript"
        exit 1
    }

    & $CleanScript -ProjectPath $ProjectPath -IncludePlugins

    if ($LASTEXITCODE -ne 0) {
        Write-Error "Clean failed with exit code $LASTEXITCODE"
        exit $LASTEXITCODE
    }

    Write-Output ""
}

# =============================================================================
# WAIT FOR UBT (default)
# =============================================================================

if (-not $SkipWaitForUBT) {
    $WaitScript = Join-Path $PSScriptRoot "Wait-UBT.ps1"
    if (Test-Path $WaitScript) {
        Write-StepProgress "Waiting for UnrealBuildTool to be available..." -Color Yellow
        & $WaitScript -PollSeconds 30
    } else {
        Write-Warning "Wait-UBT.ps1 not found at $WaitScript, skipping wait"
    }
    Write-Output ""
}

# =============================================================================
# GENERATE PROJECT FILES
# =============================================================================

if (-not $SkipGenerateProjectFiles) {
    Write-StepProgress "Generating project files..." -Color Cyan

    $GenerateScript = Join-Path $PSScriptRoot "Invoke-GenerateProjectFiles.ps1"
    if (-not (Test-Path $GenerateScript)) {
        Write-Error "Generate project files script not found: $GenerateScript"
        exit 1
    }

    & $GenerateScript -ProjectPath $ProjectPath

    if ($LASTEXITCODE -ne 0) {
        Write-Error "Project file generation failed with exit code $LASTEXITCODE"
        exit $LASTEXITCODE
    }

    Write-Output ""
}

# =============================================================================
# BUILD EDITOR
# =============================================================================

Write-StepProgress "Building editor ($Configuration)..." -Color Cyan

$BuildArgs = @(
    $BuildTarget,
    "Win64",
    $Configuration,
    "-Project=`"$ProjectPath`"",
    "-Progress",
    "-NoHotReloadFromIDE"
)

$BuildStartTime = Get-Date

try {
    $BuildProcess = Start-Process -FilePath $BuildBat -ArgumentList $BuildArgs `
        -Wait -PassThru -NoNewWindow

    $BuildDuration = ((Get-Date) - $BuildStartTime).TotalSeconds

    if ($BuildProcess.ExitCode -eq 0) {
        Write-Output ""
        Write-StepProgress "Build completed successfully ($([math]::Round($BuildDuration, 1))s)" -Color Green
        exit 0
    } else {
        Write-Output ""
        Write-StepProgress "Build failed with exit code $($BuildProcess.ExitCode) ($([math]::Round($BuildDuration, 1))s)" -Color Red
        exit $BuildProcess.ExitCode
    }
} catch {
    Write-Error "Build process failed: $($_.Exception.Message)"
    exit 1
}
