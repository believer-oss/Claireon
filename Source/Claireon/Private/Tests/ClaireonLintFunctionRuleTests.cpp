// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Function-scope rules inspect declarations, purity, and categorization without duplicating graph rules.

#if WITH_UNTESTED

#include "Untest.h"

#include "ClaireonBlueprintHelpers.h"
#include "ClaireonLintTypes.h"
#include "Tools/ClaireonTool_Lint.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_VariableSet.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"

namespace ClaireonLintFunctionRuleTestsNS
{
	// Prefix helpers to avoid unity-build collisions.

	struct FLintFnFixture
	{
		UBlueprint* Blueprint = nullptr;
		FString ObjectPath;
	};

	/** A scratch Actor Blueprint under /Game/__MCPTests, with no functions yet. */
	static bool LintFn_MakeBlueprint(FLintFnFixture& Out)
	{
		static int32 Counter = 0;
		const FString AssetName = FString::Printf(TEXT("BP_LintFnRules_%d"), Counter++);
		const FString PackagePath = FString(TEXT("/Game/__MCPTests/")) + AssetName;
		UPackage* Package = CreatePackage(*PackagePath);
		if (!IsValid(Package))
		{
			return false;
		}
		Out.Blueprint = FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), Package, FName(*AssetName), BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), NAME_None);
		if (!IsValid(Out.Blueprint))
		{
			return false;
		}
		Out.ObjectPath = PackagePath + TEXT(".") + AssetName;
		return true;
	}

	/** Add a function graph and return it, or null. */
	static UEdGraph* LintFn_AddFunction(UBlueprint* BP, const TCHAR* Name)
	{
		if (!IsValid(BP))
		{
			return nullptr;
		}
		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(
			BP, FName(Name), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (!IsValid(Graph))
		{
			return nullptr;
		}
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, Graph, /*bIsUserCreated=*/true, nullptr);
		return Graph;
	}

	static UK2Node_FunctionEntry* LintFn_Entry(UEdGraph* Graph)
	{
		if (!IsValid(Graph))
		{
			return nullptr;
		}
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node); IsValid(Entry))
			{
				return Entry;
			}
		}
		return nullptr;
	}

	/** Mark a function pure by setting the entry's extra flags, the way the setter does. */
	static void LintFn_SetPure(UEdGraph* Graph, bool bPure)
	{
		if (UK2Node_FunctionEntry* Entry = LintFn_Entry(Graph); IsValid(Entry))
		{
			const int32 Flags = Entry->GetExtraFlags();
			Entry->SetExtraFlags(bPure ? (Flags | FUNC_BlueprintPure) : (Flags & ~FUNC_BlueprintPure));
		}
	}

	static void LintFn_SetCategory(UEdGraph* Graph, const TCHAR* Category)
	{
		if (UK2Node_FunctionEntry* Entry = LintFn_Entry(Graph); IsValid(Entry))
		{
			Entry->MetaData.Category = FText::FromString(Category);
		}
	}

	/** Use a real impure UFunction so the rule observes a side effect rather than an unresolved call. */
	static bool LintFn_AddImpureCall(UEdGraph* Graph)
	{
		UFunction* Fn = UKismetSystemLibrary::StaticClass()->FindFunctionByName(FName(TEXT("PrintString")));
		if (!IsValid(Graph) || !IsValid(Fn) || Fn->HasAnyFunctionFlags(FUNC_BlueprintPure))
		{
			return false;
		}
		UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph);
		Graph->AddNode(Call, /*bUserAction=*/false, /*bSelectNewNode=*/false);
		Call->CreateNewGuid();
		Call->SetFromFunction(Fn);
		Call->PostPlacedNewNode();
		Call->AllocateDefaultPins();
		return true;
	}

	/** Put a PURE call in the graph -- a math node, which is BlueprintPure. */
	static bool LintFn_AddPureCall(UEdGraph* Graph)
	{
		UFunction* Fn = UKismetMathLibrary::StaticClass()->FindFunctionByName(FName(TEXT("Add_IntInt")));
		if (!IsValid(Graph) || !IsValid(Fn) || !Fn->HasAnyFunctionFlags(FUNC_BlueprintPure))
		{
			return false;
		}
		UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph);
		Graph->AddNode(Call, /*bUserAction=*/false, /*bSelectNewNode=*/false);
		Call->CreateNewGuid();
		Call->SetFromFunction(Fn);
		Call->PostPlacedNewNode();
		Call->AllocateDefaultPins();
		return true;
	}

	/** Every finding of one rule, as (graph_name, target) pairs. */
	static TArray<TPair<FString, FString>> LintFn_Findings(
		const FString& ObjectPath, const TCHAR* Rule, const TCHAR* Scope = TEXT("functions"))
	{
		TArray<TPair<FString, FString>> Out;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), ObjectPath);
		TArray<TSharedPtr<FJsonValue>> ScopeArr;
		ScopeArr.Add(MakeShared<FJsonValueString>(Scope));
		Args->SetArrayField(TEXT("scope"), ScopeArr);

		ClaireonTool_Lint Tool;
		const IClaireonTool::FToolResult R = Tool.Execute(Args);
		if (R.bIsError || !R.Data.IsValid())
		{
			return Out;
		}
		const TArray<TSharedPtr<FJsonValue>>* Findings = nullptr;
		if (!R.Data->TryGetArrayField(TEXT("findings"), Findings) || !Findings)
		{
			return Out;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Findings)
		{
			const TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Obj.IsValid())
			{
				continue;
			}
			FString FoundRule;
			if (Obj->TryGetStringField(TEXT("rule"), FoundRule) && FoundRule == Rule)
			{
				FString GraphName, Target;
				Obj->TryGetStringField(TEXT("graph_name"), GraphName);
				Obj->TryGetStringField(TEXT("target"), Target);
				Out.Add({GraphName, Target});
			}
		}
		return Out;
	}
}
using namespace ClaireonLintFunctionRuleTestsNS;

