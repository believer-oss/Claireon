// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonFeedbackLog.h"
#include "ClaireonLog.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

FClaireonFeedbackLog::FClaireonFeedbackLog()
{
}

TArray<FString> FClaireonFeedbackLog::FindAllWorktreeFeedbackDirs()
{
	TArray<FString> Result;

	// Run: git -C "<ProjectDir>" worktree list --porcelain
	const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString Params = FString::Printf(TEXT("-C \"%s\" worktree list --porcelain"), *ProjectDir);

	FString GitOut, GitErr;
	int32 ReturnCode = 0;
	const bool bOk = FPlatformProcess::ExecProcess(
		TEXT("git"), *Params, &ReturnCode, &GitOut, &GitErr);

	if (bOk && ReturnCode == 0)
	{
		TArray<FString> Lines;
		GitOut.ParseIntoArrayLines(Lines, false);

		for (const FString& Line : Lines)
		{
			// Each worktree block starts with "worktree <absolute-path>"
			if (!Line.StartsWith(TEXT("worktree ")))
			{
				continue;
			}

			FString WorktreePath = Line.Mid(9 /* len("worktree ") */);
			WorktreePath.TrimStartAndEndInline();
			if (WorktreePath.IsEmpty())
			{
				continue;
			}

			// The feedback dir mirrors the current project layout:
			// <worktree>/Saved/Claireon/Feedback
			const FString FeedbackDir = FPaths::Combine(
				WorktreePath, TEXT("Saved"), TEXT("Claireon"), TEXT("Feedback"));

			if (IFileManager::Get().DirectoryExists(*FeedbackDir))
			{
				Result.AddUnique(FeedbackDir);
			}
		}
	}

	if (Result.Num() == 0)
	{
		// Fallback: just the current project
		Result.Add(Get().GetFeedbackDir());
	}

	return Result;
}

FClaireonFeedbackLog& FClaireonFeedbackLog::Get()
{
	static FClaireonFeedbackLog Instance;
	return Instance;
}

FString FClaireonFeedbackLog::GetFeedbackDir() const
{
	return FPaths::ProjectSavedDir() / TEXT("Claireon") / TEXT("Feedback");
}

FString FClaireonFeedbackLog::GenerateEntryId() const
{
	const FDateTime Now = FDateTime::UtcNow();

	// Count how many entries already share this same second to create a sequence number
	int32 Sequence = 1;
	const FString DatePrefix = Now.ToString(TEXT("%Y-%m-%d_%H%M%S"));
	for (int32 i = Entries.Num() - 1; i >= 0; --i)
	{
		if (Entries[i].Id.StartsWith(DatePrefix))
		{
			++Sequence;
		}
		else
		{
			break;
		}
	}

	return FString::Printf(TEXT("%s_%03d"), *DatePrefix, Sequence);
}

