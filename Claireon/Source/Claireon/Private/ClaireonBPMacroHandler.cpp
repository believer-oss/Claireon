// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonBPMacroHandler.h"
#include "ClaireonBPNodeMapper.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_MacroInstance.h"

namespace
{
	FString MakeMacroIndent(int32 IndentLevel)
	{
		FString Result;
		for (int32 i = 0; i < IndentLevel; ++i)
		{
			Result += TEXT("\t");
		}
		return Result;
	}

	FString GetMacroGuidStr(const UEdGraphNode* Node)
	{
		if (!Node)
		{
			return TEXT("null");
		}
		return Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensInBraces);
	}

	// Get a short GUID suffix for member variable naming (first 8 hex chars)
	FString GetShortGuid(const UEdGraphNode* Node)
	{
		if (!Node)
		{
			return TEXT("0000");
		}
		FString Full = Node->NodeGuid.ToString(EGuidFormats::Digits);
		return Full.Left(8);
	}

	// Get macro graph name from a macro instance node
	// V4-1: Normalize by stripping spaces so "Switch Has Authority" matches "SwitchHasAuthority"
	FString GetMacroName(const UEdGraphNode* Node)
	{
		const UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(Node);
		if (MacroNode && MacroNode->GetMacroGraph())
		{
			FString MacroName = MacroNode->GetMacroGraph()->GetName();
			MacroName.ReplaceInline(TEXT(" "), TEXT(""));
			return MacroName;
		}
		return FString();
	}

	// Get the default value or connected expression for a pin by name
	FString GetMacroPinExpression(const UEdGraphNode* Node, const FName& PinName, EEdGraphPinDirection Direction)
	{
		if (!Node)
		{
			return TEXT("/* unconnected */");
		}
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinName == PinName && Pin->Direction == Direction)
			{
				if (Pin->LinkedTo.Num() > 0 && Pin->LinkedTo[0])
				{
					return Pin->LinkedTo[0]->PinName.ToString();
				}
				if (!Pin->DefaultValue.IsEmpty())
				{
					return Pin->DefaultValue;
				}
				return PinName.ToString();
			}
		}
		return TEXT("/* unconnected */");
	}

	// Count output exec pins on a node
	int32 CountOutputExecPins(const UEdGraphNode* Node)
	{
		int32 Count = 0;
		if (Node)
		{
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Output
					&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
				{
					++Count;
				}
			}
		}
		return Count;
	}
}

bool FClaireonBPMacroHandler::IsKnownMacro(const UEdGraphNode* Node) const
{
	FString MacroName = GetMacroName(Node);
	if (MacroName.IsEmpty())
	{
		return false;
	}

	static const TSet<FString> KnownMacros = {
		TEXT("ForEachLoop"),
		TEXT("ForEachLoopWithBreak"),
		TEXT("ForLoop"),
		TEXT("ForLoopWithBreak"),
		TEXT("WhileLoop"),
		TEXT("StandardMacroBranch"),
		TEXT("Sequence"),
		TEXT("DoOnce"),
		TEXT("DoN"),
		TEXT("Gate"),
		TEXT("FlipFlop"),
		TEXT("IsValid"),
		TEXT("Select"),
		TEXT("MultiGate"),
		TEXT("SwitchHasAuthority"),
	};

	return KnownMacros.Contains(MacroName);
}

FClaireonBPMacroResult FClaireonBPMacroHandler::HandleMacro(const UEdGraphNode* Node, int32 IndentLevel) const
{
	FString MacroName = GetMacroName(Node);

	if (MacroName == TEXT("ForEachLoop"))         return HandleForEachLoop(Node, IndentLevel);
	if (MacroName == TEXT("ForEachLoopWithBreak")) return HandleForEachLoopWithBreak(Node, IndentLevel);
	if (MacroName == TEXT("ForLoop"))              return HandleForLoop(Node, IndentLevel);
	if (MacroName == TEXT("ForLoopWithBreak"))     return HandleForLoopWithBreak(Node, IndentLevel);
	if (MacroName == TEXT("WhileLoop"))            return HandleWhileLoop(Node, IndentLevel);
	if (MacroName == TEXT("StandardMacroBranch"))  return HandleBranch(Node, IndentLevel);
	if (MacroName == TEXT("Sequence"))             return HandleSequence(Node, IndentLevel);
	if (MacroName == TEXT("DoOnce"))               return HandleDoOnce(Node, IndentLevel);
	if (MacroName == TEXT("DoN"))                  return HandleDoN(Node, IndentLevel);
	if (MacroName == TEXT("Gate"))                 return HandleGate(Node, IndentLevel);
	if (MacroName == TEXT("FlipFlop"))             return HandleFlipFlop(Node, IndentLevel);
	if (MacroName == TEXT("IsValid"))              return HandleIsValid(Node, IndentLevel);
	if (MacroName == TEXT("Select"))               return HandleSelect(Node, IndentLevel);
	if (MacroName == TEXT("MultiGate"))            return HandleMultiGate(Node, IndentLevel);
	if (MacroName == TEXT("SwitchHasAuthority"))   return HandleSwitchHasAuthority(Node, IndentLevel);

	return FClaireonBPMacroResult();
}

