// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonXmlFormatter.h"
#include "ClaireonOutputGate.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"

FString FClaireonXmlFormatter::FormatExecuteResult(const IClaireonTool::FToolResult& Result)
{
	FString Xml;

	if (Result.bIsError)
	{
		// Error path
		FString ErrorCode = TEXT("execution_error");
		FString Suggestion;

		// Try to classify the error for better suggestions
		const FString& ErrorMsg = Result.ErrorMessage;
		if (ErrorMsg.Contains(TEXT("SyntaxError")))
		{
			ErrorCode = TEXT("syntax_error");
			Suggestion = TEXT("Check Python syntax. Common issues: missing colons, unmatched brackets, incorrect indentation.");
		}
		else if (ErrorMsg.Contains(TEXT("ImportError")) || ErrorMsg.Contains(TEXT("ModuleNotFoundError")))
		{
			ErrorCode = TEXT("import_error");
			Suggestion = TEXT("The module may not be available in the Unreal Python environment. Use 'import unreal' and claireon.* for editor operations.");
		}
		else if (ErrorMsg.Contains(TEXT("NameError")))
		{
			ErrorCode = TEXT("name_error");
			Suggestion = TEXT("Variable or function not defined. Use tool_search() to discover available tools, or check variable names.");
		}
		else if (ErrorMsg.Contains(TEXT("KeyError")))
		{
			ErrorCode = TEXT("key_error");
			Suggestion = TEXT("Dictionary key not found. Check the structure of the returned data with tool_search().");
		}
		else if (ErrorMsg.Contains(TEXT("AttributeError")))
		{
			ErrorCode = TEXT("attribute_error");
			Suggestion = TEXT("Object does not have the requested attribute. Use tool_search() to discover available tool names.");
		}
		else
		{
			Suggestion = TEXT("Review the error message and logs. Use tool_search() to discover available tools.");
		}

		Xml = FormatErrorResult(Result.ErrorMessage, ErrorCode, Suggestion, Result.Logs, Result.UELog);
	}
	else
	{
		// Check if this is a disk-spilled result (Output Gate envelope)
		bool bIsSpilled = false;
		if (Result.Data.IsValid())
		{
			bool bMCPSpilled = false;
			if (Result.Data->TryGetBoolField(TEXT("__mcp_spilled__"), bMCPSpilled) && bMCPSpilled)
			{
				bIsSpilled = true;

				// Parse spilled_streams array into FClaireonSpillStream entries.
				TArray<FClaireonSpillStream> Streams;
				const TArray<TSharedPtr<FJsonValue>>* StreamsArray = nullptr;
				if (Result.Data->TryGetArrayField(TEXT("spilled_streams"), StreamsArray) && StreamsArray)
				{
					for (const TSharedPtr<FJsonValue>& StreamVal : *StreamsArray)
					{
						const TSharedPtr<FJsonObject>* StreamObj = nullptr;
						if (!StreamVal.IsValid() || !StreamVal->TryGetObject(StreamObj) || !StreamObj || !(*StreamObj).IsValid())
						{
							continue;
						}
						FClaireonSpillStream Entry;
						(*StreamObj)->TryGetStringField(TEXT("name"), Entry.Name);
						(*StreamObj)->TryGetStringField(TEXT("absolute_path"), Entry.AbsolutePath);
						double SizeDouble = 0.0;
						if ((*StreamObj)->TryGetNumberField(TEXT("size_bytes"), SizeDouble))
						{
							Entry.SizeBytes = static_cast<int64>(SizeDouble);
						}
						(*StreamObj)->TryGetStringField(TEXT("content_type"), Entry.ContentType);
						(*StreamObj)->TryGetStringField(TEXT("preview"), Entry.Preview);
						(*StreamObj)->TryGetBoolField(TEXT("over_ceiling"), Entry.bOverCeiling);
						(*StreamObj)->TryGetBoolField(TEXT("write_failed"), Entry.bWriteFailed);
						(*StreamObj)->TryGetStringField(TEXT("error_text"), Entry.ErrorText);
						Streams.Add(MoveTemp(Entry));
					}
				}

				FString SpillSummary = Result.Summary;
				if (SpillSummary.IsEmpty())
				{
					SpillSummary = TEXT("Result data exceeded inline threshold and was written to disk.");
				}

				Xml = FormatSpilledResult(SpillSummary, Streams, Result.Logs, Result.UELog);
			}
		}

		if (!bIsSpilled)
		{
			// Standard success path
			Xml = TEXT("<execute-result status=\"success\">\n");

			// Summary first (required on success)
			FString Summary = Result.Summary;
			if (Summary.IsEmpty())
			{
				Summary = TEXT("Execution completed successfully.");
			}
			Xml += TEXT("<summary>\n") + Summary + TEXT("\n</summary>\n");

			// Structured data payload (if the tool produced one).
			// Without this block the MCP transport only delivers the summary text
			// to the caller, so tools that return rich Data (e.g. search,
			// which returns a categories/tools catalog) appear empty on the wire.
			if (Result.Data.IsValid())
			{
				FString DataJson;
				TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> DataWriter =
					TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&DataJson);
				FJsonSerializer::Serialize(Result.Data.ToSharedRef(), DataWriter);
				DataWriter->Close();
				Xml += TEXT("<data>\n") + DataJson + TEXT("\n</data>\n");
			}

			// Warnings
			for (const FString& Warning : Result.Warnings)
			{
				Xml += TEXT("<warning>\n") + Warning + TEXT("\n</warning>\n");
			}

			// Logs
			if (!Result.Logs.IsEmpty())
			{
				Xml += TEXT("<logs>\n") + Result.Logs + TEXT("\n</logs>\n");
			}

			// Engine UE_LOG output (Warning/Error level captured during execution)
			if (!Result.UELog.IsEmpty())
			{
				Xml += TEXT("  <ue_log>") + Result.UELog + TEXT("</ue_log>\n");
			}

			Xml += TEXT("</execute-result>");
		}
	}

	// Hint envelope (success AND error paths; also spilled results). The tool
	// attached a structured nudge (e.g. python_execute's tool_search /
	// uobject_inspect hints); without this block the MCP HTTP transport drops
	// Result.Hint entirely (BuildResultEnvelope only serves the Python-side
	// claireon.* call envelope). Insert before the LAST closing tag so log text
	// that happens to contain the tag cannot misplace it.
	if (Result.Hint.IsValid())
	{
		FString HintJson;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> HintWriter =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&HintJson);
		FJsonSerializer::Serialize(Result.Hint.ToSharedRef(), HintWriter);
		HintWriter->Close();

		const FString HintXml = TEXT("<hint>\n") + HintJson + TEXT("\n</hint>\n");
		const int32 CloseAt = Xml.Find(TEXT("</execute-result>"),
			ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (CloseAt >= 0)
		{
			Xml.InsertAt(CloseAt, HintXml);
		}
		else
		{
			Xml += HintXml;
		}
	}

	return Xml;
}

