#Requires -Version 5.1

<#
.SYNOPSIS
    Fixes up redirectors in the specified folder or project root.

.DESCRIPTION
    Invokes the Unreal Engine ResavePackages commandlet with -FixupRedirects to consolidate
    and remove redirectors in the specified content folder. This is useful after moving
    or renaming assets to clean up the redirector chain.

.PARAMETER ProjectPath
    Path to the .uproject file. Auto-detected if not provided.

.PARAMETER ContentPath
    Content folder to process (Unreal path like "/Game/Characters" or Windows path).
    If not specified, processes the entire project content.

.PARAMETER OnlyDirtyPackages
    Only process packages that are dirty/modified.

.PARAMETER DryRun
    Preview what would be fixed without making changes.

.OUTPUTS
    Exit code 0 for success, non-zero for failure

.EXAMPLE
    .\Invoke-FixupRedirectors.ps1

.EXAMPLE
    .\Invoke-FixupRedirectors.ps1 -ContentPath "/Game/Characters"
#>

param(
    [string]$ProjectPath,
    [string]$ContentPath,
    [switch]$OnlyDirtyPackages,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

# Import common module
$ModulePath = Join-Path $PSScriptRoot "ClaireonCommon.psm1"
Import-Module $ModulePath -Force

# =============================================================================
# INITIALIZE
# =============================================================================

Write-StepProgress "=== Fixup Redirectors ===" -Color Yellow
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

$CommandletArgs = @(
    "-fixupredirects"
)

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

if ($OnlyDirtyPackages) {
    $CommandletArgs += "-OnlyDirty"
    Write-Output "Mode: Only dirty packages"
}

if ($DryRun) {
    $CommandletArgs += "-SKIPDIRTY"
    Write-Output "Mode: DRY RUN (no changes will be made)"
}

Write-Output ""

# =============================================================================
# EXECUTE
# =============================================================================

Write-StepProgress "Fixing up redirectors..." -Color Cyan

$ExitCode = Invoke-UnrealCommandlet `
    -EditorCmdExe $Engine.EditorCmdExe `
    -ProjectPath $ProjectPath `
    -CommandletName "ResavePackages" `
    -CommandletArgs $CommandletArgs

if ($ExitCode -eq 0) {
    Write-StepProgress "Redirectors fixed successfully" -Color Green
} else {
    Write-StepProgress "Redirector fixup failed with exit code $ExitCode" -Color Red
}

exit $ExitCode
