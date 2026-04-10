// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonXmlFormatter.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"

FString FClaireonXmlFormatter::EscapeXml(const FString& Input)
{
	FString Output = Input;
	// Order matters: & must be replaced first to avoid double-escaping
	Output.ReplaceInline(TEXT("&"), TEXT("&amp;"));
	Output.ReplaceInline(TEXT("<"), TEXT("&lt;"));
	Output.ReplaceInline(TEXT(">"), TEXT("&gt;"));
	Output.ReplaceInline(TEXT("\""), TEXT("&quot;"));
	Output.ReplaceInline(TEXT("'"), TEXT("&apos;"));
	return Output;
}

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
			Suggestion = TEXT("Variable or function not defined. Use claireon.tools_search() to discover available tools, or check variable names.");
		}
		else if (ErrorMsg.Contains(TEXT("KeyError")))
		{
			ErrorCode = TEXT("key_error");
			Suggestion = TEXT("Dictionary key not found. Check the structure of the returned data with claireon.tools_search().");
		}
		else if (ErrorMsg.Contains(TEXT("AttributeError")))
		{
			ErrorCode = TEXT("attribute_error");
			Suggestion = TEXT("Object does not have the requested attribute. Use claireon.tools_search() to discover available tool names.");
		}
		else
		{
			Suggestion = TEXT("Review the error message and logs. Use claireon.tools_search() to discover available tools.");
		}

		Xml = FormatErrorResult(Result.ErrorMessage, ErrorCode, Suggestion, Result.Logs);
	}
	else
	{
		// Check if this is an indexed result (Output Gate envelope)
		bool bIsIndexed = false;
		if (Result.Data.IsValid())
		{
			bool bMCPIndexed = false;
			if (Result.Data->TryGetBoolField(TEXT("__mcp_indexed__"), bMCPIndexed) && bMCPIndexed)
			{
				bIsIndexed = true;
				FString IndexId;
				Result.Data->TryGetStringField(TEXT("index_id"), IndexId);
				double ChunkCountDouble = 0.0;
				Result.Data->TryGetNumberField(TEXT("chunk_count"), ChunkCountDouble);
				FString IndexSummary;
				Result.Data->TryGetStringField(TEXT("summary"), IndexSummary);

				TArray<FString> Excerpts;
				const TArray<TSharedPtr<FJsonValue>>* ExcerptsArray = nullptr;
				if (Result.Data->TryGetArrayField(TEXT("excerpts"), ExcerptsArray) && ExcerptsArray)
				{
					for (const TSharedPtr<FJsonValue>& ExcerptVal : *ExcerptsArray)
					{
						if (ExcerptVal.IsValid() && ExcerptVal->Type == EJson::Object)
						{
							FString ExcerptStr;
							TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> ExcerptWriter =
								TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&ExcerptStr);
							FJsonSerializer::Serialize(ExcerptVal->AsObject().ToSharedRef(), ExcerptWriter);
							ExcerptWriter->Close();
							Excerpts.Add(ExcerptStr);
						}
					}
				}

				Xml = FormatIndexedResult(IndexId, static_cast<int32>(ChunkCountDouble), IndexSummary, Excerpts, Result.Logs);
			}
		}

		if (!bIsIndexed)
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

			// Result data
			if (Result.Data.IsValid())
			{
				FString DataJson;
				TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
					TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&DataJson);
				FJsonSerializer::Serialize(Result.Data.ToSharedRef(), Writer);
				Writer->Close();
				Xml += TEXT("<result>\n") + DataJson + TEXT("\n</result>\n");
			}
			else
			{
				Xml += TEXT("<result>\nnull\n</result>\n");
			}

			// Warnings
			for (const FString& Warning : Result.Warnings)
			{
				Xml += TEXT("<warning>\n") + Warning + TEXT("\n</warning>\n");
			}

			// Logs always last
			if (!Result.Logs.IsEmpty())
			{
				Xml += TEXT("<logs>\n") + Result.Logs + TEXT("\n</logs>\n");
			}

			Xml += TEXT("</execute-result>");
		}
	}

	return Xml;
}

FString FClaireonXmlFormatter::FormatErrorResult(const FString& Error, const FString& ErrorCode, const FString& Suggestion, const FString& Logs)
{
	FString Xml = TEXT("<execute-result status=\"error\">\n");

	// Error first (required on failure)
	Xml += TEXT("<error code=\"") + EscapeXml(ErrorCode) + TEXT("\">\n") + Error + TEXT("\n</error>\n");

	// Suggestion
	if (!Suggestion.IsEmpty())
	{
		Xml += TEXT("<suggestion>\n") + Suggestion + TEXT("\n</suggestion>\n");
	}

	// Logs always last
	if (!Logs.IsEmpty())
	{
		Xml += TEXT("<logs>\n") + Logs + TEXT("\n</logs>\n");
	}

	Xml += TEXT("</execute-result>");
	return Xml;
}