void FClaireonFeedbackLog::LoadIndex()
{
	if (bIndexLoaded)
	{
		return;
	}
	bIndexLoaded = true;

	const FString IndexPath = GetFeedbackDir() / TEXT("index.json");
	FString JsonString;
	if (!FFileHelper::LoadFileToString(JsonString, *IndexPath))
	{
		// No existing index — that's fine, start fresh
		return;
	}

	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		UE_LOG(LogClaireon, Warning, TEXT("[MCP] FeedbackLog: Failed to parse index.json, starting fresh"));
		return;
	}

	const TArray<TSharedPtr<FJsonValue>>* EntriesArray = nullptr;
	if (!RootObject->TryGetArrayField(TEXT("entries"), EntriesArray))
	{
		return;
	}

	for (const TSharedPtr<FJsonValue>& EntryValue : *EntriesArray)
	{
		const TSharedPtr<FJsonObject>* EntryObj = nullptr;
		if (!EntryValue->TryGetObject(EntryObj) || !(*EntryObj).IsValid())
		{
			continue;
		}

		FFeedbackEntry Entry;
		(*EntryObj)->TryGetStringField(TEXT("id"), Entry.Id);
		(*EntryObj)->TryGetStringField(TEXT("textPreview"), Entry.TextPreview);
		(*EntryObj)->TryGetBoolField(TEXT("isBug"), Entry.bIsBug);
		(*EntryObj)->TryGetBoolField(TEXT("isFeedback"), Entry.bIsFeedback);
		(*EntryObj)->TryGetBoolField(TEXT("isSuggestion"), Entry.bIsSuggestion);
		(*EntryObj)->TryGetStringField(TEXT("operatorName"), Entry.OperatorName);
		(*EntryObj)->TryGetStringField(TEXT("clientInfo"), Entry.ClientInfo);

		FString TimestampStr;
		if ((*EntryObj)->TryGetStringField(TEXT("timestamp"), TimestampStr))
		{
			FDateTime::ParseIso8601(*TimestampStr, Entry.Timestamp);
		}

		// Parse string arrays
		const TArray<TSharedPtr<FJsonValue>>* ToolsArray = nullptr;
		if ((*EntryObj)->TryGetArrayField(TEXT("relatedMCPTools"), ToolsArray))
		{
			for (const TSharedPtr<FJsonValue>& ToolValue : *ToolsArray)
			{
				FString ToolStr;
				if (ToolValue->TryGetString(ToolStr))
				{
					Entry.RelatedMCPTools.Add(MoveTemp(ToolStr));
				}
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* FeaturesArray = nullptr;
		if ((*EntryObj)->TryGetArrayField(TEXT("relatedFeatures"), FeaturesArray))
		{
			for (const TSharedPtr<FJsonValue>& FeatureValue : *FeaturesArray)
			{
				FString FeatureStr;
				if (FeatureValue->TryGetString(FeatureStr))
				{
					Entry.RelatedFeatures.Add(MoveTemp(FeatureStr));
				}
			}
		}

		if (!Entry.Id.IsEmpty())
		{
			Entries.Add(MoveTemp(Entry));
		}
	}

	UE_LOG(LogClaireon, Display, TEXT("[MCP] FeedbackLog: Loaded %d entries from index"), Entries.Num());
}

void FClaireonFeedbackLog::WriteIndex() const
{
	const FString FeedbackDir = GetFeedbackDir();
	IFileManager::Get().MakeDirectory(*FeedbackDir, true);

	TSharedPtr<FJsonObject> RootObject = MakeShared<FJsonObject>();
	RootObject->SetNumberField(TEXT("version"), 1);

	TArray<TSharedPtr<FJsonValue>> EntriesArray;
	for (const FFeedbackEntry& Entry : Entries)
	{
		TSharedPtr<FJsonObject> EntryObj = MakeShared<FJsonObject>();
		EntryObj->SetStringField(TEXT("id"), Entry.Id);
		EntryObj->SetStringField(TEXT("timestamp"), Entry.Timestamp.ToIso8601());
		EntryObj->SetStringField(TEXT("textPreview"), Entry.TextPreview);
		EntryObj->SetBoolField(TEXT("isBug"), Entry.bIsBug);
		EntryObj->SetBoolField(TEXT("isFeedback"), Entry.bIsFeedback);
		EntryObj->SetBoolField(TEXT("isSuggestion"), Entry.bIsSuggestion);
		EntryObj->SetStringField(TEXT("operatorName"), Entry.OperatorName);
		EntryObj->SetStringField(TEXT("clientInfo"), Entry.ClientInfo);

		// Serialize string arrays
		TArray<TSharedPtr<FJsonValue>> ToolsArray;
		for (const FString& Tool : Entry.RelatedMCPTools)
		{
			ToolsArray.Add(MakeShared<FJsonValueString>(Tool));
		}
		EntryObj->SetArrayField(TEXT("relatedMCPTools"), ToolsArray);

		TArray<TSharedPtr<FJsonValue>> FeaturesArray;
		for (const FString& Feature : Entry.RelatedFeatures)
		{
			FeaturesArray.Add(MakeShared<FJsonValueString>(Feature));
		}
		EntryObj->SetArrayField(TEXT("relatedFeatures"), FeaturesArray);

		EntriesArray.Add(MakeShared<FJsonValueObject>(EntryObj));
	}

	RootObject->SetArrayField(TEXT("entries"), EntriesArray);

	FString OutputString;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(RootObject.ToSharedRef(), Writer);

	const FString IndexPath = FeedbackDir / TEXT("index.json");
	if (!FFileHelper::SaveStringToFile(OutputString, *IndexPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogClaireon, Warning, TEXT("[MCP] FeedbackLog: Failed to write index.json"));
	}
}

void FClaireonFeedbackLog::RotateEntries()
{
	if (Entries.Num() <= MaxEntries)
	{
		return;
	}

	const FString FeedbackDir = GetFeedbackDir();
	const int32 NumToRemove = Entries.Num() - MaxEntries;

	for (int32 i = 0; i < NumToRemove; ++i)
	{
		const FFeedbackEntry& Entry = Entries[i];

		// Delete entry file
		if (!Entry.Id.IsEmpty())
		{
			const FString EntryFilePath = FeedbackDir / TEXT("entries") / (Entry.Id + TEXT(".json"));
			IFileManager::Get().Delete(*EntryFilePath);
		}
	}

	Entries.RemoveAt(0, NumToRemove);

	UE_LOG(LogClaireon, Display, TEXT("[MCP] FeedbackLog: Rotated %d old entries, %d remaining"), NumToRemove, Entries.Num());
}

FString FClaireonFeedbackLog::RecordFeedback(
	const FString& Text,
	const TArray<FString>& RelatedMCPTools,
	const TArray<FString>& RelatedFeatures,
	bool bIsBug,
	bool bIsFeedback,
	bool bIsSuggestion,
	const FString& OperatorName,
	const FString& ClientInfo)
{
	FScopeLock Lock(&CriticalSection);

	// Ensure index is loaded before we append
	LoadIndex();

	const FString FeedbackDir = GetFeedbackDir();
	const FString EntriesDir = FeedbackDir / TEXT("entries");
	IFileManager::Get().MakeDirectory(*EntriesDir, true);

	// Build entry
	FFeedbackEntry Entry;
	Entry.Id = GenerateEntryId();
	Entry.Timestamp = FDateTime::UtcNow();
	Entry.RelatedMCPTools = RelatedMCPTools;
	Entry.RelatedFeatures = RelatedFeatures;
	Entry.bIsBug = bIsBug;
	Entry.bIsFeedback = bIsFeedback;
	Entry.bIsSuggestion = bIsSuggestion;
	Entry.OperatorName = OperatorName;
	Entry.ClientInfo = ClientInfo;

	// Generate preview: first PreviewLength chars, collapse whitespace
	FString Preview = Text.Left(PreviewLength);
	Preview.ReplaceInline(TEXT("\r\n"), TEXT(" "));
	Preview.ReplaceInline(TEXT("\n"), TEXT(" "));
	Preview.ReplaceInline(TEXT("\r"), TEXT(" "));
	Entry.TextPreview = Preview;

	// Build entry JSON with full text
	TSharedPtr<FJsonObject> EntryJson = MakeShared<FJsonObject>();
	EntryJson->SetStringField(TEXT("id"), Entry.Id);
	EntryJson->SetStringField(TEXT("timestamp"), Entry.Timestamp.ToIso8601());
	EntryJson->SetStringField(TEXT("text"), Text);

	TArray<TSharedPtr<FJsonValue>> ToolsArray;
	for (const FString& Tool : RelatedMCPTools)
	{
		ToolsArray.Add(MakeShared<FJsonValueString>(Tool));
	}
	EntryJson->SetArrayField(TEXT("related_mcp_tools"), ToolsArray);

	TArray<TSharedPtr<FJsonValue>> FeaturesArray;
	for (const FString& Feature : RelatedFeatures)
	{
		FeaturesArray.Add(MakeShared<FJsonValueString>(Feature));
	}
	EntryJson->SetArrayField(TEXT("related_features"), FeaturesArray);

	EntryJson->SetBoolField(TEXT("is_bug"), bIsBug);
	EntryJson->SetBoolField(TEXT("is_feedback"), bIsFeedback);
	EntryJson->SetBoolField(TEXT("is_suggestion"), bIsSuggestion);
	EntryJson->SetStringField(TEXT("operator_name"), OperatorName);
	EntryJson->SetStringField(TEXT("client_info"), ClientInfo);

	FString EntryString;
	const TSharedRef<TJsonWriter<>> EntryWriter = TJsonWriterFactory<>::Create(&EntryString);
	FJsonSerializer::Serialize(EntryJson.ToSharedRef(), EntryWriter);

	// Write entry file — fail-fast on error
	const FString EntryFilePath = EntriesDir / (Entry.Id + TEXT(".json"));
	if (!FFileHelper::SaveStringToFile(EntryString, *EntryFilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogClaireon, Warning, TEXT("[MCP] FeedbackLog: Failed to save entry file: %s"), *EntryFilePath);
		return FString();
	}

	const FString EntryId = Entry.Id;
	Entries.Add(MoveTemp(Entry));

	// Rotate if needed
	RotateEntries();

	// Persist index — fail-fast on error
	const FString IndexPath = FeedbackDir / TEXT("index.json");
	{
		TSharedPtr<FJsonObject> RootObject = MakeShared<FJsonObject>();
		RootObject->SetNumberField(TEXT("version"), 1);

		TArray<TSharedPtr<FJsonValue>> IndexEntriesArray;
		for (const FFeedbackEntry& IndexEntry : Entries)
		{
			TSharedPtr<FJsonObject> IndexEntryObj = MakeShared<FJsonObject>();
			IndexEntryObj->SetStringField(TEXT("id"), IndexEntry.Id);
			IndexEntryObj->SetStringField(TEXT("timestamp"), IndexEntry.Timestamp.ToIso8601());
			IndexEntryObj->SetStringField(TEXT("textPreview"), IndexEntry.TextPreview);
			IndexEntryObj->SetBoolField(TEXT("isBug"), IndexEntry.bIsBug);
			IndexEntryObj->SetBoolField(TEXT("isFeedback"), IndexEntry.bIsFeedback);
			IndexEntryObj->SetBoolField(TEXT("isSuggestion"), IndexEntry.bIsSuggestion);
			IndexEntryObj->SetStringField(TEXT("operatorName"), IndexEntry.OperatorName);
			IndexEntryObj->SetStringField(TEXT("clientInfo"), IndexEntry.ClientInfo);

			TArray<TSharedPtr<FJsonValue>> IndexToolsArray;
			for (const FString& Tool : IndexEntry.RelatedMCPTools)
			{
				IndexToolsArray.Add(MakeShared<FJsonValueString>(Tool));
			}
			IndexEntryObj->SetArrayField(TEXT("relatedMCPTools"), IndexToolsArray);

			TArray<TSharedPtr<FJsonValue>> IndexFeaturesArray;
			for (const FString& Feature : IndexEntry.RelatedFeatures)
			{
				IndexFeaturesArray.Add(MakeShared<FJsonValueString>(Feature));
			}
			IndexEntryObj->SetArrayField(TEXT("relatedFeatures"), IndexFeaturesArray);

			IndexEntriesArray.Add(MakeShared<FJsonValueObject>(IndexEntryObj));
		}

		RootObject->SetArrayField(TEXT("entries"), IndexEntriesArray);

		FString IndexString;
		const TSharedRef<TJsonWriter<>> IndexWriter = TJsonWriterFactory<>::Create(&IndexString);
		FJsonSerializer::Serialize(RootObject.ToSharedRef(), IndexWriter);

		if (!FFileHelper::SaveStringToFile(IndexString, *IndexPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			UE_LOG(LogClaireon, Warning, TEXT("[MCP] FeedbackLog: Failed to write index.json"));
			return FString();
		}
	}

	UE_LOG(LogClaireon, Display, TEXT("[MCP] FeedbackLog: Recorded feedback %s (bug=%s, feedback=%s, suggestion=%s)"),
		*EntryId, bIsBug ? TEXT("true") : TEXT("false"), bIsFeedback ? TEXT("true") : TEXT("false"), bIsSuggestion ? TEXT("true") : TEXT("false"));

	return EntryId;
}
