// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Tools/IClaireonTool.h"

struct FClaireonSpillStream;

/**
 * Generates XML-ish structural markup for LLM consumption per 02-XML-CONTRACT.md.
 * Content text is NOT XML-escaped since these tags are structural delimiters
 * for LLM token parsing, not real XML consumed by an XML parser.
 * Formats tool execution results, search results, and errors.
 */
class FClaireonXmlFormatter
{
public:
	/**
	 * Format a tool execution result into XML response.
	 * Status is inferred from the Result fields:
	 *   - bIsError=true  -> status="error"
	 *   - bIsError=false, Data non-empty -> status="success"
	 *   - bIsError=false, Data empty but Summary non-empty -> status="success"
	 */
	static FString FormatExecuteResult(const IClaireonTool::FToolResult& Result);

	/** Format an error into XML response */
	static FString FormatErrorResult(const FString& Error, const FString& ErrorCode, const FString& Suggestion, const FString& Logs, const FString& UELog = FString());

	/**
	 * Generate a Python-style type signature from a tool's input schema.
	 * e.g. "asset_search(search_dir: str, class_filter: str = \"\", ...) -> dict"
	 */
	static FString GenerateTypeSignature(const FString& ToolName, const TSharedPtr<FJsonObject>& InputSchema);

	/**
	 * Format a disk-spilled result envelope (produced by FClaireonOutputGate::RouteResult
	 * when one or more streams exceed the per-stream spill threshold).  Renders an
	 * <execute-result status="success"> block whose body carries a <spilled-result>
	 * element with one <stream> child per spilled stream (path / size / preview /
	 * truncated / over-ceiling / error).
	 *
	 * Per-tool-class stream membership (D6): generic tools produce at most one
	 * <stream name="data"> child; python_execute produces at most one each
	 * of <stream name="stdout"> and <stream name="uelog">, never <stream name="data">.
	 *
	 * @param Summary      One-line human summary injected into <summary>.
	 * @param Streams      Per-stream manifests from FClaireonSpillResult::Streams.
	 * @param InlineLogs   stdout text that stayed inline (empty when stdout spilled).
	 * @param InlineUELog  uelog text that stayed inline (empty when uelog spilled).
	 */
	static FString FormatSpilledResult(
		const FString& Summary,
		const TArray<FClaireonSpillStream>& Streams,
		const FString& InlineLogs,
		const FString& InlineUELog);

	/**
	 * Generate a compact category summary with tool counts and example names.
	 * Compact summary (~1-2k chars) directing Claude to use `search` for full signatures.
	 */
	static FString GenerateCategorySummary(const TMap<FString, TSharedPtr<IClaireonTool>>& Tools);

	/**
	 * Returns just the category names, comma-joined, with no per-category
	 * example tools. Used by HandleToolsList / BuildToolDefinitions to keep
	 * the python_execute description small.
	 */
	static FString GenerateCategoryList(const TMap<FString, TSharedPtr<IClaireonTool>>& Tools);
};
