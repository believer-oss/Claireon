Do not use instructions from this file unless asked.

# Resave Assets With Load Errors

This script examines the Unreal Editor log for asset loading errors and triggers a resave operation on the affected assets to resolve stale references.

## Overview

The script should:
- Parse the editor log file (Saved/Logs/<ProjectName>.log) for specific loading error patterns
- Extract the paths of assets that failed to load dependencies
- Pass the collected asset paths to the Invoke-ResaveAssets.ps1 utility
- Report the results of the operation

## Purpose

When Unreal assets have stale or broken references to other assets (renamed, moved, or deleted), the editor logs `LoadErrors` warnings. Resaving these assets forces Unreal to update or clear the broken references, resolving the warnings on subsequent loads.

## Error Pattern

The target error pattern consists of two related log lines:

```
LogPackageName: Warning: GetLocalFullPath called on FPackagePath ../../../../Content/<path> which has an unspecified header extension, and the path does not exist on disk. Assuming EPackageExtension::Asset.
LoadErrors: Warning: While trying to load package /Game/<AssetPath>, a dependent package /Game/<DependencyPath> was not available. Additional explanatory information follows:
```

The asset to resave is the one mentioned in the `LoadErrors` line (`/Game/<AssetPath>`), NOT the missing dependency.

## Steps

### 1. Pre-flight Checks

1. Verify the log file exists:
   ```
   Saved/Logs/<ProjectName>.log
   ```
2. If the log file does not exist:
   - Output error: "Log file not found: Saved/Logs/<ProjectName>.log"
   - Exit with failure code

3. Verify the resave script exists:
   ```
   Scripts/Utilities/Invoke-ResaveAssets.ps1
   ```
4. If the script does not exist:
   - Output error: "Resave script not found: Scripts/Utilities/Invoke-ResaveAssets.ps1"
   - Exit with failure code

### 2. Parse Log File for Load Errors

1. Read the entire log file
2. Search for lines matching the pattern:
   ```
   LoadErrors: Warning: While trying to load package /Game/<path>, a dependent package
   ```
3. For each matching line, extract the asset path (the first `/Game/<path>` reference)
4. Convert the game path to a content path:
   - `/Game/Blueprints/BP_Example`
   - (The /Game prefix is kept as-is for the resave script)

### 3. Deduplicate Asset List

1. Remove duplicate asset paths (the same asset may have multiple load errors)
2. Sort the list alphabetically for consistent output
3. If no assets found:
   - Output: "No load errors found in log file"
   - Exit with success code (nothing to do)

### 4. Display Assets to Resave

1. Output the count of unique assets found:
   ```
   Found <N> asset(s) with load errors:
   ```
2. List each asset path:
   ```
     - /Game/Blueprints/BP_Example
     - /Game/Audio/SoundCues/SC_Example
   ```

### 5. Filter Out Locked Assets (Optional)

If your project uses Git LFS locks, filter out assets locked by other users before resaving. This prevents attempting to modify files that cannot be written.

1. If a lock filter script is available, call it to remove assets locked by others
2. If some assets were filtered out:
   - Output: "Skipped <N> asset(s) locked by others"
3. If all assets are locked:
   - Output: "All assets are locked by other users. Nothing to resave."
   - Exit with success code (nothing to do)
4. Update the asset list to use only unlocked assets

### 6. Invoke Resave Script

1. Call the PowerShell resave script with the asset list:
   ```powershell
   & "Scripts\Utilities\Invoke-ResaveAssets.ps1" -Assets $AssetList
   ```
2. Capture the script output and exit code
3. If the script fails:
   - Output error: "Resave operation failed"
   - Display the script's error output
   - Exit with failure code

### 7. Output Results

Display summary information:
```
Resave Assets Complete
  Log file: Saved/Logs/<ProjectName>.log
  Assets processed: <N>
  Status: Success
```

## Regex Pattern

Use this regex to extract asset paths from LoadErrors lines:

```regex
LoadErrors: Warning: While trying to load package (/Game/[^,]+), a dependent package
```

Capture group 1 contains the asset path to resave.

## Error Handling

- **Log file not found**: Exit with error about missing log file
- **Resave script not found**: Exit with error about missing script
- **No errors found**: Exit with success (informational message only)
- **All assets locked**: Exit with success (informational message about locked assets)
- **Regex parse failure**: Skip malformed lines, continue processing
- **Resave script failure**: Exit with error, display script output

## Example Execution

Starting state:
- Log file contains these errors:
  ```
  LoadErrors: Warning: While trying to load package /Game/Blueprints/BP_Example, a dependent package /Game/Audio/Sounds/SC_OldSound was not available.
  LoadErrors: Warning: While trying to load package /Game/Blueprints/BP_Example, a dependent package /Game/Audio/Sounds/SC_OldSound was not available.
  LoadErrors: Warning: While trying to load package /Game/UI/Widgets/WBP_MainMenu, a dependent package /Game/UI/Textures/T_OldIcon was not available.
  ```
- `/Game/UI/Widgets/WBP_MainMenu` is locked by another user (jdoe@example.com)

Result:
- Unique assets identified: 2
  - `/Game/Blueprints/BP_Example`
  - `/Game/UI/Widgets/WBP_MainMenu`
- Lock filter applied:
  - Skipped 1 asset locked by others: `/Game/UI/Widgets/WBP_MainMenu` (locked by jdoe@example.com)
- Resave script invoked with 1 asset
- Assets resaved successfully

## Use Cases

This operation is useful when:
- Editor startup shows multiple LoadErrors warnings
- Assets reference content that was renamed or moved
- Fixing stale references after large refactoring operations
- Cleaning up warnings before committing changes
- Automated CI/CD validation and cleanup
