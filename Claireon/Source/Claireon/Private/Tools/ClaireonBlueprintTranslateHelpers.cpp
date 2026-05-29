// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBlueprintTranslateHelpers.h"

#include "ClaireonBPTranslateSession.h"
#include "ClaireonScopedAssetLock.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace ClaireonBlueprintTranslateHelpers
{
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

	IClaireonTool::FToolResult DoImplement(
		const TSharedPtr<FJsonObject>& Arguments,
		const FString& ToolName,
		bool bEnforceHashCheck)
	{
		using FToolResult = IClaireonTool::FToolResult;

		auto MakeErr = [](const FString& Msg) {
			return IClaireonTool::MakeErrorResult(Msg);
		};

		FString SessionId = Arguments->GetStringField(TEXT("session_id"));
		FString SessionFile = Arguments->GetStringField(TEXT("session_file"));
		FString BlueprintPath = Arguments->GetStringField(TEXT("blueprint"));
		FString NodeGuid = Arguments->GetStringField(TEXT("node_guid"));
		FString Code = Arguments->GetStringField(TEXT("code"));

		if (SessionId.IsEmpty()) return MakeErr(TEXT("'session_id' is required."));
		if (BlueprintPath.IsEmpty()) return MakeErr(TEXT("'blueprint' is required."));
		if (NodeGuid.IsEmpty()) return MakeErr(TEXT("'node_guid' is required."));
		if (Code.IsEmpty()) return MakeErr(TEXT("'code' is required."));

		FString ResolveError;
		TSharedPtr<FClaireonBPTranslateSession> Session = FClaireonBPTranslateSession::ResolveSession(SessionId, SessionFile, ResolveError);
		if (!Session.IsValid()) return MakeErr(ResolveError);

		FClaireonScopedAssetLock Lock(BlueprintPath, ToolName);
		if (!Lock.IsAcquired()) return Lock.GetError();

		FClaireonBPTranslateBlueprintState* BPState = Session->Blueprints.Find(BlueprintPath);
		if (!BPState)
		{
			TArray<FString> ValidBPs;
			for (const auto& BPPair : Session->Blueprints) ValidBPs.Add(BPPair.Key);
			return MakeErr(FString::Printf(TEXT("Blueprint not found in session: %s. Valid: %s"),
				*BlueprintPath, *FString::Join(ValidBPs, TEXT(", "))));
		}

		FClaireonBPTranslateNodeStatus* NodeStatus = BPState->Nodes.Find(NodeGuid);
		if (!NodeStatus)
		{
			TArray<FString> ValidGuids;
			for (const auto& NodePair : BPState->Nodes) ValidGuids.Add(NodePair.Key);
			return MakeErr(FString::Printf(TEXT("Node GUID not found: %s. Valid GUIDs: %s"),
				*NodeGuid, *FString::Join(ValidGuids, TEXT(", "))));
		}

		if (NodeStatus->Status == TEXT("skipped"))
		{
			return MakeErr(FString::Printf(
				TEXT("Node %s is marked as skipped. Change status before implementing."), *NodeGuid));
		}

		FString AbsSourcePath = FPaths::Combine(FPaths::ProjectDir(), BPState->GeneratedSource);

		FString FileContent;
		if (!FFileHelper::LoadFileToString(FileContent, *AbsSourcePath))
		{
			return MakeErr(FString::Printf(TEXT("File not found: %s. Re-scaffold needed."),
				*BPState->GeneratedSource));
		}

		if (bEnforceHashCheck)
		{
			FString CurrentHash = FClaireonBPTranslateSession::ComputeFileHash(AbsSourcePath);
			if (CurrentHash != BPState->SourceHash && !BPState->SourceHash.IsEmpty())
			{
				return MakeErr(
					TEXT("File has been modified outside the translator. Use action: force_implement (or call bp_translate_force_implement) to overwrite, or re-scaffold to create a fresh session."));
			}
		}

		int32 RegionStart, RegionEnd, TagStart, TagEnd;
		if (!FindBPTagRegion(FileContent, NodeGuid, RegionStart, RegionEnd, TagStart, TagEnd))
		{
			return MakeErr(FString::Printf(
				TEXT("Could not find //[BP] region for node %s in %s"), *NodeGuid, *BPState->GeneratedSource));
		}

		FString NewContent = FileContent.Left(RegionStart) + Code + TEXT("\n") + FileContent.Mid(RegionEnd);

		if (!FFileHelper::SaveStringToFile(NewContent, *AbsSourcePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			return MakeErr(FString::Printf(TEXT("Failed to write file: %s"), *BPState->GeneratedSource));
		}

		Session->UpdateNodeStatus(BlueprintPath, NodeGuid, TEXT("implemented"));
		BPState->SourceHash = FClaireonBPTranslateSession::ComputeFileHash(AbsSourcePath);

		if (!Session->SessionFilePath.IsEmpty())
		{
			Session->SaveToFile(Session->SessionFilePath);
		}

		TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
		ResultData->SetStringField(TEXT("node_guid"), NodeGuid);
		ResultData->SetStringField(TEXT("status"), TEXT("implemented"));
		ResultData->SetStringField(TEXT("file"), BPState->GeneratedSource);

		return IClaireonTool::MakeSuccessResult(ResultData,
			FString::Printf(TEXT("Node %s implemented in %s"), *NodeGuid, *BPState->GeneratedSource));
	}
}
