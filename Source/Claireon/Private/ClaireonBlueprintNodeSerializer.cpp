// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonBlueprintNodeSerializer.h"
#include "ClaireonBlueprintHelpers.h"

#include "ClaireonLog.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "K2Node.h"
#include "K2Node_AddComponent.h"
#include "K2Node_BaseAsyncTask.h"
#include "K2Node_BaseMCDelegate.h"
#include "K2Node_CallFunction.h"
#include "Components/ActorComponent.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_Composite.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_StructOperation.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_Timeline.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableGet.h"
#include "Engine/TimelineTemplate.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveVector.h"
#include "GameplayTagsK2Node_SwitchGameplayTag.h"
#include "Engine/MemberReference.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/PropertyPortFlags.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace ClaireonBlueprintNodeSerializer_Private
{
	/** Convert EPinContainerType to the spec's lowercase string form. */
	FString ContainerTypeToString(EPinContainerType Container)
	{
		switch (Container)
		{
		case EPinContainerType::Array: return TEXT("array");
		case EPinContainerType::Set:   return TEXT("set");
		case EPinContainerType::Map:   return TEXT("map");
		case EPinContainerType::None:
		default:                        return TEXT("none");
		}
	}

	/** Split FullTitle on \n, trim each line, return first (title) and second (subtitle) lines. */
	void SplitFullTitleIntoTitleSubtitle(const FString& FullTitle, FString& OutTitle, FString& OutSubtitle)
	{
		TArray<FString> Lines;
		FullTitle.ParseIntoArray(Lines, TEXT("\n"), /*bCullEmpty=*/false);
		OutTitle = Lines.Num() > 0 ? Lines[0] : FString();
		OutSubtitle = Lines.Num() > 1 ? Lines[1] : FString();
		OutTitle.TrimStartAndEndInline();
		OutSubtitle.TrimStartAndEndInline();
	}

	/** Build the ListView title for a linked node, trimmed and with newlines replaced by spaces. */
	FString BuildLinkedNodeTitle(const UEdGraphNode* Node)
	{
		if (!IsValid(Node))
		{
			return FString();
		}
		FString Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
		Title.ReplaceInline(TEXT("\r\n"), TEXT(" "));
		Title.ReplaceInline(TEXT("\n"), TEXT(" "));
		Title.ReplaceInline(TEXT("\r"), TEXT(" "));
		Title.TrimStartAndEndInline();
		return Title;
	}

	/** Build the structured pin_type JSON object per the fracture spec. */
	TSharedPtr<FJsonObject> BuildPinTypeJson(const FEdGraphPinType& PinType)
	{
		TSharedPtr<FJsonObject> TypeObj = MakeShared<FJsonObject>();
		TypeObj->SetStringField(TEXT("category"), PinType.PinCategory.ToString());
		TypeObj->SetStringField(TEXT("sub_category"), PinType.PinSubCategory.ToString());

		FString SubCatObjPath;
		if (PinType.PinSubCategoryObject.IsValid())
		{
			SubCatObjPath = PinType.PinSubCategoryObject->GetPathName();
		}
		TypeObj->SetStringField(TEXT("sub_category_object"), SubCatObjPath);

		TypeObj->SetStringField(TEXT("container_type"), ContainerTypeToString(PinType.ContainerType));
		TypeObj->SetBoolField(TEXT("is_reference"), PinType.bIsReference);
		TypeObj->SetBoolField(TEXT("is_const"), PinType.bIsConst);
		return TypeObj;
	}
}
using namespace ClaireonBlueprintNodeSerializer_Private;

namespace ClaireonBlueprintNodeSerializer
{
	TSharedPtr<FJsonObject> SerializeNodeToJson(
		const UEdGraphNode* Node,
		bool bIncludeConnections,
		bool bIncludePinDefaults)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		if (!IsValid(Node))
		{
			return Root;
		}

