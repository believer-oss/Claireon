// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_EditorCloseAsset.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "ClaireonLog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/SoftObjectPath.h"

namespace ClaireonEditorCloseAssetInternal
{

	/**
	 * Find sessions whose bound editor would close. A dead binding cannot be rebound
	 * within that session, so require force before closing an affected window.
	 */
	TArray<FMCPSession> SessionsForAssets(const TSet<FString>& AssetPaths, bool bAll)
	{
		TArray<FMCPSession> Affected;
		for (const FMCPSession& Session : FClaireonSessionManager::Get().ListSessions())
		{
			if (bAll || AssetPaths.Contains(Session.AssetPath))
			{
				Affected.Add(Session);
			}
		}
		return Affected;
	}
}

FString ClaireonTool_EditorCloseAsset::GetOperation() const { return TEXT("close_asset"); }

FString ClaireonTool_EditorCloseAsset::GetCategory() const
{
	return TEXT("editor");
}

TArray<FString> ClaireonTool_EditorCloseAsset::GetSearchKeywords() const
{
	return {TEXT("close"), TEXT("asset"), TEXT("editor"), TEXT("window"), TEXT("all"),
			TEXT("tab"), TEXT("focus"), TEXT("cleanup"), TEXT("blueprintassist"),
			TEXT("wrong_graph_focused")};
}

FString ClaireonTool_EditorCloseAsset::GetDescription() const
{
	return TEXT("Close open asset editor windows: name them with asset_path (a string or an array), or pass "
		"all=true to close every one. Counterpart to editor_open_asset; opens no session and discards "
		"nothing, so a dirty package stays dirty. Refuses while a Claireon editing session is bound to an "
		"affected asset unless force=true. Clears the competing windows behind bp_format's "
		"wrong_graph_focused.");
}

