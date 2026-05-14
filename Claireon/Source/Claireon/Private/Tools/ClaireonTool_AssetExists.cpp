// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_AssetExists.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder
#include "ClaireonSessionManager.h"

#include "Dom/JsonObject.h"
#include "EditorAssetLibrary.h"

FString ClaireonTool_AssetExists::GetCategory() const { return TEXT("asset"); }
FString ClaireonTool_AssetExists::GetOperation() const { return TEXT("exists"); }

FString ClaireonTool_AssetExists::GetDescription() const
{
	return TEXT("Thin wrapper over UEditorAssetLibrary::DoesAssetExist. Returns whether the given /Game/ path holds an asset.");
}

TSharedPtr<FJsonObject> ClaireonTool_AssetExists::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("/Game/ path to check"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_AssetExists::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Arguments object missing"));
	}

	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	const FString Canon = FClaireonSessionManager::CanonicalizePath(AssetPath);
	if (Canon.IsEmpty())
	{
		return MakeErrorResult(TEXT("Invalid asset path (must start with /Game/)"));
	}

	const bool bExists = UEditorAssetLibrary::DoesAssetExist(Canon);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("exists"), bExists);
	Data->SetStringField(TEXT("asset_path"), Canon);

	const FString Summary = FString::Printf(TEXT("%s %s"), bExists ? TEXT("Found") : TEXT("Missing"), *Canon);
	return MakeSuccessResult(Data, Summary);
}