		// Identity fields
		if (!Node->NodeGuid.IsValid())
		{
			const_cast<UEdGraphNode*>(Node)->CreateNewGuid();
			UE_LOG(LogClaireon, Warning,
				TEXT("[serializer] Node '%s' had invalid GUID; assigned %s."),
				*Node->GetNodeTitle(ENodeTitleType::ListView).ToString(),
				*Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		}
		Root->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		const FString NodeClassName = Node->GetClass()->GetName();
		Root->SetStringField(TEXT("node_class"), NodeClassName);
		Root->SetStringField(TEXT("class"), NodeClassName);

		// Title / subtitle
		const FString FullTitle = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
		FString Title;
		FString Subtitle;
		SplitFullTitleIntoTitleSubtitle(FullTitle, Title, Subtitle);
		Root->SetStringField(TEXT("node_title"), Title);
		Root->SetStringField(TEXT("title"), Title);
		if (!Subtitle.IsEmpty())
		{
			Root->SetStringField(TEXT("node_subtitle"), Subtitle);
		}

		// Position
		{
			TSharedPtr<FJsonObject> PosObj = MakeShared<FJsonObject>();
			PosObj->SetNumberField(TEXT("x"), static_cast<int32>(Node->NodePosX));
			PosObj->SetNumberField(TEXT("y"), static_cast<int32>(Node->NodePosY));
			Root->SetObjectField(TEXT("position"), PosObj);
		}

		// Per-class fields ------------------------------------------------------

		// function_reference (K2Node_CallFunction or K2Node_CallParentFunction)
		if (const UK2Node_CallFunction* CallFuncNode = Cast<UK2Node_CallFunction>(Node); IsValid(CallFuncNode))
		{
			TSharedPtr<FJsonObject> FuncRef = MakeShared<FJsonObject>();
			FuncRef->SetStringField(TEXT("member_name"),
				CallFuncNode->FunctionReference.GetMemberName().ToString());
			const UClass* FuncParent = CallFuncNode->FunctionReference.GetMemberParentClass();
			FuncRef->SetStringField(TEXT("member_parent"),
				IsValid(FuncParent) ? FuncParent->GetPathName() : FString());
			Root->SetObjectField(TEXT("function_reference"), FuncRef);
		}

		// variable_reference (K2Node_Variable and subclasses)
		if (const UK2Node_Variable* VarNode = Cast<UK2Node_Variable>(Node); IsValid(VarNode))
		{
			TSharedPtr<FJsonObject> VarRef = MakeShared<FJsonObject>();
			VarRef->SetStringField(TEXT("member_name"),
				VarNode->VariableReference.GetMemberName().ToString());
			const UClass* VarParent = VarNode->VariableReference.GetMemberParentClass();
			VarRef->SetStringField(TEXT("member_parent"),
				IsValid(VarParent) ? VarParent->GetPathName() : FString());
			Root->SetObjectField(TEXT("variable_reference"), VarRef);
		}

		// macro_reference (K2Node_MacroInstance)
		if (const UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(Node); IsValid(MacroNode))
		{
			TSharedPtr<FJsonObject> MacroRef = MakeShared<FJsonObject>();
			if (UEdGraph* MacroGraph = MacroNode->GetMacroGraph(); IsValid(MacroGraph))
			{
				MacroRef->SetStringField(TEXT("macro_graph_name"), MacroGraph->GetName());
				if (UBlueprint* MacroBP = FBlueprintEditorUtils::FindBlueprintForGraph(MacroGraph); IsValid(MacroBP))
				{
					MacroRef->SetStringField(TEXT("macro_blueprint"), MacroBP->GetPathName());
				}
				else
				{
					MacroRef->SetStringField(TEXT("macro_blueprint"), FString());
				}
			}
			else
			{
				MacroRef->SetStringField(TEXT("macro_graph_name"), FString());
				MacroRef->SetStringField(TEXT("macro_blueprint"), FString());
			}
			Root->SetObjectField(TEXT("macro_reference"), MacroRef);
		}

		// custom_event_name (K2Node_CustomEvent)
		if (const UK2Node_CustomEvent* CustomEventNode = Cast<UK2Node_CustomEvent>(Node); IsValid(CustomEventNode))
		{
			Root->SetStringField(TEXT("custom_event_name"),
				CustomEventNode->CustomFunctionName.ToString());
		}

		// target_class (any node whose owning Blueprint can be discovered via its graph).
		if (UEdGraph* OwningGraph = Node->GetGraph(); IsValid(OwningGraph))
		{
			if (UBlueprint* OwningBlueprint = FBlueprintEditorUtils::FindBlueprintForGraph(OwningGraph); IsValid(OwningBlueprint))
			{
				if (IsValid(OwningBlueprint->GeneratedClass))
				{
					Root->SetStringField(TEXT("target_class"),
						OwningBlueprint->GeneratedClass->GetPathName());
				}
			}
		}

		// Pins ------------------------------------------------------------------
		// Pre-compute duplicate-name counts so latent nodes like AwaitDelay
		// (UCancellableAsyncAction-backed) with multiple "then" pins emit
		// unique disambiguated names. We count per-direction so an
		// input "Target" and output "Target" do not collide. When a duplicate
		// is detected, prefer PinFriendlyName as the suffix (each
		// BlueprintAssignable delegate's exec pin carries the delegate display
		// name on the friendly-name field); fall back to an indexed [N] form
		// when no friendly name is available.
		TMap<TPair<FName, EEdGraphPinDirection>, int32> PinNameCounts;
		for (const UEdGraphPin* PinScan : Node->Pins)
		{
			if (!PinScan)
			{
				continue;
			}
			++PinNameCounts.FindOrAdd(MakeTuple(PinScan->PinName, PinScan->Direction));
		}
		TMap<TPair<FName, EEdGraphPinDirection>, int32> PinNameRunningIndex;

		auto SanitizeFriendlySuffix = [](const FString& Friendly) -> FString
		{
			FString Out;
			Out.Reserve(Friendly.Len());
			for (TCHAR Ch : Friendly)
			{
				if (FChar::IsAlnum(Ch) || Ch == TEXT('_'))
				{
					Out.AppendChar(Ch);
				}
			}
			return Out;
		};

		TArray<TSharedPtr<FJsonValue>> PinValues;
		PinValues.Reserve(Node->Pins.Num());
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin)
			{
				continue;
			}

			TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
			PinObj->SetStringField(TEXT("pin_id"),
				Pin->PinId.ToString(EGuidFormats::DigitsWithHyphens));

			const FString RawPinName = Pin->PinName.ToString();
			const TPair<FName, EEdGraphPinDirection> NameKey = MakeTuple(Pin->PinName, Pin->Direction);
			const int32 NameCount = PinNameCounts.FindRef(NameKey);
			FString DisplayPinName = RawPinName;
			if (NameCount > 1)
			{
				const FString FriendlyName = Pin->PinFriendlyName.ToString();
				const FString FriendlySuffix = SanitizeFriendlySuffix(FriendlyName);
				if (!FriendlySuffix.IsEmpty() && !FriendlySuffix.Equals(RawPinName, ESearchCase::IgnoreCase))
				{
					DisplayPinName = FString::Printf(TEXT("%s_%s"), *RawPinName, *FriendlySuffix);
				}
				else
				{
					const int32 Ordinal = PinNameRunningIndex.FindOrAdd(NameKey)++;
					DisplayPinName = FString::Printf(TEXT("%s[%d]"), *RawPinName, Ordinal);
				}
			}
			PinObj->SetStringField(TEXT("pin_name"), DisplayPinName);
			if (DisplayPinName != RawPinName)
			{
				PinObj->SetStringField(TEXT("pin_name_raw"), RawPinName);
				PinObj->SetBoolField(TEXT("pin_name_disambiguated"), true);
			}
			if (!Pin->PinFriendlyName.IsEmpty())
			{
				PinObj->SetStringField(TEXT("pin_friendly_name"), Pin->PinFriendlyName.ToString());
			}
			PinObj->SetStringField(TEXT("direction"),
				Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
			PinObj->SetObjectField(TEXT("pin_type"), BuildPinTypeJson(Pin->PinType));

			// Default values (only when requested and non-empty per field)
			if (bIncludePinDefaults)
			{
				// default_value (string form, truncated at 1024 bytes)
				if (!Pin->DefaultValue.IsEmpty())
				{
					FString Value = Pin->DefaultValue;
					bool bTruncated = false;
					if (Value.Len() > 1024)
					{
						Value.LeftInline(1024);
						bTruncated = true;
					}
					PinObj->SetStringField(TEXT("default_value"), Value);
					if (bTruncated)
					{
						PinObj->SetBoolField(TEXT("default_value_truncated"), true);
					}
				}
				if (Pin->DefaultObject)
				{
					PinObj->SetStringField(TEXT("default_object"), Pin->DefaultObject->GetPathName());
				}
				if (!Pin->DefaultTextValue.IsEmpty())
				{
					PinObj->SetStringField(TEXT("default_text"), Pin->DefaultTextValue.ToString());
				}
			}

			// linked_count always emitted
			PinObj->SetNumberField(TEXT("linked_count"), Pin->LinkedTo.Num());

			// linked_to (when connections requested)
			if (bIncludeConnections)
			{
				const int32 TotalLinks = Pin->LinkedTo.Num();
				const int32 EmitCount = FMath::Min(TotalLinks, 32);
				TArray<TSharedPtr<FJsonValue>> LinkedValues;
				LinkedValues.Reserve(EmitCount);
				for (int32 LinkIdx = 0; LinkIdx < EmitCount; ++LinkIdx)
				{
					const UEdGraphPin* LinkedPin = Pin->LinkedTo[LinkIdx];
					if (!LinkedPin)
					{
						continue;
					}
					TSharedPtr<FJsonObject> LinkObj = MakeShared<FJsonObject>();
					const UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();
					LinkObj->SetStringField(TEXT("node_guid"),
						IsValid(LinkedNode) ? LinkedNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens) : FString());
					const FString LinkedNodeTitleStr = BuildLinkedNodeTitle(LinkedNode);
					LinkObj->SetStringField(TEXT("node_title"), LinkedNodeTitleStr);
					LinkObj->SetStringField(TEXT("title"), LinkedNodeTitleStr);
					LinkObj->SetStringField(TEXT("pin_name"), LinkedPin->PinName.ToString());
					LinkObj->SetStringField(TEXT("pin_direction"),
						LinkedPin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
					LinkedValues.Add(MakeShared<FJsonValueObject>(LinkObj));
				}
				PinObj->SetArrayField(TEXT("linked_to"), LinkedValues);
				if (TotalLinks > 32)
				{
					PinObj->SetBoolField(TEXT("linked_to_truncated"), true);
					PinObj->SetNumberField(TEXT("linked_to_total"), TotalLinks);
				}
			}

			PinValues.Add(MakeShared<FJsonValueObject>(PinObj));
		}
		Root->SetArrayField(TEXT("pins"), PinValues);

