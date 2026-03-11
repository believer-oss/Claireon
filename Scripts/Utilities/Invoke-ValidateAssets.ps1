#Requires -Version 5.1

<#
.SYNOPSIS
    Validates assets in the specified folder or project root.

.DESCRIPTION
    Invokes the Unreal Engine ResavePackages commandlet with verification options
    to check for asset issues, missing references, and package integrity problems.

.PARAMETER ProjectPath
    Path to the .uproject file. Auto-detected if not provided.

.PARAMETER ContentPath
    Content folder to validate (Unreal path like "/Game/Characters" or Windows path).
    If not specified, validates the entire project content.

.PARAMETER CheckReferences
    Check for missing/broken asset references.

.PARAMETER FixIssues
    Attempt to fix issues automatically where possible.

.OUTPUTS
    Exit code 0 for success/no issues, non-zero for failure or issues found

.EXAMPLE
    .\Invoke-ValidateAssets.ps1

.EXAMPLE
    .\Invoke-ValidateAssets.ps1 -ContentPath "/Game/Maps" -CheckReferences
#>

param(
    [string]$ProjectPath,
    [string]$ContentPath,
    [switch]$CheckReferences,
    [switch]$FixIssues
)

$ErrorActionPreference = "Stop"

# Import common module
$ModulePath = Join-Path $PSScriptRoot "ClaireonCommon.psm1"
Import-Module $ModulePath -Force

# =============================================================================
# INITIALIZE
# =============================================================================

Write-StepProgress "=== Validate Assets ===" -Color Yellow
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

    $CommandletArgs += "-PackageFolder=$ContentPath"
    Write-Output "Content path: $ContentPath"
} else {
    Write-Output "Content path: (entire project)"
}

if ($CheckReferences) {
    $CommandletArgs += "-CheckReferences"
    Write-Output "Mode: Check references"
}

if ($FixIssues) {
    $CommandletArgs += "-AutoFix"
    Write-Output "Mode: Auto-fix issues"
}

Write-Output ""

# =============================================================================
# EXECUTE
# =============================================================================

Write-StepProgress "Validating assets..." -Color Cyan

# Use ResavePackages with verify options as it provides good validation
$CommandletArgs += @(
    "-VerifyContent",
    "-IgnoreChangelist"
)

$ExitCode = Invoke-UnrealCommandlet `
    -EditorCmdExe $Engine.EditorCmdExe `
    -ProjectPath $ProjectPath `
    -CommandletName "ResavePackages" `
    -CommandletArgs $CommandletArgs

if ($ExitCode -eq 0) {
    Write-StepProgress "Asset validation completed successfully" -Color Green
} else {
    Write-StepProgress "Asset validation failed with exit code $ExitCode" -Color Red
}

exit $ExitCode
