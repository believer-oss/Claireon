// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Test transient-rule suppressions for designer defaults, replication, SaveGame, construction scripts,
// and expose-on-spawn values written by graphs.

#if WITH_UNTESTED

#include "Untest.h"

#include "ClaireonLintTypes.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"

namespace ClaireonLintVariableTestsNS
{
	// Prefix helpers to avoid unity-build collisions.

	struct FLintVarFixture
	{
		UBlueprint* Blueprint = nullptr;
		UEdGraph* Graph = nullptr;
	};

	static FLintVarFixture LintVar_MakeFixture()
	{
		FLintVarFixture Fixture;
		Fixture.Blueprint = FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), GetTransientPackage(), NAME_None, BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		if (!IsValid(Fixture.Blueprint))
		{
			return Fixture;
		}
		Fixture.Graph = Fixture.Blueprint->UbergraphPages.Num() > 0
			? Fixture.Blueprint->UbergraphPages[0]
			: nullptr;
		return Fixture;
	}

	/** Add a member variable with exact property flags and optional metadata. */
	static void LintVar_AddVariable(UBlueprint* Blueprint, const TCHAR* Name, uint64 Flags,
	                                const TCHAR* MetaKey = nullptr)
	{
		FBPVariableDescription Desc;
		Desc.VarName = FName(Name);
		Desc.VarGuid = FGuid::NewGuid();
		Desc.VarType.PinCategory = UEdGraphSchema_K2::PC_Int;
		Desc.PropertyFlags = Flags;
		if (MetaKey)
		{
			FBPVariableMetaDataEntry Entry;
			Entry.DataKey = FName(MetaKey);
			Entry.DataValue = TEXT("true");
			Desc.MetaDataArray.Add(Entry);
		}
		Blueprint->NewVariables.Add(Desc);
	}

	template <typename TNode>
	static void LintVar_AddVarNode(UEdGraph* Graph, const TCHAR* VarName)
	{
		TNode* Node = NewObject<TNode>(Graph, NAME_None, RF_Transient);
		Node->VariableReference.SetSelfMember(FName(VarName));
		Graph->Nodes.Add(Node);
		Node->CreateNewGuid();
	}

	static int32 LintVar_CountRule(const TArray<FClaireonLintFinding>& Findings,
	                               const TCHAR* Rule, const TCHAR* Target)
	{
		int32 Count = 0;
		for (const FClaireonLintFinding& Finding : Findings)
		{
			if (Finding.Rule == Rule && Finding.Target == Target)
			{
				++Count;
			}
		}
		return Count;
	}
}

