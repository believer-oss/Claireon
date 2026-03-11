Do not use instructions from this file unless asked.

# Fix Nanite Material Compatibility Issues

This script parses the editor log for Nanite material compatibility warnings and generates a report identifying static meshes that need Nanite disabled due to incompatible materials.

## Overview

The script should:
- Parse the editor log file (Saved/Logs/<ProjectName>.log) for Nanite material incompatibility warnings
- Extract material names, mesh paths, blend modes, and shading models
- Group issues by material and mesh
- Generate a report describing what static meshes need Nanite disabled or what materials need to be replaced

## Purpose

Unreal Engine 5's Nanite virtualized geometry system only supports opaque and masked blend modes. When a static mesh has Nanite enabled but references a material with an incompatible blend mode (e.g., translucent glass) or shading model (e.g., SingleLayerWater), the engine logs warnings and renders incorrectly. Common offenders include:

- **MT_Glass** and other translucent/glass materials
- Materials using **Additive** or **Modulate** blend modes
- Materials using the **SingleLayerWater** shading model

The fix requires either:
1. **Disabling Nanite** on the affected static meshes (preferred for meshes that need translucent materials)
2. **Replacing the material** with a Nanite-compatible alternative (if the mesh benefits from Nanite)
3. **Setting "Disallow Nanite"** on specific static mesh components in the level

## Error Patterns

### Pattern 1: Incompatible Blend Mode

```
LogStaticMesh: Warning: Invalid material [<MaterialName>] used on Nanite static mesh [<MeshPath>]. Only opaque or masked blend modes are currently supported, [<BlendMode>] blend mode was specified. (NOTE: "Disallow Nanite" on static mesh components can be used to suppress this warning and forcibly render the object as non-Nanite.)
```

Examples:
```
LogStaticMesh: Warning: Invalid material [MT_Glass] used on Nanite static mesh [/Game/Art/Environment/Props/SM_GlassPanel]. Only opaque or masked blend modes are currently supported, [Translucent] blend mode was specified. (NOTE: "Disallow Nanite" on static mesh components can be used to suppress this warning and forcibly render the object as non-Nanite.)
LogStaticMesh: Warning: Invalid material [MI_FX_Glow] used on Nanite static mesh [/Game/Art/Environment/Lights/SM_LightFixture]. Only opaque or masked blend modes are currently supported, [Additive] blend mode was specified. (NOTE: "Disallow Nanite" on static mesh components can be used to suppress this warning and forcibly render the object as non-Nanite.)
```

### Pattern 2: Incompatible Shading Model

```
LogStaticMesh: Warning: Invalid material [<MaterialName>] used on Nanite static mesh [<MeshPath>]. The SingleLayerWater shading model is currently not supported, [<ShadingModel>] shading model was specified. (NOTE: "Disallow Nanite" on static mesh components can be used to suppress this warning and forcibly render the object as non-Nanite.)
```

Example:
```
LogStaticMesh: Warning: Invalid material [MT_Water_Surface] used on Nanite static mesh [/Game/Art/Environment/Water/SM_WaterPlane]. The SingleLayerWater shading model is currently not supported, [SingleLayerWater] shading model was specified. (NOTE: "Disallow Nanite" on static mesh components can be used to suppress this warning and forcibly render the object as non-Nanite.)
```

## Steps

### 1. Pre-flight Checks

1. Verify the editor log file exists:
   ```
   Saved/Logs/<ProjectName>.log
   ```
2. If the log file does not exist:
   - Output error: "Editor log file not found. Generate a fresh log by launching the editor and running a PIE session."
   - Exit with failure code

### 2. Generate or Locate Editor Log

Generate a fresh editor log by launching the editor and running a PIE session, or use an existing editor log. The log must contain output from loading a level with Nanite-enabled meshes for the warnings to appear.

If using a PIE session:
1. Launch the editor with the default map
2. Run a PIE session for at least 30 seconds to trigger material validation
3. The log will be written to `Saved/Logs/<ProjectName>.log`

If the PIE session fails:
- Check if an editor log was still produced (partial runs may still generate useful warnings)
- If no log was produced, output the error and exit with failure code

### 3. Parse Log File for Nanite Material Warnings

1. Read the editor log file:
   ```
   Saved/Logs/<ProjectName>.log
   ```
