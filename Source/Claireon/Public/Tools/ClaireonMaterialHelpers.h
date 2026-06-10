// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "SceneTypes.h"                 // EMaterialProperty, EMaterialShadingModel
#include "Engine/EngineTypes.h"         // EBlendMode
#include "Materials/MaterialInterface.h" // EMaterialParameterType (transitively via MaterialTypes.h)

class UMaterial;
class UMaterialExpression;
class UMaterialInstanceConstant;
class UTexture;

/**
 * Shared utility functions for Material MCP tools.
 *
 * Provides asset loading, expression class/identifier resolution, markdown
 * formatting, graph mutation wrappers, parameter default setters, MIC
 * parameter mutation, root property setters, compile/save wrappers, and
 * batched edit scoping.
 *
 * Used by the five material tools (material_inspect, material_edit,
 * material_instance_inspect, material_instance_edit, material_apply) and the
 * two spec applicators (FClaireonSpecApplicator_Material,
 * FClaireonSpecApplicator_MaterialInstance).
 */
namespace ClaireonMaterialHelpers
{
	// ------------------------------------------------------------------
	// Asset loading
	// ------------------------------------------------------------------

	/** Resolve an asset path, cast to UMaterial, return nullptr with OutError on failure. */
	UMaterial* LoadMaterialAsset(const FString& AssetPath, FString& OutError);

	/** Resolve an asset path, cast to UMaterialInstanceConstant, return nullptr with OutError on failure. */
	UMaterialInstanceConstant* LoadMaterialInstanceAsset(const FString& AssetPath, FString& OutError);

	// ------------------------------------------------------------------
	// Expression class + identifier resolution
	// ------------------------------------------------------------------

	/** Fuzzy match a UMaterialExpression subclass by full, partial, or short name. */
	UClass* ResolveExpressionClass(const FString& ClassName, FString& OutError);

	/**
	 * Find an expression by identifier. Priority:
	 *   1. Numeric index into UMaterial::GetExpressions()
	 *   2. Parameter name (GetParameterName for parameter expressions)
	 *   3. GetDescription() exact match
	 *   4. Class short name when it uniquely identifies one expression
	 * OutIndex is the position in GetExpressions(); set to INDEX_NONE on failure.
	 */
	UMaterialExpression* FindExpressionByIdentifier(UMaterial* Material, const FString& Identifier, int32& OutIndex);

	// ------------------------------------------------------------------
	// Markdown formatting (for inspect tools)
	// ------------------------------------------------------------------

	/** Top-level material block: shading model, blend mode, domain, usage flags, counts. DetailLevel is "summary" or "full". */
	FString FormatMaterialStructure(const UMaterial* Material, const FString& DetailLevel);

	/** Per-expression dump: class, parameter name, editor position, output pins, input pins with sources. */
	FString FormatExpressionDetail(const UMaterial* Material, const UMaterialExpression* Expression, int32 Index, bool bIncludeConnections);

	/** Scalar/Vector/Texture/StaticSwitch/StaticComponentMask parameter default table. */
	FString FormatParameterSummary(const UMaterial* Material);

	/** MIC: parent chain + per-type parameter tables with Inherited Value and Override Value columns. */
	FString FormatMaterialInstance(const UMaterialInstanceConstant* Instance);

	// ------------------------------------------------------------------
	// Graph mutation (wraps UMaterialEditingLibrary where possible)
	// ------------------------------------------------------------------

	/** Wrap UMaterialEditingLibrary::ConnectMaterialExpressions; populates OutError with valid pin names on failure. */
	bool ConnectExpressions(UMaterial* Material, UMaterialExpression* From, const FString& FromOutput, UMaterialExpression* To, const FString& ToInput, FString& OutError);

	/** Wrap UMaterialEditingLibrary::DisconnectMaterialExpression. */
	bool DisconnectExpressionInput(UMaterial* Material, UMaterialExpression* Expr, const FString& InputName, FString& OutError);

