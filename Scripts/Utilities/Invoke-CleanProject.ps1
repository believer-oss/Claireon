#Requires -Version 5.1

<#
.SYNOPSIS
    Cleans project build artifacts and intermediate files.

.DESCRIPTION
    Removes build artifacts (Binaries, Intermediate, Saved folders) from the project
    and optionally from plugins. Useful for forcing a full rebuild or resolving
    build issues.

.PARAMETER ProjectPath
    Path to the .uproject file. Auto-detected if not provided.

.PARAMETER IncludePlugins
    Also clean plugin Binaries and Intermediate folders.

.PARAMETER IncludeSaved
    Also remove the Saved folder (logs, config cache, etc.).

.PARAMETER IncludeDerivedDataCache
    Also remove the DerivedDataCache folder.

.PARAMETER DryRun
    Show what would be deleted without actually deleting.

.OUTPUTS
    Exit code 0 for success

.EXAMPLE
    .\Invoke-CleanProject.ps1

.EXAMPLE
    .\Invoke-CleanProject.ps1 -IncludePlugins -IncludeSaved

.EXAMPLE
    .\Invoke-CleanProject.ps1 -DryRun
#>

param(
    [string]$ProjectPath,
    [switch]$IncludePlugins,
    [switch]$IncludeSaved,
    [switch]$IncludeDerivedDataCache,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

# Import common module
$ModulePath = Join-Path $PSScriptRoot "ClaireonCommon.psm1"
Import-Module $ModulePath -Force

# =============================================================================
# INITIALIZE
# =============================================================================

Write-StepProgress "=== Clean Project ===" -Color Yellow
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
Write-Output "Project: $ProjectPath"

if ($DryRun) {
    Write-Output "Mode: DRY RUN (no changes will be made)"
}
Write-Output ""

# =============================================================================
# COLLECT FOLDERS TO CLEAN
# =============================================================================

$FoldersToClean = @()

# Core project folders
$FoldersToClean += Join-Path $ProjectDir "Binaries"
$FoldersToClean += Join-Path $ProjectDir "Intermediate"

if ($IncludeSaved) {
    $FoldersToClean += Join-Path $ProjectDir "Saved"
}

if ($IncludeDerivedDataCache) {
    $FoldersToClean += Join-Path $ProjectDir "DerivedDataCache"
}

# Plugin folders
if ($IncludePlugins) {
    $PluginsDir = Join-Path $ProjectDir "Plugins"
    if (Test-Path $PluginsDir) {
        Get-ChildItem $PluginsDir -Directory | ForEach-Object {
            $FoldersToClean += Join-Path $_.FullName "Binaries"
            $FoldersToClean += Join-Path $_.FullName "Intermediate"
        }
    }
}

# =============================================================================
# CLEAN FOLDERS
# =============================================================================

Write-StepProgress "Cleaning build artifacts..." -Color Cyan

$CleanedCount = 0
$TotalSize = 0

foreach ($Folder in $FoldersToClean) {
    if (Test-Path $Folder) {
        # Calculate size
        $Size = (Get-ChildItem $Folder -Recurse -ErrorAction SilentlyContinue | Measure-Object -Property Length -Sum).Sum
        $SizeMB = [math]::Round($Size / 1MB, 2)
        $TotalSize += $Size

        $RelativePath = $Folder.Replace($ProjectDir, "").TrimStart("\")

        if ($DryRun) {
            Write-Output "[DRY RUN] Would remove: $RelativePath ($SizeMB MB)"
        } else {
            Write-Output "Removing: $RelativePath ($SizeMB MB)"
            Remove-Item $Folder -Recurse -Force -ErrorAction SilentlyContinue
        }
        $CleanedCount++
    }
}

# =============================================================================
# SUMMARY
# =============================================================================

Write-Output ""
$TotalSizeMB = [math]::Round($TotalSize / 1MB, 2)

if ($DryRun) {
    Write-StepProgress "Would clean $CleanedCount folders ($TotalSizeMB MB)" -Color Yellow
} else {
    Write-StepProgress "Cleaned $CleanedCount folders ($TotalSizeMB MB freed)" -Color Green
}

exit 0