2. Search for lines matching Pattern 1 (incompatible blend mode) and Pattern 2 (incompatible shading model)
3. For each matching line, extract:
   - Material name (e.g., `MT_Glass`)
   - Mesh asset path (e.g., `/Game/Art/Environment/Props/SM_GlassPanel`)
   - Issue type: blend mode incompatibility or shading model incompatibility
   - The specific incompatible value (e.g., `Translucent`, `Additive`, `SingleLayerWater`)

### 4. Build Issue Map

1. Create a dictionary keyed by material name
2. For each material, track:
   - All meshes that reference this material with Nanite enabled
   - The incompatibility type (blend mode or shading model)
   - The specific incompatible value
3. Also create a reverse map keyed by mesh path tracking all incompatible materials on that mesh
4. Example structure:
   ```
   {
     "by_material": {
       "MT_Glass": {
         "issue_type": "blend_mode",
         "incompatible_value": "Translucent",
         "affected_meshes": [
           "/Game/Art/Environment/Props/SM_GlassPanel",
           "/Game/Art/Environment/Building/SM_Window_Large"
         ]
       }
     },
     "by_mesh": {
       "/Game/Art/Environment/Props/SM_GlassPanel": {
         "incompatible_materials": ["MT_Glass"],
         "has_other_materials": true
       }
     }
   }
   ```

### 5. Deduplicate and Categorize

1. Remove duplicate entries (same material + mesh logged multiple times)
2. Categorize issues:
   - **Translucent Materials on Nanite Meshes**: Materials using Translucent blend mode (e.g., glass, ice, water effects)
   - **Additive/Modulate Materials on Nanite Meshes**: Materials using Additive or Modulate blend modes (e.g., VFX, glow effects)
   - **Water Materials on Nanite Meshes**: Materials using SingleLayerWater shading model
3. For each mesh, determine if it uses ONLY incompatible materials or a mix:
   - **All materials incompatible**: Mesh should have Nanite disabled entirely
   - **Mixed materials**: Mesh needs component-level "Disallow Nanite" or material replacement

### 6. Generate Report

Output a structured report:

```
=== Nanite Material Compatibility Report ===

Found <N> incompatible material/mesh combinations
Unique materials: <M>
Unique meshes: <P>

--- Translucent Materials on Nanite Meshes ---
These materials use a Translucent blend mode incompatible with Nanite:

Material: MT_Glass (Translucent)
  Affected Meshes (<count>):
    - /Game/Art/Environment/Props/SM_GlassPanel
    - /Game/Art/Environment/Building/SM_Window_Large
  Recommendation: Disable Nanite on these meshes (glass objects don't benefit from Nanite)

Material: MI_Ice_Surface (Translucent)
  Affected Meshes (<count>):
    - /Game/Art/Environment/Nature/SM_IceFormation
  Recommendation: Disable Nanite on this mesh or replace with opaque ice material

--- Additive/Modulate Materials on Nanite Meshes ---
These materials use Additive or Modulate blend modes incompatible with Nanite:

Material: MI_FX_Glow (Additive)
  Affected Meshes (<count>):
    - /Game/Art/Environment/Lights/SM_LightFixture
  Recommendation: Disable Nanite on this mesh or replace material with opaque alternative

--- Water Materials on Nanite Meshes ---
These materials use the SingleLayerWater shading model incompatible with Nanite:

Material: MT_Water_Surface (SingleLayerWater)
  Affected Meshes (<count>):
    - /Game/Art/Environment/Water/SM_WaterPlane
  Recommendation: Disable Nanite on this mesh (water surfaces should not use Nanite)

--- Action Items by Mesh ---
These meshes need Nanite disabled or materials replaced:

Mesh: /Game/Art/Environment/Props/SM_GlassPanel
  Incompatible Materials: MT_Glass (Translucent)
  Action: Open Static Mesh in editor -> Details -> Nanite Settings -> Disable "Enable Nanite Support"

Mesh: /Game/Art/Environment/Building/SM_Window_Large
  Incompatible Materials: MT_Glass (Translucent)
  Action: Open Static Mesh in editor -> Details -> Nanite Settings -> Disable "Enable Nanite Support"

--- Summary ---
Total incompatible combinations: <N>
Meshes needing Nanite disabled: <P>
Unique incompatible materials: <M>
  Translucent: <T>
  Additive/Modulate: <A>
  SingleLayerWater: <W>
```

