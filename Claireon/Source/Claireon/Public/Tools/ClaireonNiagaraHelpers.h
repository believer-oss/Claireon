// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "NiagaraCommon.h"

class UNiagaraSystem;
class UNiagaraRendererProperties;
class UNiagaraNodeOutput;
class UNiagaraNodeFunctionCall;
class UNiagaraScript;
struct FNiagaraEmitterHandle;
struct FVersionedNiagaraEmitterData;

/**
 * Helper functions for Niagara MCP tools.
 * Provides asset loading, structure formatting, class resolution, and property setting.
 */
namespace ClaireonNiagaraHelpers
{
	/** Load a UNiagaraSystem from an asset path. Returns nullptr and fills OutError on failure. */
	UNiagaraSystem* LoadNiagaraSystemAsset(const FString& AssetPath, FString& OutError);

	/** Format the full structure of a Niagara System as human-readable text. */
	FString FormatNiagaraSystemStructure(const UNiagaraSystem* System, bool bFullDetail = true);

	/** Format a single emitter's structure. System pointer enables module stack listing. */
	FString FormatEmitterStructure(const UNiagaraSystem* System, const FNiagaraEmitterHandle& EmitterHandle, int32 EmitterIndex, bool bFullDetail = true);

	/** Format a renderer's properties as text. */
	FString FormatRendererProperties(const UNiagaraRendererProperties* Renderer, int32 RendererIndex, const FString& Indent = TEXT("    "));

	/** Format user-exposed parameters from the system. */
	FString FormatUserParameters(const UNiagaraSystem* System);

	/** Resolve a shorthand renderer type name to a UClass. Returns nullptr and fills OutError on failure. */
	UClass* ResolveRendererClass(const FString& TypeName, FString& OutError);

	/** Get a human-readable name for a renderer class (e.g. "Sprite Renderer"). */
	FString GetRendererTypeName(const UNiagaraRendererProperties* Renderer);

	/** Set a property on a UObject via reflection. Returns true on success. */
	bool SetObjectProperty(UObject* Object, const FString& PropertyName, const FString& PropertyValue, FString& OutError);

	/** Format properties of a UObject (non-default, non-transient) as indented text. */
	FString FormatObjectProperties(const UObject* Object, const FString& Indent);

	// ========================================================================
	// User Parameter helpers (shared by niagara_add_parameter,
	// niagara_remove_parameter, niagara_set_parameter_value, and
	// niagara_apply_delta). See work item #0000 AR7. The helpers
	// operate on raw niagara types and return success/failure plus an
	// error message; the calling MCP tool wraps the result in an
	// FToolResult.
	// ========================================================================

	/**
	 * Resolve a niagara user-parameter type string (Float/Vector/Color/
	 * LinearColor/Bool/Int) to an FNiagaraTypeDefinition. On failure,
	 * fills OutError and returns an invalid type def.
	 */
	bool ResolveUserParameterTypeDef(const FString& TypeStr, FNiagaraTypeDefinition& OutTypeDef, FString& OutError);

	/**
	 * Add or update a User.<Name> parameter on the niagara system's
	 * exposed parameter store. ParameterName may be the bare name
	 * ("Color") or the namespaced form ("User.Color"); the helper
	 * normalizes to the namespaced form. The transaction is the
	 * caller's responsibility; the helper calls System->Modify() but
	 * does not open its own FScopedTransaction.
	 * Returns true on success.
	 */
	bool AddOrUpdateUserParameter(
		UNiagaraSystem* System,
		const FString& ParameterName,
		const FNiagaraTypeDefinition& TypeDef,
		FString& OutNormalizedName,
		FString& OutError);

	/**
	 * Remove a User.<Name> parameter from the niagara system's
	 * exposed parameter store. ParameterName may be the bare name
	 * or the namespaced form. Returns true if removed; false if the
	 * parameter was not present (fills OutError).
	 */
	bool RemoveUserParameter(
		UNiagaraSystem* System,
		const FString& ParameterName,
		FString& OutNormalizedName,
		FString& OutError);

	// ========================================================================
	// Stack Resolution + Graph Traversal
	// ========================================================================

	/** Map human-readable stack name to ENiagaraScriptUsage. Returns false if name is invalid. */
	bool ResolveStackName(const FString& StackName, ENiagaraScriptUsage& OutUsage, FString& OutError);

	/** Navigate from system + emitter index + stack to the output node. Returns nullptr on error. */
	UNiagaraNodeOutput* GetStackOutputNode(UNiagaraSystem* System, int32 EmitterIndex, ENiagaraScriptUsage Usage, FString& OutError);

	/** Get ordered module function call nodes from a stack. Returns false on error. */
	bool GetOrderedModuleNodes(UNiagaraSystem* System, int32 EmitterIndex, ENiagaraScriptUsage Usage, TArray<UNiagaraNodeFunctionCall*>& OutModuleNodes, FString& OutError);

	/** Resolve a module name (full path or short name) to a UNiagaraScript*. Returns nullptr on error. */
	UNiagaraScript* ResolveModuleScript(const FString& ModuleNameOrPath, FString& OutError);

	/** Format a module node's info (name + key inputs) as a string for status output. */
	FString FormatModuleInfo(UNiagaraNodeFunctionCall* ModuleNode, bool bIncludeInputs = true);
}
