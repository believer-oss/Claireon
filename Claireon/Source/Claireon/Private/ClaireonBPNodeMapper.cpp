// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonBPNodeMapper.h"
#include "ClaireonBPMacroHandler.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_SpawnActorFromClass.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_RemoveDelegate.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_ClearDelegate.h"
#include "K2Node_Knot.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_SwitchInteger.h"
#include "K2Node_SwitchString.h"
#include "K2Node_SwitchName.h"
#include "K2Node_Timeline.h"
#include "K2Node_BaseAsyncTask.h"
#include "Engine/LatentActionManager.h"

namespace
{
	FString MakeIndent(int32 IndentLevel)
	{
		FString Result;
		for (int32 i = 0; i < IndentLevel; ++i)
		{
			Result += TEXT("\t");
		}
		return Result;
	}

	// Get the short GUID string for a node
	FString GetNodeGuidStr(const UEdGraphNode* Node)
	{
		if (!Node)
		{
			return TEXT("null");
		}
		return Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensInBraces);
	}

	// Find a pin by name and direction on a node
	UEdGraphPin* FindPin(const UEdGraphNode* Node, const FName& PinName, EEdGraphPinDirection Direction)
	{
		if (!Node)
		{
			return nullptr;
		}
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinName == PinName && Pin->Direction == Direction)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	// V7-bpmath: Map Blueprint math/utility function names to valid C++ expressions.
	// Returns empty string if no mapping exists (use original name).
	// Format codes: {0}=first param, {1}=second param, {2}=third, etc.
	// Prefix "OP:" means binary operator format: ({0} OP {1})
	enum class EBPMathMapping : uint8
	{
		None,
		BinaryOp,    // ({0} op {1})
		UnaryOp,     // (op{0})
		Function,    // FuncName({0}, {1}, ...)
		Constructor, // TypeName({0}, {1}, ...)
		Custom,      // Fully custom format string
	};

	struct FBPMathMap
	{
		const TCHAR* BPName;
		EBPMathMapping Type;
		const TCHAR* CppExpr;  // operator for BinaryOp/UnaryOp, function name for Function/Constructor
	};

	static const FBPMathMap BPMathMappings[] = {
		// Arithmetic operators
		{ TEXT("Add_DoubleDouble"),          EBPMathMapping::BinaryOp,    TEXT("+") },
		{ TEXT("Add_FloatFloat"),            EBPMathMapping::BinaryOp,    TEXT("+") },
		{ TEXT("Add_IntInt"),                EBPMathMapping::BinaryOp,    TEXT("+") },
		{ TEXT("Add_VectorVector"),          EBPMathMapping::BinaryOp,    TEXT("+") },
		{ TEXT("Add_Vector2DVector2D"),      EBPMathMapping::BinaryOp,    TEXT("+") },
		{ TEXT("Subtract_DoubleDouble"),     EBPMathMapping::BinaryOp,    TEXT("-") },
		{ TEXT("Subtract_FloatFloat"),       EBPMathMapping::BinaryOp,    TEXT("-") },
		{ TEXT("Subtract_IntInt"),           EBPMathMapping::BinaryOp,    TEXT("-") },
		{ TEXT("Subtract_VectorVector"),     EBPMathMapping::BinaryOp,    TEXT("-") },
		{ TEXT("Multiply_DoubleDouble"),     EBPMathMapping::BinaryOp,    TEXT("*") },
		{ TEXT("Multiply_FloatFloat"),       EBPMathMapping::BinaryOp,    TEXT("*") },
		{ TEXT("Multiply_IntInt"),           EBPMathMapping::BinaryOp,    TEXT("*") },
		{ TEXT("Multiply_VectorVector"),     EBPMathMapping::BinaryOp,    TEXT("*") },
		{ TEXT("Multiply_VectorFloat"),      EBPMathMapping::BinaryOp,    TEXT("*") },
		{ TEXT("Multiply_VectorDouble"),     EBPMathMapping::BinaryOp,    TEXT("*") },
		{ TEXT("Divide_DoubleDouble"),       EBPMathMapping::BinaryOp,    TEXT("/") },
		{ TEXT("Divide_FloatFloat"),         EBPMathMapping::BinaryOp,    TEXT("/") },
		{ TEXT("Divide_IntInt"),             EBPMathMapping::BinaryOp,    TEXT("/") },
		// Comparison operators
		{ TEXT("Greater_DoubleDouble"),      EBPMathMapping::BinaryOp,    TEXT(">") },
		{ TEXT("Greater_FloatFloat"),        EBPMathMapping::BinaryOp,    TEXT(">") },
		{ TEXT("Less_DoubleDouble"),         EBPMathMapping::BinaryOp,    TEXT("<") },
		{ TEXT("Less_FloatFloat"),           EBPMathMapping::BinaryOp,    TEXT("<") },
		{ TEXT("EqualEqual_DoubleDouble"),   EBPMathMapping::BinaryOp,    TEXT("==") },
		{ TEXT("NotEqual_DoubleDouble"),     EBPMathMapping::BinaryOp,    TEXT("!=") },
		// Boolean operators
		{ TEXT("BooleanAND"),               EBPMathMapping::BinaryOp,    TEXT("&&") },
		{ TEXT("BooleanOR"),                EBPMathMapping::BinaryOp,    TEXT("||") },
		{ TEXT("Not_PreBool"),              EBPMathMapping::UnaryOp,     TEXT("!") },
		// Unary
		{ TEXT("NegateRotator"),            EBPMathMapping::UnaryOp,     TEXT("-") },
		{ TEXT("Negated"),                  EBPMathMapping::UnaryOp,     TEXT("-") },
		// Math functions
		{ TEXT("DegSin"),                   EBPMathMapping::Function,    TEXT("FMath::Sin(FMath::DegreesToRadians") },
		{ TEXT("DegCos"),                   EBPMathMapping::Function,    TEXT("FMath::Cos(FMath::DegreesToRadians") },
		{ TEXT("Abs"),                      EBPMathMapping::Function,    TEXT("FMath::Abs") },
		{ TEXT("FClamp"),                   EBPMathMapping::Function,    TEXT("FMath::Clamp") },
		{ TEXT("FMax"),                     EBPMathMapping::Function,    TEXT("FMath::Max") },
		{ TEXT("FMin"),                     EBPMathMapping::Function,    TEXT("FMath::Min") },
		{ TEXT("Sqrt"),                     EBPMathMapping::Function,    TEXT("FMath::Sqrt") },
		{ TEXT("Lerp"),                     EBPMathMapping::Function,    TEXT("FMath::Lerp") },
		{ TEXT("RandomFloatInRange"),       EBPMathMapping::Function,    TEXT("FMath::FRandRange") },
		{ TEXT("RandomFloat"),              EBPMathMapping::Function,    TEXT("FMath::FRand") },
		// Constructor-style
		{ TEXT("MakeVector"),               EBPMathMapping::Constructor, TEXT("FVector") },
		{ TEXT("MakeVector2D"),             EBPMathMapping::Constructor, TEXT("FVector2D") },
		{ TEXT("MakeRotator"),              EBPMathMapping::Constructor, TEXT("FRotator") },
		{ TEXT("MakeTransform"),            EBPMathMapping::Constructor, TEXT("FTransform") },
		{ TEXT("MakeColor"),                EBPMathMapping::Constructor, TEXT("FLinearColor") },
		{ TEXT("MakeLiteralName"),          EBPMathMapping::Constructor, TEXT("FName") },
		{ TEXT("MakeLiteralString"),        EBPMathMapping::Constructor, TEXT("FString") },
		{ TEXT("MakeLiteralText"),          EBPMathMapping::Constructor, TEXT("FText::FromString") },
		// Conversion functions
		{ TEXT("Conv_VectorToRotator"),     EBPMathMapping::Function,    TEXT("UKismetMathLibrary::Conv_VectorToRotator") },
		{ TEXT("Conv_VectorToTransform"),   EBPMathMapping::Constructor, TEXT("FTransform") },
		{ TEXT("Conv_RotatorToVector"),     EBPMathMapping::Function,    TEXT("UKismetMathLibrary::Conv_RotatorToVector") },
		{ TEXT("Conv_IntToFloat"),          EBPMathMapping::Function,    TEXT("static_cast<float>") },
		{ TEXT("Conv_FloatToDouble"),       EBPMathMapping::Function,    TEXT("static_cast<double>") },
		// Transform functions
		{ TEXT("ComposeTransforms"),        EBPMathMapping::BinaryOp,    TEXT("*") },
		{ TEXT("TLerp"),                    EBPMathMapping::Function,    TEXT("UKismetMathLibrary::TLerp") },
		// Random
		{ TEXT("RandomUnitVector"),         EBPMathMapping::Function,    TEXT("FMath::VRand") },
	};

	FString ApplyBPMathMapping(const FString& FuncName, const TArray<FString>& Params)
	{
		for (const FBPMathMap& Map : BPMathMappings)
		{
			if (FuncName != Map.BPName) continue;

			switch (Map.Type)
			{
			case EBPMathMapping::BinaryOp:
				if (Params.Num() >= 2)
				{
					return FString::Printf(TEXT("(%s %s %s)"), *Params[0], Map.CppExpr, *Params[1]);
				}
				break;
			case EBPMathMapping::UnaryOp:
				if (Params.Num() >= 1)
				{
					return FString::Printf(TEXT("(%s%s)"), Map.CppExpr, *Params[0]);
				}
				break;
			case EBPMathMapping::Function:
			{
				FString ParamStr = FString::Join(Params, TEXT(", "));
				// Special case for DegSin/DegCos which need a closing paren for the inner call
				FString CppName = Map.CppExpr;
				if (CppName.Contains(TEXT("DegreesToRadians")))
				{
					if (Params.Num() >= 1)
					{
						return FString::Printf(TEXT("%s(%s))"), Map.CppExpr, *Params[0]);
					}
				}
				return FString::Printf(TEXT("%s(%s)"), Map.CppExpr, *ParamStr);
			}
			case EBPMathMapping::Constructor:
			{
				FString ParamStr = FString::Join(Params, TEXT(", "));
				return FString::Printf(TEXT("%s(%s)"), Map.CppExpr, *ParamStr);
			}
			default:
				break;
			}
		}
		return FString(); // No mapping found
	}

}

// V7-tags: Convert BP default value strings to valid C++ expressions
static FString SanitizeDefaultValue(const FString& Value)
{
	// FGameplayTag: (TagName="Foo.Bar") -> FGameplayTag::RequestGameplayTag(TEXT("Foo.Bar"))
	if (Value.StartsWith(TEXT("(TagName=\"")))
	{
		FString TagName = Value;
		TagName.RemoveFromStart(TEXT("(TagName=\""));
		TagName.RemoveFromEnd(TEXT("\")"));
		return FString::Printf(TEXT("FGameplayTag::RequestGameplayTag(TEXT(\"%s\"))"), *TagName);
	}

	// FGameplayTagContainer: similar pattern but may have multiple tags
	// For now just pass through if it doesn't match simple tag pattern

	// FLinearColor: (R=0.0,G=0.66,B=1.0,A=1.0) -> FLinearColor(0.0f, 0.66f, 1.0f, 1.0f)
	if (Value.StartsWith(TEXT("(R=")) && Value.Contains(TEXT(",G=")) && Value.Contains(TEXT(",B=")))
	{
		FString Inner = Value;
		Inner.RemoveFromStart(TEXT("("));
		Inner.RemoveFromEnd(TEXT(")"));
		TArray<FString> Parts;
		Inner.ParseIntoArray(Parts, TEXT(","));
		TArray<FString> Vals;
		for (const FString& Part : Parts)
		{
			FString Val;
			if (Part.Split(TEXT("="), nullptr, &Val))
			{
				Vals.Add(Val.TrimStartAndEnd() + TEXT("f"));
			}
		}
		if (Vals.Num() >= 3)
		{
			return FString::Printf(TEXT("FLinearColor(%s)"), *FString::Join(Vals, TEXT(", ")));
		}
	}

	// FHitResult: massive struct dump -> FHitResult()
	if (Value.StartsWith(TEXT("(FaceIndex=")) || (Value.Contains(TEXT(",Time=")) && Value.Contains(TEXT(",Distance="))))
	{
		return TEXT("FHitResult()");
	}

	// FLatentActionInfo: (Linkage=-1,UUID=-1,...) -> FLatentActionInfo()
	if (Value.StartsWith(TEXT("(Linkage=")))
	{
		return TEXT("FLatentActionInfo()");
	}

	// FGameplayTagQuery: complex struct -> pass as-is for now (too complex to parse)
	if (Value.Contains(TEXT("TokenStreamVersion=")) && Value.Contains(TEXT("QueryTokenStream=")))
	{
		// Extract tag names for a readable comment
		return Value; // Leave as-is; this is complex
	}

	return Value;
}

