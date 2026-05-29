// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBlueprintTranslateTool_Inspect.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase.h" // kBPCategory
#include "Tools/ClaireonBlueprintTranslateHelpers.h"

#include "ClaireonBPTranslateSession.h"
#include "ClaireonScopedAssetLock.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBlueprintTranslateTool_Inspect::GetCategory() const { return kBPCategory; }
FString ClaireonBlueprintTranslateTool_Inspect::GetOperation() const { return TEXT("translate_inspect"); }

FString ClaireonBlueprintTranslateTool_Inspect::GetDescription() const
{
	return TEXT("Read-only: inspect the current state of one //[BP] tagged node region inside a translation "
	            "session created by blueprint_translate_scaffold. Returns the current code, the node's status "
	            "(pending/implemented/skipped), and a map of sibling nodes' statuses.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintTranslateTool_Inspect::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	for (const TCHAR* Field : { TEXT("session_id"), TEXT("session_file"), TEXT("blueprint"), TEXT("node_guid") })
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("type"), TEXT("string"));
		Properties->SetObjectField(Field, P);
	}
	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("session_id")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("blueprint")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("node_guid")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

FToolResult ClaireonBlueprintTranslateTool_Inspect::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId = Arguments->GetStringField(TEXT("session_id"));
	FString SessionFile = Arguments->GetStringField(TEXT("session_file"));
	FString BlueprintPath = Arguments->GetStringField(TEXT("blueprint"));
	FString NodeGuid = Arguments->GetStringField(TEXT("node_guid"));

	if (SessionId.IsEmpty()) return MakeErrorResult(TEXT("'session_id' is required."));
	if (BlueprintPath.IsEmpty()) return MakeErrorResult(TEXT("'blueprint' is required."));
	if (NodeGuid.IsEmpty()) return MakeErrorResult(TEXT("'node_guid' is required."));

	FString ResolveError;
	TSharedPtr<FClaireonBPTranslateSession> Session = FClaireonBPTranslateSession::ResolveSession(SessionId, SessionFile, ResolveError);
	if (!Session.IsValid()) return MakeErrorResult(ResolveError);

	FClaireonScopedAssetLock Lock(BlueprintPath, GetName());
	if (!Lock.IsAcquired()) return Lock.GetError();

	FClaireonBPTranslateBlueprintState* BPState = Session->Blueprints.Find(BlueprintPath);
	if (!BPState)
	{
		TArray<FString> ValidBPs;
		for (const auto& BPPair : Session->Blueprints) ValidBPs.Add(BPPair.Key);
		return MakeErrorResult(FString::Printf(TEXT("Blueprint not found in session: %s. Valid: %s"),
			*BlueprintPath, *FString::Join(ValidBPs, TEXT(", "))));
	}

	FClaireonBPTranslateNodeStatus* NodeStatus = BPState->Nodes.Find(NodeGuid);
	if (!NodeStatus)
	{
		TArray<FString> ValidGuids;
		for (const auto& NodePair : BPState->Nodes) ValidGuids.Add(NodePair.Key);
		return MakeErrorResult(FString::Printf(TEXT("Node GUID not found: %s. Valid GUIDs: %s"),
			*NodeGuid, *FString::Join(ValidGuids, TEXT(", "))));
	}

	FString AbsSourcePath = FPaths::Combine(FPaths::ProjectDir(), BPState->GeneratedSource);

	FString FileContent;
	if (!FFileHelper::LoadFileToString(FileContent, *AbsSourcePath))
	{
		return MakeErrorResult(FString::Printf(TEXT("Cannot read file: %s"), *BPState->GeneratedSource));
	}

	int32 RegionStart, RegionEnd, TagStart, TagEnd;
	FString CurrentCode;
	if (ClaireonBlueprintTranslateHelpers::FindBPTagRegion(FileContent, NodeGuid, RegionStart, RegionEnd, TagStart, TagEnd))
	{
		CurrentCode = FileContent.Mid(TagStart, RegionEnd - TagStart);
	}
	else
	{
		CurrentCode = TEXT("(tag region not found in file)");
	}

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("node_guid"), NodeGuid);
	ResultData->SetStringField(TEXT("status"), NodeStatus->Status);
	ResultData->SetStringField(TEXT("type"), NodeStatus->Type);
	ResultData->SetStringField(TEXT("name"), NodeStatus->Name);
	ResultData->SetStringField(TEXT("current_code"), CurrentCode);
	ResultData->SetStringField(TEXT("file"), BPState->GeneratedSource);

	TSharedPtr<FJsonObject> ConnectedObj = MakeShared<FJsonObject>();
	for (const auto& NodePair : BPState->Nodes)
	{
		if (NodePair.Key != NodeGuid)
		{
			TSharedPtr<FJsonObject> ConnNode = MakeShared<FJsonObject>();
			ConnNode->SetStringField(TEXT("status"), NodePair.Value.Status);
			ConnNode->SetStringField(TEXT("type"), NodePair.Value.Type);
			ConnNode->SetStringField(TEXT("name"), NodePair.Value.Name);
			ConnectedObj->SetObjectField(NodePair.Key, ConnNode);
		}
	}
	ResultData->SetObjectField(TEXT("session_nodes"), ConnectedObj);

	return MakeSuccessResult(ResultData, FString::Printf(
		TEXT("Node %s (%s): %s"), *NodeGuid, *NodeStatus->Name, *NodeStatus->Status));
}
