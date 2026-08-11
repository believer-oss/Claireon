// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// WI-2: VariableGet/VariableSet scope-resolution tests for
// ClaireonBlueprintNodeFactory::CreateNode.
//
// The historical defect: with only `variable_name`, the factory called
// VariableReference.SetSelfMember unconditionally, so a name that was really a
// function-local variable or a function input parameter produced a pinless
// unbound node plus a success message ("Added node: Get X") that only failed
// at connect/compile time.
//
// These tests build a throwaway Blueprint under /Game/__MCPTests/ with a
// member variable, a function graph carrying a local variable and an input
// parameter, then drive the factory directly and assert on the created node's
// binding (value pin named after the variable) and on the loud-failure paths.
// The asset is never saved to disk and is force-deleted in teardown.

#if WITH_UNTESTED

#include "Untest.h"

#include "ClaireonBlueprintNodeFactory.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_Variable.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

#include "ClaireonTestAssetDeletion.h"
namespace ClaireonVariableNodeScopeTestsInternal
{
	// File-local discriminator prefix 'VarNodeScope' on every anon-namespace
	// symbol (linux-build-server-v2 unity batching does not isolate anon
	// namespaces across .cpp files).

	static const TCHAR* VarNodeScope_MemberVarName = TEXT("ScopeTestMemberVar");
	static const TCHAR* VarNodeScope_LocalVarName  = TEXT("ScopeTestLocalVar");
	static const TCHAR* VarNodeScope_ParamName     = TEXT("ScopeTestParam");
	static const TCHAR* VarNodeScope_FuncName      = TEXT("ScopeTestFunc");

	// True only when the fixture actually has a .uasset on disk.
	//
	// VarNodeScope_CreateActorBP builds its Blueprint in a package created with
	// CreatePackage() and never saves it, and these tests drive
	// FBlueprintEditorUtils directly rather than any saving tool -- so the fixture
	// lives in an in-memory package only. Deleting an in-memory fixture buys
	// nothing, and every ObjectTools::ForceDeleteObjects call runs a
	// whole-object-graph referencer scan, which is the trigger for the
	// nondeterministic Niagara-serialization crash documented in
	// Docs/llm/todo/claireon-untest-harness-reliability.md item 1.
	//
	// The check is kept rather than dropping the delete outright because
	// /Game/__MCPTests is deliberately NOT gitignored: a stale .uasset left by an
	// older build or a crashed run must still be cleaned so `git status
	// --porcelain -- Content/` stays empty.
	bool VarNodeScope_HasFileOnDisk(const FString& AssetOrPackagePath)
	{
		const FString PackageName = FPackageName::ObjectPathToPackageName(AssetOrPackagePath);
		FString FileName;
		if (!FPackageName::TryConvertLongPackageNameToFilename(
				PackageName, FileName, FPackageName::GetAssetPackageExtension()))
		{
			return false;
		}
		return FPaths::FileExists(FileName);
	}

	void VarNodeScope_CleanupAsset(const FString& AssetPath)
	{
		// In-memory fixture: nothing on disk, nothing to clean, no referencer scan.
		if (!VarNodeScope_HasFileOnDisk(AssetPath))
		{
			return;
		}

		const FString ObjectPath = AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
		if (UObject* Asset = FSoftObjectPath(ObjectPath).TryLoad(); IsValid(Asset))
		{
			TArray<UObject*> AssetsToDelete;
			AssetsToDelete.Add(Asset);
			ClaireonTestAssetDeletion::DeleteObjectsForTest(AssetsToDelete);
		}
	}

