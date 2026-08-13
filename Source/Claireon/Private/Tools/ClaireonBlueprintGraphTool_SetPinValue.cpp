// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_SetPinValue.h"
#include "AssetRegistry/AssetData.h"
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
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"

#define LOCTEXT_NAMESPACE "ClaireonBlueprintGraphEditToolBase"

using FToolResult = IClaireonTool::FToolResult;

// Named (not anonymous) so unity batching cannot collide these with same-shaped
// helpers in a sibling translation unit.
namespace ClaireonSetPinValue_WildcardInternal
{
	/** Optional sign followed by at least one digit, digits only. */
	static bool IsIntegerLiteral(const FString& V)
	{
		const int32 Start = (V.Len() > 0 && V[0] == TEXT('-')) ? 1 : 0;
		if (V.Len() <= Start) { return false; }
		for (int32 I = Start; I < V.Len(); ++I)
		{
			if (!FChar::IsDigit(V[I])) { return false; }
		}
		return true;
	}

	/**
	 * Decimal-point or exponent real forms: "1.5", "-.5", "2.", "1e5", "1.5e-3".
	 * Integer-only strings are deliberately rejected so they route to PC_Int/PC_Int64.
	 */
	static bool IsRealLiteral(const FString& V)
	{
		int32 I = 0;
		if (I < V.Len() && (V[I] == TEXT('-') || V[I] == TEXT('+'))) { ++I; }

		int32 IntDigits = 0;
		while (I < V.Len() && FChar::IsDigit(V[I])) { ++I; ++IntDigits; }

		int32 FracDigits = 0;
		bool bHasDot = false;
		if (I < V.Len() && V[I] == TEXT('.'))
		{
			bHasDot = true;
			++I;
			while (I < V.Len() && FChar::IsDigit(V[I])) { ++I; ++FracDigits; }
		}
		if (IntDigits == 0 && FracDigits == 0) { return false; }

		bool bHasExp = false;
		if (I < V.Len() && (V[I] == TEXT('e') || V[I] == TEXT('E')))
		{
			bHasExp = true;
			++I;
			if (I < V.Len() && (V[I] == TEXT('-') || V[I] == TEXT('+'))) { ++I; }
			int32 ExpDigits = 0;
			while (I < V.Len() && FChar::IsDigit(V[I])) { ++I; ++ExpDigits; }
			if (ExpDigits == 0) { return false; }
		}

		// Fully consumed, and distinguishable from a plain integer.
		return I == V.Len() && (bHasDot || bHasExp);
	}

	/**
	 * Infer a concrete pin type from a literal string, per the B-1 inference table.
	 * Precedence: bool -> int32 -> int64 -> real(double) -> object path -> string.
	 * An object-path-shaped literal that names no real asset falls back to string and
	 * reports the ambiguity through OutWarning rather than failing.
	 */
	static FEdGraphPinType InferPinTypeFromLiteral(const FString& Value, FString& OutWarning)
	{
		FEdGraphPinType Type;
		Type.ContainerType = EPinContainerType::None;

		if (Value.Equals(TEXT("true"), ESearchCase::IgnoreCase)
			|| Value.Equals(TEXT("false"), ESearchCase::IgnoreCase))
		{
			Type.PinCategory = UEdGraphSchema_K2::PC_Boolean;
			return Type;
		}

		if (IsIntegerLiteral(Value))
		{
			const int64 AsInt64 = FCString::Atoi64(*Value);
			const bool bFitsInt32 = AsInt64 >= static_cast<int64>(MIN_int32)
				&& AsInt64 <= static_cast<int64>(MAX_int32);
			Type.PinCategory = bFitsInt32 ? UEdGraphSchema_K2::PC_Int : UEdGraphSchema_K2::PC_Int64;
			return Type;
		}

		if (IsRealLiteral(Value))
		{
			// Blueprint's modern real-literal default is double, not float.
			Type.PinCategory = UEdGraphSchema_K2::PC_Real;
			Type.PinSubCategory = UEdGraphSchema_K2::PC_Double;
			return Type;
		}

		if (Value.StartsWith(TEXT("/Game/")) || Value.StartsWith(TEXT("/Script/")))
		{
			// Accept the dotless package-path shorthand the object branch below also takes.
			const FString ObjectPath = Value.Contains(TEXT("."))
				? Value
				: Value + TEXT(".") + FPackageName::GetShortName(Value);

			const FAssetRegistryModule& AssetRegistryModule =
				FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
			const FAssetData AssetData =
				AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
			if (UClass* AssetClass = AssetData.IsValid() ? AssetData.GetClass() : nullptr; IsValid(AssetClass))
			{
				Type.PinCategory = UEdGraphSchema_K2::PC_Object;
				Type.PinSubCategoryObject = AssetClass;
				return Type;
			}

			OutWarning = FString::Printf(
				TEXT("'%s' looked like an object path but no asset was found there; promoted to string instead"),
				*Value);
		}

		Type.PinCategory = UEdGraphSchema_K2::PC_String;
		return Type;
	}

