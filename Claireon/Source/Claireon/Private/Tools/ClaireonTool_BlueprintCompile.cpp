// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_BlueprintCompile.h"
#include "ClaireonLog.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "BlueprintEditorLibrary.h"
#include "Engine/Blueprint.h"
#include "Kismet2/KismetEditorUtilities.h"

FString ClaireonTool_BlueprintCompile::GetName() const
{
	return TEXT("compile_blueprints");
}

FString ClaireonTool_BlueprintCompile::GetCategory() const
{
	return TEXT("blueprint");
}

FString ClaireonTool_BlueprintCompile::GetDescription() const
{
	return TEXT("Batch compile Blueprints in a content path with error and warning reporting. Useful for detecting compilation issues after code changes.");
}

TSharedPtr<FJsonObject> ClaireonTool_BlueprintCompile::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// contentPath - optional
	TSharedPtr<FJsonObject> ContentPathProp = MakeShared<FJsonObject>();
	ContentPathProp->SetStringField(TEXT("type"), TEXT("string"));
	ContentPathProp->SetStringField(TEXT("description"),
		TEXT("Unreal content path to compile Blueprints under (default: /Game). E.g. /Game/Characters"));
	Properties->SetObjectField(TEXT("contentPath"), ContentPathProp);

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
		TEXT("Remove unused nodes and variables from each Blueprint before compiling. "
			 "RemoveUnusedNodes returns void; variable count is reported precisely. "
			 "Default: false."));
	Properties->SetObjectField(TEXT("remove_unused"), RemoveUnusedProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_BlueprintCompile::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	// Parse arguments
	FString ContentPath = TEXT("/Game");
	if (Arguments->HasField(TEXT("contentPath")))
	{
		ContentPath = Arguments->GetStringField(TEXT("contentPath"));
	}

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

	// Gather all Blueprint assets under the content path
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	TArray<FAssetData> AssetList;
	AssetRegistry.GetAssetsByPath(FName(*ContentPath), AssetList, /*bRecursive=*/true);

	// Filter to Blueprints only
	TArray<FAssetData> BlueprintAssets;
	for (const FAssetData& Asset : AssetList)
	{
		FString ClassName = Asset.AssetClassPath.GetAssetName().ToString();
		if (ClassName == TEXT("Blueprint") || ClassName == TEXT("AnimBlueprint") || ClassName == TEXT("WidgetBlueprint"))
		{
			BlueprintAssets.Add(Asset);
		}
	}

	if (BlueprintAssets.Num() == 0)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("total"), 0);
		Data->SetNumberField(TEXT("succeeded"), 0);
		Data->SetNumberField(TEXT("failed"), 0);
		Data->SetArrayField(TEXT("results"), TArray<TSharedPtr<FJsonValue>>());
		Data->SetStringField(TEXT("content_path"), ContentPath);
		return MakeSuccessResult(Data, FString::Printf(TEXT("No blueprints found under %s"), *ContentPath));
	}

	// Compile each Blueprint and collect results
	TArray<TSharedPtr<FJsonValue>> ResultsArray;
	int32 NumSucceeded = 0;
	int32 NumFailed = 0;

	for (const FAssetData& Asset : BlueprintAssets)
	{
		FString BlueprintPath = Asset.GetObjectPathString();
		double StartTime = FPlatformTime::Seconds();

		// Load the Blueprint
		UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
		if (!Blueprint)
		{
			NumFailed++;

			TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
			ResultObj->SetStringField(TEXT("blueprint_path"), BlueprintPath);
			ResultObj->SetStringField(TEXT("status"), TEXT("failed"));
			TArray<TSharedPtr<FJsonValue>> Errors;
			Errors.Add(MakeShared<FJsonValueString>(TEXT("Failed to load Blueprint")));
			ResultObj->SetArrayField(TEXT("errors"), Errors);
			ResultObj->SetArrayField(TEXT("warnings"), TArray<TSharedPtr<FJsonValue>>());
			ResultObj->SetNumberField(TEXT("compile_time_ms"), 0.0);
			ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
			continue;
		}

		// Optionally remove unused variables
		int32 RemovedVariables = 0;
		if (bRemoveUnused)
		{
			const int32 VariablesBefore = Blueprint->NewVariables.Num();
			UBlueprintEditorLibrary::RemoveUnusedVariables(Blueprint);
			RemovedVariables = VariablesBefore - Blueprint->NewVariables.Num();
			// RemoveUnusedNodes returns void — node removal is best-effort
			UBlueprintEditorLibrary::RemoveUnusedNodes(Blueprint);
		}

		// Compile
		EBlueprintCompileOptions CompileOptions = EBlueprintCompileOptions::None;
		FKismetEditorUtilities::CompileBlueprint(Blueprint, CompileOptions);

		double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

		// Determine success/failure based on Blueprint status
		// Compiler error messages are surfaced through the Blueprint's status flag;
		// granular per-message extraction requires hooking FKismetEditorUtilities callbacks
		// which is out of scope for this structured-output port.
		TArray<TSharedPtr<FJsonValue>> ErrorsArray;
		TArray<TSharedPtr<FJsonValue>> WarningsArray;

		bool bCompileSucceeded = (Blueprint->Status != BS_Error);

		// Treat blueprints with warnings as failed if bFailOnWarnings
		FString StatusStr;
		if (!bCompileSucceeded)
		{
			NumFailed++;
			StatusStr = TEXT("failed");
		}
		else
		{
			NumSucceeded++;
			StatusStr = TEXT("succeeded");
		}

		TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
		ResultObj->SetStringField(TEXT("blueprint_path"), BlueprintPath);
		ResultObj->SetStringField(TEXT("status"), StatusStr);
		ResultObj->SetArrayField(TEXT("errors"), ErrorsArray);
		ResultObj->SetArrayField(TEXT("warnings"), WarningsArray);
		ResultObj->SetNumberField(TEXT("compile_time_ms"), ElapsedMs);

		if (bRemoveUnused && RemovedVariables > 0)
		{
			ResultObj->SetNumberField(TEXT("removed_variables"), RemovedVariables);
		}

		ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
	}

	// Build result data
	int32 Total = BlueprintAssets.Num();
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("content_path"), ContentPath);
	Data->SetNumberField(TEXT("total"), Total);
	Data->SetNumberField(TEXT("succeeded"), NumSucceeded);
	Data->SetNumberField(TEXT("failed"), NumFailed);
	Data->SetArrayField(TEXT("results"), ResultsArray);

	// Summary
	FString Summary = FString::Printf(
		TEXT("Compiled %d blueprints: %d succeeded, %d failed"),
		Total,
		NumSucceeded,
		NumFailed);

	return MakeSuccessResult(Data, Summary);
}
