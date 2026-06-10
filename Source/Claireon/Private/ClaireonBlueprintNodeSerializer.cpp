// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonBlueprintNodeSerializer.h"

#include "ClaireonLog.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Variable.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
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
		if (!Node)
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

namespace ClaireonBlueprintNodeSerializer
{
	TSharedPtr<FJsonObject> SerializeNodeToJson(
		const UEdGraphNode* Node,
		bool bIncludeConnections,
		bool bIncludePinDefaults)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		if (!Node)
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
		if (const UK2Node_CallFunction* CallFuncNode = Cast<UK2Node_CallFunction>(Node))
		{
			TSharedPtr<FJsonObject> FuncRef = MakeShared<FJsonObject>();
			FuncRef->SetStringField(TEXT("member_name"),
				CallFuncNode->FunctionReference.GetMemberName().ToString());
			const UClass* FuncParent = CallFuncNode->FunctionReference.GetMemberParentClass();
			FuncRef->SetStringField(TEXT("member_parent"),
				FuncParent ? FuncParent->GetPathName() : FString());
			Root->SetObjectField(TEXT("function_reference"), FuncRef);
		}

		// variable_reference (K2Node_Variable and subclasses)
		if (const UK2Node_Variable* VarNode = Cast<UK2Node_Variable>(Node))
		{
			TSharedPtr<FJsonObject> VarRef = MakeShared<FJsonObject>();
			VarRef->SetStringField(TEXT("member_name"),
				VarNode->VariableReference.GetMemberName().ToString());
			const UClass* VarParent = VarNode->VariableReference.GetMemberParentClass();
			VarRef->SetStringField(TEXT("member_parent"),
				VarParent ? VarParent->GetPathName() : FString());
			Root->SetObjectField(TEXT("variable_reference"), VarRef);
		}

		// macro_reference (K2Node_MacroInstance)
		if (const UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(Node))
		{
			TSharedPtr<FJsonObject> MacroRef = MakeShared<FJsonObject>();
			if (UEdGraph* MacroGraph = MacroNode->GetMacroGraph())
			{
				MacroRef->SetStringField(TEXT("macro_graph_name"), MacroGraph->GetName());
				if (UBlueprint* MacroBP = FBlueprintEditorUtils::FindBlueprintForGraph(MacroGraph))
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
		if (const UK2Node_CustomEvent* CustomEventNode = Cast<UK2Node_CustomEvent>(Node))
		{
			Root->SetStringField(TEXT("custom_event_name"),
				CustomEventNode->CustomFunctionName.ToString());
		}

		// target_class (any node whose owning Blueprint can be discovered via its graph).
		if (UEdGraph* OwningGraph = Node->GetGraph())
		{
			if (UBlueprint* OwningBlueprint = FBlueprintEditorUtils::FindBlueprintForGraph(OwningGraph))
			{
				if (OwningBlueprint->GeneratedClass)
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
						LinkedNode ? LinkedNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens) : FString());
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
}
