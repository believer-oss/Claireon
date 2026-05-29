// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_IsAssetEditorOpen.h"
#include "ClaireonPathResolver.h"
#include "ClaireonLog.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/IToolkit.h"
#include "UObject/Object.h"

FString ClaireonTool_IsAssetEditorOpen::GetCategory() const { return TEXT("editor"); }
FString ClaireonTool_IsAssetEditorOpen::GetOperation() const { return TEXT("is_asset_editor_open"); }

FString ClaireonTool_IsAssetEditorOpen::GetDescription() const
{
	return TEXT("Check whether an asset editor is currently open for the given asset. "
		"Returns is_open=true with the open-editor instance name when present. "
		"Use after open_asset (which defers to next tick) and wait_seconds to await editor presence.");
}

TSharedPtr<FJsonObject> ClaireonTool_IsAssetEditorOpen::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> PathProp = MakeShared<FJsonObject>();
	PathProp->SetStringField(TEXT("type"), TEXT("string"));
	PathProp->SetStringField(TEXT("description"),
		TEXT("Asset path to probe (e.g. /Game/UI/WBP_HUD)."));
	Properties->SetObjectField(TEXT("asset_path"), PathProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_IsAssetEditorOpen::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: asset_path"));
	}

	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		return MakeErrorResult(ResolveResult.Error);
	}
	AssetPath = ResolveResult.ResolvedPath.Path;

	if (!GEditor)
	{
		return MakeErrorResult(TEXT("Editor not available"));
	}
	UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!Subsystem)
	{
		return MakeErrorResult(TEXT("AssetEditorSubsystem not available"));
	}

	UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
	bool bIsOpen = false;
	if (Asset)
	{
		bIsOpen = Subsystem->FindEditorForAsset(Asset, /*bFocusIfOpen=*/ false) != nullptr;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetBoolField(TEXT("is_open"), bIsOpen);
	Data->SetBoolField(TEXT("asset_loaded"), Asset != nullptr);

	const FString Summary = FString::Printf(TEXT("Asset editor for '%s': %s"),
		*AssetPath, bIsOpen ? TEXT("open") : TEXT("closed"));
	return MakeSuccessResult(Data, Summary);
}
