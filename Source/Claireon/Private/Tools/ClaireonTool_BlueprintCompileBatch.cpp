// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_BlueprintCompileBatch.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase.h" // kBPCategory
#include "ClaireonBlueprintHelpers.h"
#include "ClaireonPathResolver.h"
#include "ClaireonLog.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "BlueprintEditorLibrary.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Framework/Application/SlateApplication.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/UObjectToken.h"
#include "UObject/SoftObjectPath.h"

FString ClaireonTool_BlueprintCompileBatch::GetCategory() const { return kBPCategory; }
FString ClaireonTool_BlueprintCompileBatch::GetOperation() const { return TEXT("compile_batch"); }

TArray<FString> ClaireonTool_BlueprintCompileBatch::GetSearchKeywords() const
{
	return {TEXT("bp"), TEXT("blueprint"), TEXT("compile"), TEXT("batch"), TEXT("build"), TEXT("validate"), TEXT("check"), TEXT("recompile")};
}

FString ClaireonTool_BlueprintCompileBatch::GetDescription() const
{
	return TEXT("Compile multiple Blueprints by asset path or content folder: each paths entry auto-detects, a folder "
		"compiling every Blueprint under it recursively. Defaults to all of /Game when paths is omitted. max_count "
		"defaults to 50 -- past that only the count is returned; pass 0 for unlimited. Immediate-mode: needs no "
		"session, but takes an editor-wide lock and fails if any session is open.");
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
namespace ClaireonTool_BlueprintCompileBatch_Private
{
	// Extract the source graph-node name from a compiler message's tokens, if
	// the message carries a node reference. Returns empty when no node token is
	// present (e.g. Blueprint-level messages).
	static FString BatchCompileGetMessageSourceNodeName(const TSharedRef<FTokenizedMessage>& Message)
	{
		for (const TSharedRef<IMessageToken>& Token : Message->GetMessageTokens())
		{
			if (Token->GetType() != EMessageToken::Object)
			{
				continue;
			}
			const TSharedRef<FUObjectToken> ObjectToken = StaticCastSharedRef<FUObjectToken>(Token);
			if (const UEdGraphNode* SourceNode = Cast<UEdGraphNode>(ObjectToken->GetObject().Get()); IsValid(SourceNode))
			{
				return SourceNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
			}
		}
		return FString();
	}

	// Render one compiler message as "text [node: <name>]", attaching the
	// source node name when the message carries a node token and the rendered
	// text does not already include it.
	static FString BatchCompileFormatMessage(const TSharedRef<FTokenizedMessage>& Message)
	{
		FString MessageText = Message->ToText().ToString();
		const FString SourceNodeName = BatchCompileGetMessageSourceNodeName(Message);
		if (!SourceNodeName.IsEmpty() && !MessageText.Contains(SourceNodeName))
		{
			MessageText += FString::Printf(TEXT(" [node: %s]"), *SourceNodeName);
		}
		return MessageText;
	}

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
		if (!IsValid(Blueprint))
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
		// deadlock the game thread when called from the MCP HTTP handler.
		// FCompilerResultsLog captures per-message errors/warnings for the payload;
		// without it a failing compile reported errors=[] and callers had to scrape
		// the UE log.
		FCompilerResultsLog ResultsLog;
		ResultsLog.SetSourcePath(Blueprint->GetPathName());
		ResultsLog.BeginEvent(TEXT("BatchCompile"));
		EBlueprintCompileOptions CompileOptions = EBlueprintCompileOptions::BatchCompile;
		FKismetEditorUtilities::CompileBlueprint(Blueprint, CompileOptions, &ResultsLog);
		ResultsLog.EndEvent();

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

		TArray<TSharedPtr<FJsonValue>> Errors;
		TArray<TSharedPtr<FJsonValue>> Warnings;
		for (const TSharedRef<FTokenizedMessage>& Message : ResultsLog.Messages)
		{
			const FString MessageText = BatchCompileFormatMessage(Message);
			switch (Message->GetSeverity())
			{
			case EMessageSeverity::Error:
				Errors.Add(MakeShared<FJsonValueString>(MessageText));
				break;
			case EMessageSeverity::Warning:
			case EMessageSeverity::PerformanceWarning:
				Warnings.Add(MakeShared<FJsonValueString>(MessageText));
				break;
			default:
				break;
			}
		}

		// Fail loudly: a BS_Error compile must never ship an empty errors[].
		// (A failOnWarnings-induced failure legitimately has its text in
		// warnings[] instead, so only the hard-error status gets the fallback.)
		if (Blueprint->Status == BS_Error && Errors.Num() == 0)
		{
			Errors.Add(MakeShared<FJsonValueString>(FString::Printf(
				TEXT("Compile failed for %s but the compiler log captured no error messages; see the editor log for details"),
				*BlueprintPath)));
		}

		TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
		ResultObj->SetStringField(TEXT("blueprint_path"), BlueprintPath);
		ResultObj->SetStringField(TEXT("status"), bCompileSucceeded ? TEXT("succeeded") : TEXT("failed"));
		ResultObj->SetArrayField(TEXT("errors"), Errors);
		ResultObj->SetArrayField(TEXT("warnings"), Warnings);
		ResultObj->SetNumberField(TEXT("error_count"), Errors.Num());
		ResultObj->SetNumberField(TEXT("warning_count"), Warnings.Num());
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
using namespace ClaireonTool_BlueprintCompileBatch_Private;

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
		FToolResult Capped = MakeSuccessResult(Data,
			FString::Printf(TEXT("Capped: %d blueprints found, max_count=%d. None compiled. Raise max_count or narrow paths."), Total, MaxCount));

		// Migrated off the retired Data.hint string convention onto the structured channel.
		// args echoes the original call with the correction applied, so it stays directly
		// callable -- a bare {max_count} delta would re-issue without 'paths'.
		//
		// NOT latched: unlike the log-filter hints, this fires only when a cap was actually
		// hit, which is a real per-call condition carrying a call-specific count.
		TSharedPtr<FJsonObject> RetryArgs = CloneHintArgs(Arguments);
		RetryArgs->SetNumberField(TEXT("max_count"), Total);
		Capped.Hint = MakeGuidanceHint(GetName(),
			FString::Printf(
				TEXT("max_count=%d capped a set of %d blueprints, so none were compiled. ")
				TEXT("Re-issue with max_count=%d to compile all of them, or narrow 'paths' to a smaller set."),
				MaxCount, Total, Total),
			RetryArgs);
		return Capped;
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