// Vary suppressor flags on otherwise graph-written, non-transient variables.
UNTEST_UNIT_OPTS(Claireon, LintVariables, LintVariables_TransientSuppressionMatrix, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonLintVariableTestsNS;
	FLintVarFixture Fixture = LintVar_MakeFixture();
	UNTEST_ASSERT_TRUE(IsValid(Fixture.Blueprint));
	UNTEST_ASSERT_TRUE(IsValid(Fixture.Graph));

	LintVar_AddVariable(Fixture.Blueprint, TEXT("PlainDerived"), 0);
	LintVar_AddVariable(Fixture.Blueprint, TEXT("SavedState"),     CPF_SaveGame);
	LintVar_AddVariable(Fixture.Blueprint, TEXT("ConfigValue"),    CPF_Config);
	LintVar_AddVariable(Fixture.Blueprint, TEXT("ReplicatedVal"),  CPF_Net);
	LintVar_AddVariable(Fixture.Blueprint, TEXT("DesignerTweak"),  CPF_Edit);
	LintVar_AddVariable(Fixture.Blueprint, TEXT("SpawnParam"),     0, TEXT("ExposeOnSpawn"));

	for (const TCHAR* Name : {TEXT("PlainDerived"), TEXT("SavedState"), TEXT("ConfigValue"),
	                          TEXT("ReplicatedVal"), TEXT("DesignerTweak"), TEXT("SpawnParam")})
	{
		LintVar_AddVarNode<UK2Node_VariableSet>(Fixture.Graph, Name);
	}

	TArray<FClaireonLintFinding> Findings;
	FClaireonLintContext Context;
	ClaireonLint::RunVariableRules(Context, Fixture.Blueprint, Findings);

	UNTEST_EXPECT_EQ(LintVar_CountRule(Findings, TEXT("transient-not-marked"), TEXT("PlainDerived")), 1);
	UNTEST_EXPECT_EQ(LintVar_CountRule(Findings, TEXT("transient-not-marked"), TEXT("SavedState")), 0);
	UNTEST_EXPECT_EQ(LintVar_CountRule(Findings, TEXT("transient-not-marked"), TEXT("ConfigValue")), 0);
	UNTEST_EXPECT_EQ(LintVar_CountRule(Findings, TEXT("transient-not-marked"), TEXT("ReplicatedVal")), 0);
	UNTEST_EXPECT_EQ(LintVar_CountRule(Findings, TEXT("transient-not-marked"), TEXT("DesignerTweak")), 0);
	UNTEST_EXPECT_EQ(LintVar_CountRule(Findings, TEXT("transient-not-marked"), TEXT("SpawnParam")), 0);
	co_return;
}

// Already-transient variables remain unreported.
UNTEST_UNIT_OPTS(Claireon, LintVariables, LintVariables_TransientMarkedIsSilent, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonLintVariableTestsNS;
	FLintVarFixture Fixture = LintVar_MakeFixture();
	UNTEST_ASSERT_TRUE(IsValid(Fixture.Blueprint));
	UNTEST_ASSERT_TRUE(IsValid(Fixture.Graph));

	LintVar_AddVariable(Fixture.Blueprint, TEXT("AlreadyTransient"), CPF_Transient);
	LintVar_AddVarNode<UK2Node_VariableSet>(Fixture.Graph, TEXT("AlreadyTransient"));

	TArray<FClaireonLintFinding> Findings;
	FClaireonLintContext Context;
	ClaireonLint::RunVariableRules(Context, Fixture.Blueprint, Findings);

	UNTEST_EXPECT_EQ(LintVar_CountRule(Findings, TEXT("transient-not-marked"), TEXT("AlreadyTransient")), 0);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, LintVariables, LintVariables_InstanceEditableIsWarning, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonLintVariableTestsNS;
	FLintVarFixture Fixture = LintVar_MakeFixture();
	UNTEST_ASSERT_TRUE(IsValid(Fixture.Blueprint));
	UNTEST_ASSERT_TRUE(IsValid(Fixture.Graph));

	LintVar_AddVariable(Fixture.Blueprint, TEXT("OverwrittenTweak"), CPF_Edit);
	LintVar_AddVarNode<UK2Node_VariableSet>(Fixture.Graph, TEXT("OverwrittenTweak"));

	TArray<FClaireonLintFinding> Findings;
	FClaireonLintContext Context;
	ClaireonLint::RunVariableRules(Context, Fixture.Blueprint, Findings);

	UNTEST_ASSERT_EQ(LintVar_CountRule(Findings, TEXT("transient-instance-editable"), TEXT("OverwrittenTweak")), 1);
	for (const FClaireonLintFinding& Finding : Findings)
	{
		if (Finding.Rule == TEXT("transient-instance-editable"))
		{
			UNTEST_EXPECT_TRUE(Finding.Severity == EClaireonLintSeverity::Warning);
		}
	}
	co_return;
}