### 7. Output Results

Display summary:
```
Nanite Material Compatibility Analysis Complete
  Log file: Saved/Logs/<ProjectName>.log
  Incompatible material/mesh combinations: <N>
  Unique meshes affected: <P>
  Unique materials involved: <M>
  Report saved to: <output_path> (if -OutputFile specified)
  Status: Issues found - manual resolution required
```

## Regex Patterns

### Incompatible Blend Mode Pattern

```regex
LogStaticMesh: Warning: Invalid material \[([^\]]+)\] used on Nanite static mesh \[([^\]]+)\]\. Only opaque or masked blend modes are currently supported, \[([^\]]+)\] blend mode was specified\.
```

Capture groups:
1. Material name
2. Mesh asset path
3. Blend mode (e.g., Translucent, Additive, Modulate)

### Incompatible Shading Model Pattern

```regex
LogStaticMesh: Warning: Invalid material \[([^\]]+)\] used on Nanite static mesh \[([^\]]+)\]\. The (\w+) shading model is currently not supported, \[([^\]]+)\] shading model was specified\.
```

Capture groups:
1. Material name
2. Mesh asset path
3. Shading model name in message text
4. Shading model name in brackets

### Broad Catch-All Pattern (for custom engine variations)

If the above patterns don't match, also search for:

```regex
LogStaticMesh: Warning:.*\[([^\]]+)\].*Nanite.*\[([^\]]+)\]
```

This broader pattern catches any LogStaticMesh warning that mentions a material and Nanite together.

## Resolution Guidelines

### When to DISABLE NANITE on the mesh:

1. **Translucent objects**: Glass, ice, water, and other transparent objects rarely benefit from Nanite's triangle virtualization. Disable Nanite on the static mesh asset:
   - Open the Static Mesh in the editor
   - In the Details panel, find **Nanite Settings**
   - Uncheck **Enable Nanite Support**
   - Save the asset

2. **Simple geometry**: If the mesh is low-poly (< 1000 triangles), Nanite overhead isn't worth it regardless of material compatibility.

3. **VFX meshes**: Meshes using additive/modulate materials for visual effects should not use Nanite.

### When to SET "Disallow Nanite" on components:

1. **Mixed material meshes**: If a mesh has both compatible and incompatible materials, and only some instances in levels need the incompatible material, use per-component override:
   - Select the Static Mesh Component in the level
   - In Details panel, check **Disallow Nanite**
   - This only affects that specific instance

2. **Shared meshes**: If the same mesh is used with different materials via material overrides, use component-level disabling.

### When to REPLACE the material:

1. **Accidental assignment**: If a translucent material was mistakenly assigned to a mesh that should be opaque, replace with the correct opaque material.
2. **Material upgrade**: If an opaque/masked alternative exists (e.g., `MT_Glass_Opaque` for non-see-through glass), switch to it.

## Error Handling

- **Log file not found**: Exit with error about missing editor log file
- **No Nanite warnings found**: Exit with success: "No Nanite material compatibility issues found"
- **Regex parse failure**: Skip malformed lines, continue processing
- **Write failure (output file)**: Log warning, display to console instead

## Example Execution

Starting state:
- Editor is already built
- Default map contains meshes with Nanite enabled that reference MT_Glass

Execution:
1. Launch the editor and run a PIE session to generate a fresh log
2. Editor launches, loads map, runs PIE for 30 seconds, exits
3. Parse the editor log for `LogStaticMesh: Warning: Invalid material` entries
4. Find 3 unique meshes referencing MT_Glass with Nanite enabled
5. Generate report listing each mesh and recommended action

Result:
```
Nanite Material Compatibility Analysis Complete
  Log file: Saved/Logs/<ProjectName>.log
  Incompatible material/mesh combinations: 3
  Unique meshes affected: 3
  Unique materials involved: 1
  Status: Issues found - manual resolution required
```

## Use Cases

This operation is useful when:
- Editor startup or PIE shows Nanite material warnings in the output log
- New meshes with Nanite enabled are imported with translucent material slots
- Materials are changed from opaque to translucent on meshes that have Nanite enabled
- Auditing Nanite usage across the project for rendering correctness
- Cleaning up warnings before a release build
- After importing new art assets that may have Nanite enabled by default