TSharedPtr<FJsonObject> ClaireonTool_EditorCloseAsset::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// Execute also accepts arrays; the schema currently declares only the common string form.
	TSharedPtr<FJsonObject> PathProp = MakeShared<FJsonObject>();
	PathProp->SetStringField(TEXT("type"), TEXT("string"));
	PathProp->SetStringField(TEXT("description"),
		TEXT("Asset path(s) whose editor windows should close. A single string or an array of strings. "
			 "Omit only when all=true. Example: /Game/Characters/BP_Hero"));
	Properties->SetObjectField(TEXT("asset_path"), PathProp);

	TSharedPtr<FJsonObject> AllProp = MakeShared<FJsonObject>();
	AllProp->SetStringField(TEXT("type"), TEXT("boolean"));
	AllProp->SetStringField(TEXT("description"),
		TEXT("Close EVERY open asset editor instead of named ones. Mutually exclusive with asset_path."));
	Properties->SetObjectField(TEXT("all"), AllProp);

	TSharedPtr<FJsonObject> ForceProp = MakeShared<FJsonObject>();
	ForceProp->SetStringField(TEXT("type"), TEXT("boolean"));
	ForceProp->SetStringField(TEXT("description"),
		TEXT("Close even when a Claireon editing session is bound to an affected asset. Those sessions "
			 "survive as ids but their editor binding dies, so close and reopen them afterwards."));
	Properties->SetObjectField(TEXT("force"), ForceProp);

	Schema->SetObjectField(TEXT("properties"), Properties);
	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_EditorCloseAsset::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!IsValid(GEditor))
	{
		return MakeErrorResult(TEXT("Editor not available"));
	}

	UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!IsValid(Subsystem))
	{
		return MakeErrorResult(TEXT("AssetEditorSubsystem not available"));
	}

	bool bAll = false;
	bool bForce = false;
	if (Arguments.IsValid())
	{
		Arguments->TryGetBoolField(TEXT("all"), bAll);
		Arguments->TryGetBoolField(TEXT("force"), bForce);
	}

	TArray<FString> AssetPaths;
	if (Arguments.IsValid())
	{
		if (Arguments->HasTypedField<EJson::Array>(TEXT("asset_path")))
		{
			for (const TSharedPtr<FJsonValue>& Val : Arguments->GetArrayField(TEXT("asset_path")))
			{
				FString Path;
				if (Val.IsValid() && Val->TryGetString(Path) && !Path.IsEmpty())
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
	}

	if (bAll && AssetPaths.Num() > 0)
	{
		return MakeErrorResult(TEXT(
			"all=true and asset_path are mutually exclusive. Pass asset_path to close named editors, "
			"or all=true to close every one. Nothing was closed."));
	}
	if (!bAll && AssetPaths.Num() == 0)
	{
		return MakeErrorResult(TEXT(
			"Nothing to close: supply asset_path (a string or an array of strings), or all=true to close "
			"every open asset editor."));
	}

	for (FString& Path : AssetPaths)
	{
		ClaireonPathResolver::FResolveResult ResolveResult = ClaireonPathResolver::Resolve(Path);
		if (ResolveResult.bSuccess)
		{
			Path = ResolveResult.ResolvedPath.Path;
		}
	}

	const TSet<FString> PathSet(AssetPaths);
	const TArray<FMCPSession> Affected = ClaireonEditorCloseAssetInternal::SessionsForAssets(PathSet, bAll);
	if (Affected.Num() > 0 && !bForce)
	{
		TArray<FString> Described;
		Described.Reserve(Affected.Num());
		TArray<TSharedPtr<FJsonValue>> SessionIds;
		for (const FMCPSession& Session : Affected)
		{
			Described.Add(FString::Printf(TEXT("%s (%s, session %s)"),
				*Session.AssetPath, *Session.ToolName, *Session.SessionId));
			SessionIds.Add(MakeShared<FJsonValueString>(Session.SessionId));
		}

		TSharedPtr<FJsonObject> BlockedData = MakeShared<FJsonObject>();
		BlockedData->SetArrayField(TEXT("blocking_sessions"), SessionIds);

		FToolResult Result = MakeErrorResult(FString::Printf(TEXT(
			"%d Claireon editing session(s) are bound to editor windows this would close: %s. Closing the "
			"window leaves the session reporting bound_editor_closed, which no later call can repair. Close "
			"those sessions first (bp_close / bp_close_all, or the matching *_close tool), or pass force=true "
			"to close anyway. Nothing was closed."),
			Affected.Num(), *FString::Join(Described, TEXT("; "))));
		Result.Data = BlockedData;
		return Result;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	FString Summary;

	if (bAll)
	{
		// Count before closing; GetAllEditedAssets is empty afterward.
		const int32 EditedAssets = Subsystem->GetAllEditedAssets().Num();
		Subsystem->CloseAllAssetEditors();
		Data->SetBoolField(TEXT("all"), true);
		Data->SetNumberField(TEXT("assets_closed"), EditedAssets);
		Summary = FString::Printf(TEXT("Closed every asset editor (%d asset(s) had one open)."), EditedAssets);
	}
	else
	{
		TArray<TSharedPtr<FJsonValue>> Closed;
		TArray<FString> NotOpen;
		for (const FString& Path : AssetPaths)
		{
			// LoadObject also resolves package-form paths to asset objects.
			UObject* Asset = LoadObject<UObject>(nullptr, *Path);
			if (!IsValid(Asset) || Subsystem->FindEditorForAsset(Asset, /*bFocusIfOpen=*/false) == nullptr)
			{
				NotOpen.Add(Path);
				continue;
			}
			Subsystem->CloseAllEditorsForAsset(Asset);
			Closed.Add(MakeShared<FJsonValueString>(Path));
		}

		TArray<TSharedPtr<FJsonValue>> NotOpenJson;
		for (const FString& Path : NotOpen)
		{
			NotOpenJson.Add(MakeShared<FJsonValueString>(Path));
		}
		Data->SetArrayField(TEXT("closed"), Closed);
		Data->SetArrayField(TEXT("not_open"), NotOpenJson);

		Summary = FString::Printf(TEXT("Closed %d asset editor(s)."), Closed.Num());
		if (NotOpen.Num() > 0)
		{
			Summary += FString::Printf(TEXT(" %d had no open editor: %s"),
				NotOpen.Num(), *FString::Join(NotOpen, TEXT(", ")));
		}
	}

	if (Affected.Num() > 0)
	{
		Data->SetBoolField(TEXT("forced_over_open_sessions"), true);
		Summary += FString::Printf(
			TEXT(" force=true closed windows under %d open Claireon session(s); their editor bindings are "
				 "now dead -- close and reopen them."), Affected.Num());
	}

	return MakeSuccessResult(Data, Summary);
}