// Pure functions with side effects.

UNTEST_UNIT_OPTS(Claireon, LintFunctionRules, PureWithSideEffects_IsReported, UNTEST_TIMEOUTMS(60000))
{
	FLintFnFixture F;
	UNTEST_ASSERT_TRUE(LintFn_MakeBlueprint(F));

	UEdGraph* Bad = LintFn_AddFunction(F.Blueprint, TEXT("PureButPrints"));
	UNTEST_ASSERT_TRUE(IsValid(Bad));
	LintFn_SetPure(Bad, true);
	LintFn_SetCategory(Bad, TEXT("Test"));
	UNTEST_ASSERT_TRUE(LintFn_AddImpureCall(Bad));

	UEdGraph* Good = LintFn_AddFunction(F.Blueprint, TEXT("PureAndClean"));
	UNTEST_ASSERT_TRUE(IsValid(Good));
	LintFn_SetPure(Good, true);
	LintFn_SetCategory(Good, TEXT("Test"));
	UNTEST_ASSERT_TRUE(LintFn_AddPureCall(Good));

	const TArray<TPair<FString, FString>> Hits =
		LintFn_Findings(F.ObjectPath, TEXT("pure-function-has-side-effects"));

	UNTEST_ASSERT_EQ(Hits.Num(), 1);
	UNTEST_EXPECT_TRUE(Hits[0].Value == TEXT("PureButPrints"));

	UNTEST_EXPECT_TRUE(Hits[0].Key == TEXT("PureButPrints"));
	co_return;
}

