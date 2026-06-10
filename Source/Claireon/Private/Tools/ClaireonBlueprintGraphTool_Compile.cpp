// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_Compile.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonBlueprintHelpers.h"
#include "Dom/JsonObject.h"
#include "Tools/ClaireonSpecApplicator_Blueprint.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase_Internal.h"
#include "ClaireonLog.h"
#include "ClaireonSafeExec.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_CallDataTableFunction.h"
#include "K2Node_CallMaterialParameterCollectionFunction.h"
#include "K2Node_CommutativeAssociativeBinaryOperator.h"
#include "K2Node_Event.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_Timeline.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_Select.h"
#include "K2Node_MacroInstance.h"
#include "Engine/MemberReference.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_SpawnActorFromClass.h"
#include "K2Node_Knot.h"
#include "EdGraphNode_Comment.h"
#include "K2Node_Literal.h"
#include "K2Node_MakeArray.h"
#include "K2Node_MakeMap.h"
#include "K2Node_MakeSet.h"
#include "K2Node_GetArrayItem.h"
#include "K2Node_AddPinInterface.h"
#include "K2Node_Switch.h"
#include "K2Node_SwitchInteger.h"
#include "K2Node_SwitchString.h"
#include "K2Node_SwitchName.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_ForEachElementInEnum.h"
#include "K2Node_DoOnceMultiInput.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_RemoveDelegate.h"
#include "K2Node_ClearDelegate.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_AssignDelegate.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Engine/TimelineTemplate.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveVector.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/UObjectToken.h"
#include "EdGraphUtilities.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ScopedTransaction.h"
#include "Animation/AnimBlueprint.h"
#include "AnimationGraph.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_Root.h"
#include "K2Node_Tunnel.h"
#include "ClaireonBlueprintNodeSerializer.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"
#include "ClaireonNameResolver.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "ClaireonBPInterfaceAuthor.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

#define LOCTEXT_NAMESPACE "ClaireonBlueprintGraphEditToolBase"

using FToolResult = IClaireonTool::FToolResult;


FString ClaireonBlueprintGraphTool_Compile::GetOperation() const { return TEXT("compile"); }

TArray<FString> ClaireonBlueprintGraphTool_Compile::GetSearchKeywords() const
{
    return {TEXT("bp"), TEXT("compile"), TEXT("build"), TEXT("validate"), TEXT("graph")};
}

