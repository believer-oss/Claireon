// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/IClaireonTool.h"
#include "PythonScriptTypes.h"

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
