// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_BlueprintCompileBatch.h"
#include "ClaireonBlueprintHelpers.h"
#include "ClaireonPathResolver.h"
#include "ClaireonLog.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "BlueprintEditorLibrary.h"
#include "Engine/Blueprint.h"
#include "Framework/Application/SlateApplication.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/SoftObjectPath.h"

FString ClaireonTool_BlueprintCompileBatch::GetCategory() const { return TEXT("blueprint"); }
FString ClaireonTool_BlueprintCompileBatch::GetOperation() const { return TEXT("compile_batch"); }

TArray<FString> ClaireonTool_BlueprintCompileBatch::GetSearchKeywords() const
{
	return {TEXT("bp"), TEXT("blueprint"), TEXT("compile"), TEXT("batch"), TEXT("build"), TEXT("validate"), TEXT("check"), TEXT("recompile")};
}

FString ClaireonTool_BlueprintCompileBatch::GetDescription() const
{
	return TEXT("Compile multiple Blueprints by asset path or content folder. Each entry in paths is auto-detected: "
		"a path that resolves to a Blueprint asset is compiled directly; a path that matches a content "
		"folder compiles all Blueprints under it recursively. Defaults to /Game (everything) when paths is omitted. "
		"Default max_count is 50 -- if more blueprints are found, returns immediately with the count. "
		"Pass max_count=0 for unlimited or raise the limit explicitly. "
		"Editor-wide tool: acquires an exclusive lock that blocks all other Claireon sessions for the duration of this call. "
		"Fails fast if any other session is currently held. Immediate-mode tool: no session required.");
}

TSharedPtr<FJsonObject> ClaireonTool_BlueprintCompileBatch::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// paths - optional array, each entry is either a Blueprint asset or a content folder
	TSharedPtr<FJsonObject> PathsProp = MakeShared<FJsonObject>();
	PathsProp->SetStringField(TEXT("type"), TEXT("array"));
	PathsProp->SetStringField(TEXT("description"),
		TEXT("Blueprint asset paths or content folder paths to compile. "
			 "Each entry is auto-detected: \"/Game/Characters/BP_Hero\" compiles one Blueprint; "
			 "\"/Game/Characters\" compiles all Blueprints under that folder recursively. "
			 "Defaults to [\"/Game\"] when omitted."));
	TSharedPtr<FJsonObject> PathsItems = MakeShared<FJsonObject>();
	PathsItems->SetStringField(TEXT("type"), TEXT("string"));
	PathsProp->SetObjectField(TEXT("items"), PathsItems);
	Properties->SetObjectField(TEXT("paths"), PathsProp);

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

	// max_count - optional, default 50
	TSharedPtr<FJsonObject> MaxCountProp = MakeShared<FJsonObject>();
	MaxCountProp->SetStringField(TEXT("type"), TEXT("integer"));
	MaxCountProp->SetStringField(TEXT("description"),
		TEXT("Maximum number of blueprints to compile in one call. "
			 "If the resolved list exceeds this, returns immediately with the count "
			 "so you can narrow paths or explicitly raise the limit. "
			 "Default: 50. Set to 0 for unlimited."));
	Properties->SetObjectField(TEXT("max_count"), MaxCountProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	return Schema;
}