	/** Human-readable type name for the promotion warning. */
	static FString DescribePinType(const FEdGraphPinType& Type)
	{
		if (Type.PinCategory == UEdGraphSchema_K2::PC_Real)
		{
			return Type.PinSubCategory.ToString();
		}
		if (Type.PinCategory == UEdGraphSchema_K2::PC_Object && Type.PinSubCategoryObject.IsValid())
		{
			return FString::Printf(TEXT("object<%s>"), *Type.PinSubCategoryObject->GetName());
		}
		return Type.PinCategory.ToString();
	}
} // namespace ClaireonSetPinValue_WildcardInternal

// Named (not anonymous) so unity batching cannot collide these with same-shaped
// helpers in a sibling translation unit.
namespace ClaireonSetPinValue_SplitInternal
{
	/**
	 * Split Body on top-level commas: commas inside nested parentheses or
	 * double-quoted runs do not split. Backslash escapes the next character
	 * inside quotes.
	 */
	static TArray<FString> SplitTopLevel(const FString& Body)
	{
		TArray<FString> Parts;
		int32 Depth = 0;
		bool bInQuotes = false;
		FString Current;
		for (int32 I = 0; I < Body.Len(); ++I)
		{
			const TCHAR C = Body[I];
			if (bInQuotes)
			{
				Current.AppendChar(C);
				if (C == TEXT('\\') && I + 1 < Body.Len())
				{
					Current.AppendChar(Body[++I]);
				}
				else if (C == TEXT('"'))
				{
					bInQuotes = false;
				}
				continue;
			}
			switch (C)
			{
			case TEXT('"'): bInQuotes = true; Current.AppendChar(C); break;
			case TEXT('('): ++Depth; Current.AppendChar(C); break;
			case TEXT(')'): --Depth; Current.AppendChar(C); break;
			case TEXT(','):
				if (Depth == 0)
				{
					Parts.Add(Current.TrimStartAndEnd());
					Current.Reset();
				}
				else
				{
					Current.AppendChar(C);
				}
				break;
			default: Current.AppendChar(C); break;
			}
		}
		Parts.Add(Current.TrimStartAndEnd());
		return Parts;
	}

	/**
	 * Parse the named struct-literal form "(A=V,B=(...),C=V)" into ordered
	 * key/value pairs. Returns false when the literal is not in that form
	 * (no outer parens, or any top-level segment lacks a '='); the caller
	 * then tries the bare positional form.
	 */
	static bool TryParseNamedPairs(const FString& InLiteral, TArray<TPair<FString, FString>>& OutPairs)
	{
		FString S = InLiteral.TrimStartAndEnd();
		if (S.Len() < 2 || !S.StartsWith(TEXT("(")) || !S.EndsWith(TEXT(")")))
		{
			return false;
		}
		S.MidInline(1, S.Len() - 2);

		const TArray<FString> Segments = SplitTopLevel(S);
		for (const FString& Segment : Segments)
		{
			int32 EqIdx = INDEX_NONE;
			int32 Depth = 0;
			for (int32 I = 0; I < Segment.Len(); ++I)
			{
				const TCHAR C = Segment[I];
				if (C == TEXT('(')) { ++Depth; }
				else if (C == TEXT(')')) { --Depth; }
				else if (C == TEXT('=') && Depth == 0) { EqIdx = I; break; }
			}
			if (EqIdx <= 0)
			{
				return false;
			}
			const FString Key = Segment.Left(EqIdx).TrimStartAndEnd();
			const FString Val = Segment.Mid(EqIdx + 1).TrimStartAndEnd();
			if (Key.IsEmpty())
			{
				return false;
			}
			OutPairs.Emplace(Key, Val);
		}
		return OutPairs.Num() > 0;
	}

	/**
	 * Member order the engine's own bare-string parser assigns to a comma
	 * list on an UNSPLIT pin of this struct type (FDefaultValueHelper):
	 * distributing a bare literal by this order keeps "20,40,0.05" meaning
	 * the same thing whether the pin is split or not. Note FRotator's bare
	 * order is Pitch,Yaw,Roll -- NOT the Roll,Pitch,Yaw order its sub-pins
	 * appear in. Empty result = no defined bare form for this struct.
	 */
	static TArray<FString> BareFormMemberOrder(const UScriptStruct* StructType)
	{
		if (StructType == TBaseStructure<FVector>::Get() || StructType == TVariantStructure<FVector3f>::Get())
		{
			return {TEXT("X"), TEXT("Y"), TEXT("Z")};
		}
		if (StructType == TBaseStructure<FVector2D>::Get())
		{
			return {TEXT("X"), TEXT("Y")};
		}
		if (StructType == TBaseStructure<FVector4>::Get())
		{
			return {TEXT("X"), TEXT("Y"), TEXT("Z"), TEXT("W")};
		}
		if (StructType == TBaseStructure<FRotator>::Get())
		{
			return {TEXT("Pitch"), TEXT("Yaw"), TEXT("Roll")};
		}
		if (StructType == TBaseStructure<FLinearColor>::Get())
		{
			return {TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A")};
		}
		return {};
	}

	/** Sub-pins are named "<Parent>_<Member>" (UEdGraphSchema_K2::SplitPin). */
	static UEdGraphPin* FindSubPinByMember(UEdGraphPin* Parent, const FString& Member)
	{
		const FString Expected = Parent->PinName.ToString() + TEXT("_") + Member;
		for (UEdGraphPin* Sub : Parent->SubPins)
		{
			if (Sub && Sub->PinName.ToString().Equals(Expected, ESearchCase::IgnoreCase))
			{
				return Sub;
			}
		}
		return nullptr;
	}

	/** Member names of Parent's sub-pins, prefix stripped, for error text. */
	static FString DescribeAvailableMembers(UEdGraphPin* Parent)
	{
		const FString Prefix = Parent->PinName.ToString() + TEXT("_");
		TArray<FString> Members;
		for (UEdGraphPin* Sub : Parent->SubPins)
		{
			if (!Sub) { continue; }
			FString Name = Sub->PinName.ToString();
			if (Name.StartsWith(Prefix, ESearchCase::IgnoreCase))
			{
				Name.RightChopInline(Prefix.Len());
			}
			Members.Add(Name);
		}
		return FString::Join(Members, TEXT(", "));
	}

	/**
	 * Validate (bApply=false) or validate-and-write (bApply=true) one concrete
	 * leaf sub-pin's default. Returns an error message, or empty on success.
	 * Mirrors the main path's object-like / literal split so the distribution
	 * pass rejects exactly what a direct sub-pin write would reject.
	 */
	static FString ValidateOrWriteLeafDefault(const UEdGraphSchema_K2* K2Schema, UEdGraphPin* Pin,
		const FString& Value, bool bApply)
	{
		const FName PinCategory = Pin->PinType.PinCategory;
		const bool bObjectLikePin =
			   PinCategory == UEdGraphSchema_K2::PC_Object
			|| PinCategory == UEdGraphSchema_K2::PC_Class
			|| PinCategory == UEdGraphSchema_K2::PC_SoftObject
			|| PinCategory == UEdGraphSchema_K2::PC_SoftClass
			|| PinCategory == UEdGraphSchema_K2::PC_Interface;

		if (bObjectLikePin)
		{
			if (Value.IsEmpty() || Value == TEXT("None"))
			{
				if (bApply) { K2Schema->TrySetDefaultObject(*Pin, nullptr); }
				return FString();
			}
			UObject* Resolved = LoadObject<UObject>(nullptr, *Value);
			if (!IsValid(Resolved) && !Value.Contains(TEXT(".")))
			{
				const FString ObjectPath = Value + TEXT(".") + FPackageName::GetShortName(Value);
				Resolved = LoadObject<UObject>(nullptr, *ObjectPath);
			}
			if (!IsValid(Resolved))
			{
				return FString::Printf(
					TEXT("Could not resolve '%s' for %s sub-pin '%s' (pass a full object path like /Game/Dir/Asset.Asset)"),
					*Value, *PinCategory.ToString(), *Pin->PinName.ToString());
			}
			const FString ValidationError = K2Schema->IsPinDefaultValid(Pin, FString(), Resolved, FText());
			if (!ValidationError.IsEmpty())
			{
				return FString::Printf(TEXT("'%s' is not a valid default for sub-pin '%s': %s"),
					*Value, *Pin->PinName.ToString(), *ValidationError);
			}
			if (bApply) { K2Schema->TrySetDefaultObject(*Pin, Resolved); }
			return FString();
		}

		FString UseDefaultValue;
		TObjectPtr<UObject> UseDefaultObject = nullptr;
		FText UseDefaultText;
		K2Schema->GetPinDefaultValuesFromString(Pin->PinType, Pin->GetOwningNodeUnchecked(), Value,
			UseDefaultValue, UseDefaultObject, UseDefaultText, /*bPreserveTextIdentity*/false);
		const FString ValidationError = K2Schema->IsPinDefaultValid(Pin, UseDefaultValue, UseDefaultObject, UseDefaultText);
		if (!ValidationError.IsEmpty())
		{
			return FString::Printf(TEXT("'%s' is not a valid default for sub-pin '%s': %s"),
				*Value, *Pin->PinName.ToString(), *ValidationError);
		}
		if (bApply) { K2Schema->TrySetDefaultValue(*Pin, Value); }
		return FString();
	}

	/**
	 * Recursively map a struct literal onto Pin's sub-pins, validating every
	 * leaf value. Appends (leaf pin, value) pairs to OutWrites; nothing is
	 * written here, so a validation failure anywhere leaves the graph
	 * untouched. Returns false with OutError set on any failure.
	 */
	static bool CollectSplitPinWrites(const UEdGraphSchema_K2* K2Schema, UEdGraphPin* Pin, const FString& Value,
		TArray<TPair<UEdGraphPin*, FString>>& OutWrites, TArray<FString>& OutNotes, FString& OutError)
	{
		if (Pin->SubPins.Num() == 0)
		{
			if (Pin->LinkedTo.Num() > 0)
			{
				OutError = FString::Printf(
					TEXT("Cannot set default value on connected sub-pin '%s'; disconnect it first via bp_disconnect_pin"),
					*Pin->PinName.ToString());
				return false;
			}
			OutError = ValidateOrWriteLeafDefault(K2Schema, Pin, Value, /*bApply*/false);
			if (!OutError.IsEmpty())
			{
				return false;
			}
			OutWrites.Emplace(Pin, Value);
			return true;
		}

		// Parse the literal into member -> value pairs: named form first, then
		// the bare comma form in the struct's own parse order.
		TArray<TPair<FString, FString>> Pairs;
		if (!TryParseNamedPairs(Value, Pairs))
		{
			const UScriptStruct* StructType = Cast<UScriptStruct>(Pin->PinType.PinSubCategoryObject.Get());
			const TArray<FString> Order = BareFormMemberOrder(StructType);
			FString Body = Value.TrimStartAndEnd();
			if (Body.Len() >= 2 && Body.StartsWith(TEXT("(")) && Body.EndsWith(TEXT(")")))
			{
				Body.MidInline(1, Body.Len() - 2);
			}
			const TArray<FString> Parts = SplitTopLevel(Body);
			if (Order.Num() == 0)
			{
				OutError = FString::Printf(
					TEXT("Pin '%s' is split into sub-pins and '%s' is not a named struct literal. ")
					TEXT("Use the named form, e.g. (%s=...), or set the sub-pins directly."),
					*Pin->PinName.ToString(), *Value, *DescribeAvailableMembers(Pin));
				return false;
			}
			if (Parts.Num() == 0 || (Parts.Num() == 1 && Parts[0].IsEmpty()) || Parts.Num() > Order.Num())
			{
				OutError = FString::Printf(
					TEXT("Split pin '%s': bare literal '%s' has %d component(s); expected 1-%d in %s order. ")
					TEXT("Use the named form (e.g. (%s=...)) to set specific members."),
					*Pin->PinName.ToString(), *Value, Parts.Num(), Order.Num(),
					*FString::Join(Order, TEXT(",")), *Order[0]);
				return false;
			}
			for (int32 I = 0; I < Parts.Num(); ++I)
			{
				Pairs.Emplace(Order[I], Parts[I]);
			}
		}

		TArray<FString> Untouched;
		{
			TSet<FString> NamedLower;
			for (const TPair<FString, FString>& Pair : Pairs)
			{
				NamedLower.Add(Pair.Key.ToLower());
			}
			const FString Prefix = Pin->PinName.ToString() + TEXT("_");
			for (UEdGraphPin* Sub : Pin->SubPins)
			{
				if (!Sub) { continue; }
				FString Member = Sub->PinName.ToString();
				if (Member.StartsWith(Prefix, ESearchCase::IgnoreCase))
				{
					Member.RightChopInline(Prefix.Len());
				}
				if (!NamedLower.Contains(Member.ToLower()))
				{
					Untouched.Add(Member);
				}
			}
		}

		for (const TPair<FString, FString>& Pair : Pairs)
		{
			UEdGraphPin* SubPin = FindSubPinByMember(Pin, Pair.Key);
			if (!SubPin)
			{
				OutError = FString::Printf(
					TEXT("Split pin '%s' has no sub-pin for member '%s'. Available members: %s"),
					*Pin->PinName.ToString(), *Pair.Key, *DescribeAvailableMembers(Pin));
				return false;
			}
			if (!CollectSplitPinWrites(K2Schema, SubPin, Pair.Value, OutWrites, OutNotes, OutError))
			{
				return false;
			}
		}

		if (Untouched.Num() > 0)
		{
			OutNotes.Add(FString::Printf(
				TEXT("split pin '%s': sub-pins not named in the literal keep their current defaults: %s"),
				*Pin->PinName.ToString(), *FString::Join(Untouched, TEXT(", "))));
		}
		return true;
	}
} // namespace ClaireonSetPinValue_SplitInternal


FString ClaireonBlueprintGraphTool_SetPinValue::GetOperation() const { return TEXT("set_pin_value"); }

TArray<FString> ClaireonBlueprintGraphTool_SetPinValue::GetSearchKeywords() const
{
    return {TEXT("bp"), TEXT("pin"), TEXT("set"), TEXT("default"), TEXT("value"), TEXT("literal"), TEXT("graph")};
}

FString ClaireonBlueprintGraphTool_SetPinValue::GetDescription() const
{
    return TEXT("Set a pin's default literal value on the current session's graph (input pin only; the pin must be unconnected). Most-common pitfall: trying to set a value on a pin that is already wired to another node, which silently does nothing -- disconnect first via bp_disconnect_pin. Accepts either session_id or asset_path; auto-opens a session when asset_path is supplied.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_SetPinValue::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("node_guid"), TEXT("GUID of the node that owns the pin."), true);
    Builder.AddString(TEXT("pin_name"), TEXT("Pin name on that node."), true);
    Builder.AddString(TEXT("value"), TEXT("Default value, encoded as the pin's literal string form."), true);
    Builder.AddEnum(TEXT("pin_direction"), TEXT("Direction hint for pin_name lookup when an input and an output pin share the name. Defaults to 'input' -- only input pins accept defaults."), { TEXT("input"), TEXT("output") });
    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_SetPinValue::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FBlueprintEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("set_pin_value"), Params, SessionId, Data, Error))
    {
        return Error;
    }
    return CheckMutationAffectedNodes(TEXT("set_pin_value"), Data, SetPinValue_Impl(SessionId, Data, Params));
}

