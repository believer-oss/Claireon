// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonPythonAuditLog.h"
#include "ClaireonLog.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

FClaireonPythonAuditLog::FClaireonPythonAuditLog()
{
}

FClaireonPythonAuditLog& FClaireonPythonAuditLog::Get()
{
	static FClaireonPythonAuditLog Instance;
	return Instance;
}

FString FClaireonPythonAuditLog::GetAuditLogDir() const
{
	return FPaths::ProjectSavedDir() / TEXT("MCP") / TEXT("PythonAuditLog");
}

FString FClaireonPythonAuditLog::GenerateEntryId() const
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

void FClaireonPythonAuditLog::LoadIndex()
{
	if (bIndexLoaded)
	{
		return;
	}
	bIndexLoaded = true;

	const FString IndexPath = GetAuditLogDir() / TEXT("index.json");
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
		UE_LOG(LogClaireon, Warning, TEXT("[MCP] PythonAuditLog: Failed to parse index.json, starting fresh"));
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

		FAuditEntry Entry;
		(*EntryObj)->TryGetStringField(TEXT("id"), Entry.Id);
		(*EntryObj)->TryGetStringField(TEXT("scriptPath"), Entry.ScriptPath);
		(*EntryObj)->TryGetStringField(TEXT("outputPath"), Entry.OutputPath);
		(*EntryObj)->TryGetBoolField(TEXT("success"), Entry.bSuccess);
		(*EntryObj)->TryGetNumberField(TEXT("durationMs"), Entry.DurationMs);
		(*EntryObj)->TryGetStringField(TEXT("scriptPreview"), Entry.ScriptPreview);
		(*EntryObj)->TryGetStringField(TEXT("resultSummary"), Entry.ResultSummary);

		double ToolCallCountDouble = 0.0;
		if ((*EntryObj)->TryGetNumberField(TEXT("toolCallCount"), ToolCallCountDouble))
		{
			Entry.ToolCallCount = static_cast<int32>(ToolCallCountDouble);
		}

		double ScriptSizeBytesDouble = 0.0;
		if ((*EntryObj)->TryGetNumberField(TEXT("scriptSizeBytes"), ScriptSizeBytesDouble))
		{
			Entry.ScriptSizeBytes = static_cast<int32>(ScriptSizeBytesDouble);
		}

		FString TimestampStr;
		if ((*EntryObj)->TryGetStringField(TEXT("timestamp"), TimestampStr))
		{
			FDateTime::ParseIso8601(*TimestampStr, Entry.Timestamp);
		}

		if (!Entry.Id.IsEmpty())
		{
			Entries.Add(MoveTemp(Entry));
		}
	}

	UE_LOG(LogClaireon, Display, TEXT("[MCP] PythonAuditLog: Loaded %d entries from index"), Entries.Num());
}

void FClaireonPythonAuditLog::WriteIndex() const
{
	const FString AuditDir = GetAuditLogDir();
	IFileManager::Get().MakeDirectory(*AuditDir, true);

	TSharedPtr<FJsonObject> RootObject = MakeShared<FJsonObject>();
	RootObject->SetNumberField(TEXT("version"), 1);

	TArray<TSharedPtr<FJsonValue>> EntriesArray;
	for (const FAuditEntry& Entry : Entries)
	{
		TSharedPtr<FJsonObject> EntryObj = MakeShared<FJsonObject>();
		EntryObj->SetStringField(TEXT("id"), Entry.Id);
		EntryObj->SetStringField(TEXT("timestamp"), Entry.Timestamp.ToIso8601());
		EntryObj->SetStringField(TEXT("scriptPath"), Entry.ScriptPath);
		EntryObj->SetStringField(TEXT("outputPath"), Entry.OutputPath);
		EntryObj->SetNumberField(TEXT("scriptSizeBytes"), Entry.ScriptSizeBytes);
		EntryObj->SetBoolField(TEXT("success"), Entry.bSuccess);
		EntryObj->SetNumberField(TEXT("durationMs"), Entry.DurationMs);
		EntryObj->SetNumberField(TEXT("toolCallCount"), Entry.ToolCallCount);
		EntryObj->SetStringField(TEXT("resultSummary"), Entry.ResultSummary);
		EntryObj->SetStringField(TEXT("scriptPreview"), Entry.ScriptPreview);
		EntriesArray.Add(MakeShared<FJsonValueObject>(EntryObj));
	}

	RootObject->SetArrayField(TEXT("entries"), EntriesArray);

	FString OutputString;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(RootObject.ToSharedRef(), Writer);

	const FString IndexPath = AuditDir / TEXT("index.json");
	if (!FFileHelper::SaveStringToFile(OutputString, *IndexPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogClaireon, Warning, TEXT("[MCP] PythonAuditLog: Failed to write index.json"));
	}
}

void FClaireonPythonAuditLog::RotateEntries()
{
	if (Entries.Num() <= MaxEntries)
	{
		return;
	}

	// Batch rotation: when we need to evict, remove all entries from the same day
	// as the oldest entry (up to MaxBatchRotation). This avoids rotating 1 entry
	// at a time on every call and keeps the log on clean day boundaries.
	static constexpr int32 MaxBatchRotation = 300;

	const FDateTime OldestDay = Entries[0].Timestamp.GetDate();
	int32 NumToRemove = 0;

	for (int32 i = 0; i < Entries.Num() && NumToRemove < MaxBatchRotation; ++i)
	{
		if (Entries[i].Timestamp.GetDate() == OldestDay)
		{
			NumToRemove = i + 1;
		}
		else
		{
			break;
		}
	}

	// Always remove at least enough to get back under MaxEntries
	NumToRemove = FMath::Max(NumToRemove, Entries.Num() - MaxEntries);

	const FString AuditDir = GetAuditLogDir();

	for (int32 i = 0; i < NumToRemove; ++i)
	{
		const FAuditEntry& Entry = Entries[i];

		// Delete script file
		if (!Entry.ScriptPath.IsEmpty())
		{
			IFileManager::Get().Delete(*(AuditDir / Entry.ScriptPath));
		}

		// Delete output file
		if (!Entry.OutputPath.IsEmpty())
		{
			IFileManager::Get().Delete(*(AuditDir / Entry.OutputPath));
		}
	}

	Entries.RemoveAt(0, NumToRemove);

	UE_LOG(LogClaireon, Display, TEXT("[MCP] PythonAuditLog: Rotated %d old entries (batch by day), %d remaining"), NumToRemove, Entries.Num());
}

