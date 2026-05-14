// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_BlueprintCompile.h"
#include "ClaireonBlueprintHelpers.h"
#include "ClaireonPathResolver.h"
#include "ClaireonLog.h"
#include "ClaireonScopedAssetLock.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "BlueprintEditorLibrary.h"
#include "Engine/Blueprint.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/SoftObjectPath.h"

FString ClaireonTool_BlueprintCompile::GetCategory() const { return TEXT("blueprint"); }
FString ClaireonTool_BlueprintCompile::GetOperation() const { return TEXT("compile"); }

TArray<FString> ClaireonTool_BlueprintCompile::GetSearchKeywords() const
{
	return {TEXT("bp"), TEXT("blueprint"), TEXT("compile"), TEXT("build"), TEXT("validate"), TEXT("check")};
}

FString ClaireonTool_BlueprintCompile::GetDescription() const
{
	return TEXT("Compile a single Blueprint by asset path. Acquires a per-asset session for the duration of the compile. "
		"For batch compilation across many blueprints or content folders, use blueprint_compile_batch instead.");
}

TSharedPtr<FJsonObject> ClaireonTool_BlueprintCompile::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// asset_path - required, single string
	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"),
		TEXT("Blueprint asset path to compile (e.g. \"/Game/Characters/BP_Hero\"). "
			 "Must be a single Blueprint asset, not a folder. Use blueprint_compile_batch for "
			 "batch / folder compilation."));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	// failOnWarnings - optional
	TSharedPtr<FJsonObject> FailOnWarningsProp = MakeShared<FJsonObject>();
	FailOnWarningsProp->SetStringField(TEXT("type"), TEXT("boolean"));
	FailOnWarningsProp->SetStringField(TEXT("description"),
		TEXT("Treat warnings as errors in the summary (default: false). Does not affect actual compilation."));
	Properties->SetObjectField(TEXT("failOnWarnings"), FailOnWarningsProp);

	// remove_unused - optional
	TSharedPtr<FJsonObject> RemoveUnusedProp = MakeShared<FJsonObject>();
	RemoveUnusedProp->SetStringField(TEXT("type"), TEXT("boolean"));
	RemoveUnusedProp->SetStringField(TEXT("description"),
		TEXT("Remove unused nodes and variables from the Blueprint before compiling. "
			 "RemoveUnusedNodes returns void; variable count is reported precisely. "
			 "Default: false."));
	Properties->SetObjectField(TEXT("remove_unused"), RemoveUnusedProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_BlueprintCompile::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Invalid arguments"));
	}

	// Reject array-shaped asset_path. The single-target tool accepts only a string;
	// callers wanting batch compilation must use blueprint_compile_batch.
	if (Arguments->HasTypedField<EJson::Array>(TEXT("asset_path")))
	{
		return MakeErrorResult(TEXT(
			"asset_path must be a single string. For batch compilation across multiple "
			"blueprints or content folders, use blueprint_compile_batch."));
	}

	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	// Reject paths that look like content folders (no Blueprint extension).
	// Use ClaireonPathResolver to canonicalize.
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		return MakeErrorResult(ResolveResult.Error);
	}
	const FString ObjectPath = ResolveResult.ResolvedPath.Path;

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
	if (!AssetData.IsValid() || !ClaireonBlueprintHelpers::IsBlueprintAssetClass(AssetData.AssetClassPath.GetAssetName().ToString()))
	{
		return MakeErrorResult(FString::Printf(
			TEXT("asset_path '%s' is not a Blueprint asset. For folder/batch compilation use blueprint_compile_batch."),
			*AssetPath));
	}

	const FString CanonicalPath = AssetData.GetObjectPathString();

	// Acquire per-asset lock for the compile.
	FClaireonScopedAssetLock Lock(CanonicalPath, GetName());
	if (!Lock.IsAcquired())
	{
		return Lock.GetError();
	}

	// Parse options
	bool bFailOnWarnings = false;
	if (Arguments->HasField(TEXT("failOnWarnings")))
	{
		bFailOnWarnings = Arguments->GetBoolField(TEXT("failOnWarnings"));
	}

	bool bRemoveUnused = false;
	if (Arguments->HasField(TEXT("remove_unused")))
	{
		bRemoveUnused = Arguments->GetBoolField(TEXT("remove_unused"));
	}

	const double StartTime = FPlatformTime::Seconds();

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *CanonicalPath);
	if (!Blueprint)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load Blueprint at %s"), *CanonicalPath));
	}

	// Optionally remove unused variables / nodes
	int32 RemovedVariables = 0;
	if (bRemoveUnused)
	{
		const int32 VariablesBefore = Blueprint->NewVariables.Num();
		UBlueprintEditorLibrary::RemoveUnusedVariables(Blueprint);
		RemovedVariables = VariablesBefore - Blueprint->NewVariables.Num();
		UBlueprintEditorLibrary::RemoveUnusedNodes(Blueprint);
	}

	// Compile with BatchCompile to suppress modal error dialogs that would
	// deadlock the game thread when called from the MCP HTTP handler.
	EBlueprintCompileOptions CompileOptions = EBlueprintCompileOptions::BatchCompile;
	FKismetEditorUtilities::CompileBlueprint(Blueprint, CompileOptions);

	const double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	bool bCompileSucceeded = (Blueprint->Status != BS_Error);
	if (bFailOnWarnings && Blueprint->Status == BS_UpToDateWithWarnings)
	{
		bCompileSucceeded = false;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("blueprint_path"), CanonicalPath);
	Data->SetStringField(TEXT("status"), bCompileSucceeded ? TEXT("succeeded") : TEXT("failed"));
	Data->SetArrayField(TEXT("errors"), TArray<TSharedPtr<FJsonValue>>());
	Data->SetArrayField(TEXT("warnings"), TArray<TSharedPtr<FJsonValue>>());
	Data->SetNumberField(TEXT("compile_time_ms"), ElapsedMs);
	if (bRemoveUnused && RemovedVariables > 0)
	{
		Data->SetNumberField(TEXT("removed_variables"), RemovedVariables);
	}

	const FString Summary = FString::Printf(
		TEXT("Compiled %s: %s (%.0f ms)"),
		*CanonicalPath,
		bCompileSucceeded ? TEXT("succeeded") : TEXT("failed"),
		ElapsedMs);

	if (!bCompileSucceeded)
	{
		IClaireonTool::FToolResult Result = MakeErrorResult(Summary);
		Result.Data = Data;
		return Result;
	}

	return MakeSuccessResult(Data, Summary);
}