FClaireonBPMacroResult FClaireonBPMacroHandler::HandleForEachLoop(const UEdGraphNode* Node, int32 IndentLevel) const
{
	FClaireonBPMacroResult Result;
	FString Indent = MakeMacroIndent(IndentLevel);
	FString GuidStr = GetMacroGuidStr(Node);

	FString ArrayExpr = GetMacroPinExpression(Node, TEXT("Array"), EGPD_Input);
	FString ElementName = TEXT("Item");

	// Try to infer element type from the Array pin
	FString ElementType = TEXT("auto&");
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->PinName == TEXT("Array Element") && Pin->Direction == EGPD_Output)
		{
			// The element pin carries the inner type
			FClaireonBPNodeMapper Mapper;
			ElementType = Mapper.PinTypeToCppType(Pin->PinType) + TEXT("&");
			break;
		}
	}

	Result.SourceCode += FString::Printf(TEXT("%s// [BP:MACRO] Guid=%s Type=ForEachLoop Array=%s Element=%s\n"),
		*Indent, *GuidStr, *ArrayExpr, *ElementName);
	Result.SourceCode += FString::Printf(TEXT("%sfor (%s %s : %s)\n"), *Indent, *ElementType, *ElementName, *ArrayExpr);
	Result.SourceCode += FString::Printf(TEXT("%s{\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s\t// [BP:LOOP_BODY] From=%s\n"), *Indent, *GuidStr);
	Result.SourceCode += FString::Printf(TEXT("%s\t// [BP] TODO: implement loop body\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s}\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s// [BP:LOOP_COMPLETE] From=%s\n"), *Indent, *GuidStr);

	return Result;
}

FClaireonBPMacroResult FClaireonBPMacroHandler::HandleForEachLoopWithBreak(const UEdGraphNode* Node, int32 IndentLevel) const
{
	FClaireonBPMacroResult Result;
	FString Indent = MakeMacroIndent(IndentLevel);
	FString GuidStr = GetMacroGuidStr(Node);

	FString ArrayExpr = GetMacroPinExpression(Node, TEXT("Array"), EGPD_Input);
	FString ElementName = TEXT("Item");

	FString ElementType = TEXT("auto&");
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->PinName == TEXT("Array Element") && Pin->Direction == EGPD_Output)
		{
			FClaireonBPNodeMapper Mapper;
			ElementType = Mapper.PinTypeToCppType(Pin->PinType) + TEXT("&");
			break;
		}
	}

	Result.SourceCode += FString::Printf(TEXT("%s// [BP:MACRO] Guid=%s Type=ForEachLoopWithBreak Array=%s Element=%s\n"),
		*Indent, *GuidStr, *ArrayExpr, *ElementName);
	Result.SourceCode += FString::Printf(TEXT("%sfor (%s %s : %s)\n"), *Indent, *ElementType, *ElementName, *ArrayExpr);
	Result.SourceCode += FString::Printf(TEXT("%s{\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s\t// [BP:LOOP_BODY] From=%s\n"), *Indent, *GuidStr);
	Result.SourceCode += FString::Printf(TEXT("%s\t// [BP] TODO: implement loop body\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s\t// [BP:BREAK_CONDITION] From=%s\n"), *Indent, *GuidStr);
	Result.SourceCode += FString::Printf(TEXT("%s\tif (/* break condition */)\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s\t{\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s\t\tbreak;\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s\t}\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s}\n"), *Indent);

	return Result;
}

