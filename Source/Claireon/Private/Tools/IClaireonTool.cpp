// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/IClaireonTool.h"
#include "PythonScriptTypes.h"

TSharedPtr<FJsonObject> IClaireonTool::CloneHintArgs(const TSharedPtr<FJsonObject>& Arguments)
{
	TSharedPtr<FJsonObject> Clone = MakeShared<FJsonObject>();
	if (Arguments.IsValid())
	{
		// Clone arguments before correcting them to avoid modifying the in-flight call.
		FJsonObject::Duplicate(Arguments, Clone);
	}
	return Clone;
}

TSharedPtr<FJsonObject> IClaireonTool::MakeGuidanceHint(
	const FString& ToolName,
	const FString& Reason,
	const TSharedPtr<FJsonObject>& CompleteArgs,
	FName Key)
{
	TSharedPtr<FJsonObject> Hint = MakeShared<FJsonObject>();
	Hint->SetStringField(TEXT("tool"), ToolName);
	Hint->SetStringField(TEXT("reason"), Reason);
	if (CompleteArgs.IsValid() && CompleteArgs->Values.Num() > 0)
	{
		Hint->SetObjectField(TEXT("args"), CompleteArgs);
	}
	if (!Key.IsNone())
	{
		Hint->SetStringField(TEXT("key"), Key.ToString());
	}
	return Hint;
}

TSharedPtr<FJsonObject> IClaireonTool::MakeResourceHint(
	const FString& ResourceUri,
	const FString& Reason,
	FName Key)
{
	TSharedPtr<FJsonObject> Hint = MakeShared<FJsonObject>();
	Hint->SetStringField(TEXT("resource"), ResourceUri);
	Hint->SetStringField(TEXT("reason"), Reason);
	if (!Key.IsNone())
	{
		Hint->SetStringField(TEXT("key"), Key.ToString());
	}
	return Hint;
}

namespace ClaireonToolHintShapeInternal
{
	static bool Cl627Hint_RequireNonEmptyString(
		const TSharedPtr<FJsonObject>& Hint,
		const FString& Key,
		FString& OutValue,
		FString& OutError)
	{
		// TryGetField accepts FString across engine versions, including FSharedString-keyed maps.
		const TSharedPtr<FJsonValue> Found = Hint->TryGetField(Key);
		if (!Found.IsValid())
		{
			OutError = FString::Printf(TEXT("hint is missing a non-empty '%s' field"), *Key);
			return false;
		}
		// Check the JSON type directly; TryGetStringField coerces numeric values.
		if (Found->Type != EJson::String)
		{
			OutError = FString::Printf(TEXT("hint field '%s' must be a string"), *Key);
			return false;
		}
		OutValue = Found->AsString();
		if (OutValue.IsEmpty())
		{
			OutError = FString::Printf(TEXT("hint field '%s' is empty"), *Key);
			return false;
		}
		return true;
	}
}

bool IClaireonTool::ValidateHint(const TSharedPtr<FJsonObject>& Hint, FString& OutError)
{
	using namespace ClaireonToolHintShapeInternal;

	OutError.Reset();
	if (!Hint.IsValid())
	{
		OutError = TEXT("hint is null");
		return false;
	}

	// Report unknown keys first and sort them for deterministic errors.
	static const TSet<FString> AllowedKeys = {
		TEXT("tool"), TEXT("resource"), TEXT("reason"), TEXT("args"), TEXT("options"),
		TEXT("key")
	};
	TArray<FString> UnknownKeys;
	for (const auto& Pair : Hint->Values)
	{
		const FString Key(*Pair.Key);
		if (!AllowedKeys.Contains(Key))
		{
			UnknownKeys.Add(Key);
		}
	}
	if (UnknownKeys.Num() > 0)
	{
		UnknownKeys.Sort();
		OutError = FString::Printf(TEXT("hint has unknown field(s): %s"), *FString::Join(UnknownKeys, TEXT(", ")));
		return false;
	}

	const bool bHasTool = Hint->HasField(TEXT("tool"));
	const bool bHasResource = Hint->HasField(TEXT("resource"));
	if (bHasTool && bHasResource)
	{
		OutError = TEXT("hint sets both 'tool' and 'resource', which are mutually exclusive");
		return false;
	}
	if (!bHasTool && !bHasResource)
	{
		OutError = TEXT("hint is missing a non-empty 'tool' or 'resource' field");
		return false;
	}

	FString TargetValue;
	if (bHasTool)
	{
		if (!Cl627Hint_RequireNonEmptyString(Hint, TEXT("tool"), TargetValue, OutError))
		{
			return false;
		}
	}
	else
	{
		if (!Cl627Hint_RequireNonEmptyString(Hint, TEXT("resource"), TargetValue, OutError))
		{
			return false;
		}
		// Resources require a URI that resources/read can resolve.
		if (!TargetValue.Contains(TEXT("://")) || TargetValue.StartsWith(TEXT("://")))
		{
			OutError = FString::Printf(
				TEXT("hint field 'resource' must be a URI with a scheme (e.g. 'claireon://instructions/x'), got '%s'"),
				*TargetValue);
			return false;
		}
	}

	FString Reason;
	if (!Cl627Hint_RequireNonEmptyString(Hint, TEXT("reason"), Reason, OutError))
	{
		return false;
	}

	// A present key must be non-empty to participate in rate limiting.
	if (Hint->HasField(TEXT("key")))
	{
		FString KeyValue;
		if (!Cl627Hint_RequireNonEmptyString(Hint, TEXT("key"), KeyValue, OutError))
		{
			return false;
		}
	}

	// args specifies one call; options specifies alternative calls.
	const bool bHasArgs = Hint->HasField(TEXT("args"));
	const bool bHasOptions = Hint->HasField(TEXT("options"));
	if (bHasArgs && bHasOptions)
	{
		OutError = TEXT("hint sets both 'args' and 'options', which are mutually exclusive");
		return false;
	}

	if (bHasResource && (bHasArgs || bHasOptions))
	{
		OutError = FString::Printf(
			TEXT("resource hint carries '%s'; a resource hint has nothing to call, so it takes neither 'args' nor 'options'"),
			bHasArgs ? TEXT("args") : TEXT("options"));
		return false;
	}

	if (bHasArgs)
	{
		const TSharedPtr<FJsonObject>* ArgsObject = nullptr;
		if (!Hint->TryGetObjectField(TEXT("args"), ArgsObject) || !ArgsObject || !ArgsObject->IsValid())
		{
			OutError = TEXT("hint field 'args' must be an object (a complete callable argument set)");
			return false;
		}
	}

	if (bHasOptions)
	{
		const TArray<TSharedPtr<FJsonValue>>* OptionsArray = nullptr;
		if (!Hint->TryGetArrayField(TEXT("options"), OptionsArray) || !OptionsArray)
		{
			OutError = TEXT("hint field 'options' must be an array of complete callable argument sets");
			return false;
		}
		if (OptionsArray->Num() == 0)
		{
			OutError = TEXT("hint field 'options' is an empty array; omit it rather than offering no alternatives");
			return false;
		}
		for (int32 Index = 0; Index < OptionsArray->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& Option = (*OptionsArray)[Index];
			if (!Option.IsValid() || Option->Type != EJson::Object)
			{
				OutError = FString::Printf(
					TEXT("hint field 'options'[%d] must be an object (a complete callable argument set)"), Index);
				return false;
			}
		}
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