FString FClaireonBPNodeMapper::GetConnectedPinExpression(const UEdGraphPin* Pin) const
{
	if (!Pin || Pin->LinkedTo.Num() == 0)
	{
		if (Pin && !Pin->DefaultValue.IsEmpty())
		{
			// V7-tags: Sanitize default values to valid C++ expressions
			return SanitizeDefaultValue(Pin->DefaultValue);
		}
		// V7-unconnected: Auto-fill well-known hidden/implicit pins
		if (Pin)
		{
			FString PinName = Pin->PinName.ToString();
			// WorldContextObject: auto-wired by engine in BP, should be "this" in C++
			if (PinName == TEXT("WorldContextObject") || PinName == TEXT("WorldContext"))
			{
				return TEXT("this");
			}

			// V9-zero-init: Emit a type-appropriate zero-initialized expression for
			// unconnected input pins with empty default values. BP function signatures
			// only expose pins whose defaults were left unset; in generated C++ these
			// map directly to `T{}` (bool=>false, numeric=>0, object=>nullptr, struct=>{}).
			// This produces compileable arguments instead of `/* unconnected */` comments.
			if (Pin->Direction == EGPD_Input && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
				&& Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Delegate)
			{
				const FName& Cat = Pin->PinType.PinCategory;
				if (Cat == UEdGraphSchema_K2::PC_Boolean)
				{
					return TEXT("false");
				}
				if (Cat == UEdGraphSchema_K2::PC_Byte || Cat == UEdGraphSchema_K2::PC_Int
					|| Cat == UEdGraphSchema_K2::PC_Int64 || Cat == UEdGraphSchema_K2::PC_Real)
				{
					return TEXT("0");
				}
				if (Cat == UEdGraphSchema_K2::PC_Object || Cat == UEdGraphSchema_K2::PC_Class
					|| Cat == UEdGraphSchema_K2::PC_Interface || Cat == UEdGraphSchema_K2::PC_SoftObject
					|| Cat == UEdGraphSchema_K2::PC_SoftClass)
				{
					return TEXT("nullptr");
				}
				if (Cat == UEdGraphSchema_K2::PC_String)
				{
					return TEXT("FString()");
				}
				if (Cat == UEdGraphSchema_K2::PC_Name)
				{
					return TEXT("NAME_None");
				}
				if (Cat == UEdGraphSchema_K2::PC_Text)
				{
					return TEXT("FText::GetEmpty()");
				}
				if (Cat == UEdGraphSchema_K2::PC_Struct)
				{
					const FString CppType = PinTypeToCppType(Pin->PinType);
					return FString::Printf(TEXT("%s()"), *CppType);
				}
			}
		}
		return TEXT("/* unconnected */");
	}

	UEdGraphPin* LinkedPin = Pin->LinkedTo[0];
	if (!LinkedPin || !LinkedPin->GetOwningNode())
	{
		return TEXT("/* unconnected */");
	}

	UEdGraphNode* SourceNode = LinkedPin->GetOwningNode();

	// V7-1: Track this node as inlined for accurate orphan classification
	InlinedPureNodes.Add(SourceNode);

	// Variable get -> return variable name
	if (UK2Node_VariableGet* VarGet = Cast<UK2Node_VariableGet>(SourceNode))
	{
		// V7-space: Sanitize variable names to valid C++ identifiers
		return SanitizeCppIdentifier(VarGet->GetVarName().ToString());
	}

	// Self reference -> return "this"
	if (SourceNode->GetClass()->GetName() == TEXT("K2Node_Self"))
	{
		return TEXT("this");
	}

	// Call function -> return FunctionName(...)  or temp variable reference
	if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(SourceNode))
	{
		FString FuncName = CallNode->FunctionReference.GetMemberName().ToString();
		FuncName = SanitizeCppIdentifier(FuncName);

		// Check if the source pin is the ReturnValue pin
		if (LinkedPin->PinName == UEdGraphSchema_K2::PN_ReturnValue)
		{
			// Check if this pure call has multiple consumers
			int32 ConsumerCount = 0;
			for (UEdGraphPin* OutPin : SourceNode->Pins)
			{
				if (OutPin && OutPin->Direction == EGPD_Output)
				{
					ConsumerCount += OutPin->LinkedTo.Num();
				}
			}

			// V5-3: Always inline pure calls (accept double evaluation for scaffold readability)
			{
				TArray<FString> Params;
				FString TargetExpr;
				for (UEdGraphPin* SrcPin : SourceNode->Pins)
				{
					if (!SrcPin || SrcPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
					if (SrcPin->PinName == UEdGraphSchema_K2::PN_Self)
					{
						if (SrcPin->LinkedTo.Num() > 0)
						{
							TargetExpr = GetConnectedPinExpression(SrcPin);
						}
						continue;
					}
					if (SrcPin->Direction == EGPD_Input)
					{
						Params.Add(GetConnectedPinExpression(SrcPin));
					}
				}
				// V7-bpmath: Apply BP-to-C++ math function name mapping
				FString MappedExpr = ApplyBPMathMapping(FuncName, Params);
				if (!MappedExpr.IsEmpty())
				{
					return MappedExpr;
				}
				FString ParamStr = FString::Join(Params, TEXT(", "));
				if (!TargetExpr.IsEmpty())
				{
					return FString::Printf(TEXT("%s->%s(%s)"), *TargetExpr, *FuncName, *ParamStr);
				}
				return FString::Printf(TEXT("%s(%s)"), *FuncName, *ParamStr);
			}
		}

		// V5-1: Non-return output pin -- check if this is a pure function with a generic output pin name
		FString OutPinName = LinkedPin->PinName.ToString();
		if (OutPinName == TEXT("OutputPin") || OutPinName == TEXT("Output"))
		{
			// Check if the source node is a pure function (no exec pins)
			bool bHasExecPin = false;
			for (UEdGraphPin* CheckPin : SourceNode->Pins)
			{
				if (CheckPin && CheckPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
				{
					bHasExecPin = true;
					break;
				}
			}
			if (!bHasExecPin)
			{
				// Inline the function call expression
				TArray<FString> Params;
				FString TargetExpr;
				for (UEdGraphPin* SrcPin : SourceNode->Pins)
				{
					if (!SrcPin || SrcPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
					if (SrcPin->PinName == UEdGraphSchema_K2::PN_Self)
					{
						if (SrcPin->LinkedTo.Num() > 0)
						{
							TargetExpr = GetConnectedPinExpression(SrcPin);
						}
						continue;
					}
					if (SrcPin->Direction == EGPD_Input)
					{
						Params.Add(GetConnectedPinExpression(SrcPin));
					}
				}
				// V7-bpmath: Apply BP-to-C++ math function name mapping
				FString MappedExpr2 = ApplyBPMathMapping(FuncName, Params);
				if (!MappedExpr2.IsEmpty())
				{
					return MappedExpr2;
				}
				FString ParamStr = FString::Join(Params, TEXT(", "));
				if (!TargetExpr.IsEmpty())
				{
					return FString::Printf(TEXT("%s->%s(%s)"), *TargetExpr, *FuncName, *ParamStr);
				}
				return FString::Printf(TEXT("%s(%s)"), *FuncName, *ParamStr);
			}
		}
		// Named output pin -- use pin name as field accessor
		return FString::Printf(TEXT("%sResult.%s"), *FuncName, *OutPinName);
	}

	// Break struct -> return struct member name
	if (Cast<UK2Node_BreakStruct>(SourceNode))
	{
		return LinkedPin->PinName.ToString();
	}

	// Dynamic cast -> return cast result variable (matches MapCastNode output)
	if (UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(SourceNode))
	{
		return TEXT("CastResult");
	}

	// V5-1: Handle macro instances with pure output pins
	if (UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(SourceNode))
	{
		FString MacroName = TEXT("Macro");
		if (MacroNode->GetMacroGraph())
		{
			MacroName = SanitizeCppIdentifier(MacroNode->GetMacroGraph()->GetName());
		}
		TArray<FString> Params;
		for (UEdGraphPin* SrcPin : SourceNode->Pins)
		{
			if (!SrcPin || SrcPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
			if (SrcPin->Direction == EGPD_Input)
			{
				Params.Add(GetConnectedPinExpression(SrcPin));
			}
		}
		FString ParamStr = FString::Join(Params, TEXT(", "));
		return FString::Printf(TEXT("%s(%s)"), *MacroName, *ParamStr);
	}

	// V6-1: Reroute/knot node -- transparent pass-through, recurse on input
	if (UK2Node_Knot* KnotNode = Cast<UK2Node_Knot>(SourceNode))
	{
		UEdGraphPin* InputPin = KnotNode->GetInputPin();
		return InputPin ? GetConnectedPinExpression(InputPin) : TEXT("/* unconnected knot */");
	}

	// V6-2: Timeline curve output -> member reference to timeline interpolated value
	if (UK2Node_Timeline* TimelineNode = Cast<UK2Node_Timeline>(SourceNode))
	{
		FString TimelineName = TimelineNode->TimelineName.ToString();
		FString TrackName = LinkedPin->PinName.ToString();
		// Only handle data output curves, not exec pins
		if (LinkedPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
		{
			return FString::Printf(TEXT("%s_%s"), *TimelineName, *TrackName);
		}
	}

	// V7-asyncproxy: Async action proxy output -> member variable reference
	if (UK2Node_BaseAsyncTask* AsyncTaskNode = Cast<UK2Node_BaseAsyncTask>(SourceNode))
	{
		// Reconstruct the proxy variable name using the same logic as MapAsyncActionNodeEx
		UClass* ProxyClassPtr = nullptr;
		if (const FObjectPropertyBase* ProxyClassProp = CastField<FObjectPropertyBase>(
			AsyncTaskNode->GetClass()->FindPropertyByName(TEXT("ProxyClass"))))
		{
			ProxyClassPtr = Cast<UClass>(ProxyClassProp->GetObjectPropertyValue_InContainer(AsyncTaskNode));
		}
		FString ActionTypeName = ProxyClassPtr ? ProxyClassPtr->GetName() : TEXT("AsyncTask");
		ActionTypeName.RemoveFromStart(TEXT("AbilityAsync_"));
		ActionTypeName.RemoveFromStart(TEXT("AbilityTask_"));
		FString ShortGuid = SourceNode->NodeGuid.ToString(EGuidFormats::Digits).Left(8);
		return FString::Printf(TEXT("%s_%s_AsyncTask"), *SanitizeCppIdentifier(ActionTypeName), *ShortGuid);
	}

	// Fallback: use pin name (still better than nothing, but not ideal)
	FString PinName = LinkedPin->PinName.ToString();
	if (PinName == TEXT("ReturnValue") || PinName == TEXT("OutputPin") || PinName == TEXT("Output"))
	{
		// V5-1: For CallFunction nodes with generic pins, inline the call
		if (UK2Node_CallFunction* FallbackCallNode = Cast<UK2Node_CallFunction>(SourceNode))
		{
			FString FBFuncName = SanitizeCppIdentifier(FallbackCallNode->FunctionReference.GetMemberName().ToString());
			TArray<FString> Params;
			FString TargetExpr;
			for (UEdGraphPin* SrcPin : SourceNode->Pins)
			{
				if (!SrcPin || SrcPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
				if (SrcPin->PinName == UEdGraphSchema_K2::PN_Self)
				{
					if (SrcPin->LinkedTo.Num() > 0)
					{
						TargetExpr = GetConnectedPinExpression(SrcPin);
					}
					continue;
				}
				if (SrcPin->Direction == EGPD_Input)
				{
					Params.Add(GetConnectedPinExpression(SrcPin));
				}
			}
			// V7-bpmath: Apply BP-to-C++ math function name mapping
			FString FBMappedExpr = ApplyBPMathMapping(FBFuncName, Params);
			if (!FBMappedExpr.IsEmpty())
			{
				return FBMappedExpr;
			}
			FString ParamStr = FString::Join(Params, TEXT(", "));
			if (!TargetExpr.IsEmpty())
			{
				return FString::Printf(TEXT("%s->%s(%s)"), *TargetExpr, *FBFuncName, *ParamStr);
			}
			return FString::Printf(TEXT("%s(%s)"), *FBFuncName, *ParamStr);
		}
		// Try to get a better name from the source node
		FString NodeTitle = SourceNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
		NodeTitle = SanitizeCppIdentifier(NodeTitle);
		return FString::Printf(TEXT("%sResult"), *NodeTitle);
	}
	return SanitizeCppIdentifier(PinName);
}

FString FClaireonBPNodeMapper::SanitizeCppIdentifier(const FString& Name)
{
	FString Result = Name;
	Result.ReplaceInline(TEXT(" "), TEXT("_"));
	for (int32 i = Result.Len() - 1; i >= 0; --i)
	{
		TCHAR C = Result[i];
		if (!FChar::IsAlnum(C) && C != '_') Result.RemoveAt(i);
	}
	return Result;
}

FString FClaireonBPNodeMapper::StripModuleRelativePrefix(const FString& Path)
{
	FString Result = Path;
	if (Result.RemoveFromStart(TEXT("Public/"))) return Result;
	if (Result.RemoveFromStart(TEXT("Classes/"))) return Result;
	return Result;
}

void FClaireonBPNodeMapper::AddInclude(const FString& Path)
{
	if (!Path.IsEmpty() && !Path.StartsWith(TEXT("//")))
	{
		AccumulatedIncludes.Add(Path);
	}
}

const TSet<FString>& FClaireonBPNodeMapper::GetAccumulatedIncludes() const
{
	return AccumulatedIncludes;
}

FString FClaireonBPNodeMapper::MapNode(const UEdGraphNode* Node, int32 IndentLevel)
{
	if (!Node)
	{
		return FString();
	}

	// Priority dispatch per NODE_MAPPING specification

	// 0. Reroute/knot nodes are visual-only -- no code emission
	if (Cast<UK2Node_Knot>(Node))
	{
		return FString();
	}

	// 1. Macro instances
	if (const UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(Node))
	{
		FClaireonBPMacroHandler MacroHandler;
		if (MacroHandler.IsKnownMacro(Node))
		{
			FClaireonBPMacroResult Result = MacroHandler.HandleMacro(Node, IndentLevel);
			return Result.SourceCode;
		}
		// Unknown macro -- emit expanded tag with TODO
		FString Indent = MakeIndent(IndentLevel);
		FString GuidStr = GetNodeGuidStr(Node);
		FString MacroName = MacroNode->GetMacroGraph() ? MacroNode->GetMacroGraph()->GetName() : TEXT("Unknown");
		FString Output;
		Output += FString::Printf(TEXT("%s// [BP:MACRO_EXPANDED] Guid=%s Type=%s\n"), *Indent, *GuidStr, *MacroName);
		Output += FString::Printf(TEXT("%s// WARNING: This macro was expanded because no idiomatic C++ mapping exists.\n"), *Indent);
		Output += FString::Printf(TEXT("%s// Consider adding a special case in ClaireonBPMacroHandler.\n"), *Indent);
		Output += FString::Printf(TEXT("%s{\n"), *Indent);
		Output += FString::Printf(TEXT("%s\t// [BP] TODO: implement expanded macro \"%s\"\n"), *Indent, *MacroName);
		Output += FString::Printf(TEXT("%s}\n"), *Indent);
		return Output;
	}

	// 2a. Native execution sequence
	if (Cast<UK2Node_ExecutionSequence>(Node))
	{
		return MapExecutionSequenceNode(Node, IndentLevel);
	}

	// 2a2. Timeline nodes
	if (Cast<UK2Node_Timeline>(Node))
	{
		return MapTimelineNode(Node, IndentLevel);
	}

	// 2a3. Async action nodes
	if (Cast<UK2Node_BaseAsyncTask>(Node))
	{
		return MapAsyncActionNode(Node, IndentLevel);
	}

	// 2b. Switch nodes
	if (Cast<UK2Node_SwitchEnum>(Node))    return MapSwitchEnumNode(Node, IndentLevel);
	if (Cast<UK2Node_SwitchInteger>(Node)) return MapSwitchIntegerNode(Node, IndentLevel);
	if (Cast<UK2Node_SwitchString>(Node))  return MapSwitchStringNode(Node, IndentLevel);
	if (Cast<UK2Node_SwitchName>(Node))    return MapSwitchNameNode(Node, IndentLevel);

	// 2c. Branch (if/then/else)
	if (Cast<UK2Node_IfThenElse>(Node))
	{
		return MapBranchNode(Node, IndentLevel);
	}

	// 2d. Delegate call/clear (before CallFunction)
	if (Cast<UK2Node_CallDelegate>(Node))
	{
		return MapCallDelegateNode(Node, IndentLevel);
	}
	if (Cast<UK2Node_ClearDelegate>(Node))
	{
		return MapClearDelegateNode(Node, IndentLevel);
	}

	// 2e. CallParentFunction -- MUST be before CallFunction since it derives from it
	if (Cast<UK2Node_CallParentFunction>(Node))
	{
		return MapCallParentFunctionNode(Node, IndentLevel);
	}

	// 3. Call function
	if (const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
	{
		// Check for array operations (priority 14 in dispatch, but must check here since CallFunction is the base)
		if (const UFunction* TargetFunc = CallNode->GetTargetFunction())
		{
			FString FuncName = TargetFunc->GetName();
			if (FuncName.Contains(TEXT("Array_")))
			{
				return MapArrayOperationNode(Node, IndentLevel);
			}
		}
		return MapCallFunctionNode(Node, IndentLevel);
	}

	// 4. Variable get
	if (Cast<UK2Node_VariableGet>(Node))
	{
		return MapVariableGetNode(Node, IndentLevel);
	}

	// 5. Variable set
	if (Cast<UK2Node_VariableSet>(Node))
	{
		return MapVariableSetNode(Node, IndentLevel);
	}

	// 6. Function result / return
	if (Cast<UK2Node_FunctionResult>(Node))
	{
		return MapReturnNode(Node, IndentLevel);
	}

	// 6b. Function entry (annotation only)
	if (Cast<UK2Node_FunctionEntry>(Node))
	{
		return MapFunctionEntryNode(Node, IndentLevel);
	}

	// 7. Dynamic cast
	if (Cast<UK2Node_DynamicCast>(Node))
	{
		return MapCastNode(Node, IndentLevel);
	}

	// 8. Spawn actor
	if (Cast<UK2Node_SpawnActorFromClass>(Node))
	{
		return MapSpawnActorNode(Node, IndentLevel);
	}

	// 9. Event node (check before CustomEvent since CustomEvent derives from Event in some cases)
	if (Cast<UK2Node_CustomEvent>(Node))
	{
		return MapCustomEventNode(Node, IndentLevel);
	}

	// 10. Event
	if (Cast<UK2Node_Event>(Node))
	{
		return MapEventNode(Node, IndentLevel);
	}

	// 11. Delegate nodes (split: AddDelegate has improved handler)
	if (Cast<UK2Node_AddDelegate>(Node))
	{
		return MapAddDelegateNode(Node, IndentLevel);
	}
	if (Cast<UK2Node_RemoveDelegate>(Node) || Cast<UK2Node_CreateDelegate>(Node))
	{
		return MapDelegateNode(Node, IndentLevel);
	}

	// 12. Make struct
	if (Cast<UK2Node_MakeStruct>(Node))
	{
		return MapMakeStructNode(Node, IndentLevel);
	}

	// 13. Break struct
	if (Cast<UK2Node_BreakStruct>(Node))
	{
		return MapBreakStructNode(Node, IndentLevel);
	}

	// 15. Unknown
	return MapUnknownNode(Node, IndentLevel);
}

FString FClaireonBPNodeMapper::PinTypeToCppType(const FEdGraphPinType& PinType) const
{
	FString BaseType;

	// Resolve the inner type first (without container)
	const FName& Category = PinType.PinCategory;

	if (Category == UEdGraphSchema_K2::PC_Boolean)
	{
		BaseType = TEXT("bool");
	}
	else if (Category == UEdGraphSchema_K2::PC_Byte)
	{
		if (PinType.PinSubCategoryObject.IsValid())
		{
			// Enum masquerading as byte
			BaseType = PinType.PinSubCategoryObject->GetName();
		}
		else
		{
			BaseType = TEXT("uint8");
		}
	}
	else if (Category == UEdGraphSchema_K2::PC_Int)
	{
		BaseType = TEXT("int32");
	}
	else if (Category == UEdGraphSchema_K2::PC_Int64)
	{
		BaseType = TEXT("int64");
	}
	else if (Category == UEdGraphSchema_K2::PC_Real)
	{
		// UE5 uses PC_Real with subcategory for float/double
		if (PinType.PinSubCategory == UEdGraphSchema_K2::PC_Double)
		{
			BaseType = TEXT("double");
		}
		else
		{
			BaseType = TEXT("float");
		}
	}
	else if (Category == UEdGraphSchema_K2::PC_String)
	{
		BaseType = TEXT("FString");
	}
	else if (Category == UEdGraphSchema_K2::PC_Name)
	{
		BaseType = TEXT("FName");
	}
	else if (Category == UEdGraphSchema_K2::PC_Text)
	{
		BaseType = TEXT("FText");
	}
	else if (Category == UEdGraphSchema_K2::PC_Object)
	{
		if (PinType.PinSubCategoryObject.IsValid())
		{
			UClass* ObjClass = Cast<UClass>(PinType.PinSubCategoryObject.Get());
			if (ObjClass)
			{
				BaseType = FString::Printf(TEXT("%s%s*"),
					ObjClass->GetPrefixCPP(), *ObjClass->GetName());
			}
			else
			{
				BaseType = TEXT("UObject*");
			}
		}
		else
		{
			BaseType = TEXT("UObject*");
		}
	}
	else if (Category == UEdGraphSchema_K2::PC_Class)
	{
		if (PinType.PinSubCategoryObject.IsValid())
		{
			UClass* ObjClass = Cast<UClass>(PinType.PinSubCategoryObject.Get());
			if (ObjClass)
			{
				BaseType = FString::Printf(TEXT("TSubclassOf<%s%s>"),
					ObjClass->GetPrefixCPP(), *ObjClass->GetName());
			}
			else
			{
				BaseType = TEXT("TSubclassOf<UObject>");
			}
		}
		else
		{
			BaseType = TEXT("TSubclassOf<UObject>");
		}
	}
	else if (Category == UEdGraphSchema_K2::PC_SoftObject)
	{
		if (PinType.PinSubCategoryObject.IsValid())
		{
			UClass* ObjClass = Cast<UClass>(PinType.PinSubCategoryObject.Get());
			if (ObjClass)
			{
				BaseType = FString::Printf(TEXT("TSoftObjectPtr<%s%s>"),
					ObjClass->GetPrefixCPP(), *ObjClass->GetName());
			}
			else
			{
				BaseType = TEXT("TSoftObjectPtr<UObject>");
			}
		}
		else
		{
			BaseType = TEXT("TSoftObjectPtr<UObject>");
		}
	}
	else if (Category == UEdGraphSchema_K2::PC_SoftClass)
	{
		if (PinType.PinSubCategoryObject.IsValid())
		{
			UClass* ObjClass = Cast<UClass>(PinType.PinSubCategoryObject.Get());
			if (ObjClass)
			{
				BaseType = FString::Printf(TEXT("TSoftClassPtr<%s%s>"),
					ObjClass->GetPrefixCPP(), *ObjClass->GetName());
			}
			else
			{
				BaseType = TEXT("TSoftClassPtr<UObject>");
			}
		}
		else
		{
			BaseType = TEXT("TSoftClassPtr<UObject>");
		}
	}
	else if (Category == UEdGraphSchema_K2::PC_Interface)
	{
		if (PinType.PinSubCategoryObject.IsValid())
		{
			BaseType = FString::Printf(TEXT("TScriptInterface<I%s>"),
				*PinType.PinSubCategoryObject->GetName());
		}
		else
		{
			BaseType = TEXT("TScriptInterface<IInterface>");
		}
	}
	else if (Category == UEdGraphSchema_K2::PC_Struct)
	{
		if (PinType.PinSubCategoryObject.IsValid())
		{
			UScriptStruct* Struct = Cast<UScriptStruct>(PinType.PinSubCategoryObject.Get());
			if (Struct)
			{
				BaseType = Struct->GetStructCPPName();
			}
			else
			{
				BaseType = PinType.PinSubCategoryObject->GetName();
			}
		}
		else
		{
			BaseType = TEXT("/* UnknownStruct */");
		}
	}
	else if (Category == UEdGraphSchema_K2::PC_Enum)
	{
		if (PinType.PinSubCategoryObject.IsValid())
		{
			BaseType = PinType.PinSubCategoryObject->GetName();
		}
		else
		{
			BaseType = TEXT("/* UnknownEnum */");
		}
	}
	else if (Category == UEdGraphSchema_K2::PC_Delegate)
	{
		if (PinType.PinSubCategoryObject.IsValid())
		{
			BaseType = PinType.PinSubCategoryObject->GetName();
		}
		else
		{
			BaseType = TEXT("FScriptDelegate");
		}
	}
	else if (Category == UEdGraphSchema_K2::PC_MCDelegate)
	{
		if (PinType.PinSubCategoryObject.IsValid())
		{
			BaseType = PinType.PinSubCategoryObject->GetName();
		}
		else
		{
			BaseType = TEXT("FMulticastScriptDelegate");
		}
	}
	else if (Category == UEdGraphSchema_K2::PC_Wildcard)
	{
		BaseType = TEXT("/* WILDCARD */ //[BP:WILDCARD]");
	}
	else
	{
		BaseType = FString::Printf(TEXT("/* %s */"), *Category.ToString());
	}

	// Handle container types
	if (PinType.ContainerType == EPinContainerType::Array)
	{
		return FString::Printf(TEXT("TArray<%s>"), *BaseType);
	}
	else if (PinType.ContainerType == EPinContainerType::Set)
	{
		return FString::Printf(TEXT("TSet<%s>"), *BaseType);
	}
	else if (PinType.ContainerType == EPinContainerType::Map)
	{
		// Resolve value type from PinValueType
		FString ValueType;
		const FName& ValCategory = PinType.PinValueType.TerminalCategory;
		if (ValCategory == UEdGraphSchema_K2::PC_String)
		{
			ValueType = TEXT("FString");
		}
		else if (ValCategory == UEdGraphSchema_K2::PC_Int)
		{
			ValueType = TEXT("int32");
		}
		else if (ValCategory == UEdGraphSchema_K2::PC_Boolean)
		{
			ValueType = TEXT("bool");
		}
		else if (ValCategory == UEdGraphSchema_K2::PC_Real)
		{
			ValueType = PinType.PinValueType.TerminalSubCategory == UEdGraphSchema_K2::PC_Double
				? TEXT("double") : TEXT("float");
		}
		else if (ValCategory == UEdGraphSchema_K2::PC_Object || ValCategory == UEdGraphSchema_K2::PC_Struct
			|| ValCategory == UEdGraphSchema_K2::PC_Enum)
		{
			if (PinType.PinValueType.TerminalSubCategoryObject.IsValid())
			{
				ValueType = PinType.PinValueType.TerminalSubCategoryObject->GetName();
				if (ValCategory == UEdGraphSchema_K2::PC_Object)
				{
					ValueType += TEXT("*");
				}
			}
			else
			{
				ValueType = TEXT("UObject*");
			}
		}
		else
		{
			ValueType = FString::Printf(TEXT("/* %s */"), *ValCategory.ToString());
		}
		return FString::Printf(TEXT("TMap<%s, %s>"), *BaseType, *ValueType);
	}

	return BaseType;
}

FString FClaireonBPNodeMapper::PropertyFlagsToSpecifiers(uint64 PropertyFlags, const FString& Category) const
{
	TArray<FString> Specifiers;

	// Edit specifiers (with override priority)
	const bool bEditConst = (PropertyFlags & CPF_EditConst) != 0;
	const bool bDisableEditOnInstance = (PropertyFlags & CPF_DisableEditOnInstance) != 0;
	const bool bDisableEditOnTemplate = (PropertyFlags & CPF_DisableEditOnTemplate) != 0;
	const bool bEdit = (PropertyFlags & CPF_Edit) != 0;

	if (bEdit)
	{
		if (bEditConst)
		{
			Specifiers.Add(TEXT("VisibleAnywhere"));
		}
		else if (bDisableEditOnInstance)
		{
			Specifiers.Add(TEXT("EditDefaultsOnly"));
		}
		else if (bDisableEditOnTemplate)
		{
			Specifiers.Add(TEXT("EditInstanceOnly"));
		}
		else
		{
			Specifiers.Add(TEXT("EditAnywhere"));
		}
	}

	// Blueprint visibility
	const bool bBPVisible = (PropertyFlags & CPF_BlueprintVisible) != 0;
	const bool bBPReadOnly = (PropertyFlags & CPF_BlueprintReadOnly) != 0;

	if (bBPVisible)
	{
		if (bBPReadOnly)
		{
			Specifiers.Add(TEXT("BlueprintReadOnly"));
		}
		else
		{
			Specifiers.Add(TEXT("BlueprintReadWrite"));
		}
	}

	// Replication
	if (PropertyFlags & CPF_Net)
	{
		if (PropertyFlags & CPF_RepNotify)
		{
			// RepNotify name will be filled in by scaffold tool
			Specifiers.Add(TEXT("ReplicatedUsing=OnRep_TODO"));
		}
		else
		{
			Specifiers.Add(TEXT("Replicated"));
		}
	}

	// Other flags
	if (PropertyFlags & CPF_Interp)
	{
		Specifiers.Add(TEXT("Interp"));
	}
	if (PropertyFlags & CPF_Transient)
	{
		Specifiers.Add(TEXT("Transient"));
	}
	if (PropertyFlags & CPF_Config)
	{
		Specifiers.Add(TEXT("Config"));
	}
	if (PropertyFlags & CPF_SaveGame)
	{
		Specifiers.Add(TEXT("SaveGame"));
	}

	// Category
	if (!Category.IsEmpty())
	{
		Specifiers.Add(FString::Printf(TEXT("Category=\"%s\""), *Category));
	}

	return FString::Join(Specifiers, TEXT(", "));
}

FString FClaireonBPNodeMapper::FunctionFlagsToSpecifiers(uint64 FunctionFlags, const FString& Category) const
{
	TArray<FString> Specifiers;

	if (FunctionFlags & FUNC_BlueprintPure)
	{
		Specifiers.Add(TEXT("BlueprintPure"));
	}
	else if (FunctionFlags & FUNC_BlueprintCallable)
	{
		Specifiers.Add(TEXT("BlueprintCallable"));
	}

	if (FunctionFlags & FUNC_BlueprintEvent)
	{
		// Distinguish between native event and implementable event
		if (FunctionFlags & FUNC_Native)
		{
			Specifiers.Add(TEXT("BlueprintNativeEvent"));
		}
		else
		{
			Specifiers.Add(TEXT("BlueprintImplementableEvent"));
		}
	}

	if (FunctionFlags & FUNC_Net)
	{
		if (FunctionFlags & FUNC_NetServer)
		{
			Specifiers.Add(TEXT("Server"));
		}
		else if (FunctionFlags & FUNC_NetClient)
		{
			Specifiers.Add(TEXT("Client"));
		}
		else if (FunctionFlags & FUNC_NetMulticast)
		{
			Specifiers.Add(TEXT("NetMulticast"));
		}
	}

	if (FunctionFlags & FUNC_BlueprintAuthorityOnly)
	{
		Specifiers.Add(TEXT("BlueprintAuthorityOnly"));
	}

	if (!Category.IsEmpty())
	{
		Specifiers.Add(FString::Printf(TEXT("Category=\"%s\""), *Category));
	}

	return FString::Join(Specifiers, TEXT(", "));
}

FString FClaireonBPNodeMapper::InferIncludePath(const FEdGraphPinType& PinType) const
{
	// Primitives and CoreMinimal types need no include
	static const TSet<FName> CoreMinimalTypes = {
		TEXT("Vector"), TEXT("Vector2D"), TEXT("Vector4"),
		TEXT("Rotator"), TEXT("Transform"), TEXT("Quat"),
		TEXT("Color"), TEXT("LinearColor"),
		TEXT("Guid"), TEXT("DateTime"), TEXT("Timespan"),
		TEXT("IntPoint"), TEXT("IntVector"),
		TEXT("Box"), TEXT("Box2D"),
		TEXT("Matrix"), TEXT("Plane"),
	};

	// No include needed for primitive categories
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean
		|| PinType.PinCategory == UEdGraphSchema_K2::PC_Byte
		|| PinType.PinCategory == UEdGraphSchema_K2::PC_Int
		|| PinType.PinCategory == UEdGraphSchema_K2::PC_Int64
		|| PinType.PinCategory == UEdGraphSchema_K2::PC_Real
		|| PinType.PinCategory == UEdGraphSchema_K2::PC_String
		|| PinType.PinCategory == UEdGraphSchema_K2::PC_Name
		|| PinType.PinCategory == UEdGraphSchema_K2::PC_Text
		|| PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
	{
		return FString();
	}

	if (!PinType.PinSubCategoryObject.IsValid())
	{
		return FString();
	}

	UObject* TypeObj = PinType.PinSubCategoryObject.Get();
	FString TypeName = TypeObj->GetName();

	// Check CoreMinimal types (structs)
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
	{
		if (CoreMinimalTypes.Contains(FName(*TypeName)))
		{
			return FString();
		}
	}

	// Try to resolve include from the class's package
	if (UClass* TypeClass = Cast<UClass>(TypeObj))
	{
		// Try authoritative metadata first
		FString ModuleRelPath = TypeClass->GetMetaData(TEXT("ModuleRelativePath"));
		if (!ModuleRelPath.IsEmpty())
		{
			return StripModuleRelativePrefix(ModuleRelPath); // V4-7
		}
		// Fallback: heuristic
		FString ModuleName = TypeClass->GetOutermost()->GetName();
		ModuleName.RemoveFromStart(TEXT("/Script/"));
		return FString::Printf(TEXT("%s/%s%s.h"), *ModuleName, TypeClass->GetPrefixCPP(), *TypeClass->GetName());
	}

	if (UScriptStruct* TypeStruct = Cast<UScriptStruct>(TypeObj))
	{
		// Try authoritative metadata first
		FString ModuleRelPath = TypeStruct->GetMetaData(TEXT("ModuleRelativePath"));
		if (!ModuleRelPath.IsEmpty())
		{
			return StripModuleRelativePrefix(ModuleRelPath); // V4-7
		}
		// Fallback: heuristic
		FString ModuleName = TypeStruct->GetOutermost()->GetName();
		ModuleName.RemoveFromStart(TEXT("/Script/"));
		return FString::Printf(TEXT("%s/%s.h"), *ModuleName, *TypeStruct->GetStructCPPName());
	}

	if (UEnum* TypeEnum = Cast<UEnum>(TypeObj))
	{
		// Try authoritative metadata first
		FString ModuleRelPath = TypeEnum->GetMetaData(TEXT("ModuleRelativePath"));
		if (!ModuleRelPath.IsEmpty())
		{
			return StripModuleRelativePrefix(ModuleRelPath); // V4-7
		}
		// Fallback: heuristic
		FString ModuleName = TypeEnum->GetOutermost()->GetName();
		ModuleName.RemoveFromStart(TEXT("/Script/"));
		return FString::Printf(TEXT("%s/%s.h"), *ModuleName, *TypeEnum->GetName());
	}

	// Fallback: emit TODO tag
	return FString::Printf(TEXT("//[BP:INCLUDE_TODO] Type=%s"), *TypeName);
}

FString FClaireonBPNodeMapper::FormatBPTag(const UEdGraphNode* Node) const
{
	if (!Node)
	{
		return TEXT("// [BP:NODE] Guid={null} Type=Unknown Name=\"\"");
	}

	FString GuidStr = GetNodeGuidStr(Node);
	FString TypeStr = Node->GetClass()->GetName();
	FString NameStr = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();

	return FString::Printf(TEXT("// [BP:NODE] Guid=%s Type=%s Name=\"%s\""), *GuidStr, *TypeStr, *NameStr);
}

FString FClaireonBPNodeMapper::MapCallFunctionNode(const UEdGraphNode* Node, int32 IndentLevel)
{
	const UK2Node_CallFunction* CallNode = CastChecked<UK2Node_CallFunction>(Node);
	FString Indent = MakeIndent(IndentLevel);
	FString Output;

	Output += FString::Printf(TEXT("%s%s\n"), *Indent, *FormatBPTag(Node));

	// Get function name
	FString FuncName = CallNode->FunctionReference.GetMemberName().ToString();
	FuncName = SanitizeCppIdentifier(FuncName);

	// Collect input parameters (non-exec, non-self pins)
	TArray<FString> Params;
	FString TargetExpr;
	FString ReturnType;
	FString ReturnVarName;

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			continue;
		}
		if (Pin->PinName == UEdGraphSchema_K2::PN_Self)
		{
			if (Pin->LinkedTo.Num() > 0)
			{
				TargetExpr = GetConnectedPinExpression(Pin);
			}
			continue;
		}
		if (Pin->Direction == EGPD_Input)
		{
			Params.Add(GetConnectedPinExpression(Pin));
		}
		else if (Pin->Direction == EGPD_Output && Pin->PinName == UEdGraphSchema_K2::PN_ReturnValue)
		{
			ReturnType = PinTypeToCppType(Pin->PinType);
			ReturnVarName = FString::Printf(TEXT("%sResult"), *FuncName);
		}
	}

	FString ParamStr = FString::Join(Params, TEXT(", "));

	if (!ReturnType.IsEmpty())
	{
		if (!TargetExpr.IsEmpty())
		{
			Output += FString::Printf(TEXT("%s%s %s = %s->%s(%s);\n"), *Indent, *ReturnType, *ReturnVarName, *TargetExpr, *FuncName, *ParamStr);
		}
		else
		{
			Output += FString::Printf(TEXT("%s%s %s = %s(%s);\n"), *Indent, *ReturnType, *ReturnVarName, *FuncName, *ParamStr);
		}
	}
	else
	{
		if (!TargetExpr.IsEmpty())
		{
			Output += FString::Printf(TEXT("%s%s->%s(%s);\n"), *Indent, *TargetExpr, *FuncName, *ParamStr);
		}
		else
		{
			Output += FString::Printf(TEXT("%s%s(%s);\n"), *Indent, *FuncName, *ParamStr);
		}
	}

	return Output;
}

FString FClaireonBPNodeMapper::MapVariableGetNode(const UEdGraphNode* Node, int32 IndentLevel) const
{
	const UK2Node_VariableGet* VarNode = CastChecked<UK2Node_VariableGet>(Node);
	FString Indent = MakeIndent(IndentLevel);
	FString Output;

	Output += FString::Printf(TEXT("%s%s\n"), *Indent, *FormatBPTag(Node));

	FString VarName = VarNode->GetVarName().ToString();

	// Find the output pin to determine the type
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
		{
			FString CppType = PinTypeToCppType(Pin->PinType);
			Output += FString::Printf(TEXT("%s// (pure get) %s %s\n"), *Indent, *CppType, *VarName);
			break;
		}
	}

	return Output;
}

FString FClaireonBPNodeMapper::MapVariableSetNode(const UEdGraphNode* Node, int32 IndentLevel)
{
	const UK2Node_VariableSet* VarNode = CastChecked<UK2Node_VariableSet>(Node);
	FString Indent = MakeIndent(IndentLevel);
	FString Output;

	Output += FString::Printf(TEXT("%s%s\n"), *Indent, *FormatBPTag(Node));

	// V7-space: Sanitize variable names to valid C++ identifiers
	FString VarName = SanitizeCppIdentifier(VarNode->GetVarName().ToString());

	// Find the input data pin
	FString ValueExpr = TEXT("/* value */");
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Input
			&& Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
			&& Pin->PinName != UEdGraphSchema_K2::PN_Self)
		{
			ValueExpr = GetConnectedPinExpression(Pin);
			break;
		}
	}

	Output += FString::Printf(TEXT("%s%s = %s;\n"), *Indent, *VarName, *ValueExpr);
	return Output;
}

FString FClaireonBPNodeMapper::MapBranchNode(const UEdGraphNode* Node, int32 IndentLevel) const
{
	FString Indent = MakeIndent(IndentLevel);
	FString GuidStr = GetNodeGuidStr(Node);
	FString Output;

	// Find condition pin
	FString ConditionExpr = TEXT("/* condition */");
	UEdGraphPin* CondPin = FindPin(Node, TEXT("Condition"), EGPD_Input);
	if (CondPin)
	{
		ConditionExpr = GetConnectedPinExpression(CondPin);
	}

	// Find true/false exec target GUIDs
	FString TrueGuid = TEXT("null");
	FString FalseGuid = TEXT("null");
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->Direction != EGPD_Output || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
		{
			continue;
		}
		if (Pin->PinName == UEdGraphSchema_K2::PN_Then || Pin->PinName == TEXT("True"))
		{
			if (Pin->LinkedTo.Num() > 0 && Pin->LinkedTo[0]->GetOwningNode())
			{
				TrueGuid = GetNodeGuidStr(Pin->LinkedTo[0]->GetOwningNode());
			}
		}
		else if (Pin->PinName == UEdGraphSchema_K2::PN_Else || Pin->PinName == TEXT("False"))
		{
			if (Pin->LinkedTo.Num() > 0 && Pin->LinkedTo[0]->GetOwningNode())
			{
				FalseGuid = GetNodeGuidStr(Pin->LinkedTo[0]->GetOwningNode());
			}
		}
	}

	Output += FString::Printf(TEXT("%s// [BP:BRANCH] Guid=%s Condition=%s TrueExec=%s FalseExec=%s\n"),
		*Indent, *GuidStr, *ConditionExpr, *TrueGuid, *FalseGuid);
	Output += FString::Printf(TEXT("%sif (%s)\n"), *Indent, *ConditionExpr);
	Output += FString::Printf(TEXT("%s{\n"), *Indent);
	Output += FString::Printf(TEXT("%s\t// [BP:EXEC] From=%s[True] To=%s\n"), *Indent, *GuidStr, *TrueGuid);
	Output += FString::Printf(TEXT("%s\t// [BP] TODO: implement true branch\n"), *Indent);
	Output += FString::Printf(TEXT("%s}\n"), *Indent);
	Output += FString::Printf(TEXT("%selse\n"), *Indent);
	Output += FString::Printf(TEXT("%s{\n"), *Indent);
	Output += FString::Printf(TEXT("%s\t// [BP:EXEC] From=%s[False] To=%s\n"), *Indent, *GuidStr, *FalseGuid);
	Output += FString::Printf(TEXT("%s\t// [BP] TODO: implement false branch\n"), *Indent);
	Output += FString::Printf(TEXT("%s}\n"), *Indent);

	return Output;
}

FString FClaireonBPNodeMapper::MapReturnNode(const UEdGraphNode* Node, int32 IndentLevel) const
{
	FString Indent = MakeIndent(IndentLevel);
	FString Output;

	Output += FString::Printf(TEXT("%s%s\n"), *Indent, *FormatBPTag(Node));

	// Find return value input pin
	FString ReturnExpr;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Input
			&& Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
		{
			ReturnExpr = GetConnectedPinExpression(Pin);
			break;
		}
	}

	if (ReturnExpr.IsEmpty())
	{
		Output += FString::Printf(TEXT("%sreturn;\n"), *Indent);
	}
	else
	{
		Output += FString::Printf(TEXT("%sreturn %s;\n"), *Indent, *ReturnExpr);
	}

	return Output;
}

FString FClaireonBPNodeMapper::MapCastNode(const UEdGraphNode* Node, int32 IndentLevel)
{
	const UK2Node_DynamicCast* CastNode = CastChecked<UK2Node_DynamicCast>(Node);
	FString Indent = MakeIndent(IndentLevel);
	FString GuidStr = GetNodeGuidStr(Node);
	FString Output;

	Output += FString::Printf(TEXT("%s%s\n"), *Indent, *FormatBPTag(Node));

	FString TargetClassName = CastNode->TargetType ? FString::Printf(TEXT("%s%s"),
		CastNode->TargetType->GetPrefixCPP(), *CastNode->TargetType->GetName()) : TEXT("UObject");

	// Find source object pin
	FString SourceExpr = TEXT("/* source */");
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Input
			&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object)
		{
			SourceExpr = GetConnectedPinExpression(Pin);
			break;
		}
	}

	Output += FString::Printf(TEXT("%s%s* CastResult = Cast<%s>(%s);\n"),
		*Indent, *TargetClassName, *TargetClassName, *SourceExpr);

	return Output;
}

