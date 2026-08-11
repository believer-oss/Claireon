// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_SetProperty.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonBlueprintHelpers.h"
#include "Tools/ClaireonPropertyUtils.h"
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
#include "ClaireonStructReflection.h"
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
    return TEXT("Set a property on a component template or the Blueprint CDO in the open editing session. property_name "
                "takes a plain UPROPERTY name or a dotted path into nested structs, array elements, and instanced "
                "sub-objects (e.g. 'Operations[0].Weight') -- the same resolver bp_set_cdo_property uses. Session-mode "
                "tool: transactional; pass session_id from bp_open, or asset_path to auto-open.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_SetProperty::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("property_name"), TEXT("Name of the property to set."), true);
    Builder.AddString(TEXT("property_value"), TEXT("New value as a string (ImportText_Direct format; for FGameplayTagContainer pass a JSON array of tag names)."), true);
    Builder.AddString(TEXT("component_name"), TEXT("Optional component name; defaults to the Blueprint CDO."));
    Builder.AddBoolean(TEXT("allow_non_editable"), TEXT("Write a property the details panel would refuse (EditConst, or no EditAnywhere/EditDefaultsOnly specifier). Same flag and same rule as uobject_set_property."));
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

	if (!IsValid(Blueprint))
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
		if (!IsValid(SCS))
		{
			return MakeErrorResult(TEXT("Blueprint does not have a SimpleConstructionScript"));
		}

		USCS_Node* ComponentNode = SCS->FindSCSNode(FName(*ComponentName));

		if (!IsValid(ComponentNode))
		{
			return MakeErrorResult(FString::Printf(TEXT("Component not found: %s"), *ComponentName));
		}

		TargetObject = ComponentNode->ComponentTemplate;
		TargetDescription = FString::Printf(TEXT("Component '%s'"), *ComponentName);
	}
	else
	{
		// Set property on CDO
		if (IsValid(Blueprint->GeneratedClass))
		{
			TargetObject = Blueprint->GeneratedClass->GetDefaultObject();
			TargetDescription = TEXT("Blueprint CDO");
		}

		if (!IsValid(TargetObject))
		{
			return MakeErrorResult(TEXT("Failed to get Blueprint CDO"));
		}
	}

	// Resolve the property through the SHARED path resolver rather than a flat
	// FindPropertyByName. The old flat lookup meant a dotted path such as
	// "PrimaryComponentTick.bStartWithTickEnabled" could never resolve -- it was looked up
	// verbatim as a single property name -- even though this tool's own description already
	// promised dot notation, and the sibling bp_set_cdo_property supported it. Both tools now
	// go through ClaireonPropertyUtils so nested structs, array elements, and instanced
	// sub-objects behave identically on either one.
	void* PropertyContainer = nullptr;
	FString ResolvePathError;
	FProperty* Property = ClaireonPropertyUtils::ResolvePropertyByPath(
		TargetObject, PropertyName, PropertyContainer, ResolvePathError);
	if (!Property || !PropertyContainer)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Property '%s' not found on %s: %s"),
			*PropertyName, *TargetDescription,
			ResolvePathError.IsEmpty() ? TEXT("no such property") : *ResolvePathError));
	}

	// Editability gate, shared with uobject_inspect / uobject_set_property.
	//
	// The old gate rejected CPF_DisableEditOnInstance, which is the flag set by
	// EditDefaultsOnly -- i.e. it refused exactly the properties this tool exists
	// to write. This tool writes CDOs and SCS templates, which IS the defaults
	// context, so EditDefaultsOnly is editable here. UActorComponent::bReplicates
	// and AFSCharacter::MassConfig were both rejected on that basis.
	//
	// The rule now is DescribeEditorAccess, so bp_set_property and
	// uobject_inspect's reported editor_access can never disagree.
	bool bAllowNonEditable = false;
	Params->TryGetBoolField(TEXT("allow_non_editable"), bAllowNonEditable);

	const FString EditorAccess = ClaireonStructReflection::DescribeEditorAccess(Property->GetPropertyFlags());
	if (EditorAccess != TEXT("edit") && !bAllowNonEditable)
	{
		const FString Why = (EditorAccess == TEXT("edit_const"))
			? TEXT("is marked EditConst")
			: TEXT("has no EditAnywhere/EditDefaultsOnly specifier");
		return MakeErrorResult(FString::Printf(
			TEXT("Property '%s' on %s %s, so the details panel would refuse this edit. "
				 "Pass allow_non_editable=true to write it anyway -- such fields often carry "
				 "invariants the owning class maintains."),
			*PropertyName, *TargetDescription, *Why));
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
			MaybeTagContainerProp->ContainerPtrToValuePtr<FGameplayTagContainer>(PropertyContainer);
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

	// Delegate the write to the shared helper. The bare ImportText_Direct that
	// used to live here discarded its return value, so a malformed value was
	// reported as a successful set; it also skipped the
	// PreEditChange/PostEditChangeProperty bracket and the object-path
	// canonicalization that a details-panel edit performs.
	FString WriteError;
	if (!ClaireonPropertyUtils::WritePropertyByPath(TargetObject, PropertyName, PropertyValue, WriteError))
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Failed to set '%s' on %s to '%s': %s"),
			*PropertyName, *TargetDescription, *PropertyValue,
			WriteError.IsEmpty() ? TEXT("the value was rejected") : *WriteError));
	}

	// Mark Blueprint as modified
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	Data->Cursor.LastOperationStatus = FString::Printf(
		TEXT("Set %s.%s = '%s'"),
		*TargetDescription, *PropertyName, *PropertyValue);

	return BuildStateResponse(SessionId, Data);
}

#undef LOCTEXT_NAMESPACE
