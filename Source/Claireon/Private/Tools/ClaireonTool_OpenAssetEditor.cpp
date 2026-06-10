// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_OpenAssetEditor.h"
#include "ClaireonPathResolver.h"
#include "ClaireonLog.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Containers/Ticker.h"

FString ClaireonTool_OpenAssetEditor::GetOperation() const { return TEXT("open_asset"); }

FString ClaireonTool_OpenAssetEditor::GetCategory() const
{
	return TEXT("editor");
}

FString ClaireonTool_OpenAssetEditor::GetDescription() const
{
	return TEXT("Safely open asset editor(s) for one or more assets. "
		"Uses deferred execution to avoid crashes from calling AssetEditorSubsystem directly.");
}

TSharedPtr<FJsonObject> ClaireonTool_OpenAssetEditor::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// asset_path - required (string or array of strings)
	TSharedPtr<FJsonObject> PathProp = MakeShared<FJsonObject>();
	PathProp->SetStringField(TEXT("description"),
		TEXT("Asset path(s) to open in the editor. Can be a single string or an array of strings. "
			 "Example: /Game/Characters/BP_Hero"));
	Properties->SetObjectField(TEXT("asset_path"), PathProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_OpenAssetEditor::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!GEditor)
	{
		return MakeErrorResult(TEXT("Editor not available"));
	}

	// Collect asset paths (support both string and array)
	TArray<FString> AssetPaths;
	if (Arguments->HasTypedField<EJson::Array>(TEXT("asset_path")))
	{
		const TArray<TSharedPtr<FJsonValue>>& PathArray = Arguments->GetArrayField(TEXT("asset_path"));
		for (const TSharedPtr<FJsonValue>& Val : PathArray)
		{
			FString Path;
			if (Val->TryGetString(Path) && !Path.IsEmpty())
			{
				AssetPaths.Add(Path);
			}
		}
	}
	else
	{
		FString SinglePath;
		if (Arguments->TryGetStringField(TEXT("asset_path"), SinglePath) && !SinglePath.IsEmpty())
		{
			AssetPaths.Add(SinglePath);
		}
	}

	if (AssetPaths.Num() == 0)
	{
		return MakeErrorResult(TEXT("Missing required field: asset_path"));
	}

	// Resolve all paths
	for (FString& Path : AssetPaths)
	{
		auto ResolveResult = ClaireonPathResolver::Resolve(Path);
		if (ResolveResult.bSuccess)
		{
			Path = ResolveResult.ResolvedPath.Path;
		}
	}

	// Validate all assets exist before deferring
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FString> ValidPaths;
	TArray<FString> InvalidPaths;

	for (const FString& Path : AssetPaths)
	{
		FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(Path));
		if (AssetData.IsValid())
		{
			ValidPaths.Add(Path);
		}
		else
		{
			InvalidPaths.Add(Path);
		}
	}

	if (ValidPaths.Num() == 0)
	{
		return MakeErrorResult(FString::Printf(TEXT("No valid assets found: %s"), *FString::Join(InvalidPaths, TEXT(", "))));
	}

	// Defer the editor open to next tick to avoid crashes in FMRUList::AddMRUItem
	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([ValidPaths](float)
		{
			UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
			if (Subsystem)
			{
				for (const FString& Path : ValidPaths)
				{
					UObject* Asset = LoadObject<UObject>(nullptr, *Path);
					if (Asset)
					{
						Subsystem->OpenEditorForAsset(Asset);
					}
				}
			}
			return false; // one-shot
		}), 0.0f);

	// Build result
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("status"), TEXT("deferred"));

	TArray<TSharedPtr<FJsonValue>> OpenedArray;
	for (const FString& Path : ValidPaths)
	{
		OpenedArray.Add(MakeShared<FJsonValueString>(Path));
	}
	Data->SetArrayField(TEXT("assets"), OpenedArray);

	FString Summary = FString::Printf(TEXT("Opening %d asset editor(s) — deferred to next tick"), ValidPaths.Num());
	if (InvalidPaths.Num() > 0)
	{
		Summary += FString::Printf(TEXT(". %d asset(s) not found: %s"), InvalidPaths.Num(), *FString::Join(InvalidPaths, TEXT(", ")));
	}

	return MakeSuccessResult(Data, Summary);
}
