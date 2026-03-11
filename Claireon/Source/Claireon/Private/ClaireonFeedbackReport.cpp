// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonFeedbackReport.h"
#include "ClaireonLog.h"
#include "ClaireonSettings.h"
#include "ClaireonPythonAuditLog.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#define LOCTEXT_NAMESPACE "ClaireonFeedbackReport"

static const FString OpusModelId = TEXT("claude-opus-4-6");

FString FClaireonFeedbackReport::GetReportDir()
{
	return FPaths::ProjectSavedDir() / TEXT("MCP") / TEXT("FeedbackReports");
}

FString FClaireonFeedbackReport::GenerateReportFilename()
{
	const FDateTime Now = FDateTime::Now();
	return FString::Printf(TEXT("report_%s.md"),
		*Now.ToString(TEXT("%Y-%m-%d_%H%M%S")));
}

FString FClaireonFeedbackReport::AggregateFeedbackEntries(int32 MaxEntries)
{
	const FString FeedbackDir = FPaths::ProjectSavedDir() / TEXT("MCP") / TEXT("Feedback");
	const FString IndexPath = FeedbackDir / TEXT("index.json");

	FString IndexJson;
	if (!FFileHelper::LoadFileToString(IndexJson, *IndexPath))
	{
		return TEXT("(No feedback entries found)");
	}

	TSharedPtr<FJsonObject> IndexObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(IndexJson);
	if (!FJsonSerializer::Deserialize(Reader, IndexObj) || !IndexObj.IsValid())
	{
		return TEXT("(Failed to parse feedback index)");
	}

	const TArray<TSharedPtr<FJsonValue>>* EntriesArray = nullptr;
	if (!IndexObj->TryGetArrayField(TEXT("entries"), EntriesArray) || EntriesArray->Num() == 0)
	{
		return TEXT("(No feedback entries found)");
	}

	FString Result;
	const int32 StartIdx = FMath::Max(0, EntriesArray->Num() - MaxEntries);

	for (int32 i = StartIdx; i < EntriesArray->Num(); ++i)
	{
		const TSharedPtr<FJsonObject>* EntryObj = nullptr;
		if (!(*EntriesArray)[i]->TryGetObject(EntryObj) || !(*EntryObj).IsValid())
		{
			continue;
		}

		FString Id;
		(*EntryObj)->TryGetStringField(TEXT("id"), Id);

		// Try to read the full entry file for complete text
		FString FullText;
		const FString EntryPath = FeedbackDir / TEXT("entries") / (Id + TEXT(".json"));
		FString EntryJson;
		if (FFileHelper::LoadFileToString(EntryJson, *EntryPath))
		{
			TSharedPtr<FJsonObject> FullEntryObj;
			TSharedRef<TJsonReader<>> EntryReader = TJsonReaderFactory<>::Create(EntryJson);
			if (FJsonSerializer::Deserialize(EntryReader, FullEntryObj) && FullEntryObj.IsValid())
			{
				FullEntryObj->TryGetStringField(TEXT("text"), FullText);
			}
		}

		// Fall back to preview if full text not available
		if (FullText.IsEmpty())
		{
			(*EntryObj)->TryGetStringField(TEXT("textPreview"), FullText);
		}

		FString Timestamp;
		(*EntryObj)->TryGetStringField(TEXT("timestamp"), Timestamp);

		bool bIsBug = false, bIsFeedback = false, bIsSuggestion = false;
		(*EntryObj)->TryGetBoolField(TEXT("isBug"), bIsBug);
		(*EntryObj)->TryGetBoolField(TEXT("isFeedback"), bIsFeedback);
		(*EntryObj)->TryGetBoolField(TEXT("isSuggestion"), bIsSuggestion);

		FString Type;
		if (bIsBug) Type += TEXT("[Bug] ");
		if (bIsFeedback) Type += TEXT("[Feedback] ");
		if (bIsSuggestion) Type += TEXT("[Suggestion] ");

		Result += FString::Printf(TEXT("### Entry %s (%s)\n%s%s\n\n"),
			*Id, *Timestamp, *Type, *FullText);
	}

	return Result.IsEmpty() ? TEXT("(No feedback entries found)") : Result;
}