FString ClaireonBlueprintGraphTool_Compile::GetDescription() const
{
    return TEXT("Compiles the Blueprint of the current session and reports structured errors and warnings. Most-common pitfall: assuming compile == save -- it does not write to disk; call bp_save (or close, which auto-saves) to persist the compiled state. Accepts either session_id or asset_path; auto-opens a session when asset_path is supplied.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_Compile::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_Compile::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FBlueprintEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("compile"), Params, SessionId, Data, Error))
    {
        return Error;
    }
	UBlueprint* Blueprint = Data->Blueprint.Get();
	if (!Blueprint)
	{
		return MakeErrorResult(TEXT("Blueprint is no longer valid"));
	}

	// Plumb FCompilerResultsLog. CompileBlueprint(Blueprint,
	// EBlueprintCompileOptions::None, &Log) routes compile-time messages
	// into the supplied log instead of dropping them on the floor. We
	// serialize the log entries into data.compile_log so callers see
	// actionable detail instead of just "Compilation failed with errors".
	FCompilerResultsLog CompilerLog;
	CompilerLog.SetSourcePath(Blueprint->GetPathName());
	CompilerLog.BeginEvent(TEXT("Compile"));
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &CompilerLog);
	CompilerLog.EndEvent();

	// Check compilation status
	EBlueprintStatus Status = Blueprint->Status;
	FString StatusText;

	switch (Status)
	{
		case BS_UpToDate:
		case BS_UpToDateWithWarnings:
			StatusText = TEXT("Compilation successful");
			if (Status == BS_UpToDateWithWarnings)
			{
				StatusText += TEXT(" (with warnings)");
			}
			break;

		case BS_Error:
			StatusText = TEXT("Compilation failed with errors");
			break;

		case BS_Unknown:
		case BS_Dirty:
			StatusText = TEXT("Compilation status unknown");
			break;

		default:
			StatusText = TEXT("Compilation completed");
			break;
	}

	Data->Cursor.LastOperationStatus = StatusText;
	FToolResult CompileResult = BuildStateResponse(SessionId, Data);

	// serialize compile log entries into the response. The top-level
	// error message gets promoted to the first Error entry's text so callers
	// reading the summary see the actionable detail without descending into
	// data.compile_log. Truncate the log array at 100 entries to keep the
	// response payload bounded; total counts always shown.
	const int32 MaxEntries = 100;
	if (CompileResult.Data.IsValid())
	{
		TArray<TSharedPtr<FJsonValue>> LogArray;
		int32 EmittedCount = 0;
		FString FirstErrorText;
		for (const TSharedRef<FTokenizedMessage>& Msg : CompilerLog.Messages)
		{
			if (EmittedCount >= MaxEntries)
			{
				break;
			}
			const EMessageSeverity::Type Severity = Msg->GetSeverity();
			const TCHAR* SevLabel = TEXT("info");
			switch (Severity)
			{
				case EMessageSeverity::Error:           SevLabel = TEXT("error"); break;
				case EMessageSeverity::PerformanceWarning:
				case EMessageSeverity::Warning:         SevLabel = TEXT("warning"); break;
				case EMessageSeverity::Info:            SevLabel = TEXT("note"); break;
				default:                                SevLabel = TEXT("info"); break;
			}
			const FString MsgText = Msg->ToText().ToString();
			if (FirstErrorText.IsEmpty() && Severity == EMessageSeverity::Error)
			{
				FirstErrorText = MsgText;
			}
			TSharedPtr<FJsonObject> EntryObj = MakeShared<FJsonObject>();
			EntryObj->SetStringField(TEXT("severity"), SevLabel);
			EntryObj->SetStringField(TEXT("message"), MsgText);
			for (const TSharedRef<IMessageToken>& Token : Msg->GetMessageTokens())
			{
				if (Token->GetType() == EMessageToken::Object)
				{
					const FUObjectToken& ObjToken = static_cast<const FUObjectToken&>(*Token);
					// Resolve the object via FindObject to dodge weak-ptr API
					// differences across engine revisions; sufficient for the
					// "name the failing node" use case since compile errors
					// reference live K2 nodes in the open Blueprint.
					if (UEdGraphNode* GraphNode = FindObject<UEdGraphNode>(
						/*Outer=*/nullptr, *ObjToken.GetOriginalObjectPathName()))
					{
						EntryObj->SetStringField(TEXT("node_guid"),
							GraphNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
						break;
					}
				}
			}
			LogArray.Add(MakeShared<FJsonValueObject>(EntryObj));
			++EmittedCount;
		}
		CompileResult.Data->SetArrayField(TEXT("compile_log"), LogArray);
		CompileResult.Data->SetNumberField(TEXT("compile_log_total_entries"), CompilerLog.Messages.Num());
		CompileResult.Data->SetNumberField(TEXT("compile_log_num_errors"), CompilerLog.NumErrors);
		CompileResult.Data->SetNumberField(TEXT("compile_log_num_warnings"), CompilerLog.NumWarnings);
		if (CompilerLog.Messages.Num() > MaxEntries)
		{
			CompileResult.Data->SetBoolField(TEXT("compile_log_truncated"), true);
		}

		// Promote the first Error's text to the top-level error message so
		// the response summary carries actionable detail.
		if (Status == BS_Error && !FirstErrorText.IsEmpty())
		{
			CompileResult.bIsError = true;
			CompileResult.ErrorMessage = FString::Printf(
				TEXT("Compilation failed: %s"), *FirstErrorText);
		}
	}

	return CompileResult;
}

// ----------------------------------------------------------------------------
// hot-path metadata enrichment
// ----------------------------------------------------------------------------

FString ClaireonBlueprintGraphTool_Compile::GetFullDescription() const
{
    return TEXT(
        "Compiles the Blueprint of the current session and returns a "
        "structured list of errors and warnings (file/line/message). Compile "
        "is in-memory only -- it does NOT write to disk; pair with "
        "bp_save (or bp_close, which "
        "auto-saves) when you want the compiled state persisted. Use this "
        "tool to validate after a sequence of add_node/connect_pins calls or "
        "after a structural refactor (e.g. variable type change), so you "
        "catch broken pin types before they propagate. Returns success even "
        "if errors are present -- inspect the errors[] array on the result.");
}

FString ClaireonBlueprintGraphTool_Compile::GetExampleUsage() const
{
    return TEXT("bp_compile session_id=\"...\"  |  bp_compile asset_path=\"/Game/BP/MyActor\"");
}

#undef LOCTEXT_NAMESPACE