FClaireonBPMacroResult FClaireonBPMacroHandler::HandleForLoop(const UEdGraphNode* Node, int32 IndentLevel) const
{
	FClaireonBPMacroResult Result;
	FString Indent = MakeMacroIndent(IndentLevel);
	FString GuidStr = GetMacroGuidStr(Node);

	FString FirstExpr = GetMacroPinExpression(Node, TEXT("FirstIndex"), EGPD_Input);
	FString LastExpr = GetMacroPinExpression(Node, TEXT("LastIndex"), EGPD_Input);

	Result.SourceCode += FString::Printf(TEXT("%s// [BP:MACRO] Guid=%s Type=ForLoop First=%s Last=%s\n"),
		*Indent, *GuidStr, *FirstExpr, *LastExpr);
	Result.SourceCode += FString::Printf(TEXT("%sfor (int32 Index = %s; Index <= %s; ++Index)\n"),
		*Indent, *FirstExpr, *LastExpr);
	Result.SourceCode += FString::Printf(TEXT("%s{\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s\t// [BP:LOOP_BODY] From=%s\n"), *Indent, *GuidStr);
	Result.SourceCode += FString::Printf(TEXT("%s\t// [BP] TODO: implement loop body\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s}\n"), *Indent);

	return Result;
}

FClaireonBPMacroResult FClaireonBPMacroHandler::HandleForLoopWithBreak(const UEdGraphNode* Node, int32 IndentLevel) const
{
	FClaireonBPMacroResult Result;
	FString Indent = MakeMacroIndent(IndentLevel);
	FString GuidStr = GetMacroGuidStr(Node);

	FString FirstExpr = GetMacroPinExpression(Node, TEXT("FirstIndex"), EGPD_Input);
	FString LastExpr = GetMacroPinExpression(Node, TEXT("LastIndex"), EGPD_Input);

	Result.SourceCode += FString::Printf(TEXT("%s// [BP:MACRO] Guid=%s Type=ForLoopWithBreak First=%s Last=%s\n"),
		*Indent, *GuidStr, *FirstExpr, *LastExpr);
	Result.SourceCode += FString::Printf(TEXT("%sfor (int32 Index = %s; Index <= %s; ++Index)\n"),
		*Indent, *FirstExpr, *LastExpr);
	Result.SourceCode += FString::Printf(TEXT("%s{\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s\t// [BP:LOOP_BODY] From=%s\n"), *Indent, *GuidStr);
	Result.SourceCode += FString::Printf(TEXT("%s\t// [BP] TODO: implement loop body\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s\t// [BP:BREAK_CONDITION] From=%s\n"), *Indent, *GuidStr);
	Result.SourceCode += FString::Printf(TEXT("%s\tif (/* break condition */)\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s\t{\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s\t\tbreak;\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s\t}\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s}\n"), *Indent);

	return Result;
}

FClaireonBPMacroResult FClaireonBPMacroHandler::HandleWhileLoop(const UEdGraphNode* Node, int32 IndentLevel) const
{
	FClaireonBPMacroResult Result;
	FString Indent = MakeMacroIndent(IndentLevel);
	FString GuidStr = GetMacroGuidStr(Node);

	Result.SourceCode += FString::Printf(TEXT("%s// [BP:MACRO] Guid=%s Type=WhileLoop\n"), *Indent, *GuidStr);
	Result.SourceCode += FString::Printf(TEXT("%swhile (/* condition */)\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s{\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s\t// [BP:LOOP_BODY] From=%s\n"), *Indent, *GuidStr);
	Result.SourceCode += FString::Printf(TEXT("%s\t// [BP] TODO: implement loop body\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s}\n"), *Indent);

	return Result;
}

FClaireonBPMacroResult FClaireonBPMacroHandler::HandleBranch(const UEdGraphNode* Node, int32 IndentLevel) const
{
	FClaireonBPMacroResult Result;
	FString Indent = MakeMacroIndent(IndentLevel);
	FString GuidStr = GetMacroGuidStr(Node);

	FString ConditionExpr = GetMacroPinExpression(Node, TEXT("Condition"), EGPD_Input);

	Result.SourceCode += FString::Printf(TEXT("%s// [BP:MACRO] Guid=%s Type=Branch Condition=%s\n"),
		*Indent, *GuidStr, *ConditionExpr);
	Result.SourceCode += FString::Printf(TEXT("%sif (%s)\n"), *Indent, *ConditionExpr);
	Result.SourceCode += FString::Printf(TEXT("%s{\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s\t// [BP:EXEC] From=%s[True]\n"), *Indent, *GuidStr);
	Result.SourceCode += FString::Printf(TEXT("%s\t// [BP] TODO: implement true branch\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s}\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%selse\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s{\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s\t// [BP:EXEC] From=%s[False]\n"), *Indent, *GuidStr);
	Result.SourceCode += FString::Printf(TEXT("%s\t// [BP] TODO: implement false branch\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s}\n"), *Indent);

	return Result;
}

FClaireonBPMacroResult FClaireonBPMacroHandler::HandleSequence(const UEdGraphNode* Node, int32 IndentLevel) const
{
	FClaireonBPMacroResult Result;
	FString Indent = MakeMacroIndent(IndentLevel);
	FString GuidStr = GetMacroGuidStr(Node);

	// Count output exec pins to determine sequence count
	int32 OutputCount = 0;
	if (Node)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output
				&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				++OutputCount;
			}
		}
	}

	Result.SourceCode += FString::Printf(TEXT("%s// [BP:MACRO] Guid=%s Type=Sequence OutputCount=%d\n"),
		*Indent, *GuidStr, OutputCount);

	for (int32 i = 0; i < OutputCount; ++i)
	{
		Result.SourceCode += FString::Printf(TEXT("%s{\n"), *Indent);
		Result.SourceCode += FString::Printf(TEXT("%s\t// [BP:SEQ] Index=%d From=%s\n"), *Indent, i, *GuidStr);
		Result.SourceCode += FString::Printf(TEXT("%s\t// [BP] TODO: implement sequence block %d\n"), *Indent, i);
		Result.SourceCode += FString::Printf(TEXT("%s}\n"), *Indent);
	}

	return Result;
}

FClaireonBPMacroResult FClaireonBPMacroHandler::HandleDoOnce(const UEdGraphNode* Node, int32 IndentLevel) const
{
	FClaireonBPMacroResult Result;
	FString Indent = MakeMacroIndent(IndentLevel);
	FString GuidStr = GetMacroGuidStr(Node);
	FString ShortGuid = GetShortGuid(Node);
	FString VarName = FString::Printf(TEXT("bDoOnce_%s"), *ShortGuid);

	// Member variable for header
	Result.MemberDeclarations += FString::Printf(TEXT("\tUPROPERTY()\n\tbool %s = false;\n"), *VarName);

	// Source code
	Result.SourceCode += FString::Printf(TEXT("%s// [BP:MACRO] Guid=%s Type=DoOnce\n"), *Indent, *GuidStr);
	Result.SourceCode += FString::Printf(TEXT("%sif (!%s)\n"), *Indent, *VarName);
	Result.SourceCode += FString::Printf(TEXT("%s{\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s\t%s = true;\n"), *Indent, *VarName);
	Result.SourceCode += FString::Printf(TEXT("%s\t// [BP:EXEC] From=%s\n"), *Indent, *GuidStr);
	Result.SourceCode += FString::Printf(TEXT("%s\t// [BP] TODO: implement do-once body\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s}\n"), *Indent);

	// Check for Reset exec pin connection
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->PinName == TEXT("Reset") && Pin->Direction == EGPD_Input
			&& Pin->LinkedTo.Num() > 0)
		{
			Result.SourceCode += FString::Printf(TEXT("%s// [BP:MACRO_RESET] Guid=%s Type=DoOnce_Reset\n"),
				*Indent, *GuidStr);
			Result.SourceCode += FString::Printf(TEXT("%s%s = false;\n"), *Indent, *VarName);
			break;
		}
	}

	return Result;
}

FClaireonBPMacroResult FClaireonBPMacroHandler::HandleDoN(const UEdGraphNode* Node, int32 IndentLevel) const
{
	FClaireonBPMacroResult Result;
	FString Indent = MakeMacroIndent(IndentLevel);
	FString GuidStr = GetMacroGuidStr(Node);
	FString ShortGuid = GetShortGuid(Node);
	FString VarName = FString::Printf(TEXT("DoNCounter_%s"), *ShortGuid);

	FString MaxNExpr = GetMacroPinExpression(Node, TEXT("N"), EGPD_Input);

	// Member variable for header
	Result.MemberDeclarations += FString::Printf(TEXT("\tUPROPERTY()\n\tint32 %s = 0;\n"), *VarName);

	// Source code
	Result.SourceCode += FString::Printf(TEXT("%s// [BP:MACRO] Guid=%s Type=DoN MaxN=%s\n"),
		*Indent, *GuidStr, *MaxNExpr);
	Result.SourceCode += FString::Printf(TEXT("%sif (%s < %s)\n"), *Indent, *VarName, *MaxNExpr);
	Result.SourceCode += FString::Printf(TEXT("%s{\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s\t++%s;\n"), *Indent, *VarName);
	Result.SourceCode += FString::Printf(TEXT("%s\t// [BP:EXEC] From=%s\n"), *Indent, *GuidStr);
	Result.SourceCode += FString::Printf(TEXT("%s\t// [BP] TODO: implement do-N body\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s}\n"), *Indent);

	return Result;
}

FClaireonBPMacroResult FClaireonBPMacroHandler::HandleGate(const UEdGraphNode* Node, int32 IndentLevel) const
{
	FClaireonBPMacroResult Result;
	FString Indent = MakeMacroIndent(IndentLevel);
	FString GuidStr = GetMacroGuidStr(Node);
	FString ShortGuid = GetShortGuid(Node);
	FString VarName = FString::Printf(TEXT("bGateOpen_%s"), *ShortGuid);

	// Member variable for header
	Result.MemberDeclarations += FString::Printf(TEXT("\tUPROPERTY()\n\tbool %s = false;\n"), *VarName);

	// Source code
	Result.SourceCode += FString::Printf(TEXT("%s// [BP:MACRO] Guid=%s Type=Gate\n"), *Indent, *GuidStr);
	Result.SourceCode += FString::Printf(TEXT("%s// Open exec:\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s%s = true;\n"), *Indent, *VarName);
	Result.SourceCode += FString::Printf(TEXT("%s// Close exec:\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s%s = false;\n"), *Indent, *VarName);
	Result.SourceCode += FString::Printf(TEXT("%s// Toggle exec:\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s%s = !%s;\n"), *Indent, *VarName, *VarName);
	Result.SourceCode += FString::Printf(TEXT("%s// Enter exec:\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%sif (%s)\n"), *Indent, *VarName);
	Result.SourceCode += FString::Printf(TEXT("%s{\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s\t// [BP:EXEC] From=%s\n"), *Indent, *GuidStr);
	Result.SourceCode += FString::Printf(TEXT("%s\t// [BP] TODO: implement gate body\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s}\n"), *Indent);

	return Result;
}

FClaireonBPMacroResult FClaireonBPMacroHandler::HandleFlipFlop(const UEdGraphNode* Node, int32 IndentLevel) const
{
	FClaireonBPMacroResult Result;
	FString Indent = MakeMacroIndent(IndentLevel);
	FString GuidStr = GetMacroGuidStr(Node);
	FString ShortGuid = GetShortGuid(Node);
	FString VarName = FString::Printf(TEXT("bFlipFlop_%s"), *ShortGuid);

	// Member variable for header
	Result.MemberDeclarations += FString::Printf(TEXT("\tUPROPERTY()\n\tbool %s = false;\n"), *VarName);

	// Source code
	Result.SourceCode += FString::Printf(TEXT("%s// [BP:MACRO] Guid=%s Type=FlipFlop\n"), *Indent, *GuidStr);
	Result.SourceCode += FString::Printf(TEXT("%s%s = !%s;\n"), *Indent, *VarName, *VarName);
	Result.SourceCode += FString::Printf(TEXT("%sif (%s)\n"), *Indent, *VarName);
	Result.SourceCode += FString::Printf(TEXT("%s{\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s\t// [BP:EXEC] From=%s[A]\n"), *Indent, *GuidStr);
	Result.SourceCode += FString::Printf(TEXT("%s\t// [BP] TODO: implement flip-flop A\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s}\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%selse\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s{\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s\t// [BP:EXEC] From=%s[B]\n"), *Indent, *GuidStr);
	Result.SourceCode += FString::Printf(TEXT("%s\t// [BP] TODO: implement flip-flop B\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s}\n"), *Indent);

	return Result;
}

FClaireonBPMacroResult FClaireonBPMacroHandler::HandleIsValid(const UEdGraphNode* Node, int32 IndentLevel) const
{
	FClaireonBPMacroResult Result;
	FString Indent = MakeMacroIndent(IndentLevel);
	FString GuidStr = GetMacroGuidStr(Node);

	FString ObjectExpr = GetMacroPinExpression(Node, TEXT("Input Object"), EGPD_Input);

	Result.SourceCode += FString::Printf(TEXT("%s// [BP:MACRO] Guid=%s Type=IsValid Object=%s\n"),
		*Indent, *GuidStr, *ObjectExpr);
	Result.SourceCode += FString::Printf(TEXT("%sif (IsValid(%s))\n"), *Indent, *ObjectExpr);
	Result.SourceCode += FString::Printf(TEXT("%s{\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s\t// [BP:EXEC] From=%s[IsValid]\n"), *Indent, *GuidStr);
	Result.SourceCode += FString::Printf(TEXT("%s\t// [BP] TODO: implement valid branch\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s}\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%selse\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s{\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s\t// [BP:EXEC] From=%s[IsNotValid]\n"), *Indent, *GuidStr);
	Result.SourceCode += FString::Printf(TEXT("%s\t// [BP] TODO: implement invalid branch\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s}\n"), *Indent);

	return Result;
}

FClaireonBPMacroResult FClaireonBPMacroHandler::HandleSelect(const UEdGraphNode* Node, int32 IndentLevel) const
{
	FClaireonBPMacroResult Result;
	FString Indent = MakeMacroIndent(IndentLevel);
	FString GuidStr = GetMacroGuidStr(Node);

	// Try to find the index/selector pin
	FString IndexExpr = GetMacroPinExpression(Node, TEXT("Index"), EGPD_Input);

	Result.SourceCode += FString::Printf(TEXT("%s// [BP:MACRO] Guid=%s Type=Select Index=%s\n"),
		*Indent, *GuidStr, *IndexExpr);

	// Check if the selector is a bool
	bool bIsBoolSelect = false;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->PinName == TEXT("Index") && Pin->Direction == EGPD_Input
			&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
		{
			bIsBoolSelect = true;
			break;
		}
	}

	if (bIsBoolSelect)
	{
		Result.SourceCode += FString::Printf(TEXT("%sauto Result = %s ? ValueA : ValueB; // [BP] TODO: fill in values\n"),
			*Indent, *IndexExpr);
	}
	else
	{
		Result.SourceCode += FString::Printf(TEXT("%sswitch (%s)\n"), *Indent, *IndexExpr);
		Result.SourceCode += FString::Printf(TEXT("%s{\n"), *Indent);
		Result.SourceCode += FString::Printf(TEXT("%s\t// [BP] TODO: implement select cases\n"), *Indent);
		Result.SourceCode += FString::Printf(TEXT("%s\tdefault:\n"), *Indent);
		Result.SourceCode += FString::Printf(TEXT("%s\t\tbreak;\n"), *Indent);
		Result.SourceCode += FString::Printf(TEXT("%s}\n"), *Indent);
	}

	return Result;
}

FClaireonBPMacroResult FClaireonBPMacroHandler::HandleMultiGate(const UEdGraphNode* Node, int32 IndentLevel) const
{
	FClaireonBPMacroResult Result;
	FString Indent = MakeMacroIndent(IndentLevel);
	FString GuidStr = GetMacroGuidStr(Node);
	FString ShortGuid = GetShortGuid(Node);
	FString VarName = FString::Printf(TEXT("MultiGateIndex_%s"), *ShortGuid);

	// Count output exec pins (excluding special pins like Reset)
	int32 OutputCount = 0;
	TArray<FString> OutputPinNames;
	if (Node)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output
				&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
				&& Pin->PinName.ToString().StartsWith(TEXT("Out ")))
			{
				OutputPinNames.Add(Pin->PinName.ToString());
				++OutputCount;
			}
		}
	}
	if (OutputCount == 0)
	{
		OutputCount = CountOutputExecPins(Node);
	}

	// Check bLoop and bRandom pins
	FString bLoopStr = TEXT("true");
	FString bRandomStr = TEXT("false");
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Input && Pin->PinName == TEXT("Loop"))
		{
			bLoopStr = Pin->DefaultValue.IsEmpty() ? TEXT("true") : Pin->DefaultValue;
		}
		if (Pin && Pin->Direction == EGPD_Input && Pin->PinName == TEXT("Random"))
		{
			bRandomStr = Pin->DefaultValue.IsEmpty() ? TEXT("false") : Pin->DefaultValue;
		}
	}

	// Member variable for header
	Result.MemberDeclarations += FString::Printf(TEXT("\tUPROPERTY()\n\tint32 %s = 0;\n"), *VarName);

	// Source code
	Result.SourceCode += FString::Printf(TEXT("%s// [BP:MACRO] Guid=%s Type=MultiGate OutputCount=%d bLoop=%s bRandom=%s\n"),
		*Indent, *GuidStr, OutputCount, *bLoopStr, *bRandomStr);
	Result.SourceCode += FString::Printf(TEXT("%s{\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s\tconst int32 GateIndex = %s;\n"), *Indent, *VarName);
	Result.SourceCode += FString::Printf(TEXT("%s\t%s = (%s + 1) %% %d; // loop\n"), *Indent, *VarName, *VarName, OutputCount);
	Result.SourceCode += FString::Printf(TEXT("%s\tswitch (GateIndex)\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s\t{\n"), *Indent);

	for (int32 i = 0; i < OutputCount; ++i)
	{
		FString PinName = OutputPinNames.IsValidIndex(i) ? OutputPinNames[i] : FString::Printf(TEXT("Out_%d"), i);
		Result.SourceCode += FString::Printf(TEXT("%s\t\tcase %d:\n"), *Indent, i);
		Result.SourceCode += FString::Printf(TEXT("%s\t\t\t// [BP:EXEC] From=%s[%s]\n"), *Indent, *GuidStr, *PinName);
		Result.SourceCode += FString::Printf(TEXT("%s\t\t\t// [BP] TODO: implement gate %d\n"), *Indent, i);
		Result.SourceCode += FString::Printf(TEXT("%s\t\t\tbreak;\n"), *Indent);
	}

	Result.SourceCode += FString::Printf(TEXT("%s\t}\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s}\n"), *Indent);

	return Result;
}