FString FClaireonFeedbackReport::AggregatePythonAuditEntries(int32 MaxEntries)
{
	return FClaireonPythonAuditLog::Get().GetRecentEntries(MaxEntries);
}

FString FClaireonFeedbackReport::BuildAnalysisPrompt(
	const FString& FeedbackData, const FString& PythonAuditData)
{
	return FString::Printf(TEXT(
		"You are analyzing feedback and usage data from an MCP (Model Context Protocol) integration "
		"in the Unreal Editor. The MCP server exposes editor tools "
		"to AI assistants (Claude) for tasks like editing blueprints, running Python scripts, "
		"searching assets, managing State Trees, etc.\n\n"
		"Please analyze the following data and produce a structured report with:\n"
		"1. **Summary**: High-level overview of feedback themes and usage patterns\n"
		"2. **Feedback Analysis**: Categorize and summarize the feedback entries (bugs, suggestions, general)\n"
		"3. **Python Execution Analysis**: Analyze the Python script execution patterns - what are common use cases, "
		"what scripts failed and why, are there opportunities for new dedicated MCP tools to replace common Python patterns\n"
		"4. **Recommendations**: Specific, actionable suggestions for:\n"
		"   - MCP tool improvements or new tools to add\n"
		"   - Workflow improvements for designer/developer experience\n"
		"   - Common pain points that should be addressed\n\n"
		"Be concise but thorough. Focus on actionable insights.\n\n"
		"---\n\n"
		"## Feedback Entries\n\n%s\n\n"
		"---\n\n"
		"## Python Execution Audit Log\n\n%s"),
		*FeedbackData, *PythonAuditData);
}

FString FClaireonFeedbackReport::FormatReport(
	const FString& OpusAnalysis, const FString& FeedbackData)
{
	const FDateTime Now = FDateTime::Now();
	return FString::Printf(TEXT(
		"# MCP Feedback Report - %s\n\n"
		"*Generated by Claude Opus analysis*\n\n"
		"---\n\n"
		"%s\n\n"
		"---\n\n"
		"## Raw Feedback Data\n\n"
		"<details>\n<summary>Click to expand raw feedback entries</summary>\n\n"
		"%s\n"
		"</details>\n"),
		*Now.ToString(TEXT("%Y-%m-%d %H:%M")),
		*OpusAnalysis,
		*FeedbackData);
}

