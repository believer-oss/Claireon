// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonPCGGraphTool_AddUserParameter.h"
#include "Tools/ClaireonPCGGraphHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonPathResolver.h"

#include "PCGGraph.h"
#include "ScopedTransaction.h"
#include "StructUtils/PropertyBag.h"

using FToolResult = IClaireonTool::FToolResult;

namespace ClaireonPCGAddUserParameter_Internal
{
	/** Scalar types need no ValueTypeObject; object-backed types require type_object. */
	bool ParseScalarType(const FString& In, EPropertyBagPropertyType& Out)
	{
		static const TMap<FString, EPropertyBagPropertyType> Map = {
			{TEXT("bool"),   EPropertyBagPropertyType::Bool},
			{TEXT("byte"),   EPropertyBagPropertyType::Byte},
			{TEXT("int32"),  EPropertyBagPropertyType::Int32},
			{TEXT("int"),    EPropertyBagPropertyType::Int32},
			{TEXT("int64"),  EPropertyBagPropertyType::Int64},
			{TEXT("uint32"), EPropertyBagPropertyType::UInt32},
			{TEXT("uint64"), EPropertyBagPropertyType::UInt64},
			{TEXT("float"),  EPropertyBagPropertyType::Float},
			{TEXT("double"), EPropertyBagPropertyType::Double},
			{TEXT("name"),   EPropertyBagPropertyType::Name},
			{TEXT("string"), EPropertyBagPropertyType::String},
			{TEXT("text"),   EPropertyBagPropertyType::Text},
		};
		if (const EPropertyBagPropertyType* Found = Map.Find(In.ToLower()))
		{
			Out = *Found;
			return true;
		}
		return false;
	}

	/** The object-backed types, each of which requires a type_object to be well-formed. */
	bool ParseObjectBackedType(const FString& In, EPropertyBagPropertyType& Out)
	{
		static const TMap<FString, EPropertyBagPropertyType> Map = {
			{TEXT("enum"),       EPropertyBagPropertyType::Enum},
			{TEXT("struct"),     EPropertyBagPropertyType::Struct},
			{TEXT("object"),     EPropertyBagPropertyType::Object},
			{TEXT("softobject"), EPropertyBagPropertyType::SoftObject},
			{TEXT("class"),      EPropertyBagPropertyType::Class},
			{TEXT("softclass"),  EPropertyBagPropertyType::SoftClass},
		};
		if (const EPropertyBagPropertyType* Found = Map.Find(In.ToLower()))
		{
			Out = *Found;
			return true;
		}
		return false;
	}

	const TCHAR* SupportedTypeList()
	{
		return TEXT("bool, byte, int32, int64, uint32, uint64, float, double, name, string, text, "
					"enum, struct, object, softobject, class, softclass");
	}
}

FString ClaireonPCGGraphTool_AddUserParameter::GetOperation() const { return TEXT("add_user_parameter"); }

FString ClaireonPCGGraphTool_AddUserParameter::GetDescription() const
{
	return TEXT("Add a graph user parameter (a Graph Parameter) to the PCG graph in an open editing "
				"session, optionally with a default value. These are what a Get Graph Parameter node "
				"reads and what a parent graph overrides per Subgraph instance. Re-running with an "
				"existing name of the same type updates its default. Requires session_id from "
				"pcg_open; persists after save.");
}

TSharedPtr<FJsonObject> ClaireonPCGGraphTool_AddUserParameter::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("name"), TEXT("Parameter name, unique within the graph."), true);
	Builder.AddString(TEXT("type"),
		TEXT("Parameter type. One of: bool, byte, int32, int64, uint32, uint64, float, double, "
			 "name, string, text, enum, struct, object, softobject, class, softclass. The last six "
			 "also require type_object."), true);
	Builder.AddString(TEXT("type_object"),
		TEXT("For enum/struct/object/softobject/class/softclass: path to the UEnum, UScriptStruct "
			 "or UClass the parameter holds, e.g. /Script/Engine.StaticMesh."));
	Builder.AddString(TEXT("default_value"),
		TEXT("Optional default, as text in the property's import format (for example 0.35, true, "
			 "or /Game/Env/SM_Tree.SM_Tree)."));
	return Builder.Build();
}

