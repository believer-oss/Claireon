// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_FeedbackSubmit.h"
#include "ClaireonFeedbackLog.h"

FString ClaireonTool_FeedbackSubmit::GetCategory() const { return TEXT("feedback"); }
FString ClaireonTool_FeedbackSubmit::GetOperation() const { return TEXT("submit"); }

FString ClaireonTool_FeedbackSubmit::GetDescription() const
{
	return TEXT("Record feedback about MCP tools, workflows, bugs, or feature suggestions. "
		"Feedback is persisted locally and aggregated into periodic reports. "
		"One submission per session is sufficient.");
}

TSharedPtr<FJsonObject> ClaireonTool_FeedbackSubmit::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// text - required
	TSharedPtr<FJsonObject> TextProp = MakeShared<FJsonObject>();
	TextProp->SetStringField(TEXT("type"), TEXT("string"));
	TextProp->SetStringField(TEXT("description"),
		TEXT("The feedback text. Be specific: what worked, what was confusing, bugs encountered, or feature suggestions."));
	Properties->SetObjectField(TEXT("text"), TextProp);

	// is_bug - optional
	TSharedPtr<FJsonObject> IsBugProp = MakeShared<FJsonObject>();
	IsBugProp->SetStringField(TEXT("type"), TEXT("boolean"));
	IsBugProp->SetStringField(TEXT("description"), TEXT("True if this is a bug report."));
	Properties->SetObjectField(TEXT("is_bug"), IsBugProp);

	// is_suggestion - optional
	TSharedPtr<FJsonObject> IsSuggestionProp = MakeShared<FJsonObject>();
	IsSuggestionProp->SetStringField(TEXT("type"), TEXT("boolean"));
	IsSuggestionProp->SetStringField(TEXT("description"), TEXT("True if this is a feature suggestion."));
	Properties->SetObjectField(TEXT("is_suggestion"), IsSuggestionProp);

	// related_tools - optional
	TSharedPtr<FJsonObject> ToolsProp = MakeShared<FJsonObject>();
	ToolsProp->SetStringField(TEXT("type"), TEXT("array"));
	TSharedPtr<FJsonObject> ToolsItems = MakeShared<FJsonObject>();
	ToolsItems->SetStringField(TEXT("type"), TEXT("string"));
	ToolsProp->SetObjectField(TEXT("items"), ToolsItems);
	ToolsProp->SetStringField(TEXT("description"), TEXT("MCP tool names related to this feedback."));
	Properties->SetObjectField(TEXT("related_tools"), ToolsProp);

	// related_features - optional
	TSharedPtr<FJsonObject> FeaturesProp = MakeShared<FJsonObject>();
	FeaturesProp->SetStringField(TEXT("type"), TEXT("array"));
	TSharedPtr<FJsonObject> FeaturesItems = MakeShared<FJsonObject>();
	FeaturesItems->SetStringField(TEXT("type"), TEXT("string"));
	FeaturesProp->SetObjectField(TEXT("items"), FeaturesItems);
	FeaturesProp->SetStringField(TEXT("description"), TEXT("Feature areas related to this feedback (e.g. 'blueprint editing', 'state tree')."));
	Properties->SetObjectField(TEXT("related_features"), FeaturesProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("text")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_FeedbackSubmit::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString Text;
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("'text' is required and must be non-empty."));
	}

	// Prefer the documented 'text' field; fall back to 'feedback' as an alias
	// when callers (e.g. some clients that mirror the tool name) supply it instead.
	// The published JSON schema still lists only 'text' as required.
	if (!Arguments->TryGetStringField(TEXT("text"), Text) || Text.IsEmpty())
	{
		FString FeedbackAlias;
		if (Arguments->TryGetStringField(TEXT("feedback"), FeedbackAlias) && !FeedbackAlias.IsEmpty())
		{
			Text = MoveTemp(FeedbackAlias);
		}
		else
		{
			return MakeErrorResult(TEXT("'text' is required and must be non-empty."));
		}
	}

	bool bIsBug = false;
	bool bIsSuggestion = false;
	Arguments->TryGetBoolField(TEXT("is_bug"), bIsBug);
	Arguments->TryGetBoolField(TEXT("is_suggestion"), bIsSuggestion);

	// If neither bug nor suggestion, it's general feedback
	bool bIsFeedback = !bIsBug && !bIsSuggestion;

	TArray<FString> RelatedTools;
	const TArray<TSharedPtr<FJsonValue>>* ToolsArray = nullptr;
	if (Arguments->TryGetArrayField(TEXT("related_tools"), ToolsArray))
	{
		for (const TSharedPtr<FJsonValue>& Val : *ToolsArray)
		{
			FString ToolStr;
			if (Val->TryGetString(ToolStr))
			{
				RelatedTools.Add(MoveTemp(ToolStr));
			}
		}
	}

	TArray<FString> RelatedFeatures;
	const TArray<TSharedPtr<FJsonValue>>* FeaturesArray = nullptr;
	if (Arguments->TryGetArrayField(TEXT("related_features"), FeaturesArray))
	{
		for (const TSharedPtr<FJsonValue>& Val : *FeaturesArray)
		{
			FString FeatureStr;
			if (Val->TryGetString(FeatureStr))
			{
				RelatedFeatures.Add(MoveTemp(FeatureStr));
			}
		}
	}

	FString EntryId = FClaireonFeedbackLog::Get().RecordFeedback(
		Text,
		RelatedTools,
		RelatedFeatures,
		bIsBug,
		bIsFeedback,
		bIsSuggestion,
		TEXT(""),  // OperatorName — not available in MCP context
		TEXT("claude-code")  // ClientInfo
	);

	if (EntryId.IsEmpty())
	{
		return MakeErrorResult(TEXT("Failed to persist feedback entry."));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("entry_id"), EntryId);
	Data->SetBoolField(TEXT("is_bug"), bIsBug);
	Data->SetBoolField(TEXT("is_feedback"), bIsFeedback);
	Data->SetBoolField(TEXT("is_suggestion"), bIsSuggestion);

	return MakeSuccessResult(Data, FString::Printf(TEXT("Feedback recorded: %s"), *EntryId));
}
