// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_RemoveVariable.h"
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


FString ClaireonBlueprintGraphTool_RemoveVariable::GetOperation() const { return TEXT("remove_variable"); }

FString ClaireonBlueprintGraphTool_RemoveVariable::GetDescription() const
{
    return TEXT("Remove a member variable from the Blueprint in the open editing session. Requires open session_id from bp_open (or pass asset_path to auto-open). Transactional. Common pitfall: refuses with an error if the variable is referenced by any node; pass force=true to delete the variable and break the references. Accepts either session_id or asset_path; auto-opens a session when asset_path is supplied.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_RemoveVariable::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("variable_name"), TEXT("Name of the variable to remove."), true);
    Builder.AddBoolean(TEXT("force"), TEXT("If true, skip referrer scan and remove unconditionally."));
    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_RemoveVariable::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FBlueprintEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("remove_variable"), Params, SessionId, Data, Error))
    {
        return Error;
    }
    // remove_variable mutates Blueprint->NewVariables (class-level state), not graph nodes,
    // so skip CheckMutationAffectedNodes (matches the old Execute_Internal arm's comment).
	UBlueprint* Blueprint = Data->Blueprint.Get();
	if (!Blueprint)
	{
		return MakeErrorResult(TEXT("Blueprint is no longer valid"));
	}

	FString VariableName;
	if (!Params->TryGetStringField(TEXT("variable_name"), VariableName) || VariableName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field 'variable_name' for remove_variable"));
	}

	bool bForce = false;
	Params->TryGetBoolField(TEXT("force"), bForce);

	const FName VarFName(*VariableName);

	// Step 1: locate the NewVariables entry.
	int32 VarIndex = INDEX_NONE;
	for (int32 Idx = 0; Idx < Blueprint->NewVariables.Num(); ++Idx)
	{
		if (Blueprint->NewVariables[Idx].VarName == VarFName)
		{
			VarIndex = Idx;
			break;
		}
	}
	if (VarIndex == INDEX_NONE)
	{
		return MakeErrorResult(FString::Printf(TEXT("variable '%s' not found"), *VariableName));
	}

	TArray<FString> AccumulatedWarnings;

	// Step 2-3: referrer scan (skipped when force=true).
	struct FReferrer
	{
		FString Graph;
		FString NodeTitle;
		FString NodeGuid;
		FString Pin;
	};
	TArray<FReferrer> Referrers;

	if (bForce)
	{
		AccumulatedWarnings.Add(TEXT("force=true specified -- referrer scan skipped"));
	}
	else
	{
		TArray<UEdGraph*> AllGraphs;
		Blueprint->GetAllGraphs(AllGraphs);

		TSet<FName> OpaqueClassesWarned;
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph)
			{
				continue;
			}
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node)
				{
					continue;
				}

				FProperty* Prop = Node->GetClass()->FindPropertyByName(TEXT("VariableReference"));
				if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
				{
					if (StructProp->Struct == FMemberReference::StaticStruct())
					{
						const FMemberReference* Ref = StructProp->ContainerPtrToValuePtr<FMemberReference>(Node);
						if (Ref && Ref->GetMemberName() == VarFName)
						{
							FReferrer R;
							R.Graph = Graph->GetName();
							R.NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
							R.NodeGuid = Node->NodeGuid.ToString();
							R.Pin = TEXT("");
							Referrers.Add(R);
						}
					}
					else
					{
						// Property exists but is not an FMemberReference -- could not verify safety.
						const FName ClassName = Node->GetClass()->GetFName();
						if (!OpaqueClassesWarned.Contains(ClassName))
						{
							OpaqueClassesWarned.Add(ClassName);
							AccumulatedWarnings.Add(FString::Printf(
								TEXT("could not verify safety - unable to scan node %s"),
								*ClassName.ToString()));
						}
					}
				}
				else if (!Prop)
				{
					// Opaque node -- emit one warning per unique class.
					const FName ClassName = Node->GetClass()->GetFName();
					if (!OpaqueClassesWarned.Contains(ClassName))
					{
						OpaqueClassesWarned.Add(ClassName);
						AccumulatedWarnings.Add(FString::Printf(
							TEXT("could not verify safety - unable to scan node %s"),
							*ClassName.ToString()));
					}
				}
			}
		}

		// Step 4: referenced -> structured error, no mutation, no compile.
		if (Referrers.Num() > 0)
		{
			FToolResult ErrResult = MakeErrorResult(FString::Printf(
				TEXT("variable '%s' is referenced; cannot remove"), *VariableName));

			TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> ReferrersJson;
			for (const FReferrer& R : Referrers)
			{
				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("graph"), R.Graph);
				Entry->SetStringField(TEXT("node_title"), R.NodeTitle);
				Entry->SetStringField(TEXT("node_guid"), R.NodeGuid);
				Entry->SetStringField(TEXT("pin"), R.Pin);
				ReferrersJson.Add(MakeShared<FJsonValueObject>(Entry));
			}
			DataObj->SetArrayField(TEXT("referrers"), ReferrersJson);
			ErrResult.Data = DataObj;
			ErrResult.Warnings.Append(AccumulatedWarnings);
			return ErrResult;
		}
	}

	// Step 5-7: remove, mark structurally modified, compile.
	{
		FScopedTransaction Transaction(LOCTEXT("RemoveVariable", "Remove Blueprint Variable"));
		Blueprint->Modify();

		FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, VarFName);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Removed %s; compiled"), *VariableName);

	// Step 8: build success response and append accumulated warnings.
	FToolResult Result = BuildStateResponse(SessionId, Data);
	Result.Warnings.Append(AccumulatedWarnings);
	return Result;
}

#undef LOCTEXT_NAMESPACE
