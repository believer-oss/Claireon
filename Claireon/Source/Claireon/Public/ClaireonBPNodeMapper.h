// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

class UEdGraphNode;
class UEdGraphPin;
struct FEdGraphPinType;

struct FBranchOutput
{
	const UEdGraphPin* ExecPin = nullptr;  // The exec output pin for this branch
	FString Label;                         // Human-readable label (e.g., "Authority", "Remote", "then_0")
};

struct FMapNodeResult
{
	FString Code;                    // C++ code for this node (header only for branch nodes)
	bool bIsBranchNode = false;      // True if multiple exec outputs requiring scoped emission
	bool bIsLatentSplit = false;     // V8: latent call splits function at this point
	TArray<FBranchOutput> Branches;  // Pin-to-label mapping for each branch output
	FString MemberDeclarations;      // Member variable declarations needed in header (from macros)
};

class FClaireonBPNodeMapper
{
public:
	// Convert a single node to C++ code with //[BP] annotation
	// Non-const: accumulates includes during mapping
	FString MapNode(const UEdGraphNode* Node, int32 IndentLevel);

	// Convert an FEdGraphPinType to a C++ type string
	// Examples: bool, float, int32, FVector, TArray<FString>, TSubclassOf<AActor>
	FString PinTypeToCppType(const FEdGraphPinType& PinType) const;

	// Generate UPROPERTY specifier string from property flags
	// Example: "EditAnywhere, BlueprintReadWrite, Category=\"Default\""
	FString PropertyFlagsToSpecifiers(uint64 PropertyFlags, const FString& Category) const;

	// Generate UFUNCTION specifier string from function flags
	// Example: "BlueprintCallable, Category=\"Gameplay\""
	FString FunctionFlagsToSpecifiers(uint64 FunctionFlags, const FString& Category) const;

	// Infer the #include path needed for a given pin type
	// Returns empty string if the type is from CoreMinimal or the same module
	FString InferIncludePath(const FEdGraphPinType& PinType) const;

	// Format a //[BP] tag line for a node
	FString FormatBPTag(const UEdGraphNode* Node) const;

	// Sanitize a string to be a valid C++ identifier (replace non-alnum with _, strip invalid chars)
	static FString SanitizeCppIdentifier(const FString& Name);

	// V4-7: Strip "Public/" or "Classes/" prefix from ModuleRelativePath metadata
	static FString StripModuleRelativePrefix(const FString& Path);

	// Include accumulation (P1-10)
	void AddInclude(const FString& Path);
	const TSet<FString>& GetAccumulatedIncludes() const;

	// Connected pin expression (P3-20 prep -- improved in stage 014)
	FString GetConnectedPinExpression(const UEdGraphPin* Pin) const;

	// V7-1: Return the set of pure nodes that were inlined during expression resolution.
	// These nodes are visited but not tracked by EmitScope, so callers must add them
	// to AllNodes/VisitedNodes for accurate orphan classification.
	const TSet<const UEdGraphNode*>& GetInlinedPureNodes() const { return InlinedPureNodes; }
	void ClearInlinedPureNodes() { InlinedPureNodes.Empty(); }

	// Extended node mapping that returns branch metadata for scope-tree traversal.
	// ArrivalPin is the exec input pin used to reach this node (used by Gate, Timeline).
	FMapNodeResult MapNodeEx(const UEdGraphNode* Node, int32 IndentLevel, const UEdGraphPin* ArrivalPin = nullptr);

private:
	TSet<FString> AccumulatedIncludes;

	// V7-1: Track pure nodes inlined by GetConnectedPinExpression
	mutable TSet<const UEdGraphNode*> InlinedPureNodes;

