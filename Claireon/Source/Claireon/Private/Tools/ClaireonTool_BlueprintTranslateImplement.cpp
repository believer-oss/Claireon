// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_BlueprintTranslateImplement.h"

#include "ClaireonBPTranslateSession.h"
#include "ClaireonLog.h"
#include "ClaireonScopedAssetLock.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

FString ClaireonTool_BlueprintTranslateImplement::GetCategory() const { return TEXT("blueprint"); }
FString ClaireonTool_BlueprintTranslateImplement::GetOperation() const { return TEXT("translate_implement"); }

FString ClaireonTool_BlueprintTranslateImplement::GetDescription() const
{
	return TEXT("Interactive implementation of scaffolded BP-to-C++ code regions. Phase 2 tool that supports "
		"inspect, implement, force_implement, skip, and mark_complete actions on individual nodes "
		"within a translation session created by blueprint_translate_scaffold. Immediate-mode tool: no session required.");
}

TSharedPtr<FJsonObject> ClaireonTool_BlueprintTranslateImplement::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> SessionProp = MakeShared<FJsonObject>();
	SessionProp->SetStringField(TEXT("type"), TEXT("string"));
	SessionProp->SetStringField(TEXT("description"), TEXT("Session ID returned by the scaffold tool."));
	Properties->SetObjectField(TEXT("session_id"), SessionProp);

	TSharedPtr<FJsonObject> FileProp = MakeShared<FJsonObject>();
	FileProp->SetStringField(TEXT("type"), TEXT("string"));
	FileProp->SetStringField(TEXT("description"), TEXT("Direct path to the session JSON file."));
	Properties->SetObjectField(TEXT("session_file"), FileProp);

	TSharedPtr<FJsonObject> BPProp = MakeShared<FJsonObject>();
	BPProp->SetStringField(TEXT("type"), TEXT("string"));
	BPProp->SetStringField(TEXT("description"), TEXT("Blueprint asset path within the session."));
	Properties->SetObjectField(TEXT("blueprint"), BPProp);

	TSharedPtr<FJsonObject> GuidProp = MakeShared<FJsonObject>();
	GuidProp->SetStringField(TEXT("type"), TEXT("string"));
	GuidProp->SetStringField(TEXT("description"), TEXT("GUID of the node to operate on."));
	Properties->SetObjectField(TEXT("node_guid"), GuidProp);

	TSharedPtr<FJsonObject> ActionProp = MakeShared<FJsonObject>();
	ActionProp->SetStringField(TEXT("type"), TEXT("string"));
	ActionProp->SetStringField(TEXT("description"),
		TEXT("Action: inspect, implement, force_implement, skip, or mark_complete."));
	TArray<TSharedPtr<FJsonValue>> ActionEnum;
	ActionEnum.Add(MakeShared<FJsonValueString>(TEXT("inspect")));
	ActionEnum.Add(MakeShared<FJsonValueString>(TEXT("implement")));
	ActionEnum.Add(MakeShared<FJsonValueString>(TEXT("force_implement")));
	ActionEnum.Add(MakeShared<FJsonValueString>(TEXT("skip")));
	ActionEnum.Add(MakeShared<FJsonValueString>(TEXT("mark_complete")));
	ActionProp->SetArrayField(TEXT("enum"), ActionEnum);
	Properties->SetObjectField(TEXT("action"), ActionProp);

	TSharedPtr<FJsonObject> CodeProp = MakeShared<FJsonObject>();
	CodeProp->SetStringField(TEXT("type"), TEXT("string"));
	CodeProp->SetStringField(TEXT("description"),
		TEXT("Replacement C++ code for the node region. Required for implement/force_implement."));
	Properties->SetObjectField(TEXT("code"), CodeProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("session_id")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("action")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

namespace ImplementInternal
{
	// Find a //[BP] tagged region in file content for a given node GUID.
	// Returns the start and end positions of the region content (after tag line, before next tag or scope end).
	bool FindBPTagRegion(const FString& FileContent, const FString& NodeGuid,
		int32& OutRegionStart, int32& OutRegionEnd, int32& OutTagLineStart, int32& OutTagLineEnd)
	{
		// Search for the tag containing this GUID
		FString GuidPattern = FString::Printf(TEXT("Guid=%s"), *NodeGuid);
		int32 TagPos = FileContent.Find(GuidPattern, ESearchCase::CaseSensitive);
		if (TagPos == INDEX_NONE)
		{
			return false;
		}

		// Find the start of the tag line (// [BP...)
		int32 TagLineStart = TagPos;
		while (TagLineStart > 0 && FileContent[TagLineStart - 1] != TEXT('\n'))
		{
			TagLineStart--;
		}

		// Find the end of the tag line
		int32 TagLineEnd = FileContent.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, TagPos);
		if (TagLineEnd == INDEX_NONE)
		{
			TagLineEnd = FileContent.Len();
		}
		else
		{
			TagLineEnd++; // Include the newline
		}

		OutTagLineStart = TagLineStart;
		OutTagLineEnd = TagLineEnd;
		OutRegionStart = TagLineEnd;

		// Find the end of the region: next //[BP tag at same or lower indent, or closing brace
		int32 SearchPos = OutRegionStart;
		int32 BraceDepth = 0;
		bool bFoundEnd = false;

		while (SearchPos < FileContent.Len())
		{
			TCHAR Ch = FileContent[SearchPos];

			if (Ch == TEXT('{'))
			{
				BraceDepth++;
			}
			else if (Ch == TEXT('}'))
			{
				if (BraceDepth <= 0)
				{
					// End of enclosing scope
					OutRegionEnd = SearchPos;
					bFoundEnd = true;
					break;
				}
				BraceDepth--;
			}
			else if (Ch == TEXT('/') && SearchPos + 1 < FileContent.Len() && FileContent[SearchPos + 1] == TEXT('/'))
			{
				// Check if this is a //[BP tag
				int32 CommentLineStart = SearchPos;
				// Go back to start of this line
				while (CommentLineStart > OutRegionStart && FileContent[CommentLineStart - 1] != TEXT('\n'))
				{
					CommentLineStart--;
				}
				FString RestOfLine = FileContent.Mid(SearchPos, FMath::Min(50, FileContent.Len() - SearchPos));
				if (RestOfLine.Contains(TEXT("// [BP:")) || RestOfLine.Contains(TEXT("//[BP:")))
				{
					// Check it's not the same node's continuation
					if (!RestOfLine.Contains(NodeGuid))
					{
						OutRegionEnd = CommentLineStart;
						bFoundEnd = true;
						break;
					}
				}
			}

			SearchPos++;
		}

		if (!bFoundEnd)
		{
			OutRegionEnd = FileContent.Len();
		}

		return true;
	}
}

