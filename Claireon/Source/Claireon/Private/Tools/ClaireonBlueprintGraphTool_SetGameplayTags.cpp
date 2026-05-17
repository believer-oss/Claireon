// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_SetGameplayTags.h"
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


FString ClaireonBlueprintGraphTool_SetGameplayTags::GetOperation() const { return TEXT("set_gameplay_tags"); }

FString ClaireonBlueprintGraphTool_SetGameplayTags::GetDescription() const
{
    return TEXT("Surgically add/remove gameplay tags from a FGameplayTagContainer property on a Blueprint CDO. Stateless / non-session: writes the asset directly by path, no open session required. Common pitfall: the property must be a FGameplayTagContainer (not a single FGameplayTag); the tags must already be registered in a tag table. Session-mode tool: open via blueprint_graph_open first.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_SetGameplayTags::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (e.g. /Game/Characters/BP_Player)."), true);
    Builder.AddString(TEXT("property_path"), TEXT("Dot-separated path to the FGameplayTagContainer property on the CDO (e.g. 'RemovalTagRequirements.require_tags')."), true);

    // tags_to_add and tags_to_remove are string arrays; FToolSchemaBuilder has no AddArray helper,
    // so inject them directly into Properties before Build().
    {
        TSharedPtr<FJsonObject> AddProp = MakeShared<FJsonObject>();
        AddProp->SetStringField(TEXT("type"), TEXT("array"));
        TSharedPtr<FJsonObject> StrItems = MakeShared<FJsonObject>();
        StrItems->SetStringField(TEXT("type"), TEXT("string"));
        AddProp->SetObjectField(TEXT("items"), StrItems);
        AddProp->SetStringField(TEXT("description"), TEXT("Tag names to add to the container."));
        Builder.Properties->SetObjectField(TEXT("tags_to_add"), AddProp);
    }
    {
        TSharedPtr<FJsonObject> RemProp = MakeShared<FJsonObject>();
        RemProp->SetStringField(TEXT("type"), TEXT("array"));
        TSharedPtr<FJsonObject> StrItems = MakeShared<FJsonObject>();
        StrItems->SetStringField(TEXT("type"), TEXT("string"));
        RemProp->SetObjectField(TEXT("items"), StrItems);
        RemProp->SetStringField(TEXT("description"), TEXT("Tag names to remove from the container."));
        Builder.Properties->SetObjectField(TEXT("tags_to_remove"), RemProp);
    }

    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_SetGameplayTags::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    // set_gameplay_tags is stateless; it operates directly on the Blueprint CDO.
    TSharedPtr<FJsonObject> Params = Arguments.IsValid() ? Arguments : MakeShared<FJsonObject>();
    if (Params->HasField(TEXT("params")))
    {
        const TSharedPtr<FJsonObject>* NestedObj = nullptr;
        if (Params->TryGetObjectField(TEXT("params"), NestedObj) && NestedObj && NestedObj->IsValid())
        {
            Params = *NestedObj;
        }
    }
	FString AssetPath, PropertyPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
		return MakeErrorResult(TEXT("Missing required field: asset_path. set_gameplay_tags requires: asset_path, property_path, tags_to_add (array), tags_to_remove (array)"));
	if (!Params->TryGetStringField(TEXT("property_path"), PropertyPath))
		return MakeErrorResult(TEXT("Missing required field: property_path. set_gameplay_tags requires: asset_path, property_path, tags_to_add (array), tags_to_remove (array)"));

	FString ValidationError;
	if (!ClaireonBlueprintHelpers::ValidateAssetPath(AssetPath, ValidationError))
		return MakeErrorResult(ValidationError);

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
		return MakeErrorResult(FString::Printf(TEXT("Failed to load Blueprint: %s"), *AssetPath));

	if (!Blueprint->GeneratedClass)
		return MakeErrorResult(TEXT("Blueprint has no GeneratedClass (compile it first)"));

	UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject();
	if (!CDO)
		return MakeErrorResult(TEXT("Failed to get Blueprint CDO"));

	// Parse tags_to_add and tags_to_remove arrays
	TArray<FString> TagsToAdd, TagsToRemove;
	{
		const TArray<TSharedPtr<FJsonValue>>* AddArray = nullptr;
		if (Params->TryGetArrayField(TEXT("tags_to_add"), AddArray))
		{
			for (const TSharedPtr<FJsonValue>& Val : *AddArray)
			{
				FString TagStr;
				if (Val->TryGetString(TagStr) && !TagStr.IsEmpty())
					TagsToAdd.Add(TagStr);
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* RemoveArray = nullptr;
		if (Params->TryGetArrayField(TEXT("tags_to_remove"), RemoveArray))
		{
			for (const TSharedPtr<FJsonValue>& Val : *RemoveArray)
			{
				FString TagStr;
				if (Val->TryGetString(TagStr) && !TagStr.IsEmpty())
					TagsToRemove.Add(TagStr);
			}
		}
	}

	if (TagsToAdd.IsEmpty() && TagsToRemove.IsEmpty())
		return MakeErrorResult(TEXT("At least one of tags_to_add or tags_to_remove must be non-empty"));

	// Walk the dot-separated property path to reach the FGameplayTagContainer
	// e.g. "RemovalTagRequirements.require_tags" -> walk two levels of FStructProperty
	TArray<FString> PathParts;
	PropertyPath.ParseIntoArray(PathParts, TEXT("."));

	void* CurrentContainer = CDO;
	UStruct* CurrentStruct = CDO->GetClass();
	FProperty* FinalProperty = nullptr;
	void* FinalContainer = nullptr;

	for (int32 i = 0; i < PathParts.Num(); i++)
	{
		FProperty* Prop = CurrentStruct->FindPropertyByName(FName(*PathParts[i]));
		if (!Prop)
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Property '%s' not found on '%s'"),
				*PathParts[i], *CurrentStruct->GetName()));
		}

		if (i == PathParts.Num() - 1)
		{
			FinalProperty = Prop;
			FinalContainer = CurrentContainer;
		}
		else
		{
			// Must be a struct property to continue walking
			FStructProperty* StructProp = CastField<FStructProperty>(Prop);
			if (!StructProp)
			{
				return MakeErrorResult(FString::Printf(
					TEXT("Property '%s' is not a struct -- cannot walk further"), *PathParts[i]));
			}
			CurrentContainer = StructProp->ContainerPtrToValuePtr<void>(CurrentContainer);
			CurrentStruct = StructProp->Struct;
		}
	}

	// Verify final property is a FGameplayTagContainer
	FStructProperty* TagContainerProp = CastField<FStructProperty>(FinalProperty);
	if (!TagContainerProp || TagContainerProp->Struct != TBaseStructure<FGameplayTagContainer>::Get())
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Property '%s' is not a FGameplayTagContainer"), *PathParts.Last()));
	}

	FGameplayTagContainer* Container =
		TagContainerProp->ContainerPtrToValuePtr<FGameplayTagContainer>(FinalContainer);

	// Check for deprecated property -- surface as warning, not error
	FString WarningText;
	if (FinalProperty->HasAnyPropertyFlags(CPF_Deprecated))
	{
		FString DeprecationMsg = FinalProperty->GetMetaData(TEXT("DeprecationMessage"));
		WarningText = FString::Printf(
			TEXT("[DEPRECATED] Property '%s' is deprecated. %s\n"),
			*FinalProperty->GetName(),
			DeprecationMsg.IsEmpty() ? TEXT("No replacement hint available.") : *DeprecationMsg);
	}

	// Apply changes in a transaction
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Blueprint Gameplay Tags")));
	Blueprint->Modify();
	CDO->Modify();

	UGameplayTagsManager& TagsManager = UGameplayTagsManager::Get();

	for (const FString& TagName : TagsToRemove)
	{
		FGameplayTag Tag = TagsManager.RequestGameplayTag(FName(*TagName), /*bErrorIfNotFound=*/false);
		if (Tag.IsValid())
		{
			Container->RemoveTag(Tag);
		}
		else
		{
			WarningText += FString::Printf(TEXT("[WARN] Tag '%s' not registered in project -- skipped removal\n"), *TagName);
		}
	}

	for (const FString& TagName : TagsToAdd)
	{
		FGameplayTag Tag = TagsManager.RequestGameplayTag(FName(*TagName), /*bErrorIfNotFound=*/false);
		if (Tag.IsValid())
		{
			Container->AddTag(Tag);
		}
		else
		{
			WarningText += FString::Printf(TEXT("[WARN] Tag '%s' not registered in project -- skipped add\n"), *TagName);
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	// Report resulting container contents
	TArray<FString> ResultTags;
	for (const FGameplayTag& Tag : Container->GetGameplayTagArray())
	{
		ResultTags.Add(Tag.ToString());
	}

	FString Output = WarningText;
	Output += FString::Printf(TEXT("Updated %s.%s\n"), *AssetPath, *PropertyPath);
	Output += FString::Printf(TEXT("Added: %s\nRemoved: %s\nResulting tags: [%s]"),
		*FString::Join(TagsToAdd, TEXT(", ")),
		*FString::Join(TagsToRemove, TEXT(", ")),
		*FString::Join(ResultTags, TEXT(", ")));

	return MakeSuccessResult(nullptr, Output);
}

#undef LOCTEXT_NAMESPACE