FToolResult ClaireonBlueprintGraphTool_SetPinValue::SetPinValue_Impl(
    const FString& SessionId,
    FBlueprintEditToolData* Data,
    const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = Data->Blueprint.Get();
	UEdGraph* Graph = Data->Graph.Get();

	if (!IsValid(Blueprint) || !IsValid(Graph))
	{
		return MakeErrorResult(TEXT("Blueprint or Graph is no longer valid"));
	}

	// Get node and pin
	FString NodeGuidStr, PinName, Value;
	if (!Params->TryGetStringField(TEXT("node_guid"), NodeGuidStr))
	{
		return MakeErrorResult(TEXT("Missing required field: node_guid"));
	}
	if (!Params->TryGetStringField(TEXT("pin_name"), PinName))
	{
		return MakeErrorResult(TEXT("Missing required field: pin_name"));
	}
	if (!Params->TryGetStringField(TEXT("value"), Value))
	{
		return MakeErrorResult(TEXT("Missing required field: value"));
	}

	// P2-17: direction hint for the pin lookup. Defaults to input -- only input
	// pins accept defaults, so a bare name shared by an in/out pair resolves to
	// the only pin this tool could act on instead of erroring as ambiguous. An
	// explicit 'output' is accepted and then answered by the output-pin error
	// below, which names the actual problem.
	EEdGraphPinDirection PinDirection = EGPD_Input;
	FString PinDirectionStr;
	const bool bPinDirectionExplicit = Params->TryGetStringField(TEXT("pin_direction"), PinDirectionStr);
	if (bPinDirectionExplicit)
	{
		if (PinDirectionStr == TEXT("output"))
		{
			PinDirection = EGPD_Output;
		}
		else if (PinDirectionStr != TEXT("input"))
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Invalid pin_direction '%s': expected 'input' or 'output'"), *PinDirectionStr));
		}
	}

	// Find node (full GUID or >=8-hex prefix)
	FString ResolveError;
	UEdGraphNode* Node = ClaireonBPGraphInternal::FindNodeForOperationStr(Graph, NodeGuidStr, Data, ResolveError);
	if (!IsValid(Node))
	{
		return MakeErrorResult(ResolveError);
	}

	// Resolve pin using fuzzy matching, constrained by the direction hint.
	TArray<FString> ResolutionWarnings;
	ClaireonNameResolver::FNameResolveResult SetPinResult;
	UEdGraphPin* Pin = ClaireonNameResolver::ResolvePinName(Node, PinName, PinDirection, SetPinResult);
	if (!Pin && !bPinDirectionExplicit)
	{
		// The name may exist only as an output pin. Re-resolve unconstrained so
		// the output-pin check below names the actual problem ("cannot set a
		// default on an output pin") instead of a not-found error.
		ClaireonNameResolver::FNameResolveResult UnconstrainedResult;
		if (UEdGraphPin* OutputOnly = ClaireonNameResolver::ResolvePinName(Node, PinName, EGPD_MAX, UnconstrainedResult))
		{
			Pin = OutputOnly;
			SetPinResult = UnconstrainedResult;
		}
	}
	if (!Pin)
	{
		return MakeErrorResult(SetPinResult.Error);
	}
	if (!SetPinResult.ResolutionNote.IsEmpty())
	{
		ResolutionWarnings.Add(SetPinResult.ResolutionNote);
	}

	// Validate pin can have default value (must be input pin with no connection)
	if (Pin->Direction != EGPD_Input)
	{
		return MakeErrorResult(FString::Printf(TEXT("Cannot set default value on output pin: %s"), *PinName));
	}

	if (Pin->LinkedTo.Num() > 0)
	{
		return MakeErrorResult(FString::Printf(TEXT("Cannot set default value on connected pin: %s"), *PinName));
	}

	// Set the default value using transaction
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Blueprint Pin Value")));
	Blueprint->Modify();
	Graph->Modify();
	Node->Modify();

	// Get schema for validation
	const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();

	// Split pin. The engine stores authored defaults on the SUB-pins; a write
	// to the parent serializes but the compiler reads the (unmodified)
	// sub-pins, so the value silently never applies (Work #6704, split-pin
	// report). Distribute the literal to the sub-pins instead: all leaf
	// values validate before anything is written, so a bad member errors
	// with the graph untouched.
	if (Pin->SubPins.Num() > 0)
	{
		using namespace ClaireonSetPinValue_SplitInternal;
		TArray<TPair<UEdGraphPin*, FString>> LeafWrites;
		FString SplitError;
		if (!CollectSplitPinWrites(K2Schema, Pin, Value, LeafWrites, ResolutionWarnings, SplitError))
		{
			return MakeErrorResult(SplitError);
		}
		TArray<FString> WrittenNames;
		for (const TPair<UEdGraphPin*, FString>& Write : LeafWrites)
		{
			const FString ApplyError = ValidateOrWriteLeafDefault(K2Schema, Write.Key, Write.Value, /*bApply*/true);
			if (!ApplyError.IsEmpty())
			{
				// Validation already passed above, so this indicates engine
				// state changed mid-operation; surface it rather than hide it.
				return MakeErrorResult(ApplyError);
			}
			WrittenNames.Add(Write.Key->PinName.ToString());
		}
		ResolutionWarnings.Add(FString::Printf(
			TEXT("pin '%s' is split; value distributed to %d sub-pin(s): %s"),
			*PinName, WrittenNames.Num(), *FString::Join(WrittenNames, TEXT(", "))));

		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

		Data->Cursor.LastOperationStatus = FString::Printf(
			TEXT("Set [%s].%s = '%s' (distributed to %d sub-pins)"),
			*Node->GetNodeTitle(ENodeTitleType::ListView).ToString(), *PinName, *Value, WrittenNames.Num());
		Data->LastOperationAffectedNodes.Add(Node->NodeGuid);

		FToolResult SplitPinResult = BuildStateResponse(SessionId, Data);
		SplitPinResult.Warnings.Append(ResolutionWarnings);
		return SplitPinResult;
	}

	// Wildcard promotion. An unconnected wildcard pin has no type for the engine to
	// validate a literal against, so the write below would fail with a generic
	// "Unsupported type" error. Infer a type from the literal, promote the pin (and
	// its container siblings / output pin), then fall through to the normal
	// validation+write path with the now-concrete type.
	const bool bPromotedFromWildcard = (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard);
	if (bPromotedFromWildcard)
	{
		FString InferenceWarning;
		const FEdGraphPinType InferredType =
			ClaireonSetPinValue_WildcardInternal::InferPinTypeFromLiteral(Value, InferenceWarning);

		if (InferredType.PinCategory.IsNone())
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Pin '%s' is a wildcard and no type could be inferred from '%s'. ")
				TEXT("Connect a typed pin to it first (bp_connect_pins resolves the wildcard), ")
				TEXT("or pass an unambiguous literal (true/false, an integer, a decimal like 0.25, ")
				TEXT("or a full object path like /Game/Dir/Asset.Asset)."),
				*PinName, *Value));
		}

		ClaireonBlueprintHelpers::PromoteWildcardContainerElementPin(Pin, InferredType);

		if (!InferenceWarning.IsEmpty())
		{
			ResolutionWarnings.Add(InferenceWarning);
		}
		ResolutionWarnings.Add(FString::Printf(
			TEXT("pin '%s' promoted wildcard -> %s from literal"),
			*PinName, *ClaireonSetPinValue_WildcardInternal::DescribePinType(InferredType)));
	}

	const FName PinCategory = Pin->PinType.PinCategory;
	const bool bObjectLikePin =
		   PinCategory == UEdGraphSchema_K2::PC_Object
		|| PinCategory == UEdGraphSchema_K2::PC_Class
		|| PinCategory == UEdGraphSchema_K2::PC_SoftObject
		|| PinCategory == UEdGraphSchema_K2::PC_SoftClass
		|| PinCategory == UEdGraphSchema_K2::PC_Interface;

	if (bObjectLikePin)
	{
		// Object/class pin defaults live in DefaultObject, and the plain
		// TrySetDefaultValue string path silently no-ops when its FindObject
		// lookup misses (e.g. an unloaded asset or a short class name). Resolve
		// explicitly and fail loudly. This is what makes ConstructObjectFromClass/
		// CreateWidget 'Class' pins and EnhancedInput asset pins settable.
		if (Value.IsEmpty() || Value == TEXT("None"))
		{
			K2Schema->TrySetDefaultObject(*Pin, nullptr);
		}
		else
		{
			UObject* Resolved = nullptr;
			if (PinCategory == UEdGraphSchema_K2::PC_Class || PinCategory == UEdGraphSchema_K2::PC_SoftClass)
			{
				ClaireonNameResolver::FNameResolveResult ClassResult;
				Resolved = ClaireonNameResolver::ResolveClassName(Value, nullptr, ClassResult);
				if (IsValid(Resolved) && !ClassResult.ResolutionNote.IsEmpty())
				{
					ResolutionWarnings.Add(ClassResult.ResolutionNote);
				}
			}
			if (!IsValid(Resolved))
			{
				Resolved = LoadObject<UObject>(nullptr, *Value);
			}
			if (!IsValid(Resolved) && !Value.Contains(TEXT(".")))
			{
				// Accept package-path shorthand /Game/Dir/Asset
				const FString ObjectPath = Value + TEXT(".") + FPackageName::GetShortName(Value);
				Resolved = LoadObject<UObject>(nullptr, *ObjectPath);
			}
			if (!IsValid(Resolved))
			{
				return MakeErrorResult(FString::Printf(
					TEXT("Could not resolve '%s' for %s pin '%s' (pass a class name or a full object path like /Game/Dir/Asset.Asset)"),
					*Value, *PinCategory.ToString(), *PinName));
			}

			const FString ValidationError = K2Schema->IsPinDefaultValid(Pin, FString(), Resolved, FText());
			if (!ValidationError.IsEmpty())
			{
				return MakeErrorResult(FString::Printf(
					TEXT("'%s' is not a valid default for pin '%s': %s"),
					*Value, *PinName, *ValidationError));
			}
			K2Schema->TrySetDefaultObject(*Pin, Resolved);
		}
	}
	else
	{
		// Validate first so a rejected value errors instead of silently keeping
		// the old default (TrySetDefaultValue swallows validation failures).
		FString UseDefaultValue;
		TObjectPtr<UObject> UseDefaultObject = nullptr;
		FText UseDefaultText;
		K2Schema->GetPinDefaultValuesFromString(Pin->PinType, Pin->GetOwningNodeUnchecked(), Value,
			UseDefaultValue, UseDefaultObject, UseDefaultText, /*bPreserveTextIdentity*/false);
		const FString ValidationError = K2Schema->IsPinDefaultValid(Pin, UseDefaultValue, UseDefaultObject, UseDefaultText);
		if (!ValidationError.IsEmpty())
		{
			if (bPromotedFromWildcard)
			{
				// The generic message would only name the type we just invented, which
				// tells the caller nothing about why their literal was rejected.
				return MakeErrorResult(FString::Printf(
					TEXT("Pin '%s' is a wildcard; '%s' was inferred as %s but rejected: %s. ")
					TEXT("Connect a typed pin to it first (bp_connect_pins resolves the wildcard), ")
					TEXT("or pass an unambiguous literal (true/false, an integer, a decimal like 0.25, ")
					TEXT("or a full object path like /Game/Dir/Asset.Asset)."),
					*PinName, *Value,
					*ClaireonSetPinValue_WildcardInternal::DescribePinType(Pin->PinType),
					*ValidationError));
			}
			return MakeErrorResult(FString::Printf(
				TEXT("'%s' is not a valid default for pin '%s': %s"),
				*Value, *PinName, *ValidationError));
		}
		K2Schema->TrySetDefaultValue(*Pin, Value);
	}

	// Mark Blueprint as modified
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
	Data->Cursor.LastOperationStatus = FString::Printf(
		TEXT("Set [%s].%s = '%s'"),
		*NodeTitle, *PinName, *Value);

	// Populate affected nodes: the node whose pin value changed
	Data->LastOperationAffectedNodes.Add(Node->NodeGuid);

	FToolResult SetPinValueResult = BuildStateResponse(SessionId, Data);
	SetPinValueResult.Warnings.Append(ResolutionWarnings);
	return SetPinValueResult;
}