	// Node-type-specific mappers
	FString MapCallFunctionNode(const UEdGraphNode* Node, int32 IndentLevel);
	FString MapVariableGetNode(const UEdGraphNode* Node, int32 IndentLevel) const;
	FString MapVariableSetNode(const UEdGraphNode* Node, int32 IndentLevel);
	FString MapBranchNode(const UEdGraphNode* Node, int32 IndentLevel) const;
	FString MapReturnNode(const UEdGraphNode* Node, int32 IndentLevel) const;
	FString MapCastNode(const UEdGraphNode* Node, int32 IndentLevel);
	FString MapSpawnActorNode(const UEdGraphNode* Node, int32 IndentLevel);
	FString MapEventNode(const UEdGraphNode* Node, int32 IndentLevel) const;
	FString MapCustomEventNode(const UEdGraphNode* Node, int32 IndentLevel) const;
	FString MapCallParentFunctionNode(const UEdGraphNode* Node, int32 IndentLevel);
	FString MapCallDelegateNode(const UEdGraphNode* Node, int32 IndentLevel);
	FString MapClearDelegateNode(const UEdGraphNode* Node, int32 IndentLevel);
	FString MapAddDelegateNode(const UEdGraphNode* Node, int32 IndentLevel);
	FString MapDelegateNode(const UEdGraphNode* Node, int32 IndentLevel);
	FString MapExecutionSequenceNode(const UEdGraphNode* Node, int32 IndentLevel) const;
	FString MapSwitchEnumNode(const UEdGraphNode* Node, int32 IndentLevel) const;
	FString MapSwitchIntegerNode(const UEdGraphNode* Node, int32 IndentLevel) const;
	FString MapSwitchStringNode(const UEdGraphNode* Node, int32 IndentLevel) const;
	FString MapSwitchNameNode(const UEdGraphNode* Node, int32 IndentLevel) const;
	FString MapFunctionEntryNode(const UEdGraphNode* Node, int32 IndentLevel) const;
	FString MapMakeStructNode(const UEdGraphNode* Node, int32 IndentLevel) const;
	FString MapBreakStructNode(const UEdGraphNode* Node, int32 IndentLevel) const;
	FString MapArrayOperationNode(const UEdGraphNode* Node, int32 IndentLevel);
	FString MapTimelineNode(const UEdGraphNode* Node, int32 IndentLevel);
	FString MapAsyncActionNode(const UEdGraphNode* Node, int32 IndentLevel);
	FString MapUnknownNode(const UEdGraphNode* Node, int32 IndentLevel) const;

	// V3 branch-aware Ex variants (return FMapNodeResult with branch metadata)
	FMapNodeResult MapTimelineNodeEx(const UEdGraphNode* Node, int32 IndentLevel, const UEdGraphPin* ArrivalPin);
	FMapNodeResult MapAsyncActionNodeEx(const UEdGraphNode* Node, int32 IndentLevel);
	FMapNodeResult MapBranchNodeEx(const UEdGraphNode* Node, int32 IndentLevel) const;
	FMapNodeResult MapExecutionSequenceNodeEx(const UEdGraphNode* Node, int32 IndentLevel) const;
	FMapNodeResult MapSwitchEnumNodeEx(const UEdGraphNode* Node, int32 IndentLevel) const;
	FMapNodeResult MapSwitchIntegerNodeEx(const UEdGraphNode* Node, int32 IndentLevel) const;
	FMapNodeResult MapSwitchStringNodeEx(const UEdGraphNode* Node, int32 IndentLevel) const;
	FMapNodeResult MapSwitchNameNodeEx(const UEdGraphNode* Node, int32 IndentLevel) const;
	FMapNodeResult MapSwitchGameplayTagNodeEx(const UEdGraphNode* Node, int32 IndentLevel) const;

	// V8: Latent call function (Delay, RetriggerableDelay, etc.)
	FMapNodeResult MapLatentCallFunctionNodeEx(const UEdGraphNode* Node, int32 IndentLevel);

	// V8: Ex variants for member variable promotion
	FMapNodeResult MapCallFunctionNodeEx(const UEdGraphNode* Node, int32 IndentLevel);
	FMapNodeResult MapCastNodeEx(const UEdGraphNode* Node, int32 IndentLevel);
	FMapNodeResult MapSpawnActorNodeEx(const UEdGraphNode* Node, int32 IndentLevel);
};
