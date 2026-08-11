// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_ConsoleExecute.h"
#include "ClaireonLog.h"
#include "ClaireonLogCapture.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Misc/OutputDeviceHelper.h"

FString ClaireonTool_ConsoleExecute::GetCategory() const { return TEXT("console"); }
FString ClaireonTool_ConsoleExecute::GetOperation() const { return TEXT("execute"); }

namespace ClaireonToolConsoleExecuteInternal
{
	// Parse "Log <Cat> <Verbosity>" into (Cat, Verbosity). Returns true on
	// match. The engine accepts both `log` and `Log`; case-insensitive on the
	// command keyword but the category name itself is passed through unchanged.
	// File-local discriminator to avoid anon-NS name collisions under unity batching.
	static bool Cl625ConsoleExec_ParseLogVerbosityCommand(
		const FString& Command, FString& OutCategory, FString& OutVerbosity)
	{
		FString Trim = Command.TrimStartAndEnd();
		TArray<FString> Parts;
		Trim.ParseIntoArray(Parts, TEXT(" "), /*InCullEmpty=*/ true);
		if (Parts.Num() != 3) { return false; }
		if (!Parts[0].Equals(TEXT("Log"), ESearchCase::IgnoreCase)) { return false; }
		OutCategory = Parts[1];
		OutVerbosity = Parts[2];
		return true;
	}

	// The FOutputDevice handed to Exec. Collects only what a handler writes to
	// `Ar` -- deliberately NOT attached to GLog, so it stays the return channel
	// and the log capture stays a separate channel.
	// File-local discriminator to avoid anon-NS name collisions under unity batching.
	class FCl625ConsoleExec_ReturnChannel : public FOutputDevice
	{
	public:
		FString Output;
		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (!Output.IsEmpty())
				Output += TEXT("\n");
			Output += V;
		}
	};
}

ClaireonTool_ConsoleExecute::FConsoleDispatchOutput ClaireonTool_ConsoleExecute::DispatchInWorld(
	UWorld* World, const FString& Command, bool bCaptureLog)
{
	FConsoleDispatchOutput Out;
	if (!IsValid(GEngine))
	{
		return Out;
	}

	if (!bCaptureLog)
	{
		ClaireonToolConsoleExecuteInternal::FCl625ConsoleExec_ReturnChannel Device;
		GEngine->Exec(World, *Command, Device);
		Out.ReturnChannel = Device.Output;
		return Out;
	}

	// Log floor, NOT the FClaireonLogCapture default of Warning: console command
	// handlers report at Display/Log, so a Warning floor captures nothing from
	// them and the fix would look like it had not worked.
	FClaireonLogCapture Capture(ELogVerbosity::Log);
	{
		ClaireonToolConsoleExecuteInternal::FCl625ConsoleExec_ReturnChannel Device;
		GEngine->Exec(World, *Command, Device);
		Out.ReturnChannel = Device.Output;
	}
	Out.LogOutput = Capture.GetCapturedOutput();
	return Out;
}

