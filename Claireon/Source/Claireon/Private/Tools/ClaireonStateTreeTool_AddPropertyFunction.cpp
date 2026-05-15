// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_AddPropertyFunction.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonStateTreeEditInternal.h"
#include "ClaireonLog.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreePropertyBindings.h"
#include "StateTreeEditorPropertyBindings.h"
#include "StateTreePropertyFunctionBase.h"
#include "StructUtils/InstancedStruct.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonStateTreeTool_AddPropertyFunction::GetName() const
{
	return TEXT("claireon.statetree_add_property_function");
}

FString ClaireonStateTreeTool_AddPropertyFunction::GetDescription() const
{
	return TEXT("Add a property function binding that feeds the function output into a target property.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_AddPropertyFunction::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("struct_name"), TEXT("Struct type name of the property function."), true);
	Builder.AddString(TEXT("target_node_id"), TEXT("GUID of the target node."), true);
	Builder.AddString(TEXT("target_property"), TEXT("Target property path."), true);
	Builder.AddString(TEXT("source_property"), TEXT("Optional output property path within the property function. Autodetected if omitted."));
	Builder.AddObject(TEXT("properties"), TEXT("Optional map of property_name -> string value to set on the property function."));
	return Builder.Build();
}

FToolResult ClaireonStateTreeTool_AddPropertyFunction::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FStateTreeEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	UStateTreeEditorData* EditorData = ClaireonStateTreeEditInternal::GetEditorDataFromSession(Data, Error);
	if (!EditorData)
		return MakeErrorResult(Error);

	FString StructName;
	if (!Arguments->TryGetStringField(TEXT("struct_name"), StructName))
		return MakeErrorResult(TEXT("Missing required parameter: struct_name"));

	FGuid TargetNodeId;
	if (!ClaireonStateTreeEditInternal::ParseGuidParam(Arguments, TEXT("target_node_id"), TargetNodeId, Error))
		return MakeErrorResult(Error);

	FString TargetProperty;
	if (!Arguments->TryGetStringField(TEXT("target_property"), TargetProperty))
		return MakeErrorResult(TEXT("Missing required parameter: target_property"));

	FString SourceProperty;
	Arguments->TryGetStringField(TEXT("source_property"), SourceProperty);

#if WITH_EDITORONLY_DATA
	UScriptStruct* NodeStruct = ClaireonStateTreeHelpers::ResolveNodeStruct(StructName, Error);
	if (!NodeStruct)
		return MakeErrorResult(Error);

	if (!NodeStruct->IsChildOf(FStateTreePropertyFunctionBase::StaticStruct()))
	{
		return MakeErrorResult(FString::Printf(TEXT("'%s' is not a property function (must derive from FStateTreePropertyFunctionBase)"), *StructName));
	}

	FStateTreePropertyPath TargetPath(TargetNodeId);
	TargetPath.FromString(TargetProperty);

	TArray<FStateTreePropertyPathSegment> SourceSegments;
	if (!SourceProperty.IsEmpty())
	{
		FStateTreePropertyPath TempPath;
		TempPath.FromString(SourceProperty);
		SourceSegments = TempPath.GetSegments();
	}
	else
	{
		FInstancedStruct TempInstance;
		TempInstance.InitializeAs(NodeStruct);
		const FStateTreePropertyFunctionBase& TempFunction = TempInstance.Get<FStateTreePropertyFunctionBase>();
		if (const UStruct* InstanceType = Cast<const UStruct>(TempFunction.GetInstanceDataType()))
		{
			for (TFieldIterator<FProperty> It(InstanceType); It; ++It)
			{
				if (It->HasMetaData(TEXT("Category")) && It->GetMetaData(TEXT("Category")) == TEXT("Output"))
				{
					SourceSegments.Add(FStateTreePropertyPathSegment(It->GetFName()));
					break;
				}
			}
		}
		if (SourceSegments.IsEmpty())
		{
			return MakeErrorResult(TEXT("Could not auto-detect output property. Specify source_property explicitly."));
		}
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Property Function Binding")));
	Data->StateTree->Modify();

	FStateTreePropertyPath ResultSourcePath = EditorData->EditorBindings.AddFunctionPropertyBinding(NodeStruct, SourceSegments, TargetPath);

	const TSharedPtr<FJsonObject>* PropertiesObj = nullptr;
	if (Arguments->TryGetObjectField(TEXT("properties"), PropertiesObj) && PropertiesObj)
	{
		for (FStateTreePropertyPathBinding& Binding : EditorData->EditorBindings.GetMutableBindings())
		{
			if (Binding.GetTargetPath() == TargetPath)
			{
				FStructView PropertyFunctionNodeView = Binding.GetMutablePropertyFunctionNode();
				if (PropertyFunctionNodeView.IsValid())
				{
					FStateTreeEditorNode& EditorNode = PropertyFunctionNodeView.Get<FStateTreeEditorNode>();
					if (EditorNode.Instance.IsValid())
					{
						for (const auto& Pair : (*PropertiesObj)->Values)
						{
							FString PropValue;
							if (Pair.Value->TryGetString(PropValue))
							{
								ClaireonStateTreeHelpers::SetNodeProperty(EditorNode, Pair.Key, PropValue, true, Error);
								if (!Error.IsEmpty())
								{
									UE_LOG(LogClaireon, Warning, TEXT("AddPropertyFunction: Failed to set property '%s': %s"), *Pair.Key, *Error);
									Error.Empty();
								}
							}
						}
					}
				}
				break;
			}
		}
	}

	Data->LastOperationStatus = FString::Printf(TEXT("add_property_function -> Added %s -> %s.%s"), *StructName, *TargetNodeId.ToString(), *TargetProperty);
#else
	Data->LastOperationStatus = TEXT("add_property_function -> Not available in non-editor builds");
#endif

	return BuildStateResponse(SessionId, Data);
}
