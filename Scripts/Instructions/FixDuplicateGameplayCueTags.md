Do not use instructions from this file unless asked.

# Fix Duplicate GameplayCue Tags

This script identifies GameplayCue assets that share the same gameplay cue tag, causing warnings during editor startup, and reports them for manual resolution.

## Overview

The script should:
- Parse the editor log file for duplicate GameplayCue tag warnings
- Extract the tag names and asset paths involved
- Group duplicates by their shared tag
- Generate a report of conflicts for manual resolution
- Optionally suggest the assets that should be renamed or deleted

## Purpose

When multiple GameplayCue assets register the same gameplay cue tag, the Gameplay Ability System logs warnings and only uses the first registered asset. This can cause:
- Missing VFX/SFX during gameplay
- Inconsistent behavior depending on load order
- Confusion about which asset is actually used

The fix requires either:
1. Renaming duplicate tags to be unique
2. Deleting redundant GameplayCue assets
3. Consolidating duplicate assets into a single shared asset

## Error Pattern

The target error pattern is:

```
LogAbilitySystem: Warning: AddGameplayCueData_Internal called for [<TagName>,<NewAssetPath>] when it already existed [<TagName>,<ExistingAssetPath>]. Skipping.
```

Key components:
- **TagName**: The GameplayCue tag (e.g., `GameplayCue.Character.DamageTaken`)
- **NewAssetPath**: The asset that was skipped (duplicate)
- **ExistingAssetPath**: The asset that was kept (first registered)

## Steps

### 1. Pre-flight Checks

1. Verify the log file exists at:
   ```
   Saved/Logs/<ProjectName>.log
   ```
2. If the log file does not exist:
   - Output error: "Log file not found. Check Saved/Logs/ for the editor log file."
   - Exit with failure code

### 2. Parse Log File for Duplicate GameplayCue Warnings

1. Read the entire log file
2. Search for lines matching the pattern:
   ```
   LogAbilitySystem: Warning: AddGameplayCueData_Internal called for
   ```
3. For each matching line, extract:
   - The GameplayCue tag name
   - The skipped asset path (duplicate)
   - The existing asset path (kept)

### 3. Build Conflict Map

1. Create a dictionary keyed by GameplayCue tag
2. For each tag, track:
   - The "winner" (first registered asset)
   - All "losers" (skipped duplicate assets)
3. Example structure:
   ```
   {
     "GameplayCue.Character.DamageTaken": {
       "kept": "/Game/GameplayCueNotifies/Framework/GCNL_Character_DamageTaken",
       "skipped": [
         "/Game/GAS/_Shared/GameplayCues/GCNL_Character_DamageTaken"
       ]
     }
   }
   ```

### 4. Deduplicate and Categorize

1. Remove duplicate entries (same tag + same paths logged multiple times)
2. Categorize conflicts by type:
   - **Framework vs Project**: Project asset duplicates a framework/engine asset
   - **Cross-Character**: Different character folders have same tag
   - **Same Folder**: Multiple assets in same folder share a tag
   - **Temp/Test**: Assets in test/temp folders duplicating production assets

### 5. Generate Report

Output a structured report:

```
=== GameplayCue Tag Conflicts Report ===

Found <N> unique tags with duplicate registrations

--- Category: Framework vs Project ---
These project assets duplicate existing framework GameplayCues:

Tag: GameplayCue.Character.DamageTaken
  KEPT: /Game/GameplayCueNotifies/Framework/GCNL_Character_DamageTaken
  SKIP: /Game/GAS/_Shared/GameplayCues/GCNL_Character_DamageTaken
  Recommendation: Delete project duplicate or rename its tag

--- Category: Cross-Character ---
These tags are used by multiple character assets:

Tag: GameplayCue.Weapon.Pistol.RecoilSmall
  KEPT: /Game/GAS/Character/Hero/CharacterA/ChargedShot/GC_ChargedShot_Hit_Shake_Small
  SKIP: /Game/GAS/Character/Hero/CharacterA/Earthquake/GC_Earthquake_Hit_Shake_Small
  SKIP: /Game/GAS/Character/Hero/CharacterB/Abilities/PistolBeam/GC_Gunshot_Hit_Shake_Small
  Recommendation: Rename tags to be character/ability-specific

--- Summary ---
Total tags with conflicts: <N>
Total duplicate assets: <M>
```

### 6. Optional: Generate Fix Script

If requested with `-GenerateFixScript`, create a list of suggested actions:

```powershell
# Fix script for duplicate GameplayCue tags
# Review and execute manually - DO NOT run automatically

# 1. Delete framework duplicates (project versions of framework assets)
# DELETE: /Game/GAS/_Shared/GameplayCues/GCNL_Character_DamageTaken.uasset

# 2. Rename cross-character duplicates
# RENAME TAG: /Game/GAS/Character/Hero/CharacterA/Earthquake/GC_Earthquake_Hit_Shake_Small
#   FROM: GameplayCue.Weapon.Pistol.RecoilSmall
#   TO:   GameplayCue.Character.CharacterA.Earthquake.RecoilSmall
```

### 7. Output Results

Display summary:
```
GameplayCue Tag Analysis Complete
  Log file: Saved/Logs/<ProjectName>.log
  Unique tags with conflicts: <N>
  Total duplicate assets: <M>
  Report saved to: <output_path> (if -OutputFile specified)
  Status: Issues found - manual resolution required
```

## Regex Pattern

Use this regex to extract GameplayCue conflict info from the log:

```regex
LogAbilitySystem: Warning: AddGameplayCueData_Internal called for \[([^,]+),([^\]]+)\] when it already existed \[([^,]+),([^\]]+)\]\. Skipping\.
```

Capture groups:
1. Tag name (duplicate)
2. Skipped asset path
3. Tag name (existing - should match group 1)
4. Kept asset path

## Resolution Guidelines

### When to DELETE the duplicate:

1. **Framework duplicates**: If a project asset duplicates a framework GameplayCue with identical behavior, delete the project version
2. **Orphaned assets**: If a duplicate asset is not referenced by any ability, delete it
3. **Test/temp assets**: If duplicates are in test folders, delete them

### When to RENAME the tag:

1. **Different behaviors**: If the duplicate has different VFX/SFX, rename its tag to be unique
2. **Character-specific**: Rename generic tags like `GameplayCue.Weapon.Pistol.RecoilSmall` to `GameplayCue.Character.CharacterA.Earthquake.RecoilSmall`
3. **Ability-specific**: Use the ability name in the tag: `GameplayCue.Ability.TimeWave.Hit`

### When to CONSOLIDATE:

1. **Identical assets**: If duplicates are identical, keep one and update all references
2. **Shared behavior**: Create a shared GameplayCue in `_Shared/GameplayCues/` and reference it from multiple abilities

## Error Handling

- **Log file not found**: Exit with error about missing log file
- **No duplicates found**: Exit with success: "No duplicate GameplayCue tags found"
- **Regex parse failure**: Skip malformed lines, continue processing
- **Write failure (output file)**: Log warning, display to console instead

## Use Cases

This operation is useful when:
- Editor startup shows many duplicate GameplayCue warnings
- Abilities don't play expected VFX/SFX (wrong asset registered)
- Cleaning up after copying/duplicating ability blueprints
- Auditing GameplayCue organization before a release
- Standardizing GameplayCue naming conventions across the project
