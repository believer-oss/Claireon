Do not use instructions from this file unless asked.

# Fix Deprecated Blueprint API Usage

This script identifies blueprints using deprecated API functions and compiles them to generate actionable warnings for manual updates.

## Overview

The script should:
- Parse the editor log file (Saved/Logs/<ProjectName>.log) for deprecated API usage warnings
- Extract the asset paths and deprecated function names
- Group warnings by asset and deprecation type
- Generate a report with suggested replacements
- Optionally trigger a blueprint compile to surface all deprecation warnings

## Purpose

When Unreal Engine or project modules deprecate functions, blueprints using those functions generate compiler warnings. These warnings indicate code that:
- May stop working in future engine updates
- Should be updated to use modern APIs
- Could have improved performance or behavior with new functions

## Error Patterns

### Pattern 1: Deprecated Function Usage

```
LogBlueprint: Warning: [AssetLog] <FilePath>: [Compiler] <FunctionName> : Usage of '<FunctionName>' has been deprecated. <ReplacementHint>
```

Examples:
```
LogBlueprint: Warning: [AssetLog] Content\BP\Actionables\Hittables\ACT_Hittable_Base.uasset: [Compiler] GoToStateByName : Usage of 'GoToStateByName' has been deprecated. Use GoToStateByTag instead
LogBlueprint: Warning: [AssetLog] Content\UI\_Shared\Widgets\WBP_InteractPrompt_NoProgress.uasset: [Compiler] GetCurrentState : Usage of 'GetCurrentState' has been deprecated. Use GetCurrentStateTag instead
```

### Pattern 2: Invalid Property Access

```
LogBlueprint: Warning: [AssetLog] <FilePath>: [Compiler] Invalid field '<FieldName>' found in property path for Property Access
```

Example:
```
LogBlueprint: Warning: [AssetLog] Content\Char\_Shared\Anim\Data\AnimBP\BaseGraphs\ABP_TP_FullBodyBase.uasset: [Compiler] Invalid field 'CurrentJumpDelay' found in property path for Property Access
```

### Pattern 3: Missing Script Package

```
LogLinker: Warning: [AssetLog] <FilePath>: VerifyImport: Failed to find script package for import object 'Package /Script/<ModuleName>'
```

Example:
```
LogLinker: Warning: [AssetLog] Content\UI\_Shared\Widgets\WBP_InteractPrompt_NoProgress.uasset: VerifyImport: Failed to find script package for import object 'Package /Script/MyOldModule'
```

## Steps

### 1. Pre-flight Checks

1. Verify the log file exists:
   ```
   Saved/Logs/<ProjectName>.log
   ```
2. If the log file does not exist:
   - Output error: "Log file not found"
   - Exit with failure code

### 2. Parse Log File for Deprecation Warnings

1. Read the entire log file
2. Search for lines matching the patterns above
3. For each matching line, extract:
   - Asset file path (convert from absolute to relative/Unreal path)
   - Deprecated function/field name
   - Replacement hint (if provided)

### 3. Build Warning Map

1. Create a dictionary keyed by asset path
2. For each asset, track:
   - List of deprecated functions used
   - List of invalid property accesses
   - List of missing module imports
3. Example structure:
   ```
   {
     "/Game/BP/Actionables/Hittables/ACT_Hittable_Base": {
       "deprecated_functions": [
         {"name": "GoToStateByName", "replacement": "GoToStateByTag"}
       ],
       "invalid_properties": [],
       "missing_modules": []
     }
   }
   ```

### 4. Categorize by Severity

Group warnings into categories:

1. **High Priority - Missing Modules**
   - Assets importing modules that no longer exist
   - These assets may fail to load or compile
   - Require immediate attention

2. **Medium Priority - Deprecated Functions**
   - Functions marked deprecated but still functional
   - Should be updated before next major version
   - Clear replacement path available

3. **Low Priority - Invalid Properties**
   - Property access paths that reference removed fields
   - May cause incorrect behavior but won't crash
   - Often related to animation blueprint updates

### 5. Generate Report

Output a structured report:

