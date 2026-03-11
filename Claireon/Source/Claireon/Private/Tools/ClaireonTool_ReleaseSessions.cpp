// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_ReleaseSessions.h"
#include "ClaireonSessionManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FString ClaireonTool_ReleaseSessions::GetName() const
{
	return TEXT("mcp_release_sessions");
}

FString ClaireonTool_ReleaseSessions::GetDescription() const
{
	return TEXT("Force-release MCP editing sessions. Release a specific asset's session by path, "
				"or release all sessions at once. No dirty-state guard — always forces release.");
}

TSharedPtr<FJsonObject> ClaireonTool_ReleaseSessions::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// asset_path (optional)
	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"),
		TEXT("Asset path to release the session for (e.g. '/Game/Art/NS_MySystem'). "
			 "Releases the single session holding a lock on this asset."));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	// force_all (optional)
	TSharedPtr<FJsonObject> ForceAllProp = MakeShared<FJsonObject>();
	ForceAllProp->SetStringField(TEXT("type"), TEXT("boolean"));
	ForceAllProp->SetStringField(TEXT("description"),
		TEXT("If true, release ALL active sessions. Overrides asset_path. Default: false."));
	Properties->SetObjectField(TEXT("force_all"), ForceAllProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_ReleaseSessions::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	const bool bForceAll = Arguments->GetBoolField(TEXT("force_all"));

	if (bForceAll)
	{
		const int32 Count = FClaireonSessionManager::Get().ForceReleaseAll();

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("released_count"), Count);

		return MakeSuccessResult(Data,
			FString::Printf(TEXT("Force-released all sessions (%d released)"), Count));
	}

	const FString AssetPath = Arguments->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Either 'asset_path' or 'force_all=true' is required."));
	}

	const int32 Count = FClaireonSessionManager::Get().ReleaseByAssetPath(AssetPath);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("released_count"), Count);
	Data->SetStringField(TEXT("asset_path"), AssetPath);

	if (Count > 0)
	{
		return MakeSuccessResult(Data,
			FString::Printf(TEXT("Released session for asset '%s'"), *AssetPath));
	}

	return MakeSuccessResult(Data,
		FString::Printf(TEXT("No active session found for asset '%s'"), *AssetPath));
}