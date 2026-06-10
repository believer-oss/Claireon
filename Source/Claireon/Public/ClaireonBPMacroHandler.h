// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

class UEdGraphNode;
class UEdGraphPin;

// For FMapNodeResult / FBranchOutput return types used by HandleMacroEx
#include "ClaireonBPNodeMapper.h"

// Result from macro handling -- contains both the source code fragment
// and any member variable declarations needed in the class header
struct FClaireonBPMacroResult
{
	// C++ code fragment for the source file
	FString SourceCode;

	// Member variable declarations to inject into the class header (empty for most macros)
	// Used by DoOnce, DoN, Gate, FlipFlop, MultiGate
	FString MemberDeclarations;

	bool IsEmpty() const { return SourceCode.IsEmpty(); }
};

class FClaireonBPMacroHandler
{
public:
	// Returns true if this node is a known macro that has a special-case handler
	bool IsKnownMacro(const UEdGraphNode* Node) const;

	// Generate idiomatic C++ for a known macro node
	// Returns empty result if not a known macro (caller should fall back to expansion)
	FClaireonBPMacroResult HandleMacro(const UEdGraphNode* Node, int32 IndentLevel) const;

	// Extended macro handling that returns scope-tree metadata
	// ArrivalPin is used for per-input-pin dispatch (Gate, V3-9)
	FMapNodeResult HandleMacroEx(const UEdGraphNode* Node, int32 IndentLevel, const UEdGraphPin* ArrivalPin = nullptr) const;

private:
	FClaireonBPMacroResult HandleForEachLoop(const UEdGraphNode* Node, int32 IndentLevel) const;
	FClaireonBPMacroResult HandleForEachLoopWithBreak(const UEdGraphNode* Node, int32 IndentLevel) const;
	FClaireonBPMacroResult HandleForLoop(const UEdGraphNode* Node, int32 IndentLevel) const;
	FClaireonBPMacroResult HandleForLoopWithBreak(const UEdGraphNode* Node, int32 IndentLevel) const;
	FClaireonBPMacroResult HandleWhileLoop(const UEdGraphNode* Node, int32 IndentLevel) const;
	FClaireonBPMacroResult HandleBranch(const UEdGraphNode* Node, int32 IndentLevel) const;
	FClaireonBPMacroResult HandleSequence(const UEdGraphNode* Node, int32 IndentLevel) const;
	FClaireonBPMacroResult HandleDoOnce(const UEdGraphNode* Node, int32 IndentLevel) const;
	FClaireonBPMacroResult HandleDoN(const UEdGraphNode* Node, int32 IndentLevel) const;
	FClaireonBPMacroResult HandleGate(const UEdGraphNode* Node, int32 IndentLevel) const;
	FClaireonBPMacroResult HandleFlipFlop(const UEdGraphNode* Node, int32 IndentLevel) const;
	FClaireonBPMacroResult HandleIsValid(const UEdGraphNode* Node, int32 IndentLevel) const;
	FClaireonBPMacroResult HandleSelect(const UEdGraphNode* Node, int32 IndentLevel) const;
	FClaireonBPMacroResult HandleMultiGate(const UEdGraphNode* Node, int32 IndentLevel) const;
	FClaireonBPMacroResult HandleSwitchHasAuthority(const UEdGraphNode* Node, int32 IndentLevel) const;
};