	UBlueprint* VarNodeScope_CreateActorBP(const FString& AssetPath)
	{
		UPackage* Package = CreatePackage(*AssetPath);
		if (!IsValid(Package)) return nullptr;

		const FString AssetName = FPackageName::GetShortName(AssetPath);
		return FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(),
			Package,
			FName(*AssetName),
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(),
			NAME_None);
	}

	// Adds the member var, the function graph, its local var, and its input
	// parameter. Returns the function graph, or nullptr if any step failed
	// (asserts live in the test bodies, not here -- UNTEST macros cannot be
	// used outside the test coroutine).
	UEdGraph* VarNodeScope_SetupScopes(UBlueprint* BP)
	{
		if (!IsValid(BP)) return nullptr;

		FEdGraphPinType BoolType;
		BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;

		if (!FBlueprintEditorUtils::AddMemberVariable(BP, FName(VarNodeScope_MemberVarName), BoolType))
		{
			return nullptr;
		}

		UEdGraph* FuncGraph = FBlueprintEditorUtils::CreateNewGraph(
			BP, FName(VarNodeScope_FuncName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (!IsValid(FuncGraph)) return nullptr;
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, FuncGraph, /*bIsUserCreated=*/true, /*SignatureFromClass=*/nullptr);

		if (!FBlueprintEditorUtils::AddLocalVariable(BP, FuncGraph, FName(VarNodeScope_LocalVarName), BoolType, FString()))
		{
			return nullptr;
		}

		UK2Node_FunctionEntry* Entry = nullptr;
		{
			TArray<UK2Node_FunctionEntry*> EntryNodes;
			FuncGraph->GetNodesOfClass<UK2Node_FunctionEntry>(EntryNodes);
			if (EntryNodes.Num() > 0) { Entry = EntryNodes[0]; }
		}
		if (!IsValid(Entry)) return nullptr;

		FEdGraphPinType ParamType;
		ParamType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		if (!Entry->CreateUserDefinedPin(FName(VarNodeScope_ParamName), ParamType, EGPD_Output))
		{
			return nullptr;
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

		return FuncGraph;
	}

	TSharedPtr<FJsonObject> VarNodeScope_MakeParams(const TCHAR* NodeType, const TCHAR* VariableName)
	{
		TSharedPtr<FJsonObject> ParamsObj = MakeShared<FJsonObject>();
		ParamsObj->SetStringField(TEXT("node_type"), NodeType);
		ParamsObj->SetStringField(TEXT("variable_name"), VariableName);
		return ParamsObj;
	}

	UEdGraphPin* VarNodeScope_FindValuePin(UEdGraphNode* Node, const TCHAR* VariableName)
	{
		return IsValid(Node) ? Node->FindPin(VariableName) : nullptr;
	}
} // namespace ClaireonVariableNodeScopeTestsInternal

using namespace ClaireonVariableNodeScopeTestsInternal;

// ============================================================================
// Test 1: Get on a MEMBER variable (session graph = the function graph) binds
// as a self-context member and carries a value pin named after the variable.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, VariableNodeScope, Get_MemberVar_HasValuePin, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_VarNodeScope_Member");
	VarNodeScope_CleanupAsset(AssetPath);

	UBlueprint* BP = VarNodeScope_CreateActorBP(AssetPath);
	UNTEST_ASSERT_PTR(BP);
	UEdGraph* FuncGraph = VarNodeScope_SetupScopes(BP);
	UNTEST_ASSERT_PTR(FuncGraph);

	ClaireonBlueprintNodeFactory::FCreateResult R = ClaireonBlueprintNodeFactory::CreateNode(
		BP, FuncGraph, VarNodeScope_MakeParams(TEXT("VariableGet"), VarNodeScope_MemberVarName), FVector2D(100.0, 100.0));

	UNTEST_ASSERT_TRUE(R.IsOk());
	UNTEST_ASSERT_PTR(R.Node);
	UNTEST_EXPECT_TRUE(FuncGraph->Nodes.Contains(R.Node));
	UNTEST_ASSERT_PTR(VarNodeScope_FindValuePin(R.Node, VarNodeScope_MemberVarName));

	VarNodeScope_CleanupAsset(AssetPath);
	co_return;
}

// ============================================================================
// Test 2 (the core fix): Get on a FUNCTION-LOCAL variable, addressed only by
// variable_name, binds as a local-scope member and carries a value pin -- it
// must not silently produce an unbound self-member node.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, VariableNodeScope, Get_LocalVar_BindsWithValuePin, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_VarNodeScope_Local");
	VarNodeScope_CleanupAsset(AssetPath);

	UBlueprint* BP = VarNodeScope_CreateActorBP(AssetPath);
	UNTEST_ASSERT_PTR(BP);
	UEdGraph* FuncGraph = VarNodeScope_SetupScopes(BP);
	UNTEST_ASSERT_PTR(FuncGraph);

	ClaireonBlueprintNodeFactory::FCreateResult R = ClaireonBlueprintNodeFactory::CreateNode(
		BP, FuncGraph, VarNodeScope_MakeParams(TEXT("VariableGet"), VarNodeScope_LocalVarName), FVector2D(100.0, 100.0));

	UNTEST_ASSERT_TRUE(R.IsOk());
	UNTEST_ASSERT_PTR(R.Node);
	UNTEST_ASSERT_PTR(VarNodeScope_FindValuePin(R.Node, VarNodeScope_LocalVarName));

	UK2Node_Variable* VarNode = Cast<UK2Node_Variable>(R.Node);
	UNTEST_ASSERT_PTR(VarNode);
	UNTEST_EXPECT_TRUE(VarNode->VariableReference.IsLocalScope());

	VarNodeScope_CleanupAsset(AssetPath);
	co_return;
}