// Function-local writes are not externally observable side effects.
UNTEST_UNIT_OPTS(Claireon, LintFunctionRules, PureWithLocalVariableSet_IsSilent, UNTEST_TIMEOUTMS(60000))
{
	FLintFnFixture F;
	UNTEST_ASSERT_TRUE(LintFn_MakeBlueprint(F));

	UEdGraph* Graph = LintFn_AddFunction(F.Blueprint, TEXT("PureWithLocal"));
	UNTEST_ASSERT_TRUE(IsValid(Graph));
	LintFn_SetPure(Graph, true);
	LintFn_SetCategory(Graph, TEXT("Test"));

	FEdGraphPinType IntType;
	IntType.PinCategory = UEdGraphSchema_K2::PC_Int;
	UNTEST_ASSERT_TRUE(FBlueprintEditorUtils::AddLocalVariable(F.Blueprint, Graph, FName(TEXT("Scratch")), IntType));

	UK2Node_FunctionEntry* Entry = LintFn_Entry(Graph);
	UNTEST_ASSERT_TRUE(IsValid(Entry));
	// Avoid lambdas in this coroutine test body because they trigger an MSVC internal compiler error.
	const FBPVariableDescription* Local = nullptr;
	for (const FBPVariableDescription& D : Entry->LocalVariables)
	{
		if (D.VarName == FName(TEXT("Scratch")))
		{
			Local = &D;
			break;
		}
	}
	UNTEST_ASSERT_TRUE(Local != nullptr);

	UK2Node_VariableSet* Setter = NewObject<UK2Node_VariableSet>(Graph);
	Graph->AddNode(Setter, /*bUserAction=*/false, /*bSelectNewNode=*/false);
	Setter->CreateNewGuid();
	Setter->VariableReference.SetLocalMember(Local->VarName, Graph->GetName(), Local->VarGuid);
	UNTEST_ASSERT_TRUE(Setter->VariableReference.IsLocalScope());
	Setter->AllocateDefaultPins();

	UNTEST_EXPECT_EQ(LintFn_Findings(F.ObjectPath, TEXT("pure-function-has-side-effects")).Num(), 0);

	UEdGraph* Bad = LintFn_AddFunction(F.Blueprint, TEXT("PureWritesMember"));
	UNTEST_ASSERT_TRUE(IsValid(Bad));
	LintFn_SetPure(Bad, true);
	LintFn_SetCategory(Bad, TEXT("Test"));
	FBPVariableDescription Member;
	Member.VarName = FName(TEXT("Counter"));
	Member.VarGuid = FGuid::NewGuid();
	Member.VarType = IntType;
	F.Blueprint->NewVariables.Add(Member);
	UK2Node_VariableSet* MemberSetter = NewObject<UK2Node_VariableSet>(Bad);
	Bad->AddNode(MemberSetter, /*bUserAction=*/false, /*bSelectNewNode=*/false);
	MemberSetter->CreateNewGuid();
	MemberSetter->VariableReference.SetSelfMember(Member.VarName);
	MemberSetter->AllocateDefaultPins();

	const TArray<TPair<FString, FString>> Hits =
		LintFn_Findings(F.ObjectPath, TEXT("pure-function-has-side-effects"));
	UNTEST_ASSERT_EQ(Hits.Num(), 1);
	UNTEST_EXPECT_TRUE(Hits[0].Value == TEXT("PureWritesMember"));
	co_return;
}

// Impure functions may have side effects.
UNTEST_UNIT_OPTS(Claireon, LintFunctionRules, ImpureWithSideEffects_IsSilent, UNTEST_TIMEOUTMS(60000))
{
	FLintFnFixture F;
	UNTEST_ASSERT_TRUE(LintFn_MakeBlueprint(F));

	UEdGraph* Graph = LintFn_AddFunction(F.Blueprint, TEXT("ImpureAndPrints"));
	UNTEST_ASSERT_TRUE(IsValid(Graph));
	LintFn_SetPure(Graph, false);
	LintFn_SetCategory(Graph, TEXT("Test"));
	UNTEST_ASSERT_TRUE(LintFn_AddImpureCall(Graph));

	UNTEST_EXPECT_EQ(LintFn_Findings(F.ObjectPath, TEXT("pure-function-has-side-effects")).Num(), 0);
	co_return;
}

// Function categories.