FString FClaireonBPNodeMapper::MapSpawnActorNode(const UEdGraphNode* Node, int32 IndentLevel)
{
	FString Indent = MakeIndent(IndentLevel);
	FString Output;

	Output += FString::Printf(TEXT("%s%s\n"), *Indent, *FormatBPTag(Node));

	// Find class pin for spawn type
	FString SpawnClassName = TEXT("AActor");
	FString ClassExpr = TEXT("/* class */");
	FString TransformExpr = TEXT("FTransform::Identity");

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			continue;
		}
		if (Pin->Direction == EGPD_Input)
		{
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class)
			{
				ClassExpr = GetConnectedPinExpression(Pin);
				if (Pin->PinType.PinSubCategoryObject.IsValid())
				{
					UClass* ObjClass = Cast<UClass>(Pin->PinType.PinSubCategoryObject.Get());
					if (ObjClass)
					{
						SpawnClassName = FString::Printf(TEXT("%s%s"),
							ObjClass->GetPrefixCPP(), *ObjClass->GetName());
					}
				}
			}
			else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
			{
				TransformExpr = GetConnectedPinExpression(Pin);
			}
		}
	}

	Output += FString::Printf(TEXT("%s%s* SpawnedActor = GetWorld()->SpawnActor<%s>(%s, %s);\n"),
		*Indent, *SpawnClassName, *SpawnClassName, *ClassExpr, *TransformExpr);

	return Output;
}

FString FClaireonBPNodeMapper::MapEventNode(const UEdGraphNode* Node, int32 IndentLevel) const
{
	const UK2Node_Event* EventNode = CastChecked<UK2Node_Event>(Node);
	FString Indent = MakeIndent(IndentLevel);
	FString Output;

	FString EventName = EventNode->EventReference.GetMemberName().ToString();
	if (EventName.IsEmpty())
	{
		EventName = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
	}
	Output += FString::Printf(TEXT("%s// [BP:NODE] Guid=%s Type=Event Name=\"%s\"\n"),
		*Indent, *GetNodeGuidStr(Node), *EventName);
	Output += FString::Printf(TEXT("%s// Event: %s\n"), *Indent, *EventName);

	return Output;
}

FString FClaireonBPNodeMapper::MapCustomEventNode(const UEdGraphNode* Node, int32 IndentLevel) const
{
	const UK2Node_CustomEvent* EventNode = CastChecked<UK2Node_CustomEvent>(Node);
	FString Indent = MakeIndent(IndentLevel);
	FString Output;

	FString EventName = EventNode->CustomFunctionName.ToString();
	Output += FString::Printf(TEXT("%s// [BP:NODE] Guid=%s Type=CustomEvent Name=\"%s\"\n"),
		*Indent, *GetNodeGuidStr(Node), *EventName);

	// Collect output pins as parameters
	TArray<FString> Params;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output
			&& Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
			&& Pin->PinName != UEdGraphSchema_K2::PN_Then
			&& Pin->PinName != UK2Node_Event::DelegateOutputName) // V5-7: filter BP-internal delegate pin
		{
			FString ParamType = PinTypeToCppType(Pin->PinType);
			Params.Add(FString::Printf(TEXT("%s %s"), *ParamType, *Pin->PinName.ToString()));
		}
	}

	FString ParamStr = FString::Join(Params, TEXT(", "));
	Output += FString::Printf(TEXT("%svoid %s(%s)\n"), *Indent, *EventName, *ParamStr);
	Output += FString::Printf(TEXT("%s{\n"), *Indent);
	Output += FString::Printf(TEXT("%s\t// [BP] TODO: implement custom event \"%s\"\n"), *Indent, *EventName);
	Output += FString::Printf(TEXT("%s}\n"), *Indent);

	return Output;
}

FString FClaireonBPNodeMapper::MapCallParentFunctionNode(const UEdGraphNode* Node, int32 IndentLevel)
{
	const UK2Node_CallFunction* CallNode = CastChecked<UK2Node_CallFunction>(Node);
	FString Indent = MakeIndent(IndentLevel);
	FString Output;

	Output += FString::Printf(TEXT("%s%s\n"), *Indent, *FormatBPTag(Node));

	FString FuncName = CallNode->FunctionReference.GetMemberName().ToString();

	// Collect input parameters (non-exec, non-self)
	TArray<FString> Params;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
		if (Pin->PinName == UEdGraphSchema_K2::PN_Self) continue;
		if (Pin->Direction == EGPD_Input)
		{
			Params.Add(GetConnectedPinExpression(Pin));
		}
	}

	FString ParamStr = FString::Join(Params, TEXT(", "));
	Output += FString::Printf(TEXT("%sSuper::%s(%s);\n"), *Indent, *FuncName, *ParamStr);

	return Output;
}

FString FClaireonBPNodeMapper::MapCallDelegateNode(const UEdGraphNode* Node, int32 IndentLevel)
{
	const UK2Node_CallDelegate* DelegateNode = CastChecked<UK2Node_CallDelegate>(Node);
	FString Indent = MakeIndent(IndentLevel);
	FString Output;

	Output += FString::Printf(TEXT("%s%s\n"), *Indent, *FormatBPTag(Node));

	FString DelegateName = DelegateNode->GetPropertyName().ToString();

	// Collect parameters (non-exec, non-self, non-delegate pins)
	TArray<FString> Params;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
		if (Pin->PinName == UEdGraphSchema_K2::PN_Self) continue;
		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Delegate
			|| Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate) continue;
		if (Pin->Direction == EGPD_Input)
		{
			Params.Add(GetConnectedPinExpression(Pin));
		}
	}

	FString ParamStr = FString::Join(Params, TEXT(", "));
	Output += FString::Printf(TEXT("%s%s.Broadcast(%s);\n"), *Indent, *DelegateName, *ParamStr);

	return Output;
}

FString FClaireonBPNodeMapper::MapClearDelegateNode(const UEdGraphNode* Node, int32 IndentLevel)
{
	const UK2Node_ClearDelegate* ClearNode = CastChecked<UK2Node_ClearDelegate>(Node);
	FString Indent = MakeIndent(IndentLevel);
	FString Output;

	Output += FString::Printf(TEXT("%s%s\n"), *Indent, *FormatBPTag(Node));

	FString DelegateName = ClearNode->GetPropertyName().ToString();
	Output += FString::Printf(TEXT("%s%s.Clear();\n"), *Indent, *DelegateName);

	return Output;
}

FString FClaireonBPNodeMapper::MapAddDelegateNode(const UEdGraphNode* Node, int32 IndentLevel)
{
	const UK2Node_AddDelegate* AddNode = CastChecked<UK2Node_AddDelegate>(Node);
	FString Indent = MakeIndent(IndentLevel);
	FString Output;

	Output += FString::Printf(TEXT("%s%s\n"), *Indent, *FormatBPTag(Node));

	FString DelegateName = AddNode->GetPropertyName().ToString();

	// Try to find the connected CreateDelegate node to get the handler function name
	FString HandlerFunc = TEXT("/* handler */");
	UEdGraphPin* DelegatePin = FindPin(Node, TEXT("Delegate"), EGPD_Input);
	if (DelegatePin && DelegatePin->LinkedTo.Num() > 0)
	{
		UEdGraphNode* LinkedNode = DelegatePin->LinkedTo[0]->GetOwningNode();
		if (UK2Node_CreateDelegate* CreateDel = Cast<UK2Node_CreateDelegate>(LinkedNode))
		{
			HandlerFunc = CreateDel->GetFunctionName().ToString();
		}
	}

	Output += FString::Printf(TEXT("%s%s.AddDynamic(this, &ThisClass::%s);\n"),
		*Indent, *DelegateName, *HandlerFunc);

	return Output;
}

FString FClaireonBPNodeMapper::MapDelegateNode(const UEdGraphNode* Node, int32 IndentLevel)
{
	FString Indent = MakeIndent(IndentLevel);
	FString Output;

	Output += FString::Printf(TEXT("%s%s\n"), *Indent, *FormatBPTag(Node));

	if (Cast<UK2Node_AddDelegate>(Node))
	{
		Output += FString::Printf(TEXT("%s// [BP] TODO: implement AddDynamic delegate binding\n"), *Indent);
		Output += FString::Printf(TEXT("%s// Delegate.AddDynamic(this, &ThisClass::HandlerFunction);\n"), *Indent);
	}
	else if (Cast<UK2Node_RemoveDelegate>(Node))
	{
		Output += FString::Printf(TEXT("%s// [BP] TODO: implement RemoveDynamic delegate unbinding\n"), *Indent);
		Output += FString::Printf(TEXT("%s// Delegate.RemoveDynamic(this, &ThisClass::HandlerFunction);\n"), *Indent);
	}
	else if (Cast<UK2Node_CreateDelegate>(Node))
	{
		Output += FString::Printf(TEXT("%s// [BP] TODO: implement delegate creation\n"), *Indent);
	}

	return Output;
}

FString FClaireonBPNodeMapper::MapMakeStructNode(const UEdGraphNode* Node, int32 IndentLevel) const
{
	const UK2Node_MakeStruct* MakeNode = CastChecked<UK2Node_MakeStruct>(Node);
	FString Indent = MakeIndent(IndentLevel);
	FString Output;

	Output += FString::Printf(TEXT("%s%s\n"), *Indent, *FormatBPTag(Node));

	FString StructName = MakeNode->StructType ? MakeNode->StructType->GetStructCPPName() : TEXT("FUnknownStruct");

	Output += FString::Printf(TEXT("%s%s NewStruct;\n"), *Indent, *StructName);

	// Set members from input pins
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Input
			&& Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
			&& !Pin->bHidden)
		{
			FString MemberName = Pin->PinName.ToString();
			FString ValueExpr = GetConnectedPinExpression(Pin);
			Output += FString::Printf(TEXT("%sNewStruct.%s = %s;\n"), *Indent, *MemberName, *ValueExpr);
		}
	}

	return Output;
}

FString FClaireonBPNodeMapper::MapBreakStructNode(const UEdGraphNode* Node, int32 IndentLevel) const
{
	const UK2Node_BreakStruct* BreakNode = CastChecked<UK2Node_BreakStruct>(Node);
	FString Indent = MakeIndent(IndentLevel);
	FString Output;

	Output += FString::Printf(TEXT("%s%s\n"), *Indent, *FormatBPTag(Node));

	FString StructName = BreakNode->StructType ? BreakNode->StructType->GetStructCPPName() : TEXT("FUnknownStruct");

	// Find the struct input
	FString StructExpr = TEXT("/* struct */");
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Input
			&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
		{
			StructExpr = GetConnectedPinExpression(Pin);
			break;
		}
	}

	Output += FString::Printf(TEXT("%s// Break %s: %s\n"), *Indent, *StructName, *StructExpr);

	// List output members
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output && !Pin->bHidden)
		{
			FString MemberType = PinTypeToCppType(Pin->PinType);
			FString MemberName = Pin->PinName.ToString();
			Output += FString::Printf(TEXT("%s%s %s = %s.%s;\n"), *Indent, *MemberType, *MemberName, *StructExpr, *MemberName);
		}
	}

	return Output;
}

FString FClaireonBPNodeMapper::MapArrayOperationNode(const UEdGraphNode* Node, int32 IndentLevel)
{
	const UK2Node_CallFunction* CallNode = CastChecked<UK2Node_CallFunction>(Node);
	FString Indent = MakeIndent(IndentLevel);
	FString Output;

	Output += FString::Printf(TEXT("%s%s\n"), *Indent, *FormatBPTag(Node));

	FString FuncName = CallNode->FunctionReference.GetMemberName().ToString();

	// Map common array operations to TArray methods
	FString ArrayExpr = TEXT("Array");
	FString Operation;

	// Find the array target pin
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Input
			&& Pin->PinType.ContainerType == EPinContainerType::Array)
		{
			ArrayExpr = GetConnectedPinExpression(Pin);
			break;
		}
	}

	if (FuncName.Contains(TEXT("Array_Add")))
	{
		Operation = TEXT("Add");
	}
	else if (FuncName.Contains(TEXT("Array_Remove")))
	{
		Operation = TEXT("RemoveAt");
	}
	else if (FuncName.Contains(TEXT("Array_Find")))
	{
		Operation = TEXT("Find");
	}
	else if (FuncName.Contains(TEXT("Array_Contains")))
	{
		Operation = TEXT("Contains");
	}
	else if (FuncName.Contains(TEXT("Array_Length")))
	{
		Operation = TEXT("Num");
	}
	else if (FuncName.Contains(TEXT("Array_Clear")))
	{
		Operation = TEXT("Empty");
	}
	else if (FuncName.Contains(TEXT("Array_Get")))
	{
		Operation = TEXT("operator[]");
	}
	else
	{
		Operation = FuncName;
	}

	Output += FString::Printf(TEXT("%s%s.%s(/* params */); // [BP] TODO: fill array operation params\n"),
		*Indent, *ArrayExpr, *Operation);

	return Output;
}

FString FClaireonBPNodeMapper::MapExecutionSequenceNode(const UEdGraphNode* Node, int32 IndentLevel) const
{
	FString Indent = MakeIndent(IndentLevel);
	FString GuidStr = GetNodeGuidStr(Node);
	FString Output;

	int32 OutputCount = 0;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output
			&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			++OutputCount;
		}
	}

	Output += FString::Printf(TEXT("%s// [BP:NODE] Guid=%s Type=ExecutionSequence OutputCount=%d\n"),
		*Indent, *GuidStr, OutputCount);

	for (int32 i = 0; i < OutputCount; ++i)
	{
		Output += FString::Printf(TEXT("%s{\n"), *Indent);
		Output += FString::Printf(TEXT("%s\t// [BP:SEQ] Index=%d From=%s\n"), *Indent, i, *GuidStr);
		Output += FString::Printf(TEXT("%s\t// [BP] TODO: implement sequence block %d\n"), *Indent, i);
		Output += FString::Printf(TEXT("%s}\n"), *Indent);
	}

	return Output;
}

FString FClaireonBPNodeMapper::MapSwitchEnumNode(const UEdGraphNode* Node, int32 IndentLevel) const
{
	const UK2Node_SwitchEnum* SwitchNode = CastChecked<UK2Node_SwitchEnum>(Node);
	FString Indent = MakeIndent(IndentLevel);
	FString GuidStr = GetNodeGuidStr(Node);
	FString Output;

	FString SelectionExpr = TEXT("/* selection */");
	UEdGraphPin* SelectionPin = FindPin(Node, TEXT("Selection"), EGPD_Input);
	if (SelectionPin)
	{
		SelectionExpr = GetConnectedPinExpression(SelectionPin);
	}

	Output += FString::Printf(TEXT("%s// [BP:NODE] Guid=%s Type=SwitchEnum\n"), *Indent, *GuidStr);
	Output += FString::Printf(TEXT("%sswitch (%s)\n"), *Indent, *SelectionExpr);
	Output += FString::Printf(TEXT("%s{\n"), *Indent);

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output
			&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
			&& Pin->PinName != TEXT("Default"))
		{
			Output += FString::Printf(TEXT("%s\tcase %s:\n"), *Indent, *Pin->PinName.ToString());
			Output += FString::Printf(TEXT("%s\t\t// [BP:EXEC] From=%s[%s]\n"), *Indent, *GuidStr, *Pin->PinName.ToString());
			Output += FString::Printf(TEXT("%s\t\t// [BP] TODO: implement case\n"), *Indent);
			Output += FString::Printf(TEXT("%s\t\tbreak;\n"), *Indent);
		}
	}

	Output += FString::Printf(TEXT("%s\tdefault:\n"), *Indent);
	Output += FString::Printf(TEXT("%s\t\t// [BP:EXEC] From=%s[Default]\n"), *Indent, *GuidStr);
	Output += FString::Printf(TEXT("%s\t\tbreak;\n"), *Indent);
	Output += FString::Printf(TEXT("%s}\n"), *Indent);

	return Output;
}

FString FClaireonBPNodeMapper::MapSwitchIntegerNode(const UEdGraphNode* Node, int32 IndentLevel) const
{
	FString Indent = MakeIndent(IndentLevel);
	FString GuidStr = GetNodeGuidStr(Node);
	FString Output;

	FString SelectionExpr = TEXT("/* selection */");
	UEdGraphPin* SelectionPin = FindPin(Node, TEXT("Selection"), EGPD_Input);
	if (SelectionPin)
	{
		SelectionExpr = GetConnectedPinExpression(SelectionPin);
	}

	Output += FString::Printf(TEXT("%s// [BP:NODE] Guid=%s Type=SwitchInteger\n"), *Indent, *GuidStr);
	Output += FString::Printf(TEXT("%sswitch (%s)\n"), *Indent, *SelectionExpr);
	Output += FString::Printf(TEXT("%s{\n"), *Indent);

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output
			&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
			&& Pin->PinName != TEXT("Default"))
		{
			Output += FString::Printf(TEXT("%s\tcase %s:\n"), *Indent, *Pin->PinName.ToString());
			Output += FString::Printf(TEXT("%s\t\t// [BP:EXEC] From=%s[%s]\n"), *Indent, *GuidStr, *Pin->PinName.ToString());
			Output += FString::Printf(TEXT("%s\t\t// [BP] TODO: implement case\n"), *Indent);
			Output += FString::Printf(TEXT("%s\t\tbreak;\n"), *Indent);
		}
	}

	Output += FString::Printf(TEXT("%s\tdefault:\n"), *Indent);
	Output += FString::Printf(TEXT("%s\t\t// [BP:EXEC] From=%s[Default]\n"), *Indent, *GuidStr);
	Output += FString::Printf(TEXT("%s\t\tbreak;\n"), *Indent);
	Output += FString::Printf(TEXT("%s}\n"), *Indent);

	return Output;
}

