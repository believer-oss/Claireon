// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_GetComponentDetails.h"
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


FString ClaireonBlueprintGraphTool_GetComponentDetails::GetOperation() const { return TEXT("graph_get_component_details"); }

FString ClaireonBlueprintGraphTool_GetComponentDetails::GetDescription() const
{
    return TEXT("Inspect a component on the Blueprint's Simple Construction Script and return its properties in the open editing session. Requires open session_id from blueprint_graph_open (or pass asset_path to auto-open). Read-only. Returns property name, type, and current value tuples for the component template.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_GetComponentDetails::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("component_name"), TEXT("Name of the component to inspect."), true);
    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_GetComponentDetails::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FBlueprintEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("get_component_details"), Params, SessionId, Data, Error))
    {
        return Error;
    }
	UBlueprint* Blueprint = Data->Blueprint.Get();
	if (!Blueprint)
	{
		return MakeErrorResult(TEXT("Blueprint is no longer valid"));
	}

	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	if (!SCS)
	{
		return MakeErrorResult(TEXT("Blueprint does not have a SimpleConstructionScript (not an Actor Blueprint?)"));
	}

	// Parse component_name
	FString ComponentName;
	if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
	{
		return MakeErrorResult(TEXT("Missing required field: component_name"));
	}

	// Parse optional include_defaults (default false)
	bool bIncludeDefaults = false;
	Params->TryGetBoolField(TEXT("include_defaults"), bIncludeDefaults);

	// Find node
	USCS_Node* Node = SCS->FindSCSNode(FName(*ComponentName));
	if (!Node)
	{
		return MakeErrorResult(FString::Printf(TEXT("Component not found: %s"), *ComponentName));
	}

	UActorComponent* ComponentTemplate = Node->ComponentTemplate;
	if (!ComponentTemplate)
	{
		return MakeErrorResult(FString::Printf(TEXT("Component '%s' has no template object"), *ComponentName));
	}

	// Build component details JSON
	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetStringField(TEXT("name"), Node->GetVariableName().ToString());
	Details->SetStringField(TEXT("component_name"), Node->GetVariableName().ToString());
	Details->SetStringField(TEXT("class"), ComponentTemplate->GetClass()->GetName());

	// is_root: check against scene root
	USCS_Node* SceneRootNode = nullptr;
	SCS->GetSceneRootComponentTemplate(true, &SceneRootNode);
	Details->SetBoolField(TEXT("is_root"), Node == SceneRootNode);

	// parent
	USCS_Node* ParentNode = SCS->FindParentNode(Node);
	if (ParentNode)
	{
		Details->SetStringField(TEXT("parent"), ParentNode->GetVariableName().ToString());
	}
	// If no parent, omit the field (root-level component)

	// children
	TArray<TSharedPtr<FJsonValue>> ChildrenArray;
	for (USCS_Node* ChildNode : Node->GetChildNodes())
	{
		if (ChildNode)
		{
			ChildrenArray.Add(MakeShared<FJsonValueString>(ChildNode->GetVariableName().ToString()));
		}
	}
	Details->SetArrayField(TEXT("children"), ChildrenArray);

	// is_scene_component and transform
	bool bIsSceneComponent = ComponentTemplate->IsA<USceneComponent>();
	Details->SetBoolField(TEXT("is_scene_component"), bIsSceneComponent);

	if (bIsSceneComponent)
	{
		USceneComponent* SceneComp = Cast<USceneComponent>(ComponentTemplate);
		FVector Location = SceneComp->GetRelativeLocation();
		FRotator Rotation = SceneComp->GetRelativeRotation();
		FVector Scale = SceneComp->GetRelativeScale3D();

		Details->SetStringField(TEXT("relative_location"), FString::Printf(TEXT("X=%.2f Y=%.2f Z=%.2f"), Location.X, Location.Y, Location.Z));
		Details->SetStringField(TEXT("relative_rotation"), FString::Printf(TEXT("P=%.2f Y=%.2f R=%.2f"), Rotation.Pitch, Rotation.Yaw, Rotation.Roll));
		Details->SetStringField(TEXT("relative_scale"), FString::Printf(TEXT("X=%.2f Y=%.2f Z=%.2f"), Scale.X, Scale.Y, Scale.Z));
	}

	// Properties
	TArray<TSharedPtr<FJsonValue>> PropertiesArray;
	UObject* CDO = ComponentTemplate->GetClass()->GetDefaultObject();

	for (TFieldIterator<FProperty> PropIt(ComponentTemplate->GetClass()); PropIt; ++PropIt)
	{
		FProperty* Property = *PropIt;

		// Only include editable properties
		if (!Property->HasAnyPropertyFlags(CPF_Edit))
		{
			continue;
		}

		// Skip deprecated and transient properties
		if (Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient))
		{
			continue;
		}

		// Get values as strings for comparison
		FString TemplateValue;
		Property->ExportTextItem_Direct(TemplateValue, Property->ContainerPtrToValuePtr<void>(ComponentTemplate), nullptr, ComponentTemplate, PPF_None);

		FString DefaultValue;
		Property->ExportTextItem_Direct(DefaultValue, Property->ContainerPtrToValuePtr<void>(CDO), nullptr, CDO, PPF_None);

		// If not including defaults, skip properties that match CDO
		if (!bIncludeDefaults && TemplateValue == DefaultValue)
		{
			continue;
		}

		TSharedPtr<FJsonObject> PropObj = MakeShared<FJsonObject>();
		PropObj->SetStringField(TEXT("name"), Property->GetName());
		PropObj->SetStringField(TEXT("type"), Property->GetCPPType());
		PropObj->SetStringField(TEXT("value"), TemplateValue);
		PropertiesArray.Add(MakeShared<FJsonValueObject>(PropObj));
	}
	Details->SetArrayField(TEXT("properties"), PropertiesArray);

	// Structured payload goes on data.component; the summary becomes a compact one-liner
	// for log-readability. Consumers that parse the response read data.component;
	// the summary body is informational only.
	const FString ComponentClassName = ComponentTemplate->GetClass()->GetName();
	FString ParentDisplay = TEXT("(root)");
	if (Details->HasField(TEXT("parent")))
	{
		Details->TryGetStringField(TEXT("parent"), ParentDisplay);
	}

	Data->Cursor.LastOperationStatus = FString::Printf(
		TEXT("Component '%s' (class=%s, parent=%s)"),
		*ComponentName,
		*ComponentClassName,
		*ParentDisplay);

	FToolResult Result = BuildStateResponse(SessionId, Data);
	if (Result.Data.IsValid())
	{
		Result.Data->SetObjectField(TEXT("component"), Details);
	}
	return Result;
}

#undef LOCTEXT_NAMESPACE