		return Root;
	}

	FString SerializeNodeToString(
		const UEdGraphNode* Node,
		bool bIncludeConnections,
		bool bIncludePinDefaults)
	{
		TSharedPtr<FJsonObject> Root = SerializeNodeToJson(Node, bIncludeConnections, bIncludePinDefaults);
		FString Output;
		TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Output);
		FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
		return Output;
	}

	void AppendMemberReferenceFields(
		const UEdGraphNode* Node,
		const TSharedPtr<FJsonObject>& OutJson)
	{
		if (!IsValid(Node) || !OutJson.IsValid())
		{
			return;
		}

		auto BuildMemberRefJson = [](const FMemberReference& Ref) -> TSharedPtr<FJsonObject>
		{
			TSharedPtr<FJsonObject> RefObj = MakeShared<FJsonObject>();
			RefObj->SetStringField(TEXT("member_name"), Ref.GetMemberName().ToString());
			if (const UClass* ParentClass = Ref.GetMemberParentClass(); IsValid(ParentClass))
			{
				RefObj->SetStringField(TEXT("member_parent"), ParentClass->GetPathName());
			}
			if (Ref.IsLocalScope())
			{
				RefObj->SetStringField(TEXT("member_scope"), Ref.GetMemberScopeName());
			}
			RefObj->SetBoolField(TEXT("self_context"), Ref.IsSelfContext());
			if (Ref.GetMemberGuid().IsValid())
			{
				RefObj->SetStringField(TEXT("member_guid"), Ref.GetMemberGuid().ToString(EGuidFormats::DigitsWithHyphens));
			}
			return RefObj;
		};

		if (const UK2Node_CallFunction* CallFuncNode = Cast<UK2Node_CallFunction>(Node); IsValid(CallFuncNode))
		{
			OutJson->SetObjectField(TEXT("function_reference"), BuildMemberRefJson(CallFuncNode->FunctionReference));
		}

		if (const UK2Node_AddComponent* AddCompNode = Cast<UK2Node_AddComponent>(Node); IsValid(AddCompNode))
		{
			// The dynamic 'Add Component' node binds a component TEMPLATE stored
			// in the Blueprint's ComponentTemplates array. Without the template
			// class the node cannot author on a copy (add_node AddComponent
			// requires component_class) and every typed connection off its
			// ReturnValue pin is lost.
			if (UActorComponent* Template = AddCompNode->GetTemplateFromNode(); IsValid(Template))
			{
				OutJson->SetStringField(TEXT("component_class"), Template->GetClass()->GetPathName());
				OutJson->SetStringField(TEXT("template_name"), Template->GetName());

				// Class-CDO-diffed template values in ImportText form, replayable
				// via bp_set_cdo_property(property_path="<template_name>.<Field>").
				// Same skip set as cdo_fields_text: transient/deprecated, delegate
				// bindings, and owned-subobject pointers (unimportable paths).
				UObject* ClassDefault = Template->GetClass()->GetDefaultObject();
				TSharedPtr<FJsonObject> TemplateFields = MakeShared<FJsonObject>();
				for (TFieldIterator<FProperty> TemplPropIt(Template->GetClass()); TemplPropIt; ++TemplPropIt)
				{
					FProperty* TemplProp = *TemplPropIt;
					if (!TemplProp || TemplProp->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
					{
						continue;
					}
					if (TemplProp->IsA<FDelegateProperty>() || TemplProp->IsA<FMulticastDelegateProperty>())
					{
						continue;
					}
					if (FObjectProperty* TemplObjProp = CastField<FObjectProperty>(TemplProp))
					{
						UObject* PointedTo = TemplObjProp->GetObjectPropertyValue_InContainer(Template);
						if (IsValid(PointedTo) && (PointedTo->IsIn(Template) || PointedTo->IsIn(Template->GetOuter())))
						{
							continue;
						}
					}
					FString ValueText;
					TemplProp->ExportTextItem_Direct(ValueText,
						TemplProp->ContainerPtrToValuePtr<void>(Template), nullptr, Template, PPF_None);
					if (ValueText == TEXT("(INVALID)"))
					{
						continue;
					}
					FString DefaultText;
					TemplProp->ExportTextItem_Direct(DefaultText,
						TemplProp->ContainerPtrToValuePtr<void>(ClassDefault), nullptr, ClassDefault, PPF_None);
					if (DefaultText == ValueText)
					{
						continue;
					}
					TemplateFields->SetStringField(TemplProp->GetName(), ValueText);
				}
				if (TemplateFields->Values.Num() > 0)
				{
					OutJson->SetObjectField(TEXT("component_template_fields"), TemplateFields);
				}
			}
			else if (const UEdGraphPin* ReturnPin = AddCompNode->FindPin(TEXT("ReturnValue")))
			{
				// Template missing (source-drift orphan): fall back to the
				// ReturnValue pin's typed class so the copy at least types its
				// downstream connections.
				if (const UClass* PinClass = Cast<UClass>(ReturnPin->PinType.PinSubCategoryObject.Get()); IsValid(PinClass))
				{
					OutJson->SetStringField(TEXT("component_class"), PinClass->GetPathName());
				}
			}
		}

		if (const UK2Node_Variable* VarNode = Cast<UK2Node_Variable>(Node); IsValid(VarNode))
		{
			OutJson->SetObjectField(TEXT("variable_reference"), BuildMemberRefJson(VarNode->VariableReference));
			// Validated (impure) gets carry execute/then/else exec pins; replay
			// re-creates them via add_node VariableGet validated=true.
			if (const UK2Node_VariableGet* GetNode = Cast<UK2Node_VariableGet>(Node); IsValid(GetNode))
			{
				if (!GetNode->IsNodePure())
				{
					OutJson->SetBoolField(TEXT("validated_get"), true);
				}
			}
		}

		if (const UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(Node); IsValid(MacroNode))
		{
			TSharedPtr<FJsonObject> MacroRef = MakeShared<FJsonObject>();
			if (UEdGraph* MacroGraph = MacroNode->GetMacroGraph(); IsValid(MacroGraph))
			{
				MacroRef->SetStringField(TEXT("macro_graph_name"), MacroGraph->GetName());
				MacroRef->SetStringField(TEXT("macro_graph_path"), MacroGraph->GetPathName());
				if (UBlueprint* MacroBP = FBlueprintEditorUtils::FindBlueprintForGraph(MacroGraph); IsValid(MacroBP))
				{
					MacroRef->SetStringField(TEXT("macro_library"), MacroBP->GetPathName());
				}
			}
			OutJson->SetObjectField(TEXT("macro_reference"), MacroRef);
		}

		if (const UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node); IsValid(CastNode))
		{
			OutJson->SetStringField(TEXT("target_type"),
				IsValid(CastNode->TargetType) ? CastNode->TargetType->GetPathName() : FString());
		}

		if (Node->IsA<UK2Node_BaseAsyncTask>())
		{
			// Proxy fields are protected on UK2Node_BaseAsyncTask; read via reflection
			// (same idiom as the node factory's proxy guard).
			auto ReadObjProp = [Node](const TCHAR* PropName) -> UObject*
			{
				if (const FObjectPropertyBase* P = CastField<FObjectPropertyBase>(
					Node->GetClass()->FindPropertyByName(PropName)))
				{
					return P->GetObjectPropertyValue_InContainer(Node);
				}
				return nullptr;
			};
			auto ReadNameProp = [Node](const TCHAR* PropName) -> FName
			{
				if (const FNameProperty* P = CastField<FNameProperty>(
					Node->GetClass()->FindPropertyByName(PropName)))
				{
					return P->GetPropertyValue_InContainer(Node);
				}
				return NAME_None;
			};
			if (UObject* PFC = ReadObjProp(TEXT("ProxyFactoryClass")); IsValid(PFC))
			{
				OutJson->SetStringField(TEXT("proxy_factory_class"), PFC->GetPathName());
			}
			if (UObject* PC = ReadObjProp(TEXT("ProxyClass")); IsValid(PC))
			{
				OutJson->SetStringField(TEXT("proxy_class"), PC->GetPathName());
			}
			const FName PFF = ReadNameProp(TEXT("ProxyFactoryFunctionName"));
			if (!PFF.IsNone())
			{
				OutJson->SetStringField(TEXT("proxy_factory_function_name"), PFF.ToString());
			}
		}

		if (const UK2Node_BaseMCDelegate* DelegateNode = Cast<UK2Node_BaseMCDelegate>(Node); IsValid(DelegateNode))
		{
			OutJson->SetObjectField(TEXT("delegate_reference"), BuildMemberRefJson(DelegateNode->DelegateReference));
		}

		if (const UK2Node_CreateDelegate* CreateDelegateNode = Cast<UK2Node_CreateDelegate>(Node); IsValid(CreateDelegateNode))
		{
			// The bound function IS the node's identity; without it a replayed
			// Create Event node cannot author (add_node requires function_name)
			// and every connection through its OutputDelegate pin is lost.
			OutJson->SetStringField(TEXT("function_name"), CreateDelegateNode->GetFunctionName().ToString());
		}

		if (const UK2Node_ComponentBoundEvent* BoundEventNode = Cast<UK2Node_ComponentBoundEvent>(Node); IsValid(BoundEventNode))
		{
			OutJson->SetStringField(TEXT("component_name"), BoundEventNode->ComponentPropertyName.ToString());
			OutJson->SetStringField(TEXT("delegate_name"), BoundEventNode->DelegatePropertyName.ToString());
		}
		else if (const UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node); IsValid(EventNode))
		{
			// CustomEvent inherits from Event; its identity is the custom name.
			if (const UK2Node_CustomEvent* CustomEventNode = Cast<UK2Node_CustomEvent>(Node); IsValid(CustomEventNode))
			{
				OutJson->SetStringField(TEXT("custom_event_name"), CustomEventNode->CustomFunctionName.ToString());
			}
			else
			{
				OutJson->SetObjectField(TEXT("event_reference"), BuildMemberRefJson(EventNode->EventReference));
				OutJson->SetBoolField(TEXT("is_override"), EventNode->bOverrideFunction);
			}
		}

		if (const UK2Node_StructOperation* StructNode = Cast<UK2Node_StructOperation>(Node); IsValid(StructNode))
		{
			OutJson->SetStringField(TEXT("struct_type"),
				StructNode->StructType ? StructNode->StructType->GetPathName() : FString());
		}

		if (const UK2Node_SwitchEnum* SwitchEnumNode = Cast<UK2Node_SwitchEnum>(Node); IsValid(SwitchEnumNode))
		{
			OutJson->SetStringField(TEXT("enum_type"),
				SwitchEnumNode->Enum ? SwitchEnumNode->Enum->GetPathName() : FString());
		}

		if (const UGameplayTagsK2Node_SwitchGameplayTag* SwitchTagNode = Cast<UGameplayTagsK2Node_SwitchGameplayTag>(Node); IsValid(SwitchTagNode))
		{
			TArray<TSharedPtr<FJsonValue>> TagValues;
			for (const FGameplayTag& Tag : SwitchTagNode->PinTags)
			{
				TagValues.Add(MakeShared<FJsonValueString>(Tag.ToString()));
			}
			OutJson->SetArrayField(TEXT("tags"), TagValues);
		}

		if (const UK2Node_Timeline* TimelineNode = Cast<UK2Node_Timeline>(Node); IsValid(TimelineNode))
		{
			OutJson->SetStringField(TEXT("timeline_name"), TimelineNode->TimelineName.ToString());

			// Emit the backing template's tracks/keys so a replay can author them via
			// add_node's float_tracks/vector_tracks/event_tracks (or bp_timeline_add_track).
			const UBlueprint* OwningBP = TimelineNode->HasValidBlueprint() ? TimelineNode->GetBlueprint() : nullptr;
			const UTimelineTemplate* Template = IsValid(OwningBP)
				? OwningBP->FindTimelineTemplateByVariableName(TimelineNode->TimelineName)
				: nullptr;
			if (IsValid(Template))
			{
				auto InterpModeToString = [](ERichCurveInterpMode Mode) -> FString
				{
					switch (Mode)
					{
					case RCIM_Constant: return TEXT("constant");
					case RCIM_Cubic:    return TEXT("cubic");
					case RCIM_Linear:
					default:            return TEXT("linear");
					}
				};
				auto BuildFloatCurveKeys = [&InterpModeToString](const FRichCurve& Curve, FString& OutTrackInterp)
				{
					TArray<TSharedPtr<FJsonValue>> Keys;
					bool bFirst = true;
					for (const FRichCurveKey& Key : Curve.GetConstRefOfKeys())
					{
						if (bFirst)
						{
							OutTrackInterp = InterpModeToString(Key.InterpMode);
							bFirst = false;
						}
						TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
						KeyObj->SetNumberField(TEXT("time"), Key.Time);
						KeyObj->SetNumberField(TEXT("value"), Key.Value);
						KeyObj->SetStringField(TEXT("interp"), InterpModeToString(Key.InterpMode));
						Keys.Add(MakeShared<FJsonValueObject>(KeyObj));
					}
					return Keys;
				};

				TSharedPtr<FJsonObject> TimelineObj = MakeShared<FJsonObject>();
				TimelineObj->SetBoolField(TEXT("autoplay"), Template->bAutoPlay);
				TimelineObj->SetBoolField(TEXT("loop"), Template->bLoop);
				TimelineObj->SetBoolField(TEXT("replicated"), Template->bReplicated);
				TimelineObj->SetBoolField(TEXT("ignore_time_dilation"), Template->bIgnoreTimeDilation);
				TimelineObj->SetNumberField(TEXT("length"), Template->TimelineLength);

				TArray<TSharedPtr<FJsonValue>> FloatTracks;
				for (const FTTFloatTrack& Track : Template->FloatTracks)
				{
					TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
					TrackObj->SetStringField(TEXT("track_name"), Track.GetTrackName().ToString());
					FString TrackInterp = TEXT("linear");
					if (Track.CurveFloat)
					{
						TrackObj->SetArrayField(TEXT("keys"), BuildFloatCurveKeys(Track.CurveFloat->FloatCurve, TrackInterp));
					}
					TrackObj->SetStringField(TEXT("interpolation"), TrackInterp);
					FloatTracks.Add(MakeShared<FJsonValueObject>(TrackObj));
				}
				TimelineObj->SetArrayField(TEXT("float_tracks"), FloatTracks);

				TArray<TSharedPtr<FJsonValue>> VectorTracks;
				for (const FTTVectorTrack& Track : Template->VectorTracks)
				{
					TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
					TrackObj->SetStringField(TEXT("track_name"), Track.GetTrackName().ToString());
					FString TrackInterp = TEXT("linear");
					if (Track.CurveVector)
					{
						// Merge the three axis curves into {time,x,y,z} keys on the union of key times.
						TSet<float> KeyTimes;
						for (int32 Axis = 0; Axis < 3; ++Axis)
						{
							for (const FRichCurveKey& Key : Track.CurveVector->FloatCurves[Axis].GetConstRefOfKeys())
							{
								KeyTimes.Add(Key.Time);
								TrackInterp = InterpModeToString(Key.InterpMode);
							}
						}
						TArray<float> SortedTimes = KeyTimes.Array();
						SortedTimes.Sort();
						TArray<TSharedPtr<FJsonValue>> Keys;
						for (float Time : SortedTimes)
						{
							TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
							KeyObj->SetNumberField(TEXT("time"), Time);
							KeyObj->SetNumberField(TEXT("x"), Track.CurveVector->FloatCurves[0].Eval(Time));
							KeyObj->SetNumberField(TEXT("y"), Track.CurveVector->FloatCurves[1].Eval(Time));
							KeyObj->SetNumberField(TEXT("z"), Track.CurveVector->FloatCurves[2].Eval(Time));
							Keys.Add(MakeShared<FJsonValueObject>(KeyObj));
						}
						TrackObj->SetArrayField(TEXT("keys"), Keys);
					}
					TrackObj->SetStringField(TEXT("interpolation"), TrackInterp);
					VectorTracks.Add(MakeShared<FJsonValueObject>(TrackObj));
				}
				TimelineObj->SetArrayField(TEXT("vector_tracks"), VectorTracks);

				TArray<TSharedPtr<FJsonValue>> EventTracks;
				for (const FTTEventTrack& Track : Template->EventTracks)
				{
					TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
					TrackObj->SetStringField(TEXT("track_name"), Track.GetTrackName().ToString());
					TArray<TSharedPtr<FJsonValue>> Keys;
					if (Track.CurveKeys)
					{
						for (const FRichCurveKey& Key : Track.CurveKeys->FloatCurve.GetConstRefOfKeys())
						{
							TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
							KeyObj->SetNumberField(TEXT("time"), Key.Time);
							Keys.Add(MakeShared<FJsonValueObject>(KeyObj));
						}
					}
					TrackObj->SetArrayField(TEXT("keys"), Keys);
					EventTracks.Add(MakeShared<FJsonValueObject>(TrackObj));
				}
				TimelineObj->SetArrayField(TEXT("event_tracks"), EventTracks);

				OutJson->SetObjectField(TEXT("timeline"), TimelineObj);
			}
		}

		if (const UK2Node_Composite* CompositeNode = Cast<UK2Node_Composite>(Node); IsValid(CompositeNode))
		{
			OutJson->SetStringField(TEXT("bound_graph"),
				CompositeNode->BoundGraph ? CompositeNode->BoundGraph->GetName() : FString());
		}

		// Input event node identity, read via reflection (the node classes live in
		// the EnhancedInput plugin; K2Node_InputDebugKey's header is private):
		//   K2Node_EnhancedInputAction / K2Node_GetInputActionValue -> input_action (asset path)
		//   K2Node_InputDebugKey / K2Node_InputKey                  -> key (FKey name)
		// These round-trip through bp_add_node's input_action/key params.
		{
			if (const FObjectPropertyBase* ActionProp = CastField<FObjectPropertyBase>(
				Node->GetClass()->FindPropertyByName(TEXT("InputAction"))))
			{
				const UObject* ActionAsset = ActionProp->GetObjectPropertyValue_InContainer(Node);
				OutJson->SetStringField(TEXT("input_action"),
					IsValid(ActionAsset) ? ActionAsset->GetPathName() : FString());
			}
			if (const FStructProperty* KeyProp = CastField<FStructProperty>(
				Node->GetClass()->FindPropertyByName(TEXT("InputKey"))))
			{
				if (KeyProp->Struct && KeyProp->Struct->GetFName() == TEXT("Key"))
				{
					FString KeyText;
					KeyProp->ExportTextItem_InContainer(KeyText, Node, nullptr, nullptr, PPF_None);
					OutJson->SetStringField(TEXT("key"), KeyText);
				}
			}
		}

		// Generic-path nodes (no add_node alias): their replayable identity lives
		// in UPROPERTYs declared on the node subclass (e.g. K2Node_EvaluateProxy2's
		// proxy asset drives its params/result pins). Emit CDO-diffed values as a
		// node_properties bag for add_node's Generic path, which applies them
		// before AllocateDefaultPins.
		if (ClaireonBlueprintHelpers::GetNodeTypeAliasForClass(Node->GetClass()).IsEmpty())
		{
			TSharedPtr<FJsonObject> NodeProps = MakeShared<FJsonObject>();
			const UObject* NodeCDO = Node->GetClass()->GetDefaultObject(/*bCreateIfNeeded=*/false);
			for (TFieldIterator<FProperty> It(Node->GetClass()); It; ++It)
			{
				FProperty* Prop = *It;
				if (!Prop || Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_DuplicateTransient))
				{
					continue;
				}
				// Skip bookkeeping declared on UEdGraphNode/UK2Node themselves
				// (position, pins, comment state) -- not node identity.
				const UClass* OwnerClass = Prop->GetOwnerClass();
				if (!IsValid(OwnerClass) || OwnerClass == UK2Node::StaticClass() || UK2Node::StaticClass()->IsChildOf(OwnerClass))
				{
					continue;
				}
				FString ValueText;
				Prop->ExportTextItem_InContainer(ValueText, Node, nullptr, nullptr, PPF_None);
				if (IsValid(NodeCDO))
				{
					FString DefaultText;
					Prop->ExportTextItem_InContainer(DefaultText, NodeCDO, nullptr, nullptr, PPF_None);
					if (DefaultText == ValueText)
					{
						continue;
					}
				}
				NodeProps->SetStringField(Prop->GetName(), ValueText);
			}
			if (NodeProps->Values.Num() > 0)
			{
				OutJson->SetObjectField(TEXT("node_properties"), NodeProps);
			}
		}
	}
}