// Read-only usage has medium confidence because external systems may write the variable.
UNTEST_UNIT_OPTS(Claireon, LintVariables, LintVariables_ReadOnlyUsageSuggestsReadOnly, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonLintVariableTestsNS;
	FLintVarFixture Fixture = LintVar_MakeFixture();
	UNTEST_ASSERT_TRUE(IsValid(Fixture.Blueprint));
	UNTEST_ASSERT_TRUE(IsValid(Fixture.Graph));

	LintVar_AddVariable(Fixture.Blueprint, TEXT("OnlyEverRead"), 0);
	LintVar_AddVarNode<UK2Node_VariableGet>(Fixture.Graph, TEXT("OnlyEverRead"));

	TArray<FClaireonLintFinding> Findings;
	FClaireonLintContext Context;
	ClaireonLint::RunVariableRules(Context, Fixture.Blueprint, Findings);

	UNTEST_EXPECT_EQ(LintVar_CountRule(Findings, TEXT("config-not-readonly"), TEXT("OnlyEverRead")), 1);
	UNTEST_EXPECT_EQ(LintVar_CountRule(Findings, TEXT("transient-not-marked"), TEXT("OnlyEverRead")), 0);
	co_return;
}

// Unreferenced findings must disclose search limits and recommend search rather than removal.
UNTEST_UNIT_OPTS(Claireon, LintVariables, LintVariables_UnreferencedNeverAuthorisesDeletion, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonLintVariableTestsNS;
	FLintVarFixture Fixture = LintVar_MakeFixture();
	UNTEST_ASSERT_TRUE(IsValid(Fixture.Blueprint));

	LintVar_AddVariable(Fixture.Blueprint, TEXT("NeverTouched"), 0);

	TArray<FClaireonLintFinding> Findings;
	FClaireonLintContext Context;
	ClaireonLint::RunVariableRules(Context, Fixture.Blueprint, Findings);

	UNTEST_ASSERT_EQ(LintVar_CountRule(Findings, TEXT("unreferenced-variable"), TEXT("NeverTouched")), 1);
	for (const FClaireonLintFinding& Finding : Findings)
	{
		if (Finding.Rule != TEXT("unreferenced-variable"))
		{
			continue;
		}

		UNTEST_EXPECT_TRUE(Finding.Confidence == EClaireonLintConfidence::Unverified);

		UNTEST_ASSERT_TRUE(Finding.Evidence.IsValid());
		const TArray<TSharedPtr<FJsonValue>>* Blind = nullptr;
		UNTEST_ASSERT_TRUE(Finding.Evidence->TryGetArrayField(TEXT("blind_spots"), Blind));
		UNTEST_EXPECT_TRUE(Blind && Blind->Num() >= 5);

		UNTEST_ASSERT_TRUE(Finding.SuggestedFix.IsValid());
		FString FixTool;
		UNTEST_ASSERT_TRUE(Finding.SuggestedFix->TryGetStringField(TEXT("tool"), FixTool));
		UNTEST_EXPECT_TRUE(FixTool == TEXT("bp_search"));
		UNTEST_EXPECT_FALSE(FixTool.Contains(TEXT("remove")));

		const FString Lowered = Finding.Message.ToLower();
		UNTEST_EXPECT_FALSE(Lowered.Contains(TEXT("safe to delete")));
		UNTEST_EXPECT_FALSE(Lowered.Contains(TEXT("can be removed")));
		UNTEST_EXPECT_FALSE(Lowered.Contains(TEXT("unused")) && !Lowered.Contains(TEXT("not evidence")));
	}
	co_return;
}