FString FClaireonBPNodeMapper::MapSwitchStringNode(const UEdGraphNode* Node, int32 IndentLevel) const
{
	FString Indent = MakeIndent(IndentLevel);
	FString GuidStr = GetNodeGuidStr(Node);
	FString Output;

	FString SelectionExpr = TEXT("/* selection */");
	UEdGraphPin* SelectionPin = FindPin(Node, TEXT("Selection"), EGPD_Input);
	if (SelectionPin) SelectionExpr = GetConnectedPinExpression(SelectionPin);

	Output += FString::Printf(TEXT("%s// [BP:NODE] Guid=%s Type=SwitchString\n"), *Indent, *GuidStr);

	bool bFirst = true;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output
			&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
			&& Pin->PinName != TEXT("Default"))
		{
			if (bFirst)
			{
				Output += FString::Printf(TEXT("%sif (%s == TEXT(\"%s\"))\n"), *Indent, *SelectionExpr, *Pin->PinName.ToString());
				bFirst = false;
			}
			else
			{
				Output += FString::Printf(TEXT("%selse if (%s == TEXT(\"%s\"))\n"), *Indent, *SelectionExpr, *Pin->PinName.ToString());
			}
			Output += FString::Printf(TEXT("%s{\n"), *Indent);
			Output += FString::Printf(TEXT("%s\t// [BP:EXEC] From=%s[%s]\n"), *Indent, *GuidStr, *Pin->PinName.ToString());
			Output += FString::Printf(TEXT("%s\t// [BP] TODO: implement case\n"), *Indent);
			Output += FString::Printf(TEXT("%s}\n"), *Indent);
		}
	}

	Output += FString::Printf(TEXT("%selse\n"), *Indent);
	Output += FString::Printf(TEXT("%s{\n"), *Indent);
	Output += FString::Printf(TEXT("%s\t// [BP:EXEC] From=%s[Default]\n"), *Indent, *GuidStr);
	Output += FString::Printf(TEXT("%s\t// [BP] TODO: implement default\n"), *Indent);
	Output += FString::Printf(TEXT("%s}\n"), *Indent);

	return Output;
}

FString FClaireonBPNodeMapper::MapSwitchNameNode(const UEdGraphNode* Node, int32 IndentLevel) const
{
	FString Indent = MakeIndent(IndentLevel);
	FString GuidStr = GetNodeGuidStr(Node);
	FString Output;

	FString SelectionExpr = TEXT("/* selection */");
	UEdGraphPin* SelectionPin = FindPin(Node, TEXT("Selection"), EGPD_Input);
	if (SelectionPin) SelectionExpr = GetConnectedPinExpression(SelectionPin);

	Output += FString::Printf(TEXT("%s// [BP:NODE] Guid=%s Type=SwitchName\n"), *Indent, *GuidStr);

	bool bFirst = true;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output
			&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
			&& Pin->PinName != TEXT("Default"))
		{
			if (bFirst)
			{
				Output += FString::Printf(TEXT("%sif (%s == FName(TEXT(\"%s\")))\n"), *Indent, *SelectionExpr, *Pin->PinName.ToString());
				bFirst = false;
			}
			else
			{
				Output += FString::Printf(TEXT("%selse if (%s == FName(TEXT(\"%s\")))\n"), *Indent, *SelectionExpr, *Pin->PinName.ToString());
			}
			Output += FString::Printf(TEXT("%s{\n"), *Indent);
			Output += FString::Printf(TEXT("%s\t// [BP:EXEC] From=%s[%s]\n"), *Indent, *GuidStr, *Pin->PinName.ToString());
			Output += FString::Printf(TEXT("%s\t// [BP] TODO: implement case\n"), *Indent);
			Output += FString::Printf(TEXT("%s}\n"), *Indent);
		}
	}

	Output += FString::Printf(TEXT("%selse\n"), *Indent);
	Output += FString::Printf(TEXT("%s{\n"), *Indent);
	Output += FString::Printf(TEXT("%s\t// [BP:EXEC] From=%s[Default]\n"), *Indent, *GuidStr);
	Output += FString::Printf(TEXT("%s\t// [BP] TODO: implement default\n"), *Indent);
	Output += FString::Printf(TEXT("%s}\n"), *Indent);

	return Output;
}

FString FClaireonBPNodeMapper::MapFunctionEntryNode(const UEdGraphNode* Node, int32 IndentLevel) const
{
	FString Indent = MakeIndent(IndentLevel);
	return FString::Printf(TEXT("%s// [BP:NODE] Guid=%s Type=FunctionEntry\n"), *Indent, *GetNodeGuidStr(Node));
}

FString FClaireonBPNodeMapper::MapTimelineNode(const UEdGraphNode* Node, int32 IndentLevel)
{
	const UK2Node_Timeline* TimelineNode = CastChecked<UK2Node_Timeline>(Node);
	FString Indent = MakeIndent(IndentLevel);
	FString GuidStr = GetNodeGuidStr(Node);
	FString Output;

	FString TimelineName = TimelineNode->TimelineName.ToString();

	Output += FString::Printf(TEXT("%s// [BP:NODE] Guid=%s Type=Timeline Name=\"%s\"\n"),
		*Indent, *GuidStr, *TimelineName);

	// Emit timeline play/reverse/stop based on connected exec pins
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->Direction != EGPD_Input
			|| Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
		{
			continue;
		}

		FString PinName = Pin->PinName.ToString();
		if (PinName == TEXT("Play"))
		{
			Output += FString::Printf(TEXT("%s%s.Play();\n"), *Indent, *TimelineName);
		}
		else if (PinName == TEXT("PlayFromStart"))
		{
			Output += FString::Printf(TEXT("%s%s.PlayFromStart();\n"), *Indent, *TimelineName);
		}
		else if (PinName == TEXT("Stop"))
		{
			Output += FString::Printf(TEXT("%s%s.Stop();\n"), *Indent, *TimelineName);
		}
		else if (PinName == TEXT("Reverse"))
		{
			Output += FString::Printf(TEXT("%s%s.Reverse();\n"), *Indent, *TimelineName);
		}
		else if (PinName == TEXT("ReverseFromEnd"))
		{
			Output += FString::Printf(TEXT("%s%s.ReverseFromEnd();\n"), *Indent, *TimelineName);
		}
	}

	// Emit callback declarations for Update and Finished events
	Output += FString::Printf(TEXT("%s// [BP:TIMELINE_CALLBACKS] Timeline=%s\n"), *Indent, *TimelineName);
	Output += FString::Printf(TEXT("%s// TODO: Declare FTimeline %s member, UCurveFloat* per track\n"), *Indent, *TimelineName);
	Output += FString::Printf(TEXT("%s// TODO: Bind %s_Update and %s_Finished callbacks\n"),
		*Indent, *TimelineName, *TimelineName);
	Output += FString::Printf(TEXT("%s// TODO: Call %s.TickTimeline(DeltaTime) in Tick()\n"),
		*Indent, *TimelineName);
	Output += FString::Printf(TEXT("%s// TODO: Set PrimaryActorTick.bCanEverTick = true in constructor\n"), *Indent);

	AddInclude(TEXT("Components/TimelineComponent.h"));

	return Output;
}

FString FClaireonBPNodeMapper::MapAsyncActionNode(const UEdGraphNode* Node, int32 IndentLevel)
{
	const UK2Node_BaseAsyncTask* AsyncNode = CastChecked<UK2Node_BaseAsyncTask>(Node);
	FString Indent = MakeIndent(IndentLevel);
	FString GuidStr = GetNodeGuidStr(Node);
	FString Output;

	Output += FString::Printf(TEXT("%s%s\n"), *Indent, *FormatBPTag(Node));

	// Read protected UPROPERTY members via reflection (no public accessors)
	FName ProxyFactoryFuncName;
	UClass* ProxyClassPtr = nullptr;
	UClass* FactoryClassPtr = nullptr;

	if (const FNameProperty* FuncNameProp = CastField<FNameProperty>(AsyncNode->GetClass()->FindPropertyByName(TEXT("ProxyFactoryFunctionName"))))
	{
		ProxyFactoryFuncName = FuncNameProp->GetPropertyValue_InContainer(AsyncNode);
	}
	if (const FObjectPropertyBase* ProxyClassProp = CastField<FObjectPropertyBase>(AsyncNode->GetClass()->FindPropertyByName(TEXT("ProxyClass"))))
	{
		ProxyClassPtr = Cast<UClass>(ProxyClassProp->GetObjectPropertyValue_InContainer(AsyncNode));
	}
	if (const FObjectPropertyBase* FactoryClassProp = CastField<FObjectPropertyBase>(AsyncNode->GetClass()->FindPropertyByName(TEXT("ProxyFactoryClass"))))
	{
		FactoryClassPtr = Cast<UClass>(FactoryClassProp->GetObjectPropertyValue_InContainer(AsyncNode));
	}

	FString ProxyClassName = ProxyClassPtr ? FString::Printf(TEXT("%s%s"),
		ProxyClassPtr->GetPrefixCPP(), *ProxyClassPtr->GetName()) : TEXT("UObject");
	FString FactoryFunc = ProxyFactoryFuncName.IsNone() ? TEXT("Create") : ProxyFactoryFuncName.ToString();
	FString FactoryClassName = FactoryClassPtr ? FString::Printf(TEXT("%s%s"),
		FactoryClassPtr->GetPrefixCPP(), *FactoryClassPtr->GetName()) : ProxyClassName;

	// Collect input parameters
	TArray<FString> Params;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
		if (Pin->PinName == UEdGraphSchema_K2::PN_Self) continue;
		if (Pin->Direction == EGPD_Input)
		{
			Params.Add(GetConnectedPinExpression(Pin));
		}
	}
	FString ParamStr = FString::Join(Params, TEXT(", "));

	// Emit proxy member declaration TODO
	FString ProxyVarName = FString::Printf(TEXT("AsyncTask_%s"), *GuidStr.Left(8));
	Output += FString::Printf(TEXT("%s// TODO: Add UPROPERTY() %s* %s; to header\n"),
		*Indent, *ProxyClassName, *ProxyVarName);

	// Emit factory call
	Output += FString::Printf(TEXT("%s%s* %s = %s::%s(%s);\n"),
		*Indent, *ProxyClassName, *ProxyVarName, *FactoryClassName, *FactoryFunc, *ParamStr);

	// Emit AddDynamic bindings for each output delegate pin
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->Direction != EGPD_Output) continue;
		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			// Each exec output is a delegate pin on the async action
			FString DelegateName = Pin->PinName.ToString();
			if (DelegateName == TEXT("then")) continue; // skip the completion exec pin

			Output += FString::Printf(TEXT("%s// [BP:ASYNC_EXEC] Delegate=%s\n"), *Indent, *DelegateName);
			Output += FString::Printf(TEXT("%s%s->%s.AddDynamic(this, &ThisClass::On%s);\n"),
				*Indent, *ProxyVarName, *DelegateName, *DelegateName);
		}
	}

	// Add include for the proxy class
	if (ProxyClassPtr)
	{
		FString ProxyInclude = ProxyClassPtr->GetMetaData(TEXT("ModuleRelativePath"));
		if (!ProxyInclude.IsEmpty())
		{
			AddInclude(ProxyInclude);
		}
	}

	return Output;
}

FString FClaireonBPNodeMapper::MapUnknownNode(const UEdGraphNode* Node, int32 IndentLevel) const
{
	FString Indent = MakeIndent(IndentLevel);
	FString GuidStr = GetNodeGuidStr(Node);
	FString TypeStr = Node ? Node->GetClass()->GetName() : TEXT("null");
	FString NameStr = Node ? Node->GetNodeTitle(ENodeTitleType::ListView).ToString() : TEXT("");

	FString Output;
	Output += FString::Printf(TEXT("%s// [BP:UNKNOWN] Guid=%s Type=%s Name=\"%s\"\n"),
		*Indent, *GuidStr, *TypeStr, *NameStr);
	Output += FString::Printf(TEXT("%s// [BP] TODO: implement -- unknown node type, inspect via blueprint_get_graph\n"),
		*Indent);

	return Output;
}

// V3 scope-tree infrastructure: MapNodeEx and branch-aware Ex variants

FMapNodeResult FClaireonBPNodeMapper::MapNodeEx(const UEdGraphNode* Node, int32 IndentLevel, const UEdGraphPin* ArrivalPin)
{
	FMapNodeResult Result;

	// Check if this is a known macro
	FClaireonBPMacroHandler MacroHandler;
	if (Cast<UK2Node_MacroInstance>(Node) && MacroHandler.IsKnownMacro(Node))
	{
		return MacroHandler.HandleMacroEx(Node, IndentLevel, ArrivalPin);
	}

	// K2Node_IfThenElse (Branch node)
	if (Cast<UK2Node_IfThenElse>(Node))
	{
		return MapBranchNodeEx(Node, IndentLevel);
	}

	// K2Node_ExecutionSequence
	if (Cast<UK2Node_ExecutionSequence>(Node))
	{
		return MapExecutionSequenceNodeEx(Node, IndentLevel);
	}

	// Switch nodes
	if (Cast<UK2Node_SwitchEnum>(Node))    return MapSwitchEnumNodeEx(Node, IndentLevel);
	if (Cast<UK2Node_SwitchInteger>(Node)) return MapSwitchIntegerNodeEx(Node, IndentLevel);
	if (Cast<UK2Node_SwitchString>(Node))  return MapSwitchStringNodeEx(Node, IndentLevel);
	if (Cast<UK2Node_SwitchName>(Node))    return MapSwitchNameNodeEx(Node, IndentLevel);

	// SwitchGameplayTag (new in V3-3)
	if (Node && Node->GetClass()->GetName().Contains(TEXT("SwitchGameplayTag")))
	{
		return MapSwitchGameplayTagNodeEx(Node, IndentLevel);
	}

	// Timeline nodes (V3-5: per-exec-input dispatch)
	if (Cast<UK2Node_Timeline>(Node))
	{
		return MapTimelineNodeEx(Node, IndentLevel, ArrivalPin);
	}

	// Async action nodes (V3-6: valid names + scoped callbacks)
	if (Cast<UK2Node_BaseAsyncTask>(Node))
	{
		return MapAsyncActionNodeEx(Node, IndentLevel);
	}

	// V8: Latent K2Node_CallFunction detection (Delay, RetriggerableDelay, etc.)
	if (const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
	{
		if (const UFunction* TargetFunc = CallNode->GetTargetFunction())
		{
			for (TFieldIterator<FProperty> It(TargetFunc); It; ++It)
			{
				if (const FStructProperty* StructProp = CastField<FStructProperty>(*It))
				{
					if (StructProp->Struct == FLatentActionInfo::StaticStruct())
					{
						return MapLatentCallFunctionNodeEx(Node, IndentLevel);
					}
				}
			}
		}
	}

	// V4-3: Custom events during scope-tree traversal emit comment tag only (not nested function defs)
	// Must be checked before UK2Node_Event since UK2Node_CustomEvent derives from it
	if (const UK2Node_CustomEvent* CustomEventNode = Cast<UK2Node_CustomEvent>(Node))
	{
		FString EventName = CustomEventNode->CustomFunctionName.ToString();
		if (EventName.IsEmpty() || EventName == TEXT("None"))
		{
			EventName = CustomEventNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
		}
		EventName = SanitizeCppIdentifier(EventName);
		Result.Code = FString::Printf(TEXT("%s// [BP:CUSTOM_EVENT] %s\n"),
			*MakeIndent(IndentLevel), *EventName);
		Result.bIsBranchNode = false;
		return Result;
	}

	// V8: Dispatch Ex variants for nodes that produce member declarations
	if (Cast<UK2Node_CallFunction>(Node))
	{
		return MapCallFunctionNodeEx(Node, IndentLevel);
	}
	if (Cast<UK2Node_DynamicCast>(Node))
	{
		return MapCastNodeEx(Node, IndentLevel);
	}
	if (Cast<UK2Node_SpawnActorFromClass>(Node))
	{
		return MapSpawnActorNodeEx(Node, IndentLevel);
	}

	// Default: non-branch node (no member declarations needed)
	Result.Code = MapNode(Node, IndentLevel);
	Result.bIsBranchNode = false;
	return Result;
}

FMapNodeResult FClaireonBPNodeMapper::MapBranchNodeEx(const UEdGraphNode* Node, int32 IndentLevel) const
{
	FMapNodeResult Result;
	FString Indent = MakeIndent(IndentLevel);
	FString GuidStr = GetNodeGuidStr(Node);

	FString ConditionExpr = TEXT("/* condition */");
	UEdGraphPin* CondPin = FindPin(Node, TEXT("Condition"), EGPD_Input);
	if (CondPin)
	{
		ConditionExpr = GetConnectedPinExpression(CondPin);
	}

	Result.Code += FString::Printf(TEXT("%s// [BP:BRANCH] Guid=%s Condition=%s\n"),
		*Indent, *GuidStr, *ConditionExpr);
	Result.Code += FString::Printf(TEXT("%sif (%s)\n"), *Indent, *ConditionExpr);
	Result.Code += FString::Printf(TEXT("%s{\n"), *Indent);

	Result.bIsBranchNode = true;

	// Find True and False exec pins
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->Direction != EGPD_Output
			|| Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;
		if (Pin->PinName == UEdGraphSchema_K2::PN_Then || Pin->PinName == TEXT("True"))
		{
			FBranchOutput Branch;
			Branch.ExecPin = Pin;
			Branch.Label = TEXT("True");
			Result.Branches.Add(Branch);
		}
		else if (Pin->PinName == UEdGraphSchema_K2::PN_Else || Pin->PinName == TEXT("False"))
		{
			FBranchOutput Branch;
			Branch.ExecPin = Pin;
			Branch.Label = TEXT("False");
			Result.Branches.Add(Branch);
		}
	}

	return Result;
}

