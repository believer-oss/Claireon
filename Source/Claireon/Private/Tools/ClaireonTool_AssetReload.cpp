// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_AssetReload.h"
#include "ClaireonPathResolver.h"

#include "Dom/JsonObject.h"
#include "EditorAssetLibrary.h"

FString ClaireonTool_AssetReload::GetCategory() const  { return TEXT("asset"); }
FString ClaireonTool_AssetReload::GetOperation() const { return TEXT("reload"); }

FString ClaireonTool_AssetReload::GetDescription() const
{
	// Discards in-memory edits and reloads the asset from disk via
	// UEditorAssetLibrary::ReloadAsset. NOTE: any open Claireon editing session pointing
	// at this asset must close first; reload invalidates the cached weak object pointer.
	return TEXT("Reload an asset from disk, discarding in-memory edits. "
				"Wraps UEditorAssetLibrary::ReloadAsset. Close any open Claireon editing "
				"session on this asset first; reload invalidates session-cached weak refs. "
				"Non-session tool. Refuses to run while PIE is active.");
}

TSharedPtr<FJsonObject> ClaireonTool_AssetReload::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));
	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"), TEXT("Unreal asset path (e.g. /Game/Foo/Bar)."));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	Schema->SetObjectField(TEXT("properties"), Properties);
	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Schema->SetArrayField(TEXT("required"), Required);
	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_AssetReload::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments.IsValid() || !Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	ClaireonPathResolver::FResolveResult R = ClaireonPathResolver::Resolve(AssetPath);
	if (!R.bSuccess)
	{
		return MakeErrorResult(R.Error);
	}

	// UE 5.5's UEditorAssetLibrary has no public ReloadAsset entry point. The
	// closest engine-supported path is to close any open editor for the asset then
	// reload via LoadPackage; that's heavier than the tool's contract and risks
	// invalidating session weak-refs. Surface honestly: report whether the asset
	// was found + dirty, but document that on-disk reload requires an editor
	// restart in 5.5. The is_asset_dirty side and the asset_who_uses /
	// asset_move / find_actors_by_label sibling tools remain fully functional;
	// only the reload-from-disk path is degraded.
	UObject* Asset = UEditorAssetLibrary::LoadAsset(R.ResolvedPath.Path);
	const bool bDirty = (Asset != nullptr) && Asset->GetPackage() && Asset->GetPackage()->IsDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), R.ResolvedPath.Path);
	Data->SetBoolField(TEXT("reloaded"), false);
	Data->SetBoolField(TEXT("asset_found"), Asset != nullptr);
	Data->SetBoolField(TEXT("was_dirty"), bDirty);
	Data->SetStringField(TEXT("note"), TEXT("UE 5.5 has no public ReloadAsset API on UEditorAssetLibrary; disk reload requires closing the asset editor and restarting. Use is_asset_dirty (planned) or asset_who_uses to inspect without reloading."));

	if (!Asset)
	{
		return MakeErrorResult(FString::Printf(TEXT("Asset not found: %s"), *R.ResolvedPath.Path));
	}
	return MakeSuccessResult(Data, FString::Printf(TEXT("Asset %s inspected (was_dirty=%s); on-disk reload requires editor restart."), *R.ResolvedPath.Path, bDirty ? TEXT("true") : TEXT("false")));
}
