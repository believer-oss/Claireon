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
#include "UObject/SoftObjectPath.h"

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
	return TEXT("Compile Blueprints by asset path or content folder. Each entry in paths is auto-detected: "
		"a path that resolves to a Blueprint asset is compiled directly; a path that matches a content "
		"folder compiles all Blueprints under it recursively. Defaults to /Game (everything) when paths is omitted.");
}

TSharedPtr<FJsonObject> ClaireonTool_BlueprintCompile::GetInputSchema() const
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

	Schema->SetObjectField(TEXT("properties"), Properties);

	return Schema;
}

// Helper: compile a single Blueprint and return a per-asset result object.
static TSharedPtr<FJsonObject> CompileOneBlueprint(
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

// Normalize a user-supplied path into a /Game/... content path.
// Accepts: "/Game/Foo/BP_Bar", "Game/Foo/BP_Bar", "Content/Foo/BP_Bar",
//          "D:/proj/Content/Foo/BP_Bar", or relative "Foo/BP_Bar" (treated as /Game/Foo/BP_Bar).
static FString NormalizeToContentPath(const FString& InPath)
{
	FString Path = InPath;
	Path.TrimStartAndEndInline();
	FPaths::NormalizeDirectoryName(Path);

	// Already a content path
	if (Path.StartsWith(TEXT("/Game/")) || Path == TEXT("/Game"))
	{
		return Path;
	}

	// Strip leading slash for easier matching
	if (Path.StartsWith(TEXT("/")))
	{
		// Could be /Engine, /Script, etc. — pass through as-is
		return Path;
	}

	// Absolute filesystem path — find "Content/" and map to /Game/
	int32 ContentIdx = Path.Find(TEXT("Content/"), ESearchCase::IgnoreCase);
	if (ContentIdx != INDEX_NONE)
	{
		FString Remainder = Path.Mid(ContentIdx + 8); // skip "Content/"
		return TEXT("/Game/") + Remainder;
	}
	// Also handle "Content" at the very end (folder path)
	if (Path.EndsWith(TEXT("Content"), ESearchCase::IgnoreCase))
	{
		return TEXT("/Game");
	}

	// "Game/Foo/Bar" without leading slash
	if (Path.StartsWith(TEXT("Game/"), ESearchCase::IgnoreCase))
	{
		return TEXT("/") + Path;
	}

	// Bare relative path — assume relative to /Game/
	return TEXT("/Game/") + Path;
}

static bool IsBlueprintAssetClass(const FString& ClassName)
{
	return ClassName == TEXT("Blueprint")
		|| ClassName == TEXT("AnimBlueprint")
		|| ClassName == TEXT("WidgetBlueprint");
}

// Resolve a single path entry: if it's a loadable Blueprint, return it directly;
// if it's a folder, expand to all Blueprints under it.
static void ResolvePath(const FString& Path, IAssetRegistry& AssetRegistry, TArray<FString>& OutBlueprintPaths)
{
	// First, try to load as a direct Blueprint asset
	FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(Path));
	if (AssetData.IsValid() && IsBlueprintAssetClass(AssetData.AssetClassPath.GetAssetName().ToString()))
	{
		OutBlueprintPaths.Add(AssetData.GetObjectPathString());
		return;
	}

	// Not a direct asset — treat as a folder and scan recursively
	TArray<FAssetData> AssetList;
	AssetRegistry.GetAssetsByPath(FName(*Path), AssetList, /*bRecursive=*/true);

	for (const FAssetData& Asset : AssetList)
	{
		if (IsBlueprintAssetClass(Asset.AssetClassPath.GetAssetName().ToString()))
		{
			OutBlueprintPaths.Add(Asset.GetObjectPathString());
		}
	}
}

IClaireonTool::FToolResult ClaireonTool_BlueprintCompile::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
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
	TArray<FString> InputPaths;
	if (Arguments->HasField(TEXT("paths")))
	{
		const TArray<TSharedPtr<FJsonValue>>& PathsArray = Arguments->GetArrayField(TEXT("paths"));
		for (const TSharedPtr<FJsonValue>& Val : PathsArray)
		{
			FString Normalized = NormalizeToContentPath(Val->AsString());
			if (!Normalized.IsEmpty())
			{
				InputPaths.Add(Normalized);
			}
		}
	}
	if (InputPaths.Num() == 0)
	{
		InputPaths.Add(TEXT("/Game"));
	}

	// Resolve each input path to concrete Blueprint asset paths
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	TArray<FString> BlueprintPaths;
	for (const FString& Path : InputPaths)
	{
		ResolvePath(Path, AssetRegistry, BlueprintPaths);
	}

	FString SourceDescription = FString::Join(InputPaths, TEXT(", "));

	if (BlueprintPaths.Num() == 0)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("total"), 0);
		Data->SetNumberField(TEXT("succeeded"), 0);
		Data->SetNumberField(TEXT("failed"), 0);
		Data->SetArrayField(TEXT("results"), TArray<TSharedPtr<FJsonValue>>());
		Data->SetStringField(TEXT("source"), SourceDescription);
		return MakeSuccessResult(Data, FString::Printf(TEXT("No blueprints found for: %s"), *SourceDescription));
	}

	// Compile each Blueprint
	TArray<TSharedPtr<FJsonValue>> ResultsArray;
	int32 NumSucceeded = 0;
	int32 NumFailed = 0;

	for (const FString& Path : BlueprintPaths)
	{
		TSharedPtr<FJsonObject> Result = CompileOneBlueprint(Path, bRemoveUnused, bFailOnWarnings, NumSucceeded, NumFailed);
		ResultsArray.Add(MakeShared<FJsonValueObject>(Result));
	}

	int32 Total = BlueprintPaths.Num();
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