FMapNodeResult FClaireonBPNodeMapper::MapExecutionSequenceNodeEx(const UEdGraphNode* Node, int32 IndentLevel) const
{
	FMapNodeResult Result;
	FString Indent = MakeIndent(IndentLevel);
	FString GuidStr = GetNodeGuidStr(Node);

	Result.Code += FString::Printf(TEXT("%s// [BP:NODE] Guid=%s Type=ExecutionSequence\n"), *Indent, *GuidStr);
	Result.bIsBranchNode = true;

	// Collect exec output pins in order
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output
			&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			FBranchOutput Branch;
			Branch.ExecPin = Pin;
			Branch.Label = FString::Printf(TEXT("Sequence block %s"), *Pin->PinName.ToString());
			Result.Branches.Add(Branch);
		}
	}

	// V5-6: Emit first branch label + opening brace so EmitScope finds a matched pair
	if (Result.Branches.Num() > 0)
	{
		Result.Code += FString::Printf(TEXT("%s// %s\n"), *Indent, *Result.Branches[0].Label);
		Result.Code += FString::Printf(TEXT("%s{\n"), *Indent);
	}

	return Result;
}

FMapNodeResult FClaireonBPNodeMapper::MapSwitchEnumNodeEx(const UEdGraphNode* Node, int32 IndentLevel) const
{
	FMapNodeResult Result;
	FString Indent = MakeIndent(IndentLevel);
	FString GuidStr = GetNodeGuidStr(Node);

	FString SelectionExpr = TEXT("/* selection */");
	UEdGraphPin* SelectionPin = FindPin(Node, TEXT("Selection"), EGPD_Input);
	if (SelectionPin)
	{
		SelectionExpr = GetConnectedPinExpression(SelectionPin);
	}

	Result.Code += FString::Printf(TEXT("%s// [BP:NODE] Guid=%s Type=SwitchEnum\n"), *Indent, *GuidStr);
	Result.Code += FString::Printf(TEXT("%sswitch (%s)\n"), *Indent, *SelectionExpr);
	Result.Code += FString::Printf(TEXT("%s{\n"), *Indent);
	Result.bIsBranchNode = true;

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output
			&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
			&& Pin->PinName != TEXT("Default"))
		{
			FBranchOutput Branch;
			Branch.ExecPin = Pin;
			Branch.Label = FString::Printf(TEXT("case %s"), *Pin->PinName.ToString());
			Result.Branches.Add(Branch);
		}
	}

	// Add default pin last
	UEdGraphPin* DefaultPin = FindPin(Node, TEXT("Default"), EGPD_Output);
	if (DefaultPin && DefaultPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
	{
		FBranchOutput Branch;
		Branch.ExecPin = DefaultPin;
		Branch.Label = TEXT("Default");
		Result.Branches.Add(Branch);
	}

	return Result;
}

FMapNodeResult FClaireonBPNodeMapper::MapSwitchIntegerNodeEx(const UEdGraphNode* Node, int32 IndentLevel) const
{
	FMapNodeResult Result;
	FString Indent = MakeIndent(IndentLevel);
	FString GuidStr = GetNodeGuidStr(Node);

	FString SelectionExpr = TEXT("/* selection */");
	UEdGraphPin* SelectionPin = FindPin(Node, TEXT("Selection"), EGPD_Input);
	if (SelectionPin)
	{
		SelectionExpr = GetConnectedPinExpression(SelectionPin);
	}

	Result.Code += FString::Printf(TEXT("%s// [BP:NODE] Guid=%s Type=SwitchInteger\n"), *Indent, *GuidStr);
	Result.Code += FString::Printf(TEXT("%sswitch (%s)\n"), *Indent, *SelectionExpr);
	Result.Code += FString::Printf(TEXT("%s{\n"), *Indent);
	Result.bIsBranchNode = true;

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output
			&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
			&& Pin->PinName != TEXT("Default"))
		{
			FBranchOutput Branch;
			Branch.ExecPin = Pin;
			Branch.Label = FString::Printf(TEXT("case %s"), *Pin->PinName.ToString());
			Result.Branches.Add(Branch);
		}
	}

	UEdGraphPin* DefaultPin = FindPin(Node, TEXT("Default"), EGPD_Output);
	if (DefaultPin && DefaultPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
	{
		FBranchOutput Branch;
		Branch.ExecPin = DefaultPin;
		Branch.Label = TEXT("Default");
		Result.Branches.Add(Branch);
	}

	return Result;
}

FMapNodeResult FClaireonBPNodeMapper::MapSwitchStringNodeEx(const UEdGraphNode* Node, int32 IndentLevel) const
{
	FMapNodeResult Result;
	FString Indent = MakeIndent(IndentLevel);
	FString GuidStr = GetNodeGuidStr(Node);

	FString SelectionExpr = TEXT("/* selection */");
	UEdGraphPin* SelectionPin = FindPin(Node, TEXT("Selection"), EGPD_Input);
	if (SelectionPin)
	{
		SelectionExpr = GetConnectedPinExpression(SelectionPin);
	}

	Result.Code += FString::Printf(TEXT("%s// [BP:NODE] Guid=%s Type=SwitchString\n"), *Indent, *GuidStr);
	Result.bIsBranchNode = true;

	bool bFirst = true;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output
			&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
			&& Pin->PinName != TEXT("Default"))
		{
			if (bFirst)
			{
				Result.Code += FString::Printf(TEXT("%sif (%s == TEXT(\"%s\"))\n"), *Indent, *SelectionExpr, *Pin->PinName.ToString());
				Result.Code += FString::Printf(TEXT("%s{\n"), *Indent);
				bFirst = false;
			}

			FBranchOutput Branch;
			Branch.ExecPin = Pin;
			Branch.Label = Pin->PinName.ToString();
			Result.Branches.Add(Branch);
		}
	}

	// Add default pin last
	UEdGraphPin* DefaultPin = FindPin(Node, TEXT("Default"), EGPD_Output);
	if (DefaultPin && DefaultPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
	{
		FBranchOutput Branch;
		Branch.ExecPin = DefaultPin;
		Branch.Label = TEXT("Default");
		Result.Branches.Add(Branch);
	}

	return Result;
}

FMapNodeResult FClaireonBPNodeMapper::MapSwitchNameNodeEx(const UEdGraphNode* Node, int32 IndentLevel) const
{
	FMapNodeResult Result;
	FString Indent = MakeIndent(IndentLevel);
	FString GuidStr = GetNodeGuidStr(Node);

	FString SelectionExpr = TEXT("/* selection */");
	UEdGraphPin* SelectionPin = FindPin(Node, TEXT("Selection"), EGPD_Input);
	if (SelectionPin)
	{
		SelectionExpr = GetConnectedPinExpression(SelectionPin);
	}

	Result.Code += FString::Printf(TEXT("%s// [BP:NODE] Guid=%s Type=SwitchName\n"), *Indent, *GuidStr);
	Result.bIsBranchNode = true;

	bool bFirst = true;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output
			&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
			&& Pin->PinName != TEXT("Default"))
		{
			if (bFirst)
			{
				Result.Code += FString::Printf(TEXT("%sif (%s == FName(TEXT(\"%s\")))\n"), *Indent, *SelectionExpr, *Pin->PinName.ToString());
				Result.Code += FString::Printf(TEXT("%s{\n"), *Indent);
				bFirst = false;
			}

			FBranchOutput Branch;
			Branch.ExecPin = Pin;
			Branch.Label = Pin->PinName.ToString();
			Result.Branches.Add(Branch);
		}
	}

	// Add default pin last
	UEdGraphPin* DefaultPin = FindPin(Node, TEXT("Default"), EGPD_Output);
	if (DefaultPin && DefaultPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
	{
		FBranchOutput Branch;
		Branch.ExecPin = DefaultPin;
		Branch.Label = TEXT("Default");
		Result.Branches.Add(Branch);
	}

	return Result;
}

FMapNodeResult FClaireonBPNodeMapper::MapSwitchGameplayTagNodeEx(const UEdGraphNode* Node, int32 IndentLevel) const
{
	FMapNodeResult Result;
	FString Indent = MakeIndent(IndentLevel);
	FString GuidStr = GetNodeGuidStr(Node);

	FString SelectionExpr = TEXT("GameplayTag");
	UEdGraphPin* SelectionPin = FindPin(Node, TEXT("Selection"), EGPD_Input);
	if (SelectionPin)
	{
		SelectionExpr = GetConnectedPinExpression(SelectionPin);
	}

	Result.Code += FString::Printf(TEXT("%s// [BP:NODE] Guid=%s Type=SwitchGameplayTag Selection=%s\n"),
		*Indent, *GuidStr, *SelectionExpr);
	Result.bIsBranchNode = true;

	bool bFirst = true;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->Direction != EGPD_Output
			|| Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;

		FString TagName = Pin->PinName.ToString();
		if (TagName == TEXT("Default"))
		{
			continue;
		}

		if (bFirst)
		{
			Result.Code += FString::Printf(TEXT("%sif (%s.MatchesTag(FGameplayTag::RequestGameplayTag(TEXT(\"%s\"))))\n"),
				*Indent, *SelectionExpr, *TagName);
			Result.Code += FString::Printf(TEXT("%s{\n"), *Indent);
			bFirst = false;
		}

		FBranchOutput Branch;
		Branch.ExecPin = Pin;
		Branch.Label = TagName;
		Result.Branches.Add(Branch);
	}

	// Add default pin last if it exists
	UEdGraphPin* DefaultPin = FindPin(Node, TEXT("Default"), EGPD_Output);
	if (DefaultPin && DefaultPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
	{
		FBranchOutput Branch;
		Branch.ExecPin = DefaultPin;
		Branch.Label = TEXT("Default");
		Result.Branches.Add(Branch);
	}

	return Result;
}

// V3-5: Timeline per-exec-input dispatch
FMapNodeResult FClaireonBPNodeMapper::MapTimelineNodeEx(const UEdGraphNode* Node, int32 IndentLevel, const UEdGraphPin* ArrivalPin)
{
	const UK2Node_Timeline* TimelineNode = CastChecked<UK2Node_Timeline>(Node);
	FMapNodeResult Result;
	FString Indent = MakeIndent(IndentLevel);
	FString GuidStr = GetNodeGuidStr(Node);
	FString TimelineName = TimelineNode->TimelineName.ToString();

	Result.Code += FString::Printf(TEXT("%s// [BP:NODE] Guid=%s Type=Timeline Name=\"%s\"\n"),
		*Indent, *GuidStr, *TimelineName);

	// Per-exec-input dispatch: emit only the verb matching the arrival pin
	if (ArrivalPin)
	{
		FString ArrivalPinName = ArrivalPin->PinName.ToString();
		if (ArrivalPinName == TEXT("Play"))
			Result.Code += FString::Printf(TEXT("%s%s.Play();\n"), *Indent, *TimelineName);
		else if (ArrivalPinName == TEXT("PlayFromStart"))
			Result.Code += FString::Printf(TEXT("%s%s.PlayFromStart();\n"), *Indent, *TimelineName);
		else if (ArrivalPinName == TEXT("Stop"))
			Result.Code += FString::Printf(TEXT("%s%s.Stop();\n"), *Indent, *TimelineName);
		else if (ArrivalPinName == TEXT("Reverse"))
			Result.Code += FString::Printf(TEXT("%s%s.Reverse();\n"), *Indent, *TimelineName);
		else if (ArrivalPinName == TEXT("ReverseFromEnd"))
			Result.Code += FString::Printf(TEXT("%s%s.ReverseFromEnd();\n"), *Indent, *TimelineName);
		else
			Result.Code += FString::Printf(TEXT("%s%s.Play(); // [BP:TIMELINE_DEFAULT] Unrecognized arrival pin: %s\n"),
				*Indent, *TimelineName, *ArrivalPinName);
	}
	else
	{
		// Fallback: no arrival pin info, emit Play as default
		Result.Code += FString::Printf(TEXT("%s%s.Play(); // [BP:TIMELINE_DEFAULT] No arrival pin context\n"),
			*Indent, *TimelineName);
	}

	// Timeline exec outputs (Update/Finished) are callbacks, not inline branches
	Result.bIsBranchNode = false;

	// Collect callback exec output pins for separate callback generation
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->Direction != EGPD_Output
			|| Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;

		FString PinName = Pin->PinName.ToString();
		if ((PinName == TEXT("Update") || PinName == TEXT("Finished"))
			&& Pin->LinkedTo.Num() > 0)
		{
			FString CallbackName = FString::Printf(TEXT("%s_%s"), *TimelineName, *PinName);
			Result.Code += FString::Printf(TEXT("%s// [BP:TIMELINE_CALLBACK] %s -> %s()\n"),
				*Indent, *PinName, *CallbackName);
		}
	}

	// Member declarations for header
	Result.MemberDeclarations += FString::Printf(TEXT("\tUPROPERTY()\n\tFTimeline %s;\n\n"), *TimelineName);

	// Collect curve tracks
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->Direction != EGPD_Output) continue;
		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Float
			&& Pin->PinName != TEXT("Direction"))
		{
			FString TrackName = Pin->PinName.ToString();
			Result.MemberDeclarations += FString::Printf(TEXT("\tUPROPERTY()\n\tUCurveFloat* %s_%sCurve = nullptr;\n\n"),
				*TimelineName, *TrackName);
		}
	}

	// Callback declarations
	Result.MemberDeclarations += FString::Printf(TEXT("\tUFUNCTION()\n\tvoid %s_Update();\n\n"), *TimelineName);
	Result.MemberDeclarations += FString::Printf(TEXT("\tUFUNCTION()\n\tvoid %s_Finished();\n\n"), *TimelineName);

	AddInclude(TEXT("Components/TimelineComponent.h"));

	return Result;
}

