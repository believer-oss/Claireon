// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBlueprintTranslateTool_MarkComplete.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase.h" // kBPCategory

#include "ClaireonBPTranslateSession.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBlueprintTranslateTool_MarkComplete::GetCategory() const { return kBPCategory; }
FString ClaireonBlueprintTranslateTool_MarkComplete::GetOperation() const { return TEXT("translate_mark_complete"); }

FString ClaireonBlueprintTranslateTool_MarkComplete::GetDescription() const
{
	return TEXT("Mark a bp_translate session 'complete' once every //[BP] tagged node is implemented or "
	            "skipped. Walks each blueprint's node map, flags residual TODO markers, and updates the "
	            "session status. Requires session_id from bp_translate_scaffold. Returns implemented/"
	            "skipped/remaining counts, pending-node and inconsistency lists, and a verdict "
	            "('complete' or 'incomplete'). Does not mutate source files.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintTranslateTool_MarkComplete::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	// Explicit per-field declarations, not a name loop: the loop form could
	// carry no description, so every one of these parameters was undescribed
	// -- invisible in help and in the MCP schema.
	auto AddStringParam = [&Properties](const TCHAR* Name, const TCHAR* Description)
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("type"), TEXT("string"));
		P->SetStringField(TEXT("description"), Description);
		Properties->SetObjectField(Name, P);
	};
	AddStringParam(TEXT("session_id"), TEXT("Session ID returned by the scaffold tool."));
	AddStringParam(TEXT("session_file"), TEXT("Direct path to the session JSON file. Alternative to session_id."));
	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("session_id")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

FToolResult ClaireonBlueprintTranslateTool_MarkComplete::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId = Arguments->GetStringField(TEXT("session_id"));
	FString SessionFile = Arguments->GetStringField(TEXT("session_file"));

	if (SessionId.IsEmpty()) return MakeErrorResult(TEXT("'session_id' is required."));

	FString ResolveError;
	TSharedPtr<FClaireonBPTranslateSession> Session = FClaireonBPTranslateSession::ResolveSession(SessionId, SessionFile, ResolveError);
	if (!Session.IsValid()) return MakeErrorResult(ResolveError);

	TArray<FString> RemainingTodos;
	TArray<FString> Inconsistencies;
	int32 TotalNodes = 0;
	int32 ImplementedNodes = 0;
	int32 SkippedNodes = 0;
	int32 PendingNodes = 0;

	for (auto& BPPair : Session->Blueprints)
	{
		FString AbsSourcePath = FPaths::Combine(FPaths::ProjectDir(), BPPair.Value.GeneratedSource);

		FString FileContent;
		FFileHelper::LoadFileToString(FileContent, *AbsSourcePath);

		for (const auto& NodePair : BPPair.Value.Nodes)
		{
			TotalNodes++;
			if (NodePair.Value.Status == TEXT("implemented"))
			{
				ImplementedNodes++;
				if (FileContent.Contains(FString::Printf(TEXT("Guid=%s"), *NodePair.Key)))
				{
					int32 GuidPos = FileContent.Find(FString::Printf(TEXT("Guid=%s"), *NodePair.Key));
					FString NearbyContent = FileContent.Mid(GuidPos, FMath::Min(500, FileContent.Len() - GuidPos));
					if (NearbyContent.Contains(TEXT("// [BP] TODO")))
					{
						Inconsistencies.Add(FString::Printf(TEXT("Node %s in %s is marked implemented but still has TODO marker"),
							*NodePair.Key, *BPPair.Key));
					}
				}
			}
			else if (NodePair.Value.Status == TEXT("skipped"))
			{
				SkippedNodes++;
			}
			else
			{
				PendingNodes++;
				RemainingTodos.Add(FString::Printf(TEXT("%s: %s (%s)"),
					*BPPair.Key, *NodePair.Key, *NodePair.Value.Name));
			}
		}
	}

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetNumberField(TEXT("total_nodes"), TotalNodes);
	ResultData->SetNumberField(TEXT("implemented"), ImplementedNodes);
	ResultData->SetNumberField(TEXT("skipped"), SkippedNodes);
	ResultData->SetNumberField(TEXT("remaining"), PendingNodes);

	TArray<TSharedPtr<FJsonValue>> TodoArray;
	for (const FString& Todo : RemainingTodos)
	{
		TodoArray.Add(MakeShared<FJsonValueString>(Todo));
	}
	ResultData->SetArrayField(TEXT("remaining_todos"), TodoArray);

	TArray<TSharedPtr<FJsonValue>> InconArray;
	for (const FString& Inc : Inconsistencies)
	{
		InconArray.Add(MakeShared<FJsonValueString>(Inc));
	}
	ResultData->SetArrayField(TEXT("inconsistencies"), InconArray);

	FString Verdict = PendingNodes == 0 ? TEXT("complete") : TEXT("incomplete");
	ResultData->SetStringField(TEXT("verdict"), Verdict);

	if (Verdict == TEXT("complete"))
	{
		Session->Status = TEXT("complete");
		if (!Session->SessionFilePath.IsEmpty())
		{
			Session->SaveToFile(Session->SessionFilePath);
		}
	}

	return MakeSuccessResult(ResultData, FString::Printf(
		TEXT("Completion check: %s (%d/%d implemented, %d skipped, %d pending)"),
		*Verdict, ImplementedNodes, TotalNodes, SkippedNodes, PendingNodes));
}