UNTEST_UNIT_OPTS(Claireon, LintFunctionRules, Uncategorized_ReportsAndSuggestsTheSetter,
	UNTEST_TIMEOUTMS(60000))
{
	FLintFnFixture F;
	UNTEST_ASSERT_TRUE(LintFn_MakeBlueprint(F));

	UEdGraph* NoCategory = LintFn_AddFunction(F.Blueprint, TEXT("NeedsACategory"));
	UNTEST_ASSERT_TRUE(IsValid(NoCategory));

	UEdGraph* Defaulted = LintFn_AddFunction(F.Blueprint, TEXT("AlsoNeedsOne"));
	UNTEST_ASSERT_TRUE(IsValid(Defaulted));
	LintFn_SetCategory(Defaulted, TEXT("Default"));   // "Default" is not a category

	UEdGraph* Categorised = LintFn_AddFunction(F.Blueprint, TEXT("HasOne"));
	UNTEST_ASSERT_TRUE(IsValid(Categorised));
	LintFn_SetCategory(Categorised, TEXT("Combat|Targeting"));

    TArray<FString> Targets;
	for (const TPair<FString, FString>& Hit : LintFn_Findings(F.ObjectPath, TEXT("function-uncategorized")))
	{
		Targets.Add(Hit.Value);
	}
	Targets.Sort();

	UNTEST_ASSERT_EQ(Targets.Num(), 2);
	UNTEST_EXPECT_TRUE(Targets[0] == TEXT("AlsoNeedsOne"));
	UNTEST_EXPECT_TRUE(Targets[1] == TEXT("NeedsACategory"));
	co_return;
}

// Function scope remains distinct from graph scope.

// Keep graph rules out of the function family.
UNTEST_UNIT_OPTS(Claireon, LintFunctionRules, FunctionScope_DoesNotDuplicateGraphScope,
	UNTEST_TIMEOUTMS(60000))
{
	FLintFnFixture F;
	UNTEST_ASSERT_TRUE(LintFn_MakeBlueprint(F));
	UEdGraph* Graph = LintFn_AddFunction(F.Blueprint, TEXT("SomeFunction"));
	UNTEST_ASSERT_TRUE(IsValid(Graph));
	UNTEST_ASSERT_TRUE(LintFn_AddImpureCall(Graph));

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), F.ObjectPath);
	TArray<TSharedPtr<FJsonValue>> ScopeArr;
	ScopeArr.Add(MakeShared<FJsonValueString>(TEXT("functions")));
	Args->SetArrayField(TEXT("scope"), ScopeArr);

	ClaireonTool_Lint Tool;
	const IClaireonTool::FToolResult R = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(R.bIsError);
	UNTEST_ASSERT_TRUE(R.Data.IsValid());

	const TArray<TSharedPtr<FJsonValue>>* Findings = nullptr;
	UNTEST_ASSERT_TRUE(R.Data->TryGetArrayField(TEXT("findings"), Findings) && Findings);
	for (const TSharedPtr<FJsonValue>& Value : *Findings)
	{
		const TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
		if (!Obj.IsValid()) { continue; }
		FString Scope;
		UNTEST_EXPECT_TRUE(Obj->TryGetStringField(TEXT("scope"), Scope));
		UNTEST_EXPECT_TRUE(Scope == TEXT("functions"));
	}

	const TSharedPtr<FJsonObject>* Subjects = nullptr;
	UNTEST_ASSERT_TRUE(R.Data->TryGetObjectField(TEXT("scope_subjects"), Subjects) && Subjects);
	FString FunctionsSubject;
	UNTEST_EXPECT_TRUE((*Subjects)->TryGetStringField(TEXT("functions"), FunctionsSubject));
	UNTEST_EXPECT_TRUE(FunctionsSubject == TEXT("graph"));
	co_return;
}

// Ignore the engine-owned construction-script signature.
UNTEST_UNIT_OPTS(Claireon, LintFunctionRules, ConstructionScript_IsNeverReported,
	UNTEST_TIMEOUTMS(60000))
{
	FLintFnFixture F;
	UNTEST_ASSERT_TRUE(LintFn_MakeBlueprint(F));

	for (const TCHAR* Rule : {TEXT("function-uncategorized"), TEXT("pure-function-has-side-effects"),
	                          TEXT("function-could-be-pure")})
	{
		for (const TPair<FString, FString>& Hit : LintFn_Findings(F.ObjectPath, Rule))
		{
			UNTEST_EXPECT_FALSE(Hit.Value.Contains(TEXT("UserConstructionScript")));
		}
	}
	co_return;
}

