// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_ConsoleExecute.h"
#include "ClaireonLog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Misc/OutputDeviceHelper.h"

FString ClaireonTool_ConsoleExecute::GetName() const
{
	return TEXT("console_execute");
}

FString ClaireonTool_ConsoleExecute::GetCategory() const
{
	return TEXT("build");
}

FString ClaireonTool_ConsoleExecute::GetDescription() const
{
	return TEXT("Execute an Unreal console command and return its output");
}

TSharedPtr<FJsonObject> ClaireonTool_ConsoleExecute::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// command - required
	TSharedPtr<FJsonObject> CommandProp = MakeShared<FJsonObject>();
	CommandProp->SetStringField(TEXT("type"), TEXT("string"));
	CommandProp->SetStringField(TEXT("description"),
		TEXT("The console command to execute (e.g. 'stat fps', 'obj list')"));
	Properties->SetObjectField(TEXT("command"), CommandProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("command")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_ConsoleExecute::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString Command;
	if (!Arguments->TryGetStringField(TEXT("command"), Command) || Command.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: command"));
	}

	if (!GEngine)
	{
		return MakeErrorResult(TEXT("GEngine not available"));
	}

	// Capture output via an output device that records the text
	class FStringOutputDevice : public FOutputDevice
	{
	public:
		FString Output;
		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (!Output.IsEmpty()) Output += TEXT("\n");
			Output += V;
		}
	};

	FStringOutputDevice OutputDevice;
	GEngine->Exec(GEditor ? GEditor->GetEditorWorldContext().World() : nullptr,
		*Command, OutputDevice);

	const bool bSuccess = true; // Exec doesn't return failure in most cases
	const FString Output = OutputDevice.Output;

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("command"), Command);
	Data->SetStringField(TEXT("output"), Output);
	Data->SetBoolField(TEXT("success"), bSuccess);

	const FString Summary = FString::Printf(TEXT("Executed '%s': %s"),
		*Command, Output.IsEmpty() ? TEXT("output captured") : TEXT("output captured"));

	return MakeSuccessResult(Data, Summary);
}