// File-prefixed helpers (anon-namespace collisions under unity batching are
// avoided by giving these unique file-local names).
namespace
{
	// Compile a single Blueprint and return a per-asset result object.
	static TSharedPtr<FJsonObject> BatchCompileOneBlueprint(
		const FString& BlueprintPath,
		bool bRemoveUnused,
		bool bFailOnWarnings,
		int32& OutSucceeded,
		int32& OutFailed)
	{
		double StartTime = FPlatformTime::Seconds();

		UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
		if (!Blueprint)
		{
			OutFailed++;

			TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
			ResultObj->SetStringField(TEXT("blueprint_path"), BlueprintPath);
			ResultObj->SetStringField(TEXT("status"), TEXT("failed"));
			TArray<TSharedPtr<FJsonValue>> Errors;
			Errors.Add(MakeShared<FJsonValueString>(TEXT("Failed to load Blueprint")));
			ResultObj->SetArrayField(TEXT("errors"), Errors);
			ResultObj->SetArrayField(TEXT("warnings"), TArray<TSharedPtr<FJsonValue>>());
			ResultObj->SetNumberField(TEXT("compile_time_ms"), 0.0);
			return ResultObj;
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
		// deadlock the game thread when called from the MCP HTTP handler
		EBlueprintCompileOptions CompileOptions = EBlueprintCompileOptions::BatchCompile;
		FKismetEditorUtilities::CompileBlueprint(Blueprint, CompileOptions);

		double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

		bool bCompileSucceeded = (Blueprint->Status != BS_Error);
		if (bFailOnWarnings && Blueprint->Status == BS_UpToDateWithWarnings)
		{
			bCompileSucceeded = false;
		}

		if (bCompileSucceeded)
		{
			OutSucceeded++;
		}
		else
		{
			OutFailed++;
		}

		TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
		ResultObj->SetStringField(TEXT("blueprint_path"), BlueprintPath);
		ResultObj->SetStringField(TEXT("status"), bCompileSucceeded ? TEXT("succeeded") : TEXT("failed"));
		ResultObj->SetArrayField(TEXT("errors"), TArray<TSharedPtr<FJsonValue>>());
		ResultObj->SetArrayField(TEXT("warnings"), TArray<TSharedPtr<FJsonValue>>());
		ResultObj->SetNumberField(TEXT("compile_time_ms"), ElapsedMs);

		if (bRemoveUnused && RemovedVariables > 0)
		{
			ResultObj->SetNumberField(TEXT("removed_variables"), RemovedVariables);
		}

		return ResultObj;
	}

	// Resolve a single path entry: if it's a loadable Blueprint, return it directly;
	// if it's a folder, expand to all Blueprints under it.
	static void BatchResolvePath(const FString& ObjectPath, const FString& PackagePath, IAssetRegistry& AssetRegistry, TArray<FString>& OutBlueprintPaths)
	{
		// First, try to load as a direct Blueprint asset (uses object-path form).
		FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
		if (AssetData.IsValid() && ClaireonBlueprintHelpers::IsBlueprintAssetClass(AssetData.AssetClassPath.GetAssetName().ToString()))
		{
			OutBlueprintPaths.Add(AssetData.GetObjectPathString());
			return;
		}

		// Not a direct asset -- treat as a folder and scan recursively (uses package-prefix form).
		TArray<FAssetData> AssetList;
		AssetRegistry.GetAssetsByPath(FName(*PackagePath), AssetList, /*bRecursive=*/true);

		for (const FAssetData& Asset : AssetList)
		{
			if (ClaireonBlueprintHelpers::IsBlueprintAssetClass(Asset.AssetClassPath.GetAssetName().ToString()))
			{
				OutBlueprintPaths.Add(Asset.GetObjectPathString());
			}
		}
	}
}

IClaireonTool::FToolResult ClaireonTool_BlueprintCompileBatch::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Invalid arguments"));
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

	// Collect input paths (default: ["/Game"])
	// Parallel arrays: InputObjectPaths[i] is the object-path canonical form
	// (appended .AssetName when applicable), InputPackagePaths[i] is the
	// package-prefix form for folder-scan use.
	TArray<FString> InputObjectPaths;
	TArray<FString> InputPackagePaths;
	if (Arguments->HasField(TEXT("paths")))
	{
		const TArray<TSharedPtr<FJsonValue>>& PathsArray = Arguments->GetArrayField(TEXT("paths"));
		for (const TSharedPtr<FJsonValue>& Val : PathsArray)
		{
			auto ResolveResult = ClaireonPathResolver::Resolve(Val->AsString());
			if (ResolveResult.bSuccess)
			{
				InputObjectPaths.Add(ResolveResult.ResolvedPath.Path);
				InputPackagePaths.Add(ResolveResult.ResolvedPath.PackagePath);
			}
			else
			{
				UE_LOG(LogClaireon, Warning, TEXT("Skipping invalid path: %s"), *ResolveResult.Error);
			}
		}
	}
	if (InputObjectPaths.Num() == 0)
	{
		InputObjectPaths.Add(TEXT("/Game"));
		InputPackagePaths.Add(TEXT("/Game"));
	}

