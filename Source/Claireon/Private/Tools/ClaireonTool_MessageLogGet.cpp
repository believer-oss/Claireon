// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_MessageLogGet.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "IMessageLogListing.h"
#include "Logging/TokenizedMessage.h"
#include "MessageLogModule.h"
#include "Misc/UObjectToken.h"
#include "Modules/ModuleManager.h"
#include "Presentation/MessageLogListingViewModel.h"

namespace ClaireonTool_MessageLogGetInternal
{

const TCHAR* MessageLogGet_SeverityTag(EMessageSeverity::Type Severity)
{
	if (Severity == EMessageSeverity::Error)
	{
		return TEXT("error");
	}
	if (Severity == EMessageSeverity::PerformanceWarning)
	{
		return TEXT("perf_warning");
	}
	if (Severity == EMessageSeverity::Warning)
	{
		return TEXT("warning");
	}
	return TEXT("info");
}

bool MessageLogGet_MessageReferencesAsset(const FTokenizedMessage& Msg, const FString& AssetPath)
{
	for (const TSharedRef<IMessageToken>& Token : Msg.GetMessageTokens())
	{
		const EMessageToken::Type TokenType = Token->GetType();
		if (TokenType == EMessageToken::Object)
		{
			const FUObjectToken& UObjT = static_cast<const FUObjectToken&>(*Token);
			const FString& Path = UObjT.GetOriginalObjectPathName();
			if (Path.Equals(AssetPath, ESearchCase::IgnoreCase) ||
				Path.Contains(AssetPath, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		else if (TokenType == EMessageToken::AssetName)
		{
			const FAssetNameToken& AnT = static_cast<const FAssetNameToken&>(*Token);
			if (AnT.GetAssetName().Contains(AssetPath, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
	}
	return false;
}

TSharedPtr<FJsonObject> MessageLogGet_FlattenToken(const TSharedRef<IMessageToken>& Token)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	const EMessageToken::Type TokenType = Token->GetType();

	const TCHAR* TypeStr = TEXT("Text");
	switch (TokenType)
	{
		case EMessageToken::Action:        TypeStr = TEXT("Action"); break;
		case EMessageToken::Actor:         TypeStr = TEXT("Actor"); break;
		case EMessageToken::AssetName:     TypeStr = TEXT("AssetName"); break;
		case EMessageToken::AssetData:     TypeStr = TEXT("AssetData"); break;
		case EMessageToken::Documentation: TypeStr = TEXT("Documentation"); break;
		case EMessageToken::Image:         TypeStr = TEXT("Image"); break;
		case EMessageToken::Object:        TypeStr = TEXT("Object"); break;
		case EMessageToken::Severity:      TypeStr = TEXT("Severity"); break;
		case EMessageToken::Text:          TypeStr = TEXT("Text"); break;
		case EMessageToken::Tutorial:      TypeStr = TEXT("Tutorial"); break;
		case EMessageToken::URL:           TypeStr = TEXT("URL"); break;
		case EMessageToken::EdGraph:       TypeStr = TEXT("EdGraph"); break;
		case EMessageToken::DynamicText:   TypeStr = TEXT("DynamicText"); break;
		case EMessageToken::Fix:           TypeStr = TEXT("Fix"); break;
		default:                           TypeStr = TEXT("Text"); break;
	}

	Obj->SetStringField(TEXT("type"), TypeStr);
	Obj->SetStringField(TEXT("text"), Token->ToText().ToString());

	if (TokenType == EMessageToken::Object)
	{
		const FUObjectToken& UObjT = static_cast<const FUObjectToken&>(*Token);
		Obj->SetStringField(TEXT("object_path"), UObjT.GetOriginalObjectPathName());
	}
	else if (TokenType == EMessageToken::AssetName)
	{
		const FAssetNameToken& AnT = static_cast<const FAssetNameToken&>(*Token);
		Obj->SetStringField(TEXT("asset_name"), AnT.GetAssetName());
	}

	return Obj;
}

}  // namespace ClaireonTool_MessageLogGetInternal

FString ClaireonTool_MessageLogGet::GetOperation() const { return TEXT("message_log_get"); }

FString ClaireonTool_MessageLogGet::GetCategory() const
{
	return TEXT("editor");
}

FString ClaireonTool_MessageLogGet::GetDescription() const
{
    return TEXT("Read the editor Message Log panel buffer for any category (Sequencer, AssetCheck, MapCheck, LoadErrors, Blueprint, PIE, etc.). Returns the listing's per-page filtered container; pass page='all' to span every archived page (cost is unbounded -- prefer page='current'). Category names are case-sensitive FNames. Stateless / read-only / non-session.");
}

TSharedPtr<FJsonObject> ClaireonTool_MessageLogGet::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> CategoryProp = MakeShared<FJsonObject>();
	CategoryProp->SetStringField(TEXT("type"), TEXT("string"));
	CategoryProp->SetStringField(TEXT("description"),
		TEXT("Message Log category name (e.g. Sequencer, AssetCheck, MapCheck, LoadErrors, Blueprint, PIE, BuildAndSubmitErrors, AnimBlueprintLog, PythonLog, MaterialLog). Case-sensitive: must match the FName the producer registered."));
	Properties->SetObjectField(TEXT("category"), CategoryProp);

	TSharedPtr<FJsonObject> SeverityProp = MakeShared<FJsonObject>();
	SeverityProp->SetStringField(TEXT("type"), TEXT("string"));
	SeverityProp->SetStringField(TEXT("description"),
		TEXT("Filter: 'error' | 'warning' | 'perf_warning' | 'info' | 'all'. Default 'all'. Each tag is exact -- 'warning' returns only EMessageSeverity::Warning; 'perf_warning' returns only EMessageSeverity::PerformanceWarning. 'all' returns every severity."));
	Properties->SetObjectField(TEXT("severity"), SeverityProp);

	TSharedPtr<FJsonObject> PageProp = MakeShared<FJsonObject>();
	PageProp->SetStringField(TEXT("type"), TEXT("string"));
	PageProp->SetStringField(TEXT("description"),
		TEXT("Which page(s) of the listing to read. 'current' (default) returns only the active page. 'all' iterates every archived page oldest-first and concatenates messages. An integer string ('0', '1', ...) reads that specific page index."));
	Properties->SetObjectField(TEXT("page"), PageProp);

	TSharedPtr<FJsonObject> MaxProp = MakeShared<FJsonObject>();
	MaxProp->SetStringField(TEXT("type"), TEXT("integer"));
	MaxProp->SetStringField(TEXT("description"),
		TEXT("Truncate at this count (default 200, max 1000). Total counts in the result reflect pre-truncation totals across all pages requested."));
	Properties->SetObjectField(TEXT("max_messages"), MaxProp);

	TSharedPtr<FJsonObject> AssetProp = MakeShared<FJsonObject>();
	AssetProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetProp->SetStringField(TEXT("description"),
		TEXT("Optional. Retain only messages whose tokens reference this asset. Matches FUObjectToken::GetOriginalObjectPathName() (full /Game/... path equality preferred) or FAssetNameToken::GetAssetName() (case-insensitive substring fallback)."));
	Properties->SetObjectField(TEXT("asset_path"), AssetProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("category")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_MessageLogGet::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString Category;
	if (!Arguments->TryGetStringField(TEXT("category"), Category) || Category.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: category"));
	}

	FString SeverityFilter = TEXT("all");
	Arguments->TryGetStringField(TEXT("severity"), SeverityFilter);
	if (SeverityFilter.IsEmpty())
	{
		SeverityFilter = TEXT("all");
	}

	FString PageArg = TEXT("current");
	Arguments->TryGetStringField(TEXT("page"), PageArg);
	if (PageArg.IsEmpty())
	{
		PageArg = TEXT("current");
	}

	int32 MaxMessages = 200;
	Arguments->TryGetNumberField(TEXT("max_messages"), MaxMessages);
	MaxMessages = FMath::Clamp(MaxMessages, 1, 1000);

	FString AssetPath;
	Arguments->TryGetStringField(TEXT("asset_path"), AssetPath);

	FMessageLogModule& Module = FModuleManager::LoadModuleChecked<FMessageLogModule>("MessageLog");
	TSharedRef<IMessageLogListing> Listing = Module.GetLogListing(FName(*Category));
	TSharedRef<FMessageLogListingViewModel> ViewModel = StaticCastSharedRef<FMessageLogListingViewModel>(Listing);

	const uint32 PageCount = ViewModel->GetPageCount();
	const uint32 SavedCurrentPageIndex = ViewModel->GetCurrentPageIndex();

	TArray<uint32> PagesToRead;
	if (PageArg.Equals(TEXT("current"), ESearchCase::IgnoreCase))
	{
		PagesToRead.Add(SavedCurrentPageIndex);
	}
	else if (PageArg.Equals(TEXT("all"), ESearchCase::IgnoreCase))
	{
		if (PageCount == 0)
		{
			PagesToRead.Add(0);
		}
		else
		{
			for (int32 P = static_cast<int32>(PageCount) - 1; P >= 0; --P)
			{
				PagesToRead.Add(static_cast<uint32>(P));
			}
		}
	}
	else
	{
		const int32 ParsedIndex = FCString::Atoi(*PageArg);
		if (ParsedIndex < 0 || (PageCount > 0 && static_cast<uint32>(ParsedIndex) >= PageCount))
		{
			return MakeErrorResult(FString::Printf(
				TEXT("page index %d out of range [0, %u)"), ParsedIndex, PageCount));
		}
		PagesToRead.Add(static_cast<uint32>(ParsedIndex));
	}

	int32 TotalMessages = 0;
	int32 FilteredCount = 0;
	int32 ErrorCount = 0;
	int32 WarningCount = 0;
	int32 PerfWarningCount = 0;
	int32 InfoCount = 0;

	TArray<TSharedPtr<FJsonValue>> MessagesArray;

	for (const uint32 P : PagesToRead)
	{
		if (P != ViewModel->GetCurrentPageIndex())
		{
			ViewModel->SetCurrentPageIndex(P);
		}

		const TArray<TSharedRef<FTokenizedMessage>>& PageMessages = ViewModel->GetFilteredMessages();
		for (const TSharedRef<FTokenizedMessage>& Msg : PageMessages)
		{
			const EMessageSeverity::Type S = Msg->GetSeverity();
			const TCHAR* Tag = ClaireonTool_MessageLogGetInternal::MessageLogGet_SeverityTag(S);

			TotalMessages++;
			if (S == EMessageSeverity::Error)
			{
				ErrorCount++;
			}
			else if (S == EMessageSeverity::PerformanceWarning)
			{
				PerfWarningCount++;
			}
			else if (S == EMessageSeverity::Warning)
			{
				WarningCount++;
			}
			else
			{
				InfoCount++;
			}

			if (!SeverityFilter.Equals(TEXT("all"), ESearchCase::IgnoreCase) &&
				!SeverityFilter.Equals(Tag, ESearchCase::IgnoreCase))
			{
				continue;
			}

			if (!AssetPath.IsEmpty() && !ClaireonTool_MessageLogGetInternal::MessageLogGet_MessageReferencesAsset(*Msg, AssetPath))
			{
				continue;
			}

			TArray<TSharedPtr<FJsonValue>> TokensArray;
			for (const TSharedRef<IMessageToken>& Token : Msg->GetMessageTokens())
			{
				TokensArray.Add(MakeShared<FJsonValueObject>(ClaireonTool_MessageLogGetInternal::MessageLogGet_FlattenToken(Token)));
			}

			TSharedPtr<FJsonObject> MsgObj = MakeShared<FJsonObject>();
			MsgObj->SetStringField(TEXT("severity"), Tag);
			MsgObj->SetNumberField(TEXT("page_index"), P);
			MsgObj->SetStringField(TEXT("text"), Msg->ToText().ToString());
			MsgObj->SetArrayField(TEXT("tokens"), TokensArray);

			MessagesArray.Add(MakeShared<FJsonValueObject>(MsgObj));
			FilteredCount++;
		}
	}

	if (SavedCurrentPageIndex != ViewModel->GetCurrentPageIndex())
	{
		ViewModel->SetCurrentPageIndex(SavedCurrentPageIndex);
	}

	if (MessagesArray.Num() > MaxMessages)
	{
		MessagesArray.RemoveAt(0, MessagesArray.Num() - MaxMessages);
	}
	const int32 ReturnedCount = MessagesArray.Num();

	TArray<TSharedPtr<FJsonValue>> PagesReadArray;
	for (const uint32 P : PagesToRead)
	{
		PagesReadArray.Add(MakeShared<FJsonValueNumber>(P));
	}

	TSharedPtr<FJsonObject> SeverityCounts = MakeShared<FJsonObject>();
	SeverityCounts->SetNumberField(TEXT("error"), ErrorCount);
	SeverityCounts->SetNumberField(TEXT("warning"), WarningCount);
	SeverityCounts->SetNumberField(TEXT("perf_warning"), PerfWarningCount);
	SeverityCounts->SetNumberField(TEXT("info"), InfoCount);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("category"), Category);
	Data->SetStringField(TEXT("page_requested"), PageArg);
	Data->SetNumberField(TEXT("page_count"), PageCount);
	Data->SetArrayField(TEXT("pages_read"), PagesReadArray);
	Data->SetArrayField(TEXT("messages"), MessagesArray);
	Data->SetNumberField(TEXT("total_messages"), TotalMessages);
	Data->SetNumberField(TEXT("filtered_count"), FilteredCount);
	Data->SetNumberField(TEXT("returned_count"), ReturnedCount);
	Data->SetObjectField(TEXT("severity_counts"), SeverityCounts);

	FString PageLabel;
	if (PageArg.Equals(TEXT("all"), ESearchCase::IgnoreCase))
	{
		PageLabel = FString::Printf(TEXT("[all %u pages]"), PageCount);
	}
	else
	{
		const uint32 LabelPage = (PagesToRead.Num() > 0) ? PagesToRead[0] : 0;
		PageLabel = FString::Printf(TEXT("[page %u/%u]"), LabelPage, PageCount);
	}

	const FString Summary = FString::Printf(
		TEXT("%s message log %s: %d of %d messages match (%d errors, %d warnings, %d perf_warnings, %d info; showing %d)"),
		*Category, *PageLabel, FilteredCount, TotalMessages,
		ErrorCount, WarningCount, PerfWarningCount, InfoCount, ReturnedCount);

	return MakeSuccessResult(Data, Summary);
}
