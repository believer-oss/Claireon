// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_SetProperty.h"
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


FString ClaireonBlueprintGraphTool_SetProperty::GetOperation() const { return TEXT("set_property"); }

FString ClaireonBlueprintGraphTool_SetProperty::GetDescription() const
{
    return TEXT("Set a property on a component template or the Blueprint CDO in the open editing session. Requires open session_id from bp_open (or pass asset_path to auto-open). Transactional. Common pitfall: property_name must match the UPROPERTY name on the target class; nested property paths use dot notation. Accepts either session_id or asset_path; auto-opens a session when asset_path is supplied.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_SetProperty::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("property_name"), TEXT("Name of the property to set."), true);
    Builder.AddString(TEXT("property_value"), TEXT("New value as a string (ImportText_Direct format; for FGameplayTagContainer pass a JSON array of tag names)."), true);
    Builder.AddString(TEXT("component_name"), TEXT("Optional component name; defaults to the Blueprint CDO."));
    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_SetProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FBlueprintEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("set_property"), Params, SessionId, Data, Error))
    {
        return Error;
    }
	UBlueprint* Blueprint = Data->Blueprint.Get();

	if (!Blueprint)
	{
		return MakeErrorResult(TEXT("Blueprint is no longer valid"));
	}

	// Get property name and value
	FString PropertyName, PropertyValue;
	if (!Params->TryGetStringField(TEXT("property_name"), PropertyName))
	{
		return MakeErrorResult(TEXT("Missing required field: property_name"));
	}
	if (!Params->TryGetStringField(TEXT("property_value"), PropertyValue))
	{
		return MakeErrorResult(TEXT("Missing required field: property_value"));
	}

	// Get optional component name (if not specified, set on CDO)
	FString ComponentName;
	UObject* TargetObject = nullptr;
	FString TargetDescription;

	if (Params->TryGetStringField(TEXT("component_name"), ComponentName))
	{
		// Find component in SCS
		USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
		if (!SCS)
		{
			return MakeErrorResult(TEXT("Blueprint does not have a SimpleConstructionScript"));
		}

		USCS_Node* ComponentNode = SCS->FindSCSNode(FName(*ComponentName));

		if (!ComponentNode)
		{
			return MakeErrorResult(FString::Printf(TEXT("Component not found: %s"), *ComponentName));
		}

		TargetObject = ComponentNode->ComponentTemplate;
		TargetDescription = FString::Printf(TEXT("Component '%s'"), *ComponentName);
	}
	else
	{
		// Set property on CDO
		if (Blueprint->GeneratedClass)
		{
			TargetObject = Blueprint->GeneratedClass->GetDefaultObject();
			TargetDescription = TEXT("Blueprint CDO");
		}

		if (!TargetObject)
		{
			return MakeErrorResult(TEXT("Failed to get Blueprint CDO"));
		}
	}

	// Find property on target object
	FProperty* Property = TargetObject->GetClass()->FindPropertyByName(FName(*PropertyName));
	if (!Property)
	{
		return MakeErrorResult(FString::Printf(TEXT("Property '%s' not found on %s"), *PropertyName, *TargetDescription));
	}

	// Check if property is editable
	if (Property->HasAnyPropertyFlags(CPF_DisableEditOnInstance))
	{
		return MakeErrorResult(FString::Printf(TEXT("Property '%s' is not editable"), *PropertyName));
	}

	// Set property using transaction
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Blueprint Property")));
	Blueprint->Modify();
	TargetObject->Modify();

	// Smart detection: GameplayTagContainer from JSON array
	FStructProperty* MaybeTagContainerProp = CastField<FStructProperty>(Property);
	if (MaybeTagContainerProp && MaybeTagContainerProp->Struct == TBaseStructure<FGameplayTagContainer>::Get() && PropertyValue.StartsWith(TEXT("[")))
	{
		// Parse JSON array of tag name strings
		TArray<TSharedPtr<FJsonValue>> TagArray;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(PropertyValue);
		if (!FJsonSerializer::Deserialize(Reader, TagArray))
		{
			return MakeErrorResult(TEXT("property_value starts with '[' but is not valid JSON array"));
		}

		FGameplayTagContainer NewContainer;
		UGameplayTagsManager& TagsManager = UGameplayTagsManager::Get();
		TArray<FString> Warnings;

		for (const TSharedPtr<FJsonValue>& Val : TagArray)
		{
			FString TagName;
			if (!Val->TryGetString(TagName))
				continue;
			FGameplayTag Tag = TagsManager.RequestGameplayTag(FName(*TagName), false);
			if (Tag.IsValid())
				NewContainer.AddTag(Tag);
			else
				Warnings.Add(FString::Printf(TEXT("[WARN] Tag '%s' not registered -- skipped"), *TagName));
		}

		FGameplayTagContainer* Target =
			MaybeTagContainerProp->ContainerPtrToValuePtr<FGameplayTagContainer>(TargetObject);
		*Target = NewContainer;

		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

		FString WarningText = Warnings.IsEmpty() ? TEXT("") : FString::Join(Warnings, TEXT("\n")) + TEXT("\n");
		Data->Cursor.LastOperationStatus = FString::Printf(
			TEXT("Set %s.%s = [%d tags]"), *TargetDescription, *PropertyName, NewContainer.Num());

		// Prepend warnings to the normal state response
		FToolResult StateResult = BuildStateResponse(SessionId, Data);
		if (!WarningText.IsEmpty())
		{
			StateResult.Summary = WarningText + StateResult.Summary;
		}
		return StateResult;
	}

	// Import property value from string
	const TCHAR* ValuePtr = *PropertyValue;
	Property->ImportText_Direct(ValuePtr, Property->ContainerPtrToValuePtr<void>(TargetObject), TargetObject, PPF_None);

	// Mark Blueprint as modified
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	Data->Cursor.LastOperationStatus = FString::Printf(
		TEXT("Set %s.%s = '%s'"),
		*TargetDescription, *PropertyName, *PropertyValue);

	return BuildStateResponse(SessionId, Data);
}

#undef LOCTEXT_NAMESPACE
