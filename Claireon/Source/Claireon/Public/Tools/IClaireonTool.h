// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

struct FPythonLogOutputEntry;

/**
 * Interface for MCP tools that can be registered with the server.
 * Each tool provides a name, description, input schema, and an Execute method.
 */
class CLAIREON_API IClaireonTool
{
public:
	virtual ~IClaireonTool() = default;

	/** Returns the tool name used in tools/list and tools/call */
	virtual FString GetName() const = 0;

	/** Whether this tool must be blocked while a Play-In-Editor session is active.
	 *  Tools that mutate assets should override this to return true. */
	virtual bool RequiresNoPIE() const { return false; }

	virtual bool RequiresEditorWorld() const { return false; }

	/** Returns a human-readable description of what the tool does (standard tier, ~150-300 chars) */
	virtual FString GetDescription() const = 0;

	/** One-line summary (~40-80 chars). Default: first sentence of GetDescription(). */
	virtual FString GetBriefDescription() const
	{
		FString Desc = GetDescription();
		// Extract first sentence (up to ". " or first 100 chars)
		int32 DotPos;
		if (Desc.FindChar(TEXT('.'), DotPos) && DotPos < 100)
		{
			return Desc.Left(DotPos + 1);
		}
		if (Desc.Len() <= 100)
		{
			return Desc;
		}
		return Desc.Left(97) + TEXT("...");
	}

	/** Extended docs with examples, workflows, recovery procedures. Default: same as GetDescription(). */
	virtual FString GetFullDescription() const { return GetDescription(); }

	/** Returns the JSON Schema for the tool's input parameters */
	virtual TSharedPtr<FJsonObject> GetInputSchema() const = 0;

	/** Returns the tool category for registry grouping and search_tools.
	 *  Default: auto-derived from GetName() by stripping "claireon." prefix and returning
	 *  everything before the first underscore. E.g. "claireon.asset_search" → "asset".
	 *  Override only if the auto-derived category is incorrect. */
	virtual FString GetCategory() const
	{
		FString Name = GetName();
		// Strip "claireon." prefix
		if (Name.StartsWith(TEXT("claireon.")))
		{
			Name.RightChopInline(7);
		}
		// Return everything before the first underscore
		int32 UnderscorePos;
		if (Name.FindChar(TEXT('_'), UnderscorePos))
		{
			return Name.Left(UnderscorePos);
		}
		return Name;
	}

	/** Result of a tool execution */
	struct FToolResult
	{
		/** Structured result data (primary), serialized to Python dict */
		TSharedPtr<FJsonObject> Data;

		/** Tool-generated summary for XML <summary> tag */
		FString Summary;

		/** Non-fatal warnings */
		TArray<FString> Warnings;

		/** Execution logs (stdout/stderr lines) */
		FString Logs;

		/** Engine UE_LOG output captured during execution (Warning/Error level) */
		FString UELog;

		/** Whether this result represents an error */
		bool bIsError = false;

		/** Error description */
		FString ErrorMessage;

		/** Returns ErrorMessage (if error) or Summary (if success) as a single string. */
		FString GetContentAsString() const
		{
			return bIsError ? ErrorMessage : Summary;
		}

		/** Build a log string from Python log output entries */
		static FString BuildLogString(const TArray<FPythonLogOutputEntry>& LogOutput);
	};

	/**
	 * Execute the tool with the given arguments.
	 * Called on the game thread.
	 * @param Arguments - The parsed arguments from the tools/call request
	 * @return The tool result with structured data and error status
	 */
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) = 0;

	/** Helper to create a success result with structured data and summary */
	static FToolResult MakeSuccessResult(TSharedPtr<FJsonObject> InData, const FString& InSummary)
	{
		FToolResult Result;
		Result.Data = MoveTemp(InData);
		Result.Summary = InSummary;
		Result.bIsError = false;
		return Result;
	}

	/** Helper to create an error result */
	static FToolResult MakeErrorResult(const FString& InErrorMessage)
	{
		FToolResult Result;
		Result.bIsError = true;
		Result.ErrorMessage = InErrorMessage;
		return Result;
	}
};