void FClaireonFeedbackReport::Generate(FOnFeedbackReportComplete OnComplete)
{
	// Validate API key
	const UClaireonSettings* Settings = UClaireonSettings::Get();
	if (!Settings || Settings->AnthropicApiKey.IsEmpty())
	{
		OnComplete.ExecuteIfBound(false, TEXT("No API key configured. Set your Anthropic API key in Editor Preferences > Plugins > MCP REPL."));
		return;
	}

	// Aggregate data
	const FString FeedbackData = AggregateFeedbackEntries();
	const FString PythonAuditData = AggregatePythonAuditEntries();

	if (FeedbackData.Contains(TEXT("(No feedback entries found)")) &&
		PythonAuditData.Contains(TEXT("[]")))
	{
		OnComplete.ExecuteIfBound(false, TEXT("No feedback or Python audit data found to analyze."));
		return;
	}

	// Build API request
	const FString Prompt = BuildAnalysisPrompt(FeedbackData, PythonAuditData);

	TSharedPtr<FJsonObject> RequestBody = MakeShared<FJsonObject>();
	RequestBody->SetStringField(TEXT("model"), OpusModelId);
	RequestBody->SetNumberField(TEXT("max_tokens"), 4096);

	TArray<TSharedPtr<FJsonValue>> Messages;
	TSharedPtr<FJsonObject> UserMessage = MakeShared<FJsonObject>();
	UserMessage->SetStringField(TEXT("role"), TEXT("user"));
	UserMessage->SetStringField(TEXT("content"), Prompt);
	Messages.Add(MakeShared<FJsonValueObject>(UserMessage));
	RequestBody->SetArrayField(TEXT("messages"), Messages);

	FString RequestBodyString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBodyString);
	FJsonSerializer::Serialize(RequestBody.ToSharedRef(), Writer);

	// Send to Opus
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetURL(Settings->ApiEndpointUrl);
	HttpRequest->SetVerb(TEXT("POST"));
	HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	HttpRequest->SetHeader(TEXT("x-api-key"), Settings->AnthropicApiKey);
	HttpRequest->SetHeader(TEXT("anthropic-version"), Settings->AnthropicVersion);
	HttpRequest->SetContentAsString(RequestBodyString);
	HttpRequest->SetTimeout(120.0f); // Opus can take a while

	HttpRequest->OnProcessRequestComplete().BindLambda(
		[OnComplete, FeedbackData](FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully)
	{
		if (!bConnectedSuccessfully || !Response.IsValid())
		{
			OnComplete.ExecuteIfBound(false, TEXT("Failed to connect to Anthropic API."));
			return;
		}

		const int32 Code = Response->GetResponseCode();
		if (Code != 200)
		{
			OnComplete.ExecuteIfBound(false,
				FString::Printf(TEXT("API error (HTTP %d): %s"), Code,
					*Response->GetContentAsString().Left(500)));
			return;
		}

		// Parse response
		TSharedPtr<FJsonObject> ResponseObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
		if (!FJsonSerializer::Deserialize(Reader, ResponseObj) || !ResponseObj.IsValid())
		{
			OnComplete.ExecuteIfBound(false, TEXT("Failed to parse API response."));
			return;
		}

		// Extract text from response content array
		FString AnalysisText;
		const TArray<TSharedPtr<FJsonValue>>* ContentArray = nullptr;
		if (ResponseObj->TryGetArrayField(TEXT("content"), ContentArray))
		{
			for (const TSharedPtr<FJsonValue>& ContentValue : *ContentArray)
			{
				const TSharedPtr<FJsonObject>* ContentObj = nullptr;
				if (ContentValue->TryGetObject(ContentObj) && (*ContentObj).IsValid())
				{
					FString Type;
					if ((*ContentObj)->TryGetStringField(TEXT("type"), Type) && Type == TEXT("text"))
					{
						FString Text;
						if ((*ContentObj)->TryGetStringField(TEXT("text"), Text))
						{
							AnalysisText += Text;
						}
					}
				}
			}
		}

		if (AnalysisText.IsEmpty())
		{
			OnComplete.ExecuteIfBound(false, TEXT("Opus returned empty analysis."));
			return;
		}

		// Format report
		const FString Report = FormatReport(AnalysisText, FeedbackData);

		// Save to file
		const FString ReportDir = GetReportDir();
		IFileManager::Get().MakeDirectory(*ReportDir, true);
		const FString Filename = GenerateReportFilename();
		const FString FilePath = ReportDir / Filename;

		if (!FFileHelper::SaveStringToFile(Report, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OnComplete.ExecuteIfBound(false,
				FString::Printf(TEXT("Failed to save report to: %s"), *FilePath));
			return;
		}

		// Copy to clipboard with file path as first line
		const FString AbsolutePath = FPaths::ConvertRelativePathToFull(FilePath);
		const FString ClipboardContent = AbsolutePath + TEXT("\n\n") + Report;
		FPlatformApplicationMisc::ClipboardCopy(*ClipboardContent);

		UE_LOG(LogClaireon, Display, TEXT("[MCP] Feedback report saved to: %s"), *AbsolutePath);

		OnComplete.ExecuteIfBound(true,
			FString::Printf(TEXT("Report saved to %s and copied to clipboard."), *AbsolutePath));
	});

	HttpRequest->ProcessRequest();
	UE_LOG(LogClaireon, Display, TEXT("[MCP] Generating feedback report via Opus..."));
}

#undef LOCTEXT_NAMESPACE
