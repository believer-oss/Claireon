#Requires -Version 5.1

<#
.SYNOPSIS
    Compiles all Blueprints and reports errors/warnings.

.DESCRIPTION
    Invokes the Unreal Engine CompileAllBlueprints commandlet to compile all Blueprint
    assets and report any compilation errors or warnings. Useful for CI/CD validation
    and detecting broken Blueprints after code changes.

.PARAMETER ProjectPath
    Path to the .uproject file. Auto-detected if not provided.

.PARAMETER ContentPath
    Content folder to process (Unreal path like "/Game/Characters" or Windows path).
    If not specified, processes all Blueprints in the project.

.PARAMETER CookedOnly
    Only compile Blueprints that would be included in a cooked build.

.PARAMETER FailOnWarnings
    Treat warnings as errors and fail the commandlet.

.PARAMETER OutputReport
    Path to write a JSON report of compilation results.

.OUTPUTS
    Exit code 0 for success, non-zero for failure

.EXAMPLE
    .\Invoke-CompileBlueprints.ps1

.EXAMPLE
    .\Invoke-CompileBlueprints.ps1 -ContentPath "/Game/Characters" -FailOnWarnings
#>

param(
    [string]$ProjectPath,
    [string]$ContentPath,
    [switch]$CookedOnly,
    [switch]$FailOnWarnings,
    [string]$OutputReport
)

$ErrorActionPreference = "Stop"

# Import common module
$ModulePath = Join-Path $PSScriptRoot "ClaireonCommon.psm1"
Import-Module $ModulePath -Force

# =============================================================================
# INITIALIZE
# =============================================================================

Write-StepProgress "=== Compile All Blueprints ===" -Color Yellow
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
# BUILD COMMANDLET ARGUMENTS
# =============================================================================

$CommandletArgs = @()

if (-not [string]::IsNullOrWhiteSpace($ContentPath)) {
    # Convert Windows path to Unreal path if necessary
    if ($ContentPath -match "^[A-Za-z]:" -or $ContentPath -match "^\\\\") {
        try {
            $ContentPath = ConvertTo-UnrealPath -FilePath $ContentPath -ProjectPath $ProjectPath
            Write-Output "Converted to Unreal path: $ContentPath"
        } catch {
            Write-Warning "Could not convert path, using as filter: $ContentPath"
        }
    }

    $CommandletArgs += "-PackageSubString=$ContentPath"
    Write-Output "Content path: $ContentPath"
} else {
    Write-Output "Content path: (entire project)"
}

if ($CookedOnly) {
    $CommandletArgs += "-CookedOnly"
    Write-Output "Mode: Cooked assets only"
}

if ($FailOnWarnings) {
    $CommandletArgs += "-WarningsAsErrors"
    Write-Output "Mode: Warnings as errors"
}

if (-not [string]::IsNullOrWhiteSpace($OutputReport)) {
    $CommandletArgs += "-ReportOutputPath=`"$OutputReport`""
    Write-Output "Report output: $OutputReport"
}

Write-Output ""

# =============================================================================
# EXECUTE
# =============================================================================

Write-StepProgress "Compiling Blueprints..." -Color Cyan

$ExitCode = Invoke-UnrealCommandlet `
    -EditorCmdExe $Engine.EditorCmdExe `
    -ProjectPath $ProjectPath `
    -CommandletName "CompileAllBlueprints" `
    -CommandletArgs $CommandletArgs

if ($ExitCode -eq 0) {
    Write-StepProgress "Blueprint compilation completed successfully" -Color Green
} else {
    Write-StepProgress "Blueprint compilation failed with exit code $ExitCode" -Color Red
}

exit $ExitCode
