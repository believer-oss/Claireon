// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_Create.h"
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
#include "Animation/AnimInstance.h"
#include "Blueprint/UserWidget.h"
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


FString ClaireonBlueprintGraphTool_Create::GetOperation() const { return TEXT("create"); }

FString ClaireonBlueprintGraphTool_Create::GetDescription() const
{
    return TEXT("Create a new Blueprint asset at asset_path and open an editing session on it. Returns the session_id other bp_* tools take; release it with bp_close, or let it expire on idle timeout. Common pitfall: parent_class must be a Blueprintable native class, and the package directory portion of asset_path must already exist.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_Create::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("asset_path"), TEXT("New Blueprint asset path."), true);
    Builder.AddString(TEXT("parent_class"), TEXT("Parent class path (e.g. /Script/Engine.Actor)."), true);
    Builder.AddString(TEXT("blueprint_type"), TEXT("Optional: 'Normal' (default), 'MacroLibrary', or 'Interface'. MacroLibrary and Interface Blueprints have no EventGraph -- the session opens with no active graph until bp_add_macro or bp_add_function creates one. parent_class defaults to Actor, or Interface for blueprint_type='Interface'."));
    Builder.AddNumber(TEXT("timeout_minutes"), TEXT("Session inactivity timeout in minutes (default 10; every operation resets the clock)."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_Create::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    // create is stateless; it opens its own session internally.
    TSharedPtr<FJsonObject> Params = Arguments.IsValid() ? Arguments : MakeShared<FJsonObject>();
    if (Params->HasField(TEXT("params")))
    {
        const TSharedPtr<FJsonObject>* NestedObj = nullptr;
        if (Params->TryGetObjectField(TEXT("params"), NestedObj) && NestedObj && NestedObj->IsValid())
        {
            Params = *NestedObj;
        }
    }
	// Get asset_path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return MakeErrorResult(TEXT("Missing required field: asset_path"));
	}

	// Validate asset path
	FString ValidationError;
	if (!ClaireonBlueprintHelpers::ValidateAssetPath(AssetPath, ValidationError))
	{
		return MakeErrorResult(ValidationError);
	}

	// blueprint_type. Advertised since forever but never read, so every asset came out
	// BPTYPE_Normal regardless. LevelScript is rejected by name rather than silently
	// downgraded: level script Blueprints are inner objects of a ULevel, not standalone
	// package assets, so this tool's create-a-package flow cannot produce one.
	EBlueprintType BPType = BPTYPE_Normal;
	FString BlueprintTypeName;
	if (Params->TryGetStringField(TEXT("blueprint_type"), BlueprintTypeName) && !BlueprintTypeName.IsEmpty())
	{
		if (BlueprintTypeName.Equals(TEXT("Normal"), ESearchCase::IgnoreCase))
		{
			BPType = BPTYPE_Normal;
		}
		else if (BlueprintTypeName.Equals(TEXT("MacroLibrary"), ESearchCase::IgnoreCase))
		{
			BPType = BPTYPE_MacroLibrary;
		}
		else if (BlueprintTypeName.Equals(TEXT("Interface"), ESearchCase::IgnoreCase))
		{
			BPType = BPTYPE_Interface;
		}
		else if (BlueprintTypeName.Equals(TEXT("LevelScript"), ESearchCase::IgnoreCase))
		{
			return MakeErrorResult(TEXT(
				"blueprint_type 'LevelScript' is not supported: level script Blueprints are inner "
				"objects of a ULevel, not standalone package assets, so they cannot be created at an "
				"asset_path. Use 'Normal', 'MacroLibrary', or 'Interface'."));
		}
		else
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Unknown blueprint_type '%s'. Valid values: 'Normal', 'MacroLibrary', 'Interface'."),
				*BlueprintTypeName));
		}
	}

	// Get parent_class. Defaults follow the editor's own factories:
	// UBlueprintMacroFactory uses AActor, UBlueprintInterfaceFactory uses UInterface.
	FString ParentClassName;
	if (!Params->TryGetStringField(TEXT("parent_class"), ParentClassName))
	{
		ParentClassName = (BPType == BPTYPE_Interface) ? TEXT("Interface") : TEXT("Actor");
	}

	// Find parent class
	ClaireonNameResolver::FNameResolveResult ParentClassResult;
	UClass* ParentClass = ClaireonNameResolver::ResolveClassName(ParentClassName, nullptr, ParentClassResult);
	if (!IsValid(ParentClass))
	{
		return MakeErrorResult(ParentClassResult.Error);
	}
	TArray<FString> ResolutionWarnings;
	if (!ParentClassResult.ResolutionNote.IsEmpty())
	{
		ResolutionWarnings.Add(ParentClassResult.ResolutionNote);
	}

	// UserWidget / AnimInstance parents need a specialized Blueprint asset class
	// (UWidgetBlueprint / UAnimBlueprint). CreateBlueprint below would silently
	// produce a plain UBlueprint -- no widget tree, no anim-graph host -- and
	// anim-node authoring into such an asset cast-fatals. Refuse loudly.
	if (ParentClass->IsChildOf(UUserWidget::StaticClass()))
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Parent class '%s' derives from UserWidget: a plain Blueprint asset would have no widget tree. Use widgetbp_create instead."),
			*ParentClassName));
	}
	if (ParentClass->IsChildOf(UAnimInstance::StaticClass()))
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Parent class '%s' derives from AnimInstance: a plain Blueprint asset cannot host an anim graph. Use animbp_create instead."),
			*ParentClassName));
	}
	// ControlRig parents need UControlRigBlueprint (RigVM-hosted graphs). Guard by
	// name walk: the ControlRig module is not a Claireon dependency.
	for (const UClass* Cls = ParentClass; IsValid(Cls); Cls = Cls->GetSuperClass())
	{
		if (Cls->GetFName() == FName(TEXT("ControlRig")))
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Parent class '%s' derives from ControlRig: a plain Blueprint asset cannot host a RigVM graph. Control Rig authoring is not supported by the bp_* tools."),
				*ParentClassName));
		}
	}

	// Route package + Blueprint creation through the shared helper. The helper handles
	// asset-path splitting, existing-file deletion, package creation, blueprint creation,
	// externally-referenceable flag, asset-registry notification, and EventGraph capture.
	ClaireonBlueprintHelpers::FCreateBlueprintResult BPCreateResult;
	ClaireonBlueprintHelpers::CreateBlueprint(AssetPath, ParentClass, BPCreateResult, BPType);
	if (!BPCreateResult.IsOk())
	{
		return MakeErrorResult(BPCreateResult.Error);
	}
	for (const FString& W : BPCreateResult.Warnings)
	{
		ResolutionWarnings.Add(W);
	}
	UBlueprint* Blueprint = BPCreateResult.Blueprint;
	UEdGraph* EventGraph = BPCreateResult.EventGraph;
	// MacroLibrary and Interface Blueprints get no ubergraph at all
	// (FBlueprintEditorUtils::DoesSupportEventGraphs is false for both), so a missing
	// EventGraph is only a failure for BPTYPE_Normal. The session opens with no active
	// graph until bp_add_macro (or bp_add_function) creates one.
	if (!IsValid(EventGraph) && BPType == BPTYPE_Normal)
	{
		return MakeErrorResult(TEXT("Failed to find EventGraph in newly created Blueprint"));
	}

	// Register delegate on first use
	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonBlueprintGraphEditToolBase::HandleSessionClosed);
		bDelegateRegistered = true;
	}

	// Open session via the manager (handles locking)
	double TimeoutMinutes = ClaireonDefaultSessionTimeoutMinutes;
	Params->TryGetNumberField(TEXT("timeout_minutes"), TimeoutMinutes);
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(Blueprint->GetPathName(), TEXT("bp"), TimeoutMinutes);

	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		const FTimespan Elapsed = FDateTime::UtcNow() - Blocker.LastAccessTime;
		return MakeErrorResult(FString::Printf(
			TEXT("Asset is locked by %s session %s (last activity %dm %ds ago). Close that session first, or call session_release(session_id='%s') to force-release it."),
			*Blocker.ToolName, *Blocker.SessionId,
			static_cast<int32>(Elapsed.GetTotalMinutes()),
			static_cast<int32>(Elapsed.GetTotalSeconds()) % 60,
			*Blocker.SessionId));
	}

	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid asset path for created Blueprint: %s"), *Blueprint->GetPathName()));
	}

	const FString& SessionId = OpenResult.SessionId;

	// Create tool-specific data
	FBlueprintEditToolData NewData;
	NewData.Blueprint = Blueprint;
	NewData.Graph = EventGraph;
	// Null for MacroLibrary/Interface, which have no ubergraph at all.
	NewData.Cursor.GraphName = IsValid(EventGraph) ? EventGraph->GetName() : FString();
	NewData.Cursor.ViewportCenter = FVector2D(0.0f, 0.0f);

	// Find first event node to focus cursor
	TArray<UEdGraphNode*> RootNodes = IsValid(EventGraph)
		? ClaireonBlueprintHelpers::FindRootNodes(EventGraph)
		: TArray<UEdGraphNode*>();
	if (RootNodes.Num() > 0)
	{
		UEdGraphNode* FirstNode = RootNodes[0];
		NewData.Cursor.FocusedNodeGuid = FirstNode->NodeGuid;
		UEdGraphPin* FirstOutput = ClaireonBlueprintHelpers::GetFirstOutputPin(FirstNode);
		if (FirstOutput)
		{
			NewData.Cursor.FocusedPinName = FirstOutput->PinName;
			NewData.Cursor.FocusedPinDirection = FirstOutput->Direction;
		}
	}

	ToolData.Add(SessionId, MoveTemp(NewData));
	FBlueprintEditToolData* Data = ToolData.Find(SessionId);

	UE_LOG(LogClaireon, Log, TEXT("[EditBlueprintGraph] Created session %s for new Blueprint %s"), *SessionId, *Blueprint->GetPathName());

	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Created new Blueprint %s with parent class %s"), *AssetPath, *ParentClassName);

	// "create" always returns the full state regardless of response_mode so the
	// caller can parse the newly-minted Session ID out of the response (mirrors
	// Operation_Open behavior).
	Data->ResponseMode = TEXT("full");
	Data->bSuppressOutput = false;

	FToolResult CreateResult = BuildStateResponse(SessionId, Data);
	CreateResult.Warnings.Append(ResolutionWarnings);
	return CreateResult;
}

#undef LOCTEXT_NAMESPACE