FString FClaireonXmlFormatter::FormatIndexedResult(
	const FString& IndexId,
	int32 ChunkCount,
	const FString& Summary,
	const TArray<FString>& Excerpts,
	const FString& Logs)
{
	FString Xml = TEXT("<execute-result status=\"success\">\n");

	// Compact summary surfacing the most important metadata up front
	FString SummaryLine = FString::Printf(
		TEXT("Result too large for inline display — indexed as '%s' (%d chunks). "
			"Use claireon.index_search(index_id, query) to retrieve content."),
		*IndexId, ChunkCount);
	Xml += TEXT("<summary>\n") + SummaryLine + TEXT("\n</summary>\n");

	// Indexed result envelope
	Xml += TEXT("<indexed-result>\n");
	Xml += TEXT("<index-id>\n") + IndexId + TEXT("\n</index-id>\n");
	Xml += FString::Printf(TEXT("<chunk-count>\n%d\n</chunk-count>\n"), ChunkCount);

	if (!Summary.IsEmpty())
	{
		Xml += TEXT("<chunk-summary>\n") + Summary + TEXT("\n</chunk-summary>\n");
	}

	if (Excerpts.Num() > 0)
	{
		Xml += TEXT("<top-excerpts>\n");
		for (int32 i = 0; i < Excerpts.Num(); ++i)
		{
			Xml += FString::Printf(TEXT("<excerpt rank=\"%d\">\n"), i + 1);
			Xml += Excerpts[i];
			Xml += TEXT("\n</excerpt>\n");
		}
		Xml += TEXT("</top-excerpts>\n");
	}

	Xml += TEXT("<hint>\nCall claireon.index_search(\"") + EscapeXml(IndexId)
		+ TEXT("\", \"your query\") to search this index, or claireon.index_search(\"")
		+ EscapeXml(IndexId) + TEXT("\", \"\") to list the first chunks.\n</hint>\n");

	Xml += TEXT("</indexed-result>\n");

	// Logs always last
	if (!Logs.IsEmpty())
	{
		Xml += TEXT("<logs>\n") + Logs + TEXT("\n</logs>\n");
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
		const FString& ParamName = Pair.Key;
		const TSharedPtr<FJsonObject>* PropObj = nullptr;

		FString TypeStr = TEXT("any");
		FString DefaultStr;

		if (Pair.Value->TryGetObject(PropObj) && PropObj && (*PropObj).IsValid())
		{
			FString JsonType;
			if ((*PropObj)->TryGetStringField(TEXT("type"), JsonType))
			{
				// Map JSON Schema types to Python types
				if (JsonType == TEXT("string")) TypeStr = TEXT("str");
				else if (JsonType == TEXT("integer")) TypeStr = TEXT("int");
				else if (JsonType == TEXT("number")) TypeStr = TEXT("float");
				else if (JsonType == TEXT("boolean")) TypeStr = TEXT("bool");
				else if (JsonType == TEXT("array")) TypeStr = TEXT("list");
				else if (JsonType == TEXT("object")) TypeStr = TEXT("dict");
				else TypeStr = JsonType;
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

FString FClaireonXmlFormatter::GenerateToolStubs(const TMap<FString, TSharedPtr<IClaireonTool>>& Tools)
{
	// Group by category, excluding meta tools (execute, search_tools)
	TMap<FString, TArray<TSharedPtr<IClaireonTool>>> Grouped;
	for (const auto& Pair : Tools)
	{
		const TSharedPtr<IClaireonTool>& Tool = Pair.Value;
		const FString Category = Tool->GetCategory();
		const FString Name = Tool->GetName();

		// Skip meta tools — they are the MCP-visible tools, not bridge tools
		if (Name == TEXT("claireon.python_execute") || Name == TEXT("claireon.tools_search"))
		{
			continue;
		}

		Grouped.FindOrAdd(Category).Add(Tool);
	}

	// Sort categories
	TArray<FString> Categories;
	Grouped.GetKeys(Categories);
	Categories.Sort();

	FString Stubs = TEXT("<tool-stubs>\n");
	for (const FString& Category : Categories)
	{
		Stubs += FString::Printf(TEXT("<category name=\"%s\">\n"), *EscapeXml(Category));

		TArray<TSharedPtr<IClaireonTool>>& CategoryTools = Grouped[Category];
		// Sort tools within category by name for deterministic output
		CategoryTools.Sort([](const TSharedPtr<IClaireonTool>& A, const TSharedPtr<IClaireonTool>& B)
		{
			return A->GetName() < B->GetName();
		});

		for (const TSharedPtr<IClaireonTool>& Tool : CategoryTools)
		{
			FString Signature = GenerateTypeSignature(Tool->GetName(), Tool->GetInputSchema());
			Stubs += Signature + TEXT("\n");
		}

		Stubs += TEXT("</category>\n");
	}
	Stubs += TEXT("</tool-stubs>");

	return Stubs;
}

FString FClaireonXmlFormatter::GenerateCategorySummary(const TMap<FString, TSharedPtr<IClaireonTool>>& Tools)
{
	// Group by category, excluding meta tools
	TMap<FString, TArray<FString>> Grouped;
	for (const auto& Pair : Tools)
	{
		const TSharedPtr<IClaireonTool>& Tool = Pair.Value;
		const FString Name = Tool->GetName();
		if (Name == TEXT("claireon.python_execute") || Name == TEXT("claireon.tools_search"))
		{
			continue;
		}
		Grouped.FindOrAdd(Tool->GetCategory()).Add(Name);
	}

	TArray<FString> Categories;
	Grouped.GetKeys(Categories);
	Categories.Sort();

	constexpr int32 MaxExamples = 3;

	FString Summary = TEXT("Available tool categories (use claireon.tools_search() to discover full signatures):\n");
	for (const FString& Category : Categories)
	{
		TArray<FString>& Names = Grouped[Category];
		Names.Sort();

		FString Examples;
		for (int32 i = 0; i < Names.Num() && i < MaxExamples; ++i)
		{
			if (i > 0) Examples += TEXT(", ");
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
