// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/IClaireonTool.h"
#include "PythonScriptTypes.h"

TSharedPtr<FJsonObject> IClaireonTool::CloneHintArgs(const TSharedPtr<FJsonObject>& Arguments)
{
	TSharedPtr<FJsonObject> Clone = MakeShared<FJsonObject>();
	if (Arguments.IsValid())
	{
		// Duplicate rather than share: callers mutate the clone to apply the correction, and
		// mutating the live argument object would corrupt the in-flight call.
		FJsonObject::Duplicate(Arguments, Clone);
	}
	return Clone;
}

TSharedPtr<FJsonObject> IClaireonTool::MakeGuidanceHint(
	const FString& ToolName,
	const FString& Reason,
	const TSharedPtr<FJsonObject>& CompleteArgs)
{
	TSharedPtr<FJsonObject> Hint = MakeShared<FJsonObject>();
	Hint->SetStringField(TEXT("tool"), ToolName);
	Hint->SetStringField(TEXT("reason"), Reason);
	if (CompleteArgs.IsValid() && CompleteArgs->Values.Num() > 0)
	{
		Hint->SetObjectField(TEXT("args"), CompleteArgs);
	}
	return Hint;
}

namespace ClaireonToolHintLatchInternal
{
	// File-local discriminator per project convention on anonymous-namespace collisions under
	// unity batching. Game-thread only, so a plain TSet needs no synchronization.
	static TSet<FName> Cl625Hint_FiredCodesThisSession;
}

bool IClaireonTool::ShouldEmitLatchedHint(FName HintCode)
{
	if (HintCode.IsNone())
	{
		return false;
	}
	if (ClaireonToolHintLatchInternal::Cl625Hint_FiredCodesThisSession.Contains(HintCode))
	{
		return false;
	}
	ClaireonToolHintLatchInternal::Cl625Hint_FiredCodesThisSession.Add(HintCode);
	return true;
}

void IClaireonTool::ResetHintLatchForTests()
{
	ClaireonToolHintLatchInternal::Cl625Hint_FiredCodesThisSession.Reset();
}

bool IClaireonTool::ValidateHint(const TSharedPtr<FJsonObject>& Hint, FString& OutError)
{
	OutError.Reset();
	if (!Hint.IsValid())
	{
		OutError = TEXT("hint is null");
		return false;
	}

	FString ToolName;
	if (!Hint->TryGetStringField(TEXT("tool"), ToolName) || ToolName.IsEmpty())
	{
		OutError = TEXT("hint is missing a non-empty 'tool' field");
		return false;
	}

	// args means "call with exactly this"; options means "call with exactly one of these".
	// Both at once has no coherent reading, so it is rejected rather than guessed at.
	if (Hint->HasField(TEXT("args")) && Hint->HasField(TEXT("options")))
	{
		OutError = TEXT("hint sets both 'args' and 'options', which are mutually exclusive");
		return false;
	}

	return true;
}

FString IClaireonTool::FToolResult::BuildLogString(const TArray<FPythonLogOutputEntry>& LogOutput)
{
	FString Logs;
	for (const FPythonLogOutputEntry& Entry : LogOutput)
	{
		FString TypePrefix;
		switch (Entry.Type)
		{
		case EPythonLogOutputType::Info:
			// No prefix for info — cleaner output
			break;
		case EPythonLogOutputType::Warning:
			TypePrefix = TEXT("[Warning] ");
			break;
		case EPythonLogOutputType::Error:
			TypePrefix = TEXT("[Error] ");
			break;
		}

		if (!Logs.IsEmpty())
		{
			Logs += TEXT("\n");
		}
		Logs += TypePrefix + Entry.Output;
	}
	return Logs;
}