// ----------------------------------------------------------------------------
// hot-path metadata enrichment
// ----------------------------------------------------------------------------

FString ClaireonBlueprintGraphTool_SetPinValue::GetFullDescription() const
{
    return TEXT(
        "Sets a pin's default literal value on the current session's graph. "
        "Only applies to input pins that have NO incoming connection: a pin "
        "wired to another node ignores the literal value (the connection "
        "wins). If you need to override a wired pin, first disconnect it via "
        "bp_disconnect_pin. Value strings go through "
        "FProperty::ImportText_Direct, so primitives use plain text ('42', "
        "'1.5', 'true', 'Hello') while structs use the engine's text format "
        "(e.g. '(X=1.0,Y=2.0,Z=3.0)' for FVector). Object pins accept asset "
        "paths. A pin that has been SPLIT into sub-pins is handled: the "
        "struct literal is distributed onto the sub-pins (named form "
        "'(X=..,Y=..)' by member, bare form '1,2,3' in the struct's own "
        "component order), because the compiler reads split defaults from "
        "the sub-pins, not the parent. Individual sub-pins can also be set "
        "directly by name (e.g. 'NewScale3D_X').");
}

FString ClaireonBlueprintGraphTool_SetPinValue::GetExampleUsage() const
{
    return TEXT(
        "bp_set_pin_value session_id=\"...\" "
        "node=\"PrintString_0\" pin=\"InString\" value=\"Hello\"");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_SetPinValue::GetParameterTooltips() const
{
    TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
    T->SetStringField(TEXT("session_id"), TEXT("Session ID returned by bp_open or _create."));
    T->SetStringField(TEXT("node_guid"), TEXT("GUID of the node that owns the pin."));
    T->SetStringField(TEXT("pin_name"), TEXT("Pin name. Must be an INPUT pin with no incoming connection."));
    T->SetStringField(TEXT("value"), TEXT("Literal value as text. Primitives use plain text; structs use engine text format (e.g. (X=1,Y=2,Z=3))."));
    return T;
}

#undef LOCTEXT_NAMESPACE
