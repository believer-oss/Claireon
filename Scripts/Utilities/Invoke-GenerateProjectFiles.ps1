#Requires -Version 5.1

<#
.SYNOPSIS
    Generates Visual Studio project files and compilation database.

.DESCRIPTION
    Invokes UnrealBuildTool to regenerate Visual Studio solution and project files,
    as well as the compile_commands.json for IDE integration. Useful after adding
    new source files or modules.

.PARAMETER ProjectPath
    Path to the .uproject file. Auto-detected if not provided.

.PARAMETER SkipClangDatabase
    Skip generating the Clang compilation database (compile_commands.json).

.OUTPUTS
    Exit code 0 for success, non-zero for failure

.EXAMPLE
    .\Invoke-GenerateProjectFiles.ps1

.EXAMPLE
    .\Invoke-GenerateProjectFiles.ps1 -SkipClangDatabase
#>

param(
    [string]$ProjectPath,
    [switch]$SkipClangDatabase
)

$ErrorActionPreference = "Stop"

# Import common module
$ModulePath = Join-Path $PSScriptRoot "ClaireonCommon.psm1"
Import-Module $ModulePath -Force

# =============================================================================
# INITIALIZE
# =============================================================================

Write-StepProgress "=== Generate Project Files ===" -Color Yellow
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

Write-Output "Project: $ProjectPath"
$ProjectDir = Split-Path $ProjectPath -Parent
$BuildTarget = Get-EditorBuildTarget -ProjectPath $ProjectPath

# Find engine
Write-StepProgress "Detecting Unreal Engine..." -Color Cyan
try {
    $Engine = Find-UnrealEngine -ProjectPath $ProjectPath
} catch {
    Write-Error "Failed to find Unreal Engine: $($_.Exception.Message)"
    exit 1
}

Write-Output "Engine: $($Engine.EnginePath)"
Write-Output ""

# =============================================================================
# GENERATE PROJECT FILES
# =============================================================================

$UnrealBuildTool = Join-Path $Engine.EnginePath "Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe"

if (-not (Test-Path $UnrealBuildTool)) {
    Write-Error "UnrealBuildTool.exe not found: $UnrealBuildTool"
    exit 1
}

Write-StepProgress "Generating Visual Studio project files..." -Color Cyan

$GenArgs = @(
    "-Mode=GenerateProjectFiles",
    "-Project=`"$ProjectPath`""
)

try {
    $GenProcess = Start-Process -FilePath $UnrealBuildTool -ArgumentList $GenArgs `
        -Wait -PassThru -NoNewWindow

    if ($GenProcess.ExitCode -ne 0) {
        Write-StepProgress "Project file generation failed with exit code $($GenProcess.ExitCode)" -Color Red
        exit $GenProcess.ExitCode
    }

    Write-StepProgress "Visual Studio project files generated" -Color Green
} catch {
    Write-Error "Failed to generate project files: $($_.Exception.Message)"
    exit 1
}

# =============================================================================
# GENERATE CLANG DATABASE
# =============================================================================

if (-not $SkipClangDatabase) {
    Write-Output ""
    Write-StepProgress "Generating Clang compilation database..." -Color Cyan

    $ClangArgs = @(
        "-mode=GenerateClangDatabase",
        "-Project=`"$ProjectPath`"",
        "-game",
        "-engine",
        $BuildTarget,
        "DebugGame",
        "Win64"
    )

    try {
        $ClangProcess = Start-Process -FilePath $UnrealBuildTool -ArgumentList $ClangArgs `
            -Wait -PassThru -NoNewWindow

        if ($ClangProcess.ExitCode -eq 0) {
            # Copy compile_commands.json from engine to project
            $SourceCompileCommands = Join-Path $Engine.EnginePath "compile_commands.json"
            $DestCompileCommands = Join-Path $ProjectDir "compile_commands.json"

            if (Test-Path $SourceCompileCommands) {
                Copy-Item -Force $SourceCompileCommands $DestCompileCommands
                Write-StepProgress "Clang database generated and copied to project" -Color Green
            } else {
                Write-Warning "compile_commands.json not found at: $SourceCompileCommands"
            }
        } else {
            Write-Warning "Clang database generation failed (non-critical)"
        }
    } catch {
        Write-Warning "Failed to generate Clang database (non-critical): $($_.Exception.Message)"
    }
}

Write-Output ""
Write-StepProgress "Project files generated successfully" -Color Green

exit 0