IClaireonTool::FToolResult ClaireonTool_BlueprintTranslateImplement::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId = Arguments->GetStringField(TEXT("session_id"));
	FString SessionFile = Arguments->GetStringField(TEXT("session_file"));
	FString Action = Arguments->GetStringField(TEXT("action"));
	FString BlueprintPath = Arguments->GetStringField(TEXT("blueprint"));
	FString NodeGuid = Arguments->GetStringField(TEXT("node_guid"));
	FString Code = Arguments->GetStringField(TEXT("code"));

	if (SessionId.IsEmpty())
	{
		return MakeErrorResult(TEXT("'session_id' is required."));
	}
	if (Action.IsEmpty())
	{
		return MakeErrorResult(TEXT("'action' is required. Valid values: inspect, implement, force_implement, skip, mark_complete."));
	}

	// Resolve session
	FString ResolveError;
	TSharedPtr<FClaireonBPTranslateSession> Session = FClaireonBPTranslateSession::ResolveSession(
		SessionId, SessionFile, ResolveError);
	if (!Session.IsValid())
	{
		return MakeErrorResult(ResolveError);
	}

	// --- mark_complete ---
	if (Action == TEXT("mark_complete"))
	{
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
					// Check for inconsistency: implemented but still has TODO
					if (FileContent.Contains(FString::Printf(TEXT("Guid=%s"), *NodePair.Key)))
					{
						// Search nearby for TODO
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

	// All other actions require blueprint and node_guid
	if (BlueprintPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("'blueprint' is required for this action."));
	}
	if (NodeGuid.IsEmpty())
	{
		return MakeErrorResult(TEXT("'node_guid' is required for this action."));
	}

	// Acquire per-asset lock on the blueprint path for actions that may mutate it.
	// inspect is read-only but locking it costs little and prevents racing readers.
	FClaireonScopedAssetLock Lock(BlueprintPath, GetName());
	if (!Lock.IsAcquired())
	{
		return Lock.GetError();
	}

	// Look up blueprint state
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

	// --- inspect ---
	if (Action == TEXT("inspect"))
	{
		FString FileContent;
		if (!FFileHelper::LoadFileToString(FileContent, *AbsSourcePath))
		{
			return MakeErrorResult(FString::Printf(TEXT("Cannot read file: %s"), *BPState->GeneratedSource));
		}

		int32 RegionStart, RegionEnd, TagStart, TagEnd;
		FString CurrentCode;
		if (ImplementInternal::FindBPTagRegion(FileContent, NodeGuid, RegionStart, RegionEnd, TagStart, TagEnd))
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

		// List connected nodes and their statuses
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

	// --- skip ---
	if (Action == TEXT("skip"))
	{
		Session->UpdateNodeStatus(BlueprintPath, NodeGuid, TEXT("skipped"));

		// Optionally update file to mark as skipped
		FString FileContent;
		if (FFileHelper::LoadFileToString(FileContent, *AbsSourcePath))
		{
			FString Reason = Code.IsEmpty() ? TEXT("skipped by user") : Code;
			FString SearchStr = FString::Printf(TEXT("// [BP] TODO: implement"));
			int32 GuidPos = FileContent.Find(FString::Printf(TEXT("Guid=%s"), *NodeGuid));
			if (GuidPos != INDEX_NONE)
			{
				// Find the nearest TODO after this GUID
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

		// Persist session
		if (!Session->SessionFilePath.IsEmpty())
		{
			Session->SaveToFile(Session->SessionFilePath);
		}

		TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
		ResultData->SetStringField(TEXT("node_guid"), NodeGuid);
		ResultData->SetStringField(TEXT("status"), TEXT("skipped"));
		return MakeSuccessResult(ResultData, FString::Printf(TEXT("Node %s marked as skipped"), *NodeGuid));
	}

	// --- implement / force_implement ---
	if (Action == TEXT("implement") || Action == TEXT("force_implement"))
	{
		if (Code.IsEmpty())
		{
			return MakeErrorResult(TEXT("'code' is required for implement/force_implement actions."));
		}

		if (NodeStatus->Status == TEXT("skipped"))
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Node %s is marked as skipped. Change status before implementing."), *NodeGuid));
		}

		FString FileContent;
		if (!FFileHelper::LoadFileToString(FileContent, *AbsSourcePath))
		{
			return MakeErrorResult(FString::Printf(TEXT("File not found: %s. Re-scaffold needed."),
				*BPState->GeneratedSource));
		}

		// Hash check (skip for force_implement)
		if (Action == TEXT("implement"))
		{
			FString CurrentHash = FClaireonBPTranslateSession::ComputeFileHash(AbsSourcePath);
			if (CurrentHash != BPState->SourceHash && !BPState->SourceHash.IsEmpty())
			{
				return MakeErrorResult(
					TEXT("File has been modified outside the translator. Use action: force_implement to overwrite, or re-scaffold to create a fresh session."));
			}
		}

		// Find and replace the tagged region
		int32 RegionStart, RegionEnd, TagStart, TagEnd;
		if (!ImplementInternal::FindBPTagRegion(FileContent, NodeGuid, RegionStart, RegionEnd, TagStart, TagEnd))
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Could not find //[BP] region for node %s in %s"), *NodeGuid, *BPState->GeneratedSource));
		}

		// Replace the region content (preserve the tag line)
		FString NewContent = FileContent.Left(RegionStart) + Code + TEXT("\n") + FileContent.Mid(RegionEnd);

		if (!FFileHelper::SaveStringToFile(NewContent, *AbsSourcePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			return MakeErrorResult(FString::Printf(TEXT("Failed to write file: %s"), *BPState->GeneratedSource));
		}

		// Update session
		Session->UpdateNodeStatus(BlueprintPath, NodeGuid, TEXT("implemented"));
		BPState->SourceHash = FClaireonBPTranslateSession::ComputeFileHash(AbsSourcePath);

		// Persist session
		if (!Session->SessionFilePath.IsEmpty())
		{
			Session->SaveToFile(Session->SessionFilePath);
		}

		TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
		ResultData->SetStringField(TEXT("node_guid"), NodeGuid);
		ResultData->SetStringField(TEXT("status"), TEXT("implemented"));
		ResultData->SetStringField(TEXT("file"), BPState->GeneratedSource);
		return MakeSuccessResult(ResultData, FString::Printf(
			TEXT("Node %s implemented in %s"), *NodeGuid, *BPState->GeneratedSource));
	}

	return MakeErrorResult(FString::Printf(
		TEXT("Invalid action: %s. Valid values: inspect, implement, force_implement, skip, mark_complete."), *Action));
}