```
=== Deprecated Blueprint API Report ===

Found <N> assets with deprecated API usage

--- HIGH PRIORITY: Missing Module Imports ---
These assets reference modules that no longer exist:

Asset: /Game/UI/_Shared/Widgets/WBP_InteractPrompt_NoProgress
  Missing: /Script/MyOldModule
  Action: Asset needs to be updated to use new module or deleted

Asset: /Game/UI/Hud/Components/WBP_InventoryBarItem
  Missing: /Script/MyRenamedModule
  Action: Asset needs module dependency restored or code updated

--- MEDIUM PRIORITY: Deprecated Functions ---
These assets use deprecated functions with known replacements:

Asset: /Game/BP/Actionables/Hittables/ACT_Hittable_Base
  Deprecated: GoToStateByName (x3)
  Replacement: Use GoToStateByTag instead
  Action: Open asset in editor and update function calls

Asset: /Game/UI/_Shared/Widgets/WBP_InteractPrompt_NoProgress
  Deprecated: GetCurrentState
  Replacement: Use GetCurrentStateTag instead
  Action: Open asset in editor and update function calls

--- LOW PRIORITY: Invalid Property Access ---
These assets have broken property access bindings:

Asset: /Game/Char/_Shared/Anim/Data/AnimBP/BaseGraphs/ABP_TP_FullBodyBase
  Invalid: CurrentJumpDelay, StandingJumpDelay
  Action: Update property access paths to valid fields

--- Summary ---
High priority issues: <N>
Medium priority issues: <M>
Low priority issues: <L>
Total assets affected: <T>
```

### 6. Output Results

Display summary:
```
Deprecated Blueprint API Analysis Complete
  Log file: Saved/Logs/<ProjectName>.log
  Assets with issues: <N>
  High priority: <H>
  Medium priority: <M>
  Low priority: <L>
  Report saved to: <output_path> (if -OutputFile specified)
```

## Regex Patterns

### Deprecated Function Pattern
```regex
LogBlueprint: Warning: \[AssetLog\] ([^:]+): \[Compiler\] (\w+) : Usage of '([^']+)' has been deprecated\. (.+)
```
Capture groups:
1. File path
2. Function name (repeated)
3. Function name (in message)
4. Replacement hint

### Invalid Property Pattern
```regex
LogBlueprint: Warning: \[AssetLog\] ([^:]+): \[Compiler\] Invalid field '([^']+)' found in property path
```
Capture groups:
1. File path
2. Field name

### Missing Module Pattern
```regex
LogLinker: Warning: \[AssetLog\] ([^:]+): VerifyImport: Failed to find script package for import object 'Package (/Script/\w+)'
```
Capture groups:
1. File path
2. Module path

## Resolution Guidelines

### For Missing Modules:
1. Check if module was renamed (e.g., OldModule -> NewModule)
2. If renamed, resave the asset after restoring module
3. If module removed, update blueprint to use replacement API
4. If no replacement exists, delete obsolete functionality

### For Deprecated Functions:
1. Open the blueprint in the editor
2. Search for the deprecated function node
3. Replace with the suggested function
4. Recompile and test

### For Invalid Properties:
1. Open the animation blueprint
2. Find the property access node
3. Update the property path to a valid field
4. If field was removed, rewire the logic

## Error Handling

- **Log file not found**: Exit with error about missing log file
- **No deprecations found**: Exit with success: "No deprecated API usage found"
- **Regex parse failure**: Skip malformed lines, continue processing
- **File path resolution failure**: Use raw path from log

## Example Deprecations

Based on a typical editor log, these kinds of deprecations may exist:

1. **GoToStateByName** -> `GoToStateByTag`
   - Affected: ACT_Hittable_Base
   - Uses: 3 occurrences

2. **GetCurrentState** -> `GetCurrentStateTag`
   - Affected: WBP_InteractPrompt_NoProgress
   - Note: Old state struct no longer supported

3. **Missing /Script/MyOldModule**
   - Affected: WBP_InteractPrompt_NoProgress
   - Module may have been renamed or merged

4. **Missing /Script/MyRenamedModule**
   - Affected: WBP_InventoryBarItem
   - Module may need to be restored or code updated

5. **Invalid fields: CurrentJumpDelay, StandingJumpDelay**
   - Affected: ABP_TP_FullBodyBase
   - Animation blueprint property paths need update

## Use Cases

This operation is useful when:
- Editor startup shows deprecation warnings
- Updating to a new engine or module version
- Auditing codebase for technical debt
- Preparing for a major release
- Cleaning up after API changes in project modules