void FClaireonPythonAuditLog::RecordInvocation(
	const FString& ScriptText,
	const FString& Output,
	bool bSuccess,
	double DurationMs,
	int32 ToolCallCount,
	const FString& ResultSummary)
{
	FScopeLock Lock(&CriticalSection);

	// Ensure index is loaded before we append
	LoadIndex();

	const FString AuditDir = GetAuditLogDir();
	const FString ScriptsDir = AuditDir / TEXT("scripts");
	IFileManager::Get().MakeDirectory(*ScriptsDir, true);

	// Build entry
	FAuditEntry Entry;
	Entry.Id = GenerateEntryId();
	Entry.Timestamp = FDateTime::UtcNow();
	Entry.ScriptPath = FString::Printf(TEXT("scripts/%s.py"), *Entry.Id);
	Entry.OutputPath = FString::Printf(TEXT("scripts/%s_output.txt"), *Entry.Id);
	Entry.ScriptSizeBytes = ScriptText.Len();
	Entry.bSuccess = bSuccess;
	Entry.DurationMs = DurationMs;
	Entry.ToolCallCount = ToolCallCount;
	Entry.ResultSummary = ResultSummary;

	// Generate preview: first PreviewLength chars, collapse whitespace
	FString Preview = ScriptText.Left(PreviewLength);
	Preview.ReplaceInline(TEXT("\r\n"), TEXT(" "));
	Preview.ReplaceInline(TEXT("\n"), TEXT(" "));
	Preview.ReplaceInline(TEXT("\r"), TEXT(" "));
	Entry.ScriptPreview = Preview;

	// Write script file
	const FString ScriptFilePath = AuditDir / Entry.ScriptPath;
	if (!FFileHelper::SaveStringToFile(ScriptText, *ScriptFilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogClaireon, Warning, TEXT("[MCP] PythonAuditLog: Failed to save script file: %s"), *ScriptFilePath);
	}

	// Write output file
	const FString OutputFilePath = AuditDir / Entry.OutputPath;
	if (!FFileHelper::SaveStringToFile(Output, *OutputFilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogClaireon, Warning, TEXT("[MCP] PythonAuditLog: Failed to save output file: %s"), *OutputFilePath);
	}

	Entries.Add(MoveTemp(Entry));

	// Rotate if needed
	RotateEntries();

	// Persist index
	WriteIndex();

	UE_LOG(LogClaireon, Display, TEXT("[MCP] PythonAuditLog: Recorded invocation %s (success=%s, %.1fms)"),
		*Entries.Last().Id, bSuccess ? TEXT("true") : TEXT("false"), DurationMs);
}

FString FClaireonPythonAuditLog::GetRecentEntries(int32 Limit, TOptional<bool> FilterSuccess) const
{
	FScopeLock Lock(&CriticalSection);

	// Need to load index if not yet loaded — but LoadIndex is non-const, so cast away const
	// since the mutable CriticalSection already indicates this is a logically const operation
	// that may need to perform one-time initialization
	const_cast<FClaireonPythonAuditLog*>(this)->LoadIndex();

	// Build a filtered list iterating from newest to oldest
	TArray<const FAuditEntry*> Filtered;
	for (int32 i = Entries.Num() - 1; i >= 0 && Filtered.Num() < Limit; --i)
	{
		const FAuditEntry& Entry = Entries[i];
		if (FilterSuccess.IsSet() && Entry.bSuccess != FilterSuccess.GetValue())
		{
			continue;
		}
		Filtered.Add(&Entry);
	}

	// Build JSON response
	TSharedPtr<FJsonObject> RootObject = MakeShared<FJsonObject>();
	RootObject->SetBoolField(TEXT("success"), true);
	RootObject->SetNumberField(TEXT("totalEntries"), Entries.Num());

	TArray<TSharedPtr<FJsonValue>> EntriesArray;
	for (const FAuditEntry* Entry : Filtered)
	{
		TSharedPtr<FJsonObject> EntryObj = MakeShared<FJsonObject>();
		EntryObj->SetStringField(TEXT("id"), Entry->Id);
		EntryObj->SetStringField(TEXT("timestamp"), Entry->Timestamp.ToIso8601());
		EntryObj->SetBoolField(TEXT("success"), Entry->bSuccess);
		EntryObj->SetNumberField(TEXT("durationMs"), Entry->DurationMs);
		EntryObj->SetNumberField(TEXT("scriptSizeBytes"), Entry->ScriptSizeBytes);
		EntryObj->SetNumberField(TEXT("toolCallCount"), Entry->ToolCallCount);
		EntryObj->SetStringField(TEXT("resultSummary"), Entry->ResultSummary);
		EntryObj->SetStringField(TEXT("scriptPreview"), Entry->ScriptPreview);
		EntriesArray.Add(MakeShared<FJsonValueObject>(EntryObj));
	}

	RootObject->SetArrayField(TEXT("entries"), EntriesArray);

	FString OutputString;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(RootObject.ToSharedRef(), Writer);

	return OutputString;
}