// graph_name must filter function findings.
UNTEST_UNIT_OPTS(Claireon, LintFunctionRules, GraphName_FiltersFunctionFindings,
	UNTEST_TIMEOUTMS(60000))
{
	FLintFnFixture F;
	UNTEST_ASSERT_TRUE(LintFn_MakeBlueprint(F));
	UNTEST_ASSERT_TRUE(IsValid(LintFn_AddFunction(F.Blueprint, TEXT("FirstUncategorised"))));
	UNTEST_ASSERT_TRUE(IsValid(LintFn_AddFunction(F.Blueprint, TEXT("SecondUncategorised"))));

	UNTEST_EXPECT_EQ(LintFn_Findings(F.ObjectPath, TEXT("function-uncategorized")).Num(), 2);

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), F.ObjectPath);
	Args->SetStringField(TEXT("graph_name"), TEXT("FirstUncategorised"));
	TArray<TSharedPtr<FJsonValue>> ScopeArr;
	ScopeArr.Add(MakeShared<FJsonValueString>(TEXT("functions")));
	Args->SetArrayField(TEXT("scope"), ScopeArr);

	ClaireonTool_Lint Tool;
	const IClaireonTool::FToolResult R = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(R.bIsError);

	const TArray<TSharedPtr<FJsonValue>>* Findings = nullptr;
	UNTEST_ASSERT_TRUE(R.Data->TryGetArrayField(TEXT("findings"), Findings) && Findings);
	int32 Uncategorised = 0;
	for (const TSharedPtr<FJsonValue>& Value : *Findings)
	{
		const TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
		FString Rule, GraphName;
		if (Obj.IsValid() && Obj->TryGetStringField(TEXT("rule"), Rule)
			&& Rule == TEXT("function-uncategorized"))
		{
			++Uncategorised;
			UNTEST_EXPECT_TRUE(Obj->TryGetStringField(TEXT("graph_name"), GraphName));
			UNTEST_EXPECT_TRUE(GraphName == TEXT("FirstUncategorised"));
		}
	}
	UNTEST_EXPECT_EQ(Uncategorised, 1);
	co_return;
}


// Skip inherited signatures the author cannot edit; ReceiveTick supplies the parent-owned case.
UNTEST_UNIT_OPTS(Claireon, LintFunctionRules, Overrides_AreSkipped, UNTEST_TIMEOUTMS(60000))
{
	FLintFnFixture F;
	UNTEST_ASSERT_TRUE(LintFn_MakeBlueprint(F));

	UNTEST_ASSERT_TRUE(AActor::StaticClass()->FindFunctionByName(FName(TEXT("ReceiveTick"))) != nullptr);

	UEdGraph* Override = LintFn_AddFunction(F.Blueprint, TEXT("ReceiveTick"));
	UNTEST_ASSERT_TRUE(IsValid(Override));
	// Give the override both defects so either rule would reveal a missing skip.
	LintFn_SetPure(Override, true);
	UNTEST_ASSERT_TRUE(LintFn_AddImpureCall(Override));

	// Include an author-owned function with the same defects as a negative control for skipping.
	UEdGraph* Own = LintFn_AddFunction(F.Blueprint, TEXT("MyOwnHelper"));
	UNTEST_ASSERT_TRUE(IsValid(Own));
	LintFn_SetPure(Own, true);
	UNTEST_ASSERT_TRUE(LintFn_AddImpureCall(Own));

	for (const TCHAR* Rule : {TEXT("function-uncategorized"), TEXT("pure-function-has-side-effects")})
	{
		TArray<FString> Targets;
		for (const TPair<FString, FString>& Hit : LintFn_Findings(F.ObjectPath, Rule))
		{
			Targets.Add(Hit.Value);
		}
		UNTEST_EXPECT_FALSE(Targets.Contains(FString(TEXT("ReceiveTick"))));
		UNTEST_EXPECT_TRUE(Targets.Contains(FString(TEXT("MyOwnHelper"))));
	}
	co_return;
}

#endif // WITH_UNTESTED