FString FClaireonXmlFormatter::FormatErrorResult(const FString& Error, const FString& ErrorCode, const FString& Suggestion, const FString& Logs, const FString& UELog)
{
	FString Xml = TEXT("<execute-result status=\"error\">\n");

	// Error first (required on failure)
	Xml += TEXT("<error code=\"") + ErrorCode + TEXT("\">\n") + Error + TEXT("\n</error>\n");

	// Suggestion
	if (!Suggestion.IsEmpty())
	{
		Xml += TEXT("<suggestion>\n") + Suggestion + TEXT("\n</suggestion>\n");
	}

	// Logs
	if (!Logs.IsEmpty())
	{
		Xml += TEXT("<logs>\n") + Logs + TEXT("\n</logs>\n");
	}

	// Engine UE_LOG output
	if (!UELog.IsEmpty())
	{
		Xml += TEXT("  <ue_log>") + UELog + TEXT("</ue_log>\n");
	}

	Xml += TEXT("</execute-result>");
	return Xml;
}

FString FClaireonXmlFormatter::FormatSpilledResult(
	const FString& Summary,
	const TArray<FClaireonSpillStream>& Streams,
	const FString& InlineLogs,
	const FString& InlineUELog)
{
	FString Xml = TEXT("<execute-result status=\"success\">\n");

	Xml += TEXT("<summary>\n") + Summary + TEXT("\n</summary>\n");

	Xml += TEXT("<spilled-result>\n");
	for (const FClaireonSpillStream& Stream : Streams)
	{
		Xml += FString::Printf(TEXT("<stream name=\"%s\">\n"), *Stream.Name);

		if (Stream.bWriteFailed)
		{
			Xml += TEXT("<error>\n") + Stream.ErrorText + TEXT("\n</error>\n");
		}
		else
		{
			Xml += TEXT("<path>\n") + Stream.AbsolutePath + TEXT("\n</path>\n");
		}

		Xml += FString::Printf(TEXT("<size-bytes>\n%lld\n</size-bytes>\n"), Stream.SizeBytes);
		Xml += TEXT("<content-type>\n") + Stream.ContentType + TEXT("\n</content-type>\n");

		// The preview is always a prefix of the on-disk file; whenever the preview is
		// shorter than the raw size the envelope surfaces <truncated>true</truncated>.
		const int64 PreviewBytes = static_cast<int64>(Stream.Preview.Len());
		if (PreviewBytes < Stream.SizeBytes)
		{
			Xml += TEXT("<truncated>true</truncated>\n");
		}

		if (Stream.bOverCeiling)
		{
			Xml += TEXT("<over-ceiling>true</over-ceiling>\n");
		}

		if (!Stream.bWriteFailed)
		{
			Xml += TEXT("<preview>\n") + Stream.Preview + TEXT("\n</preview>\n");
		}

		Xml += TEXT("</stream>\n");
	}
	Xml += TEXT("</spilled-result>\n");

	// Inline logs / UE log (only those that stayed inline -- spilled streams cleared these).
	if (!InlineLogs.IsEmpty())
	{
		Xml += TEXT("<logs>\n") + InlineLogs + TEXT("\n</logs>\n");
	}
	if (!InlineUELog.IsEmpty())
	{
		Xml += TEXT("  <ue_log>") + InlineUELog + TEXT("</ue_log>\n");
	}

	Xml += TEXT("</execute-result>");
	return Xml;
}