FClaireonBPMacroResult FClaireonBPMacroHandler::HandleSwitchHasAuthority(const UEdGraphNode* Node, int32 IndentLevel) const
{
	FClaireonBPMacroResult Result;
	FString Indent = MakeMacroIndent(IndentLevel);
	FString GuidStr = GetMacroGuidStr(Node);

	Result.SourceCode += FString::Printf(TEXT("%s// [BP:MACRO] Guid=%s Type=SwitchHasAuthority\n"), *Indent, *GuidStr);
	Result.SourceCode += FString::Printf(TEXT("%sif (HasAuthority())\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s{\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s\t// [BP:EXEC] From=%s[Authority]\n"), *Indent, *GuidStr);
	Result.SourceCode += FString::Printf(TEXT("%s\t// [BP] TODO: implement authority branch\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s}\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%selse\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s{\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s\t// [BP:EXEC] From=%s[Remote]\n"), *Indent, *GuidStr);
	Result.SourceCode += FString::Printf(TEXT("%s\t// [BP] TODO: implement remote branch\n"), *Indent);
	Result.SourceCode += FString::Printf(TEXT("%s}\n"), *Indent);

	return Result;
}

// V3 scope-tree: HandleMacroEx returns FMapNodeResult with branch metadata

FMapNodeResult FClaireonBPMacroHandler::HandleMacroEx(const UEdGraphNode* Node, int32 IndentLevel, const UEdGraphPin* ArrivalPin) const
{
	FMapNodeResult Result;
	FString MacroName = GetMacroName(Node);
	FString Indent = MakeMacroIndent(IndentLevel);
	FString GuidStr = GetMacroGuidStr(Node);

	// StandardMacroBranch -> if/else with True/False branches
	if (MacroName == TEXT("StandardMacroBranch"))
	{
		FString ConditionExpr = GetMacroPinExpression(Node, TEXT("Condition"), EGPD_Input);

		Result.Code += FString::Printf(TEXT("%s// [BP:MACRO] Guid=%s Type=Branch Condition=%s\n"),
			*Indent, *GuidStr, *ConditionExpr);
		Result.Code += FString::Printf(TEXT("%sif (%s)\n"), *Indent, *ConditionExpr);
		Result.Code += FString::Printf(TEXT("%s{\n"), *Indent);
		Result.bIsBranchNode = true;

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output
				|| Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;
			FString PinName = Pin->PinName.ToString();
			if (PinName == TEXT("True"))
			{
				FBranchOutput Branch;
				Branch.ExecPin = Pin;
				Branch.Label = TEXT("True");
				Result.Branches.Add(Branch);
			}
			else if (PinName == TEXT("False"))
			{
				FBranchOutput Branch;
				Branch.ExecPin = Pin;
				Branch.Label = TEXT("False");
				Result.Branches.Add(Branch);
			}
		}
		return Result;
	}

	// Sequence -> sequential blocks
	if (MacroName == TEXT("Sequence"))
	{
		Result.Code += FString::Printf(TEXT("%s// [BP:MACRO] Guid=%s Type=Sequence\n"), *Indent, *GuidStr);
		Result.bIsBranchNode = true;

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

	// SwitchHasAuthority -> if (HasAuthority()) with Authority/Remote branches
	if (MacroName == TEXT("SwitchHasAuthority"))
	{
		Result.Code += FString::Printf(TEXT("%s// [BP:MACRO] Guid=%s Type=SwitchHasAuthority\n"), *Indent, *GuidStr);
		Result.Code += FString::Printf(TEXT("%sif (HasAuthority())\n"), *Indent);
		Result.Code += FString::Printf(TEXT("%s{\n"), *Indent);
		Result.bIsBranchNode = true;

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output
				&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				FBranchOutput Branch;
				Branch.ExecPin = Pin;
				Branch.Label = Pin->PinName.ToString(); // "Authority" or "Remote"
				Result.Branches.Add(Branch);
			}
		}
		return Result;
	}

	// FlipFlop -> toggle + if/else with A/B branches
	if (MacroName == TEXT("FlipFlop"))
	{
		FString ShortGuid = GetShortGuid(Node);
		FString VarName = FString::Printf(TEXT("bFlipFlop_%s"), *ShortGuid);

		Result.MemberDeclarations += FString::Printf(TEXT("\tUPROPERTY()\n\tbool %s = false;\n"), *VarName);

		Result.Code += FString::Printf(TEXT("%s// [BP:MACRO] Guid=%s Type=FlipFlop\n"), *Indent, *GuidStr);
		Result.Code += FString::Printf(TEXT("%s%s = !%s;\n"), *Indent, *VarName, *VarName);
		Result.Code += FString::Printf(TEXT("%sif (%s)\n"), *Indent, *VarName);
		Result.Code += FString::Printf(TEXT("%s{\n"), *Indent);
		Result.bIsBranchNode = true;

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output
				|| Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;
			FString PinName = Pin->PinName.ToString();
			if (PinName == TEXT("A") || PinName == TEXT("B"))
			{
				FBranchOutput Branch;
				Branch.ExecPin = Pin;
				Branch.Label = PinName;
				Result.Branches.Add(Branch);
			}
		}
		return Result;
	}

	// IsValid -> if (IsValid(obj)) with IsValid/IsNotValid branches
	if (MacroName == TEXT("IsValid"))
	{
		FString ObjectExpr = GetMacroPinExpression(Node, TEXT("Input Object"), EGPD_Input);

		Result.Code += FString::Printf(TEXT("%s// [BP:MACRO] Guid=%s Type=IsValid Object=%s\n"),
			*Indent, *GuidStr, *ObjectExpr);
		Result.Code += FString::Printf(TEXT("%sif (IsValid(%s))\n"), *Indent, *ObjectExpr);
		Result.Code += FString::Printf(TEXT("%s{\n"), *Indent);
		Result.bIsBranchNode = true;

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output
				|| Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;
			FString PinName = Pin->PinName.ToString();
			if (PinName == TEXT("Is Valid") || PinName == TEXT("IsValid"))
			{
				FBranchOutput Branch;
				Branch.ExecPin = Pin;
				Branch.Label = TEXT("IsValid");
				Result.Branches.Add(Branch);
			}
			else if (PinName == TEXT("Is Not Valid") || PinName == TEXT("IsNotValid"))
			{
				FBranchOutput Branch;
				Branch.ExecPin = Pin;
				Branch.Label = TEXT("IsNotValid");
				Result.Branches.Add(Branch);
			}
		}
		return Result;
	}

	// Gate -> per-input-pin dispatch using ArrivalPin (V3-9)
	if (MacroName == TEXT("Gate"))
	{
		FString ShortGuid = GetShortGuid(Node);
		FString VarName = FString::Printf(TEXT("bGateOpen_%s"), *ShortGuid);

		Result.MemberDeclarations += FString::Printf(TEXT("\tUPROPERTY()\n\tbool %s = false;\n"), *VarName);
		Result.Code += FString::Printf(TEXT("%s// [BP:MACRO] Guid=%s Type=Gate\n"), *Indent, *GuidStr);

		FString ArrivalPinName;
		if (ArrivalPin)
		{
			ArrivalPinName = ArrivalPin->PinName.ToString();
		}

		if (ArrivalPinName == TEXT("Open"))
		{
			Result.Code += FString::Printf(TEXT("%s%s = true;\n"), *Indent, *VarName);
			Result.bIsBranchNode = false;
		}
		else if (ArrivalPinName == TEXT("Close"))
		{
			Result.Code += FString::Printf(TEXT("%s%s = false;\n"), *Indent, *VarName);
			Result.bIsBranchNode = false;
		}
		else if (ArrivalPinName == TEXT("Toggle"))
		{
			Result.Code += FString::Printf(TEXT("%s%s = !%s;\n"), *Indent, *VarName, *VarName);
			Result.bIsBranchNode = false;
		}
		else // Enter or unknown
		{
			Result.Code += FString::Printf(TEXT("%sif (%s)\n%s{\n"), *Indent, *VarName, *Indent);
			Result.bIsBranchNode = true;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Output
					&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
					&& Pin->PinName == TEXT("Exit"))
				{
					FBranchOutput Branch;
					Branch.ExecPin = Pin;
					Branch.Label = TEXT("Exit");
					Result.Branches.Add(Branch);
				}
			}
		}
		return Result;
	}

	// Non-branch macros: delegate to existing HandleMacro and wrap result
	FClaireonBPMacroResult MacroResult = HandleMacro(Node, IndentLevel);
	Result.Code = MacroResult.SourceCode;
	Result.MemberDeclarations = MacroResult.MemberDeclarations;
	Result.bIsBranchNode = false;
	return Result;
}