// ============================================================================
// Test 3: Get on a FUNCTION INPUT PARAMETER either yields a correctly bound
// node (value pin present) or the specific documented error pointing at the
// K2Node_FunctionEntry output pin -- NEVER a success with zero value pins, and
// on error no orphan node is left in the graph.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, VariableNodeScope, Get_InputParam_BoundOrDocumentedError, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_VarNodeScope_Param");
	VarNodeScope_CleanupAsset(AssetPath);

	UBlueprint* BP = VarNodeScope_CreateActorBP(AssetPath);
	UNTEST_ASSERT_PTR(BP);
	UEdGraph* FuncGraph = VarNodeScope_SetupScopes(BP);
	UNTEST_ASSERT_PTR(FuncGraph);

	const int32 NodeCountBefore = FuncGraph->Nodes.Num();

	ClaireonBlueprintNodeFactory::FCreateResult R = ClaireonBlueprintNodeFactory::CreateNode(
		BP, FuncGraph, VarNodeScope_MakeParams(TEXT("VariableGet"), VarNodeScope_ParamName), FVector2D(100.0, 100.0));

	if (R.IsOk())
	{
		// Correctly bound: the node must carry the value pin.
		UNTEST_ASSERT_PTR(R.Node);
		UNTEST_ASSERT_PTR(VarNodeScope_FindValuePin(R.Node, VarNodeScope_ParamName));
	}
	else
	{
		// Documented error path: names the K2Node_FunctionEntry redirect and
		// leaves no orphan node behind.
		UNTEST_EXPECT_TRUE(R.Error.Contains(TEXT("K2Node_FunctionEntry")));
		UNTEST_EXPECT_TRUE(R.Error.Contains(VarNodeScope_ParamName));
		UNTEST_EXPECT_EQ(FuncGraph->Nodes.Num(), NodeCountBefore);
	}

	VarNodeScope_CleanupAsset(AssetPath);
	co_return;
}

// ============================================================================
// Test 4: Get on a NONEXISTENT name errors (naming the member/local/parameter
// scope contract) and leaves no orphan node in the graph.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, VariableNodeScope, Get_Nonexistent_ErrorAndNoOrphanNode, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_VarNodeScope_Missing");
	VarNodeScope_CleanupAsset(AssetPath);

	UBlueprint* BP = VarNodeScope_CreateActorBP(AssetPath);
	UNTEST_ASSERT_PTR(BP);
	UEdGraph* FuncGraph = VarNodeScope_SetupScopes(BP);
	UNTEST_ASSERT_PTR(FuncGraph);

	const int32 NodeCountBefore = FuncGraph->Nodes.Num();

	ClaireonBlueprintNodeFactory::FCreateResult R = ClaireonBlueprintNodeFactory::CreateNode(
		BP, FuncGraph, VarNodeScope_MakeParams(TEXT("VariableGet"), TEXT("TotallyBogusVariableName")), FVector2D(100.0, 100.0));

	UNTEST_ASSERT_FALSE(R.IsOk());
	UNTEST_EXPECT_TRUE(R.Error.Contains(TEXT("not found in member/local/parameter scope")));
	UNTEST_EXPECT_EQ(FuncGraph->Nodes.Num(), NodeCountBefore);

	VarNodeScope_CleanupAsset(AssetPath);
	co_return;
}

// ============================================================================
// Test 5: The Set half shares the resolution chain -- Set on the function-local
// variable binds and carries the value pin (exec pins alone are not enough).
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, VariableNodeScope, Set_LocalVar_BindsWithValuePin, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_VarNodeScope_SetLocal");
	VarNodeScope_CleanupAsset(AssetPath);

	UBlueprint* BP = VarNodeScope_CreateActorBP(AssetPath);
	UNTEST_ASSERT_PTR(BP);
	UEdGraph* FuncGraph = VarNodeScope_SetupScopes(BP);
	UNTEST_ASSERT_PTR(FuncGraph);

	ClaireonBlueprintNodeFactory::FCreateResult R = ClaireonBlueprintNodeFactory::CreateNode(
		BP, FuncGraph, VarNodeScope_MakeParams(TEXT("VariableSet"), VarNodeScope_LocalVarName), FVector2D(100.0, 100.0));

	UNTEST_ASSERT_TRUE(R.IsOk());
	UNTEST_ASSERT_PTR(R.Node);
	UNTEST_ASSERT_PTR(VarNodeScope_FindValuePin(R.Node, VarNodeScope_LocalVarName));

	UK2Node_Variable* VarNode = Cast<UK2Node_Variable>(R.Node);
	UNTEST_ASSERT_PTR(VarNode);
	UNTEST_EXPECT_TRUE(VarNode->VariableReference.IsLocalScope());

	VarNodeScope_CleanupAsset(AssetPath);
	co_return;
}

#endif // WITH_UNTESTED
