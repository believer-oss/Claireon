// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBlueprintTranslateTool_Skip.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase.h" // kBPCategory

#include "ClaireonBPTranslateSession.h"
#include "ClaireonScopedAssetLock.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBlueprintTranslateTool_Skip::GetCategory() const { return kBPCategory; }
FString ClaireonBlueprintTranslateTool_Skip::GetOperation() const { return TEXT("translate_skip_node"); }

FString ClaireonBlueprintTranslateTool_Skip::GetDescription() const
{
	return TEXT("Mark a //[BP] tagged node 'skipped' in the translation session. Optionally rewrites the "
	            "in-file '// [BP] TODO: implement' marker to a 'SKIPPED: <reason>' note. The 'code' field "
	            "carries the reason; if empty, 'skipped by user' is used.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintTranslateTool_Skip::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	for (const TCHAR* Field : { TEXT("session_id"), TEXT("session_file"), TEXT("blueprint"), TEXT("node_guid"), TEXT("code") })
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

FToolResult ClaireonBlueprintTranslateTool_Skip::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId = Arguments->GetStringField(TEXT("session_id"));
	FString SessionFile = Arguments->GetStringField(TEXT("session_file"));
	FString BlueprintPath = Arguments->GetStringField(TEXT("blueprint"));
	FString NodeGuid = Arguments->GetStringField(TEXT("node_guid"));
	FString Code = Arguments->GetStringField(TEXT("code"));

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

	Session->UpdateNodeStatus(BlueprintPath, NodeGuid, TEXT("skipped"));

	FString FileContent;
	if (FFileHelper::LoadFileToString(FileContent, *AbsSourcePath))
	{
		FString Reason = Code.IsEmpty() ? TEXT("skipped by user") : Code;
		FString SearchStr = FString::Printf(TEXT("// [BP] TODO: implement"));
		int32 GuidPos = FileContent.Find(FString::Printf(TEXT("Guid=%s"), *NodeGuid));
		if (GuidPos != INDEX_NONE)
		{
			int32 TodoPos = FileContent.Find(SearchStr, ESearchCase::CaseSensitive, ESearchDir::FromStart, GuidPos);
			if (TodoPos != INDEX_NONE && (TodoPos - GuidPos) < 500)
			{
				FString Replacement = FString::Printf(TEXT("// [BP] SKIPPED: %s"), *Reason);
				FileContent = FileContent.Left(TodoPos) + Replacement
					+ FileContent.Mid(TodoPos + SearchStr.Len());
				FFileHelper::SaveStringToFile(FileContent, *AbsSourcePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
				BPState->SourceHash = FClaireonBPTranslateSession::ComputeFileHash(AbsSourcePath);
			}
		}
	}

	if (!Session->SessionFilePath.IsEmpty())
	{
		Session->SaveToFile(Session->SessionFilePath);
	}

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("node_guid"), NodeGuid);
	ResultData->SetStringField(TEXT("status"), TEXT("skipped"));
	return MakeSuccessResult(ResultData, FString::Printf(TEXT("Node %s marked as skipped"), *NodeGuid));
}