FString ClaireonTool_ConsoleExecute::GetDescription() const
{
	// Kept under the description lint's 400-char budget
	// (ClaireonDescriptionLintTests AllP5CategoriesConformToTemplate).
	return TEXT("Execute an Unreal console command. Returns 'output' (the command's return "
				"channel) and 'log_output' (what it wrote to the log). 'stat dumpframe' and "
				"'obj list' report only via the log; capture_log=false skips that capture. "
				"In PIE, routes through APlayerController::ConsoleCommand so cheat manager "
				"commands work. Bypass-mode: refused while any Claireon session is held.");
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

	// capture_log - optional
	TSharedPtr<FJsonObject> CaptureLogProp = MakeShared<FJsonObject>();
	CaptureLogProp->SetStringField(TEXT("type"), TEXT("boolean"));
	CaptureLogProp->SetStringField(TEXT("description"),
		TEXT("Capture log output emitted while the command runs and return it in 'log_output' "
			 "(default: true). Everything logged during the call is captured, including "
			 "unrelated engine chatter. Set false for the return channel only."));
	CaptureLogProp->SetBoolField(TEXT("default"), true);
	Properties->SetObjectField(TEXT("capture_log"), CaptureLogProp);

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

	if (!IsValid(GEngine))
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

	bool bCaptureLog = true;
	Arguments->TryGetBoolField(TEXT("capture_log"), bCaptureLog);

	// --- Try PIE path: route through PlayerController::ConsoleCommand ---
	// This dispatches through ProcessConsoleExec -> UCheatManager::ProcessConsoleExec,
	// so cheat manager Exec UFUNCTIONs fire correctly.

	if (ContextMode != TEXT("editor"))
	{
		UWorld* PIEWorld = nullptr;
		for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
		{
			if (WorldContext.WorldType == EWorldType::PIE && IsValid(WorldContext.World()))
			{
				PIEWorld = WorldContext.World();
				break;
			}
		}

		if (IsValid(PIEWorld))
		{
			// Find player controller at the requested index
			APlayerController* TargetPC = nullptr;
			int32 CurrentIndex = 0;
			for (auto It = PIEWorld->GetPlayerControllerIterator(); It; ++It)
			{
				APlayerController* PC = It->Get();
				if (IsValid(PC) && CurrentIndex == PlayerIndex)
				{
					TargetPC = PC;
					break;
				}
				CurrentIndex++;
			}

			if (!IsValid(TargetPC))
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
				// It returns only what the handler wrote to its output device, so
				// bracket it with a log capture for the handlers that report via UE_LOG.
				FString Output;
				FString LogOutput;
				if (bCaptureLog)
				{
					FClaireonLogCapture Capture(ELogVerbosity::Log);
					Output = TargetPC->ConsoleCommand(Command);
					LogOutput = Capture.GetCapturedOutput();
				}
				else
				{
					Output = TargetPC->ConsoleCommand(Command);
				}

				TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetStringField(TEXT("command"), Command);
				Data->SetStringField(TEXT("output"), Output);
				Data->SetStringField(TEXT("log_output"), LogOutput);
				Data->SetBoolField(TEXT("capture_log"), bCaptureLog);
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

	if (!IsValid(GEditor))
	{
		return MakeErrorResult(TEXT("Editor is not available. Wait for the editor to finish initializing."));
	}

	UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
	if (!IsValid(EditorWorld))
	{
		return MakeErrorResult(TEXT("No world loaded. Use open_map to load a map first."));
	}

	// Detect `Log <Cat> <Verbosity>` so we can (a) echo the
	// verbosity-change to OutputDevice (without this echo the engine's response is
	// empty), and (b) re-issue the command against the active PIE world so the
	// filter change applies to PIE logs as well as editor logs.
	FString LogVerbosityCat;
	FString LogVerbosityLevel;
	const bool bIsLogVerbosityCmd =
		ClaireonToolConsoleExecuteInternal::Cl625ConsoleExec_ParseLogVerbosityCommand(
			Command, LogVerbosityCat, LogVerbosityLevel);

	FConsoleDispatchOutput Dispatch = DispatchInWorld(EditorWorld, Command, bCaptureLog);

	// also apply to the PIE world if one is active. `Log <Cat> <Verbosity>`
	// is world-scoped at the GLog redirector level, but exec dispatch goes through
	// world-bound parsing for some console paths. Re-execing on the PIE world is a
	// cheap idempotent way to ensure both filters move together.
	if (bIsLogVerbosityCmd)
	{
		UWorld* PIEWorldForLog = nullptr;
		for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
		{
			if (WorldContext.WorldType == EWorldType::PIE && IsValid(WorldContext.World()))
			{
				PIEWorldForLog = WorldContext.World();
				break;
			}
		}
		if (IsValid(PIEWorldForLog))
		{
			const FConsoleDispatchOutput PIEDispatch = DispatchInWorld(PIEWorldForLog, Command, bCaptureLog);
			if (!PIEDispatch.ReturnChannel.IsEmpty())
			{
				if (!Dispatch.ReturnChannel.IsEmpty()) { Dispatch.ReturnChannel += TEXT("\n"); }
				Dispatch.ReturnChannel += PIEDispatch.ReturnChannel;
			}
			if (!PIEDispatch.LogOutput.IsEmpty())
			{
				Dispatch.LogOutput += PIEDispatch.LogOutput;
			}
		}

		// echo the requested filter change so callers do not see an empty
		// response. The engine's `Log <Cat> <Verbosity>` exec is silent on success;
		// without this echo the result looks like the command had no effect.
		const FString EchoLine = FString::Printf(TEXT("Log: %s -> %s"),
			*LogVerbosityCat, *LogVerbosityLevel);
		if (!Dispatch.ReturnChannel.IsEmpty()) { Dispatch.ReturnChannel += TEXT("\n"); }
		Dispatch.ReturnChannel += EchoLine;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("command"), Command);
	Data->SetStringField(TEXT("output"), Dispatch.ReturnChannel);
	Data->SetStringField(TEXT("log_output"), Dispatch.LogOutput);
	Data->SetBoolField(TEXT("capture_log"), bCaptureLog);
	Data->SetBoolField(TEXT("success"), true);
	Data->SetStringField(TEXT("usedContext"), TEXT("editor"));
	if (bIsLogVerbosityCmd)
	{
		Data->SetStringField(TEXT("log_category"), LogVerbosityCat);
		Data->SetStringField(TEXT("log_verbosity"), LogVerbosityLevel);
		Data->SetBoolField(TEXT("verbosity_change"), true);
	}

	const FString Summary = FString::Printf(TEXT("Executed '%s' via editor context"),
		*Command);

	return MakeSuccessResult(Data, Summary);
}
