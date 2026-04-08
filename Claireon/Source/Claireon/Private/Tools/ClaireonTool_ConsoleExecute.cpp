// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_ConsoleExecute.h"
#include "ClaireonLog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "GameFramework/PlayerController.h"
#include "Misc/OutputDeviceHelper.h"

FString ClaireonTool_ConsoleExecute::GetName() const
{
	return TEXT("claireon.console_execute");
}

FString ClaireonTool_ConsoleExecute::GetDescription() const
{
	return TEXT("Execute an Unreal console command and return its output. "
		"In PIE context, commands route through APlayerController::ConsoleCommand so "
		"cheat manager commands (e.g. God, Slomo, custom exec functions) work correctly.");
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
		TEXT("The console command to execute (e.g. 'stat fps', 'God', 'Slomo 0.5')"));
	Properties->SetObjectField(TEXT("command"), CommandProp);

	// context - optional
	TSharedPtr<FJsonObject> ContextProp = MakeShared<FJsonObject>();
	ContextProp->SetStringField(TEXT("type"), TEXT("string"));
	ContextProp->SetStringField(TEXT("description"),
		TEXT("Execution context: 'auto' (default: PIE if running, else editor), "
			"'pie' (PIE player controller - required for cheat manager commands), "
			"'editor' (editor world via GEngine::Exec)"));
	TArray<TSharedPtr<FJsonValue>> ContextEnum;
	ContextEnum.Add(MakeShared<FJsonValueString>(TEXT("auto")));
	ContextEnum.Add(MakeShared<FJsonValueString>(TEXT("pie")));
	ContextEnum.Add(MakeShared<FJsonValueString>(TEXT("editor")));
	ContextProp->SetArrayField(TEXT("enum"), ContextEnum);
	ContextProp->SetStringField(TEXT("default"), TEXT("auto"));
	Properties->SetObjectField(TEXT("context"), ContextProp);

	// playerIndex - optional
	TSharedPtr<FJsonObject> PlayerIndexProp = MakeShared<FJsonObject>();
	PlayerIndexProp->SetStringField(TEXT("type"), TEXT("integer"));
	PlayerIndexProp->SetStringField(TEXT("description"),
		TEXT("Player index for PIE context (default: 0). Ignored in editor context."));
	PlayerIndexProp->SetNumberField(TEXT("default"), 0);
	Properties->SetObjectField(TEXT("playerIndex"), PlayerIndexProp);

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

	// Parse parameters
	FString ContextMode;
	if (!Arguments->TryGetStringField(TEXT("context"), ContextMode) || ContextMode.IsEmpty())
	{
		ContextMode = TEXT("auto");
	}

	int32 PlayerIndex = 0;
	if (Arguments->HasField(TEXT("playerIndex")))
	{
		PlayerIndex = static_cast<int32>(Arguments->GetNumberField(TEXT("playerIndex")));
	}

	// --- Try PIE path: route through PlayerController::ConsoleCommand ---
	// This dispatches through ProcessConsoleExec -> UCheatManager::ProcessConsoleExec,
	// so cheat manager Exec UFUNCTIONs fire correctly.

	if (ContextMode != TEXT("editor"))
	{
		UWorld* PIEWorld = nullptr;
		for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
		{
			if (WorldContext.WorldType == EWorldType::PIE && WorldContext.World())
			{
				PIEWorld = WorldContext.World();
				break;
			}
		}

		if (PIEWorld)
		{
			// Find player controller at the requested index
			APlayerController* TargetPC = nullptr;
			int32 CurrentIndex = 0;
			for (auto It = PIEWorld->GetPlayerControllerIterator(); It; ++It)
			{
				APlayerController* PC = It->Get();
				if (PC && CurrentIndex == PlayerIndex)
				{
					TargetPC = PC;
					break;
				}
				CurrentIndex++;
			}

			if (!TargetPC)
			{
				if (ContextMode == TEXT("pie"))
				{
					return MakeErrorResult(FString::Printf(
						TEXT("No player controller found at index %d in PIE. Found %d controller(s)."),
						PlayerIndex, CurrentIndex));
				}
				// auto mode: fall through to editor path
			}
			else
			{
				// ConsoleCommand routes through the full dispatch chain:
				// APlayerController::ConsoleCommand -> ProcessConsoleExec -> UCheatManager::ProcessConsoleExec
				const FString Output = TargetPC->ConsoleCommand(Command);

				TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetStringField(TEXT("command"), Command);
				Data->SetStringField(TEXT("output"), Output);
				Data->SetBoolField(TEXT("success"), true);
				Data->SetStringField(TEXT("usedContext"), TEXT("pie"));
				Data->SetNumberField(TEXT("playerIndex"), PlayerIndex);

				const FString Summary = FString::Printf(TEXT("Executed '%s' via PIE player %d"),
					*Command, PlayerIndex);

				return MakeSuccessResult(Data, Summary);
			}
		}
		else if (ContextMode == TEXT("pie"))
		{
			return MakeErrorResult(TEXT("PIE is not running. Start a PIE session first."));
		}
		// auto mode with no PIE: fall through to editor path
	}

	// --- Editor path: use GEngine::Exec ---

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

	UWorld* EditorWorld = nullptr;
	if (GEditor)
	{
		EditorWorld = GEditor->GetEditorWorldContext().World();
	}

	FStringOutputDevice OutputDevice;
	GEngine->Exec(EditorWorld, *Command, OutputDevice);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("command"), Command);
	Data->SetStringField(TEXT("output"), OutputDevice.Output);
	Data->SetBoolField(TEXT("success"), true);
	Data->SetStringField(TEXT("usedContext"), TEXT("editor"));

	const FString Summary = FString::Printf(TEXT("Executed '%s' via editor context"),
		*Command);

	return MakeSuccessResult(Data, Summary);
}