	// Resolve each input path to concrete Blueprint asset paths
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	TArray<FString> BlueprintPaths;
	for (int32 Idx = 0; Idx < InputObjectPaths.Num(); ++Idx)
	{
		BatchResolvePath(InputObjectPaths[Idx], InputPackagePaths[Idx], AssetRegistry, BlueprintPaths);
	}

	FString SourceDescription = FString::Join(InputObjectPaths, TEXT(", "));

	int32 Total = BlueprintPaths.Num();

	if (Total == 0)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("total"), 0);
		Data->SetNumberField(TEXT("succeeded"), 0);
		Data->SetNumberField(TEXT("failed"), 0);
		Data->SetArrayField(TEXT("results"), TArray<TSharedPtr<FJsonValue>>());
		Data->SetStringField(TEXT("source"), SourceDescription);
		return MakeSuccessResult(Data, FString::Printf(TEXT("No blueprints found for: %s"), *SourceDescription));
	}

	// Parse max_count (default 50, 0 = unlimited)
	int32 MaxCount = 50;
	if (Arguments->HasField(TEXT("max_count")))
	{
		MaxCount = static_cast<int32>(Arguments->GetNumberField(TEXT("max_count")));
	}

	// Cap check: if the resolved list exceeds max_count, return immediately
	if (MaxCount > 0 && Total > MaxCount)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("status"), TEXT("capped"));
		Data->SetStringField(TEXT("source"), SourceDescription);
		Data->SetNumberField(TEXT("total_found"), Total);
		Data->SetNumberField(TEXT("max_count"), MaxCount);
		Data->SetNumberField(TEXT("compiled"), 0);
		Data->SetStringField(TEXT("hint"),
			FString::Printf(TEXT("Found %d blueprints. Pass max_count=%d to compile all, or use narrower paths."), Total, Total));
		return MakeSuccessResult(Data,
			FString::Printf(TEXT("Capped: %d blueprints found, max_count=%d. None compiled. Raise max_count or narrow paths."), Total, MaxCount));
	}

	// Compile each Blueprint, yielding after every compile to keep the editor alive
	TArray<TSharedPtr<FJsonValue>> ResultsArray;
	int32 NumSucceeded = 0;
	int32 NumFailed = 0;

	for (int32 i = 0; i < Total; ++i)
	{
		TSharedPtr<FJsonObject> Result = BatchCompileOneBlueprint(BlueprintPaths[i], bRemoveUnused, bFailOnWarnings, NumSucceeded, NumFailed);
		ResultsArray.Add(MakeShared<FJsonValueObject>(Result));

		// Tick the editor thread after every compile to keep the editor responsive
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().PumpMessages();
		}

		// Progress log every 50 blueprints and at completion
		if ((i + 1) % 50 == 0 || i + 1 == Total)
		{
			UE_LOG(LogClaireon, Display, TEXT("[blueprint_compile_batch] %d / %d compiled (%d ok, %d failed)"),
				i + 1, Total, NumSucceeded, NumFailed);
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("source"), SourceDescription);
	Data->SetNumberField(TEXT("total"), Total);
	Data->SetNumberField(TEXT("succeeded"), NumSucceeded);
	Data->SetNumberField(TEXT("failed"), NumFailed);
	Data->SetArrayField(TEXT("results"), ResultsArray);

	FString Summary = FString::Printf(
		TEXT("Compiled %d blueprint(s): %d succeeded, %d failed"),
		Total,
		NumSucceeded,
		NumFailed);

	return MakeSuccessResult(Data, Summary);
}