FToolResult ClaireonPCGGraphTool_AddUserParameter::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	using namespace ClaireonPCGAddUserParameter_Internal;

	FString SessionId;
	FPCGGraphEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	UPCGGraph* Graph = Data->PCGGraph.Get();
	if (!IsValid(Graph))
	{
		return MakeErrorResult(TEXT("Graph is no longer valid"));
	}

	FString ParamName;
	if (!Arguments->TryGetStringField(TEXT("name"), ParamName) || ParamName.TrimStartAndEnd().IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: name"));
	}
	ParamName = ParamName.TrimStartAndEnd();

	FString TypeName;
	if (!Arguments->TryGetStringField(TEXT("type"), TypeName) || TypeName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: type"));
	}

	FString TypeObjectPath;
	Arguments->TryGetStringField(TEXT("type_object"), TypeObjectPath);

	EPropertyBagPropertyType ValueType = EPropertyBagPropertyType::None;
	UObject* ValueTypeObject = nullptr;

	if (ParseScalarType(TypeName, ValueType))
	{
		if (!TypeObjectPath.IsEmpty())
		{
			return MakeErrorResult(FString::Printf(
				TEXT("type '%s' is a scalar and takes no type_object"), *TypeName));
		}
	}
	else if (ParseObjectBackedType(TypeName, ValueType))
	{
		if (TypeObjectPath.IsEmpty())
		{
			return MakeErrorResult(FString::Printf(
				TEXT("type '%s' requires type_object naming the UEnum, UScriptStruct or UClass it holds"),
				*TypeName));
		}
		const auto Resolved = ClaireonPathResolver::Resolve(TypeObjectPath);
		const FString LoadPath = Resolved.bSuccess ? Resolved.ResolvedPath.Path : TypeObjectPath;
		ValueTypeObject = LoadObject<UObject>(nullptr, *LoadPath);
		if (!IsValid(ValueTypeObject))
		{
			return MakeErrorResult(FString::Printf(TEXT("type_object '%s' could not be loaded"), *TypeObjectPath));
		}
	}
	else
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Unknown type '%s'. Supported: %s"), *TypeName, SupportedTypeList()));
	}

	// Refuse type changes to avoid migrating parent overrides. Same-type calls update the default.
	const FInstancedPropertyBag* ExistingBag = Graph->GetUserParametersStruct();
	bool bAlreadyExists = false;
	if (ExistingBag)
	{
		if (const FPropertyBagPropertyDesc* Desc = ExistingBag->FindPropertyDescByName(FName(*ParamName)))
		{
			if (Desc->ValueType != ValueType || Desc->ValueTypeObject != ValueTypeObject)
			{
				return MakeErrorResult(FString::Printf(
					TEXT("Parameter '%s' already exists with a different type; remove it in the Graph "
						 "Parameters panel before redeclaring it."), *ParamName));
			}
			bAlreadyExists = true;
		}
	}

	// Parse defaults in a probe bag before mutation; transaction cancellation does not restore objects.
	FString DefaultValue;
	const bool bHasDefault =
		Arguments->TryGetStringField(TEXT("default_value"), DefaultValue) && !DefaultValue.IsEmpty();
	if (bHasDefault)
	{
		FInstancedPropertyBag ProbeBag;
		ProbeBag.AddProperties({FPropertyBagPropertyDesc(FName(*ParamName), ValueType, ValueTypeObject)});
		if (ProbeBag.SetValueSerializedString(FName(*ParamName), DefaultValue) != EPropertyBagResult::Success)
		{
			return MakeErrorResult(FString::Printf(
				TEXT("default_value '%s' could not be parsed as %s; parameter '%s' was not declared"),
				*DefaultValue, *TypeName, *ParamName));
		}
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add PCG Graph Parameter")));
	Graph->Modify();

	if (!bAlreadyExists)
	{
		// AddUserParameters notifies graph instances and parameter-getter nodes.
		const FPropertyBagPropertyDesc Desc(FName(*ParamName), ValueType, ValueTypeObject);
		Graph->AddUserParameters({Desc});
	}

	bool bDefaultApplied = false;
	if (bHasDefault)
	{
		EPropertyBagResult SetResult = EPropertyBagResult::PropertyNotFound;
		Graph->UpdateUserParametersStruct(
			[&ParamName, &DefaultValue, &SetResult](FInstancedPropertyBag& Bag)
			{
				SetResult = Bag.SetValueSerializedString(FName(*ParamName), DefaultValue);
			});

		if (SetResult != EPropertyBagResult::Success)
		{
			// The probe should prevent parse failure. Remove a new declaration explicitly; Cancel is not rollback.
			if (!bAlreadyExists)
			{
				Graph->UpdateUserParametersStruct(
					[&ParamName](FInstancedPropertyBag& Bag)
					{
						Bag.RemovePropertyByName(FName(*ParamName));
					});
			}
			Transaction.Cancel();
			return MakeErrorResult(FString::Printf(
				TEXT("default_value '%s' could not be applied as %s; parameter '%s' was not declared"),
				*DefaultValue, *TypeName, *ParamName));
		}
		bDefaultApplied = true;
	}

	Data->LastOperationStatus = FString::Printf(TEXT("%s graph parameter %s (%s)%s"),
		bAlreadyExists ? TEXT("Updated") : TEXT("Added"), *ParamName, *TypeName,
		bDefaultApplied ? TEXT(" with default") : TEXT(""));

	// Parameter-getter nodes bind by descriptor GUID, not name.
	FString PropertyGuidString;
	if (const FInstancedPropertyBag* Bag = Graph->GetUserParametersStruct())
	{
		if (const FPropertyBagPropertyDesc* Desc = Bag->FindPropertyDescByName(FName(*ParamName)))
		{
			PropertyGuidString = Desc->ID.ToString(EGuidFormats::Digits);
		}
	}

	if (PropertyGuidString.IsEmpty())
	{
		// An unreadable descriptor cannot supply a binding GUID. Remove new declarations before
		// cancelling; preserve parameters that existed before this call.
		if (!bAlreadyExists)
		{
			Graph->UpdateUserParametersStruct(
				[&ParamName](FInstancedPropertyBag& Bag)
				{
					Bag.RemovePropertyByName(FName(*ParamName));
				});
		}
		Transaction.Cancel();
		return MakeErrorResult(FString::Printf(
			TEXT("The graph's parameter bag returned no descriptor for '%s', so no PropertyGuid ")
			TEXT("could be read back; %s"),
			*ParamName,
			bAlreadyExists
				? TEXT("the pre-existing parameter was left unchanged")
				: TEXT("the new declaration was removed again and the parameter is not declared")));
	}

	Data->LastOperationStatus += FString::Printf(TEXT(" guid=%s"), *PropertyGuidString);

	FToolResult Result = BuildStateResponse(SessionId, Data);
	if (Result.Data.IsValid())
	{
		Result.Data->SetStringField(TEXT("parameter"), ParamName);
		Result.Data->SetStringField(TEXT("parameter_type"), TypeName);
		Result.Data->SetBoolField(TEXT("already_existed"), bAlreadyExists);
		Result.Data->SetBoolField(TEXT("default_applied"), bDefaultApplied);
		Result.Data->SetStringField(TEXT("property_guid"), PropertyGuidString);
	}
	return Result;
}