// Delegate nodes reference dispatchers through DelegateReference, not VariableReference.
UNTEST_UNIT_OPTS(Claireon, LintVariables, LintVariables_DelegateNodeIsAReference, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonLintVariableTestsNS;
	FLintVarFixture Fixture = LintVar_MakeFixture();
	UNTEST_ASSERT_TRUE(IsValid(Fixture.Blueprint));
	UNTEST_ASSERT_TRUE(IsValid(Fixture.Graph));

	LintVar_AddVariable(Fixture.Blueprint, TEXT("OnSomething"), 0);
	Fixture.Blueprint->NewVariables.Last().VarType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
	UK2Node_CallDelegate* Call = NewObject<UK2Node_CallDelegate>(Fixture.Graph, NAME_None, RF_Transient);
	Call->DelegateReference.SetSelfMember(FName(TEXT("OnSomething")));
	Fixture.Graph->Nodes.Add(Call);
	Call->CreateNewGuid();

	TArray<FClaireonLintFinding> Findings;
	FClaireonLintContext Context;
	ClaireonLint::RunVariableRules(Context, Fixture.Blueprint, Findings);

	UNTEST_EXPECT_EQ(LintVar_CountRule(Findings, TEXT("unreferenced-variable"), TEXT("OnSomething")), 0);
	// Dispatcher reads must not suggest BlueprintReadOnly.
	UNTEST_EXPECT_EQ(LintVar_CountRule(Findings, TEXT("config-not-readonly"), TEXT("OnSomething")), 0);
	co_return;
}

// Attribute usage to declaration identity, excluding same-name locals and unrelated-class properties.
UNTEST_UNIT_OPTS(Claireon, LintVariables, LintVariables_UsageResolvesOwnerNotName, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonLintVariableTestsNS;
	FLintVarFixture Fixture = LintVar_MakeFixture();
	UNTEST_ASSERT_TRUE(IsValid(Fixture.Blueprint));
	UNTEST_ASSERT_TRUE(IsValid(Fixture.Graph));

	LintVar_AddVariable(Fixture.Blueprint, TEXT("Health"), 0);

	// APawn is outside the Actor fixture's ancestry, so its write belongs to another declaration.
	{
		UK2Node_VariableSet* Setter = NewObject<UK2Node_VariableSet>(Fixture.Graph, NAME_None, RF_Transient);
		Setter->VariableReference.SetExternalMember(FName(TEXT("Health")), APawn::StaticClass());
		Fixture.Graph->Nodes.Add(Setter);
		Setter->CreateNewGuid();
	}
	{
		UK2Node_VariableGet* Getter = NewObject<UK2Node_VariableGet>(Fixture.Graph, NAME_None, RF_Transient);
		Getter->VariableReference.SetLocalMember(FName(TEXT("Health")), Fixture.Graph->GetName(), FGuid());
		Fixture.Graph->Nodes.Add(Getter);
		Getter->CreateNewGuid();
	}

	TArray<FClaireonLintFinding> Findings;
	FClaireonLintContext Context;
	ClaireonLint::RunVariableRules(Context, Fixture.Blueprint, Findings);

	UNTEST_EXPECT_EQ(LintVar_CountRule(Findings, TEXT("transient-not-marked"), TEXT("Health")), 0);
	UNTEST_ASSERT_EQ(LintVar_CountRule(Findings, TEXT("unreferenced-variable"), TEXT("Health")), 1);

	// An explicit receiver of this Blueprint's class still counts as usage of its declaration.
	{
		UK2Node_VariableSet* OwnSetter = NewObject<UK2Node_VariableSet>(Fixture.Graph, NAME_None, RF_Transient);
		OwnSetter->VariableReference.SetExternalMember(
			FName(TEXT("Health")), Fixture.Blueprint->SkeletonGeneratedClass
				? Fixture.Blueprint->SkeletonGeneratedClass.Get()
				: Fixture.Blueprint->GeneratedClass.Get());
		Fixture.Graph->Nodes.Add(OwnSetter);
		OwnSetter->CreateNewGuid();
	}

	Findings.Reset();
	ClaireonLint::RunVariableRules(Context, Fixture.Blueprint, Findings);
	UNTEST_EXPECT_EQ(LintVar_CountRule(Findings, TEXT("unreferenced-variable"), TEXT("Health")), 0);
	UNTEST_EXPECT_EQ(LintVar_CountRule(Findings, TEXT("transient-not-marked"), TEXT("Health")), 1);
	co_return;
}

#endif // WITH_UNTESTED