FString FClaireonXmlFormatter::GenerateTypeSignature(const FString& ToolName, const TSharedPtr<FJsonObject>& InputSchema)
{
	FString Sig = FString::Printf(TEXT("%s("), *ToolName);

	if (!InputSchema.IsValid())
	{
		Sig += TEXT(") -> dict");
		return Sig;
	}

	const TSharedPtr<FJsonObject>* PropertiesPtr = nullptr;
	if (!InputSchema->TryGetObjectField(TEXT("properties"), PropertiesPtr) || !PropertiesPtr || !(*PropertiesPtr).IsValid())
	{
		Sig += TEXT(") -> dict");
		return Sig;
	}

	// Get required params list
	TSet<FString> RequiredParams;
	const TArray<TSharedPtr<FJsonValue>>* RequiredArray = nullptr;
	if (InputSchema->TryGetArrayField(TEXT("required"), RequiredArray))
	{
		for (const TSharedPtr<FJsonValue>& Val : *RequiredArray)
		{
			FString ParamName;
			if (Val->TryGetString(ParamName))
			{
				RequiredParams.Add(ParamName);
			}
		}
	}

	// Build parameter list
	TArray<FString> ParamStrings;
	for (const auto& Pair : (*PropertiesPtr)->Values)
	{
		const FString ParamName(*Pair.Key);
		const TSharedPtr<FJsonObject>* PropObj = nullptr;

		FString TypeStr = TEXT("any");
		FString DefaultStr;

		if (Pair.Value->TryGetObject(PropObj) && PropObj && (*PropObj).IsValid())
		{
			FString JsonType;
			if ((*PropObj)->TryGetStringField(TEXT("type"), JsonType))
			{
				// Map JSON Schema types to Python types
				if (JsonType == TEXT("string"))
					TypeStr = TEXT("str");
				else if (JsonType == TEXT("integer"))
					TypeStr = TEXT("int");
				else if (JsonType == TEXT("number"))
					TypeStr = TEXT("float");
				else if (JsonType == TEXT("boolean"))
					TypeStr = TEXT("bool");
				else if (JsonType == TEXT("array"))
					TypeStr = TEXT("list");
				else if (JsonType == TEXT("object"))
					TypeStr = TEXT("dict");
				else
					TypeStr = JsonType;
			}

			// Check for default value
			if ((*PropObj)->HasField(TEXT("default")))
			{
				const TSharedPtr<FJsonValue>& DefaultVal = (*PropObj)->Values[TEXT("default")];
				if (DefaultVal->Type == EJson::String)
				{
					FString DefStr;
					DefaultVal->TryGetString(DefStr);
					DefaultStr = FString::Printf(TEXT(" = \"%s\""), *DefStr);
				}
				else if (DefaultVal->Type == EJson::Number)
				{
					double DefNum = 0;
					DefaultVal->TryGetNumber(DefNum);
					if (JsonType == TEXT("integer"))
					{
						DefaultStr = FString::Printf(TEXT(" = %d"), static_cast<int32>(DefNum));
					}
					else
					{
						DefaultStr = FString::Printf(TEXT(" = %g"), DefNum);
					}
				}
				else if (DefaultVal->Type == EJson::Boolean)
				{
					bool DefBool = false;
					DefaultVal->TryGetBool(DefBool);
					DefaultStr = DefBool ? TEXT(" = True") : TEXT(" = False");
				}
			}
		}

		bool bIsRequired = RequiredParams.Contains(ParamName);

		FString ParamStr = ParamName + TEXT(": ") + TypeStr;
		if (!bIsRequired && DefaultStr.IsEmpty())
		{
			// Optional param with no explicit default
			ParamStr += TEXT(" = None");
		}
		else
		{
			ParamStr += DefaultStr;
		}

		// Required params go first
		if (bIsRequired)
		{
			ParamStrings.Insert(ParamStr, 0);
		}
		else
		{
			ParamStrings.Add(ParamStr);
		}
	}

	Sig += FString::Join(ParamStrings, TEXT(", "));
	Sig += TEXT(") -> dict");

	return Sig;
}