	/**
	 * Map AttributeName to EMaterialProperty and connect via
	 * UMaterialEditingLibrary::ConnectMaterialProperty. Accepts the 16 legacy
	 * attribute strings and "FrontMaterial" (gated by Substrate::IsSubstrateEnabled).
	 */
	bool ConnectToMaterialAttribute(UMaterial* Material, UMaterialExpression* From, const FString& AttributeName, const FString& OutputName, FString& OutError);

	/**
	 * Set a property on an expression by name using FProperty::ImportText_Direct,
	 * wrapped in PreEditChange/PostEditChangeProperty.
	 */
	bool SetExpressionProperty(UMaterial* Material, UMaterialExpression* Expr, const FString& PropertyName, const FString& TextValue, FString& OutError);

	// ------------------------------------------------------------------
	// Parameter defaults (direct expression mutation -- no library setters exist)
	// ------------------------------------------------------------------

	bool SetScalarParameterDefault(UMaterial* Material, const FName& ParamName, float Value, FString& OutError);
	bool SetVectorParameterDefault(UMaterial* Material, const FName& ParamName, const FLinearColor& Value, FString& OutError);
	bool SetTextureParameterDefault(UMaterial* Material, const FName& ParamName, UTexture* Value, FString& OutError);
	bool SetStaticSwitchParameterDefault(UMaterial* Material, const FName& ParamName, bool Value, FString& OutError);
	bool SetStaticComponentMaskParameterDefault(UMaterial* Material, const FName& ParamName, bool R, bool G, bool B, bool A, FString& OutError);

	// ------------------------------------------------------------------
	// MIC parameter mutation (used by material_instance_edit)
	// ------------------------------------------------------------------

	bool SetMICScalar(UMaterialInstanceConstant* Instance, const FName& ParamName, float Value, FString& OutError);
	bool SetMICVector(UMaterialInstanceConstant* Instance, const FName& ParamName, const FLinearColor& Value, FString& OutError);
	bool SetMICTexture(UMaterialInstanceConstant* Instance, const FName& ParamName, UTexture* Value, FString& OutError);
	bool SetMICStaticSwitch(UMaterialInstanceConstant* Instance, const FName& ParamName, bool Value, FString& OutError);
	bool SetMICStaticComponentMask(UMaterialInstanceConstant* Instance, const FName& ParamName, bool R, bool G, bool B, bool A, FString& OutError);

	/** Remove matching override from MIC's parameter arrays (or static set). Type selects which array. */
	bool ClearMICOverride(UMaterialInstanceConstant* Instance, const FName& ParamName, EMaterialParameterType Type, FString& OutError);

	// ------------------------------------------------------------------
	// Root properties
	// ------------------------------------------------------------------

	bool SetShadingModel(UMaterial* Material, EMaterialShadingModel NewModel, FString& OutError);
	bool SetBlendMode(UMaterial* Material, EBlendMode NewMode, FString& OutError);

	// ------------------------------------------------------------------
	// Compile + save
	// ------------------------------------------------------------------

	/** Call RecompileMaterial; when bWaitForCompile is true, block on GShaderCompilingManager up to 30s. Drains errors into OutError. */
	bool CompileMaterial(UMaterial* Material, bool bWaitForCompile, FString& OutError);

	/** Wrap UEditorAssetLibrary::SaveLoadedAsset for a UMaterial. */
	bool SaveMaterialAsset(UMaterial* Material, FString& OutError);

	/** Wrap UEditorAssetLibrary::SaveLoadedAsset for a UMaterialInstanceConstant. */
	bool SaveMaterialInstanceAsset(UMaterialInstanceConstant* Instance, FString& OutError);

	// ------------------------------------------------------------------
	// Edit scoping (used by material_edit when bCompileDeferred is true)
	// ------------------------------------------------------------------

	/** Construct and stash a FMaterialUpdateContext keyed by weak-ptr; call PreEditChange(nullptr). */
	void BeginEdit(UMaterial* Material);

	/** PostEditChange() then destroy the stashed FMaterialUpdateContext (refreshes dependent MICs). */
	void EndEdit(UMaterial* Material);
} // namespace ClaireonMaterialHelpers