// V3-6: Async Action with valid variable names and scoped delegate callbacks
FMapNodeResult FClaireonBPNodeMapper::MapAsyncActionNodeEx(const UEdGraphNode* Node, int32 IndentLevel)
{
	const UK2Node_BaseAsyncTask* AsyncNode = CastChecked<UK2Node_BaseAsyncTask>(Node);
	FMapNodeResult Result;
	FString Indent = MakeIndent(IndentLevel);
	FString GuidStr = GetNodeGuidStr(Node);

	Result.Code += FString::Printf(TEXT("%s%s\n"), *Indent, *FormatBPTag(Node));

	// Read proxy/factory info via reflection (same as existing MapAsyncActionNode)
	FName ProxyFactoryFuncName;
	UClass* ProxyClassPtr = nullptr;
	UClass* FactoryClassPtr = nullptr;

	if (const FNameProperty* FuncNameProp = CastField<FNameProperty>(AsyncNode->GetClass()->FindPropertyByName(TEXT("ProxyFactoryFunctionName"))))
	{
		ProxyFactoryFuncName = FuncNameProp->GetPropertyValue_InContainer(AsyncNode);
	}
	if (const FObjectPropertyBase* ProxyClassProp = CastField<FObjectPropertyBase>(AsyncNode->GetClass()->FindPropertyByName(TEXT("ProxyClass"))))
	{
		ProxyClassPtr = Cast<UClass>(ProxyClassProp->GetObjectPropertyValue_InContainer(AsyncNode));
	}
	if (const FObjectPropertyBase* FactoryClassProp = CastField<FObjectPropertyBase>(AsyncNode->GetClass()->FindPropertyByName(TEXT("ProxyFactoryClass"))))
	{
		FactoryClassPtr = Cast<UClass>(FactoryClassProp->GetObjectPropertyValue_InContainer(AsyncNode));
	}

	FString ProxyClassName = ProxyClassPtr ? FString::Printf(TEXT("%s%s"),
		ProxyClassPtr->GetPrefixCPP(), *ProxyClassPtr->GetName()) : TEXT("UObject");
	FString FactoryFunc = ProxyFactoryFuncName.IsNone() ? TEXT("Create") : ProxyFactoryFuncName.ToString();
	FString FactoryClassName = FactoryClassPtr ? FString::Printf(TEXT("%s%s"),
		FactoryClassPtr->GetPrefixCPP(), *FactoryClassPtr->GetName()) : ProxyClassName;

	// Generate valid member variable name from the async action type
	FString ActionTypeName = ProxyClassPtr ? ProxyClassPtr->GetName() : TEXT("AsyncTask");
	ActionTypeName.RemoveFromStart(TEXT("AbilityAsync_"));
	ActionTypeName.RemoveFromStart(TEXT("AbilityTask_"));
	// V5-2: Include short GUID to disambiguate multiple async actions of the same type
	FString ShortGuid = Node->NodeGuid.ToString(EGuidFormats::Digits).Left(8);
	FString ProxyVarName = FString::Printf(TEXT("%s_%s_AsyncTask"),
		*SanitizeCppIdentifier(ActionTypeName), *ShortGuid);

	// Collect input parameters
	TArray<FString> Params;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
		if (Pin->PinName == UEdGraphSchema_K2::PN_Self) continue;
		if (Pin->Direction == EGPD_Input)
		{
			Params.Add(GetConnectedPinExpression(Pin));
		}
	}
	FString ParamStr = FString::Join(Params, TEXT(", "));

	// Emit factory call
	Result.Code += FString::Printf(TEXT("%s%s = %s::%s(%s);\n"),
		*Indent, *ProxyVarName, *FactoryClassName, *FactoryFunc, *ParamStr);

	// Member declaration for header
	Result.MemberDeclarations += FString::Printf(TEXT("\tUPROPERTY()\n\t%s* %s = nullptr;\n\n"),
		*ProxyClassName, *ProxyVarName);

	// Delegate callback declarations and bindings
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->Direction != EGPD_Output) continue;
		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			FString PinName = Pin->PinName.ToString();
			if (PinName == TEXT("then")) continue; // Immediate continuation, not a callback

			// V5-2: Generate callback name with short GUID for uniqueness
			FString CallbackName = FString::Printf(TEXT("On%s_%s_%s"),
				*SanitizeCppIdentifier(ActionTypeName), *ShortGuid, *SanitizeCppIdentifier(PinName));

			Result.Code += FString::Printf(TEXT("%s%s->%s.AddDynamic(this, &ThisClass::%s);\n"),
				*Indent, *ProxyVarName, *PinName, *CallbackName);

			// Callback declaration in header
			Result.MemberDeclarations += FString::Printf(TEXT("\tUFUNCTION()\n\tvoid %s();\n\n"),
				*CallbackName);
		}
	}

	// Activate
	Result.Code += FString::Printf(TEXT("%s%s->Activate();\n"), *Indent, *ProxyVarName);

	// The "then" exec output is the immediate continuation
	Result.bIsBranchNode = false;

	// Add include for proxy class
	if (ProxyClassPtr)
	{
		FString IncludePath = ProxyClassPtr->GetMetaData(TEXT("ModuleRelativePath"));
		if (!IncludePath.IsEmpty())
		{
			AddInclude(IncludePath);
		}
	}

	return Result;
}

FMapNodeResult FClaireonBPNodeMapper::MapLatentCallFunctionNodeEx(const UEdGraphNode* Node, int32 IndentLevel)
{
	const UK2Node_CallFunction* CallNode = CastChecked<UK2Node_CallFunction>(Node);
	FMapNodeResult Result;
	FString Indent = MakeIndent(IndentLevel);

	Result.Code += FString::Printf(TEXT("%s%s\n"), *Indent, *FormatBPTag(Node));

	// Get function name
	FString FuncName = CallNode->FunctionReference.GetMemberName().ToString();
	FuncName = SanitizeCppIdentifier(FuncName);

	// Generate unique callback name using short GUID
	FString ShortGuid = Node->NodeGuid.ToString(EGuidFormats::Digits).Left(8);
	FString TimerHandleName = FString::Printf(TEXT("TimerHandle_%s_%s"), *FuncName, *ShortGuid);
	FString CallbackName = FString::Printf(TEXT("On%s_%s_Complete"), *FuncName, *ShortGuid);

	// Collect input parameters, suppressing FLatentActionInfo and WorldContextObject
	TArray<FString> Params;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
		if (Pin->PinName == UEdGraphSchema_K2::PN_Self) continue;
		if (Pin->Direction == EGPD_Input)
		{
			// Suppress FLatentActionInfo parameter
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
			{
				if (UScriptStruct* Struct = Cast<UScriptStruct>(Pin->PinType.PinSubCategoryObject.Get()))
				{
					if (Struct == FLatentActionInfo::StaticStruct())
					{
						continue;
					}
				}
			}
			// Suppress WorldContextObject (auto-filled)
			if (Pin->PinName == TEXT("WorldContextObject") || Pin->PinName == TEXT("WorldContext"))
			{
				continue;
			}
			Params.Add(GetConnectedPinExpression(Pin));
		}
	}

	// Emit FTimerManager::SetTimer pattern
	// The Duration parameter is the first (and typically only) remaining parameter
	FString DurationExpr = Params.Num() > 0 ? Params[0] : TEXT("1.0f");

	Result.Code += FString::Printf(TEXT("%sGetWorld()->GetTimerManager().SetTimer(\n"), *Indent);
	Result.Code += FString::Printf(TEXT("%s\t%s,\n"), *Indent, *TimerHandleName);
	Result.Code += FString::Printf(TEXT("%s\tthis,\n"), *Indent);
	Result.Code += FString::Printf(TEXT("%s\t&ThisClass::%s,\n"), *Indent, *CallbackName);
	Result.Code += FString::Printf(TEXT("%s\t%s,\n"), *Indent, *DurationExpr);
	Result.Code += FString::Printf(TEXT("%s\t/*bLoop=*/false);\n"), *Indent);
	Result.Code += FString::Printf(TEXT("%sreturn;\n"), *Indent);

	// Member declarations: timer handle + callback UFUNCTION
	Result.MemberDeclarations += FString::Printf(TEXT("\tUPROPERTY()\n\tFTimerHandle %s;\n\n"), *TimerHandleName);
	Result.MemberDeclarations += FString::Printf(TEXT("\tUFUNCTION()\n\tvoid %s();\n\n"), *CallbackName);

	// Mark as latent split so EmitScope stops following exec chain
	Result.bIsLatentSplit = true;

	return Result;
}

FMapNodeResult FClaireonBPNodeMapper::MapCallFunctionNodeEx(const UEdGraphNode* Node, int32 IndentLevel)
{
	const UK2Node_CallFunction* CallNode = CastChecked<UK2Node_CallFunction>(Node);
	FMapNodeResult Result;
	FString Indent = MakeIndent(IndentLevel);

	Result.Code += FString::Printf(TEXT("%s%s\n"), *Indent, *FormatBPTag(Node));

	FString FuncName = CallNode->FunctionReference.GetMemberName().ToString();
	FuncName = SanitizeCppIdentifier(FuncName);

	TArray<FString> Params;
	FString TargetExpr;
	FString ReturnType;
	FString ReturnVarName;

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
		if (Pin->PinName == UEdGraphSchema_K2::PN_Self)
		{
			if (Pin->LinkedTo.Num() > 0) TargetExpr = GetConnectedPinExpression(Pin);
			continue;
		}
		if (Pin->Direction == EGPD_Input)
		{
			Params.Add(GetConnectedPinExpression(Pin));
		}
		else if (Pin->Direction == EGPD_Output && Pin->PinName == UEdGraphSchema_K2::PN_ReturnValue)
		{
			ReturnType = PinTypeToCppType(Pin->PinType);
			ReturnVarName = FString::Printf(TEXT("%sResult"), *FuncName);
		}
	}

	FString ParamStr = FString::Join(Params, TEXT(", "));

	if (!ReturnType.IsEmpty())
	{
		// Emit assignment only (no type declaration)
		if (!TargetExpr.IsEmpty())
		{
			Result.Code += FString::Printf(TEXT("%s%s = %s->%s(%s);\n"),
				*Indent, *ReturnVarName, *TargetExpr, *FuncName, *ParamStr);
		}
		else
		{
			Result.Code += FString::Printf(TEXT("%s%s = %s(%s);\n"),
				*Indent, *ReturnVarName, *FuncName, *ParamStr);
		}

		// Add UPROPERTY member declaration
		Result.MemberDeclarations += FString::Printf(TEXT("\tUPROPERTY()\n\t%s %s;\n\n"),
			*ReturnType, *ReturnVarName);
	}
	else
	{
		// No return value -- same as before, just a call
		if (!TargetExpr.IsEmpty())
		{
			Result.Code += FString::Printf(TEXT("%s%s->%s(%s);\n"),
				*Indent, *TargetExpr, *FuncName, *ParamStr);
		}
		else
		{
			Result.Code += FString::Printf(TEXT("%s%s(%s);\n"),
				*Indent, *FuncName, *ParamStr);
		}
	}

	Result.bIsBranchNode = false;
	return Result;
}

FMapNodeResult FClaireonBPNodeMapper::MapCastNodeEx(const UEdGraphNode* Node, int32 IndentLevel)
{
	const UK2Node_DynamicCast* CastNode = CastChecked<UK2Node_DynamicCast>(Node);
	FMapNodeResult Result;
	FString Indent = MakeIndent(IndentLevel);

	Result.Code += FString::Printf(TEXT("%s%s\n"), *Indent, *FormatBPTag(Node));

	FString TargetClassName = CastNode->TargetType ? FString::Printf(TEXT("%s%s"),
		CastNode->TargetType->GetPrefixCPP(), *CastNode->TargetType->GetName()) : TEXT("UObject");

	FString SourceExpr = TEXT("/* source */");
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Input
			&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object)
		{
			SourceExpr = GetConnectedPinExpression(Pin);
			break;
		}
	}

	// Assignment only
	Result.Code += FString::Printf(TEXT("%sCastResult = Cast<%s>(%s);\n"),
		*Indent, *TargetClassName, *SourceExpr);

	// Member declaration
	Result.MemberDeclarations += FString::Printf(TEXT("\tUPROPERTY()\n\t%s* CastResult = nullptr;\n\n"),
		*TargetClassName);

	Result.bIsBranchNode = false;
	return Result;
}

FMapNodeResult FClaireonBPNodeMapper::MapSpawnActorNodeEx(const UEdGraphNode* Node, int32 IndentLevel)
{
	FMapNodeResult Result;
	FString Indent = MakeIndent(IndentLevel);

	Result.Code += FString::Printf(TEXT("%s%s\n"), *Indent, *FormatBPTag(Node));

	FString SpawnClassName = TEXT("AActor");
	FString ClassExpr = TEXT("/* class */");
	FString TransformExpr = TEXT("FTransform::Identity");

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
		if (Pin->Direction == EGPD_Input)
		{
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class)
			{
				ClassExpr = GetConnectedPinExpression(Pin);
				if (Pin->PinType.PinSubCategoryObject.IsValid())
				{
					UClass* ObjClass = Cast<UClass>(Pin->PinType.PinSubCategoryObject.Get());
					if (ObjClass)
					{
						SpawnClassName = FString::Printf(TEXT("%s%s"),
							ObjClass->GetPrefixCPP(), *ObjClass->GetName());
					}
				}
			}
			else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
			{
				TransformExpr = GetConnectedPinExpression(Pin);
			}
		}
	}

	// Assignment only
	Result.Code += FString::Printf(TEXT("%sSpawnedActor = GetWorld()->SpawnActor<%s>(%s, %s);\n"),
		*Indent, *SpawnClassName, *ClassExpr, *TransformExpr);

	// Member declaration
	Result.MemberDeclarations += FString::Printf(TEXT("\tUPROPERTY()\n\t%s* SpawnedActor = nullptr;\n\n"),
		*SpawnClassName);

	Result.bIsBranchNode = false;
	return Result;
}