FString FClaireonXmlFormatter::GenerateCategorySummary(const TMap<FString, TSharedPtr<IClaireonTool>>& Tools)
{
	// Group by category, excluding meta tools
	TMap<FString, TArray<FString>> Grouped;
	for (const auto& Pair : Tools)
	{
		const TSharedPtr<IClaireonTool>& Tool = Pair.Value;
		const FString Name = Tool->GetName();
		if (Name == TEXT("python_execute") || Name == TEXT("tool_search"))
		{
			continue;
		}
		Grouped.FindOrAdd(Tool->GetCategory()).Add(Name);
	}

	TArray<FString> Categories;
	Grouped.GetKeys(Categories);
	Categories.Sort();

	constexpr int32 MaxExamples = 3;

	FString Summary = TEXT("Available tool categories (use tool_search() to discover full signatures):\n");
	for (const FString& Category : Categories)
	{
		TArray<FString>& Names = Grouped[Category];
		Names.Sort();

		FString Examples;
		for (int32 i = 0; i < Names.Num() && i < MaxExamples; ++i)
		{
			if (i > 0)
				Examples += TEXT(", ");
			Examples += Names[i];
		}
		if (Names.Num() > MaxExamples)
		{
			Examples += TEXT(", ...");
		}

		Summary += FString::Printf(TEXT("- %s (%d): %s\n"), *Category, Names.Num(), *Examples);
	}

	return Summary;
}

// Returns just the category names, comma-joined, with no per-category
// example tools. Used by HandleToolsList / BuildToolDefinitions to keep
// the python_execute description small.
FString FClaireonXmlFormatter::GenerateCategoryList(const TMap<FString, TSharedPtr<IClaireonTool>>& Tools)
{
	TSet<FString> Categories;
	for (const auto& Pair : Tools)
	{
		const FString& Name = Pair.Key;
		const TSharedPtr<IClaireonTool>& Tool = Pair.Value;
		if (!Tool.IsValid()) { continue; }
		if (Name == TEXT("python_execute") || Name == TEXT("tool_search")) { continue; }
		Categories.Add(Tool->GetCategory());
	}
	TArray<FString> SortedCategories = Categories.Array();
	SortedCategories.Sort();
	return FString::Join(SortedCategories, TEXT(", "));
}
