// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Test lint scope resolution, filters, envelopes, and ordering through the tool.

#if WITH_UNTESTED

#include "Untest.h"

#include "Tools/ClaireonTool_Lint.h"
#include "Tools/ClaireonTool_SearchInBlueprintsIndexStatus.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"

namespace ClaireonLintFrameworkTestsNS
{
	// Prefix helpers to avoid unity-build collisions.

	/** An in-memory Blueprint with a long wire to make layout-scope execution observable. */
	static FString LintFw_MakeFixture()
	{
		static int32 Counter = 0;
		const FString AssetName = FString::Printf(TEXT("BP_LintFramework_%d"), Counter++);
		const FString PackagePath = FString(TEXT("/Game/__MCPTests/")) + AssetName;
		UPackage* Package = CreatePackage(*PackagePath);
		if (!IsValid(Package))
		{
			return FString();
		}

		UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), Package, FName(*AssetName), BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), NAME_None);
		if (!IsValid(BP) || BP->UbergraphPages.Num() == 0)
		{
			return FString();
		}
		UEdGraph* Graph = BP->UbergraphPages[0];

		auto AddCall = [Graph](int32 X, int32 Y) -> UK2Node_CallFunction*
		{
			UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph);
			Graph->AddNode(Call, /*bFromUI=*/false, /*bSelectNewNode=*/false);
			Call->FunctionReference.SetExternalMember(
				FName(TEXT("PrintString")), UKismetSystemLibrary::StaticClass());
			Call->CreateNewGuid();
			Call->NodePosX = X;
			Call->NodePosY = Y;
			Call->PostPlacedNewNode();
			Call->AllocateDefaultPins();
			return Call;
		};

		auto FirstExec = [](UEdGraphNode* Node, EEdGraphPinDirection Dir) -> UEdGraphPin*
		{
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin && Pin->Direction == Dir
					&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
				{
					return Pin;
				}
			}
			return nullptr;
		};

		// A forward wire exceeds the long-wire threshold without triggering backward-wire.
		UK2Node_CallFunction* A = AddCall(0, 0);
		UK2Node_CallFunction* B = AddCall(5000, 0);
		if (UEdGraphPin* From = FirstExec(A, EGPD_Output))
		{
			if (UEdGraphPin* To = FirstExec(B, EGPD_Input))
			{
				From->LinkedTo.AddUnique(To);
				To->LinkedTo.AddUnique(From);
			}
		}

		return PackagePath + TEXT(".") + AssetName;
	}

	static TSharedPtr<FJsonObject> LintFw_Args(const FString& ObjectPath)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), ObjectPath);
		return Args;
	}

	static TArray<FString> LintFw_ScopesRun(const IClaireonTool::FToolResult& Result)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* Scopes = nullptr;
		if (Result.Data.IsValid() && Result.Data->TryGetArrayField(TEXT("scopes_run"), Scopes) && Scopes)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Scopes)
			{
				FString Name;
				if (Value.IsValid() && Value->TryGetString(Name))
				{
					Out.Add(Name);
				}
			}
		}
		return Out;
	}

	static bool LintFw_HasRule(const IClaireonTool::FToolResult& Result, const TCHAR* Rule)
	{
		const TSharedPtr<FJsonObject>* Counts = nullptr;
		return Result.Data.IsValid()
			&& Result.Data->TryGetObjectField(TEXT("counts_by_rule"), Counts)
			&& Counts && (*Counts).IsValid() && (*Counts)->HasField(Rule);
	}

	/** Rule ids in response order, for the determinism case. */
	static TArray<FString> LintFw_FindingOrder(const IClaireonTool::FToolResult& Result)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* Findings = nullptr;
		if (Result.Data.IsValid() && Result.Data->TryGetArrayField(TEXT("findings"), Findings) && Findings)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Findings)
			{
				const TSharedPtr<FJsonObject>* Obj = nullptr;
				FString Rule, Target;
				if (Value.IsValid() && Value->TryGetObject(Obj) && Obj && (*Obj).IsValid())
				{
					(*Obj)->TryGetStringField(TEXT("rule"), Rule);
					(*Obj)->TryGetStringField(TEXT("target"), Target);
					Out.Add(Rule + TEXT("|") + Target);
				}
			}
		}
		return Out;
	}
}

using namespace ClaireonLintFrameworkTestsNS;

// Default scope excludes layout.
UNTEST_UNIT_OPTS(Claireon, LintFramework, Scope_OmittedExcludesLayout, UNTEST_TIMEOUTMS(60000))
{
	const FString Path = LintFw_MakeFixture();
	UNTEST_ASSERT_FALSE(Path.IsEmpty());

	ClaireonTool_Lint Tool;
	const IClaireonTool::FToolResult Result = Tool.Execute(LintFw_Args(Path));
	UNTEST_ASSERT_FALSE(Result.bIsError);

	const TArray<FString> Scopes = LintFw_ScopesRun(Result);
	UNTEST_EXPECT_TRUE(Scopes.Contains(TEXT("variables")));
	UNTEST_EXPECT_TRUE(Scopes.Contains(TEXT("graph")));
	UNTEST_EXPECT_TRUE(Scopes.Contains(TEXT("functions")));
	UNTEST_EXPECT_TRUE(Scopes.Contains(TEXT("hygiene")));
	UNTEST_EXPECT_FALSE(Scopes.Contains(TEXT("layout")));

	UNTEST_EXPECT_FALSE(LintFw_HasRule(Result, TEXT("long-wire")));
	co_return;
}

// Accept array scope spelling and all.
UNTEST_UNIT_OPTS(Claireon, LintFramework, Scope_AllAsListIncludesLayout, UNTEST_TIMEOUTMS(60000))
{
	const FString Path = LintFw_MakeFixture();
	UNTEST_ASSERT_FALSE(Path.IsEmpty());

	TSharedPtr<FJsonObject> Args = LintFw_Args(Path);
	TArray<TSharedPtr<FJsonValue>> Scope;
	Scope.Add(MakeShared<FJsonValueString>(TEXT("all")));
	Args->SetArrayField(TEXT("scope"), Scope);

	ClaireonTool_Lint Tool;
	const IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_EXPECT_TRUE(LintFw_ScopesRun(Result).Contains(TEXT("layout")));
	UNTEST_EXPECT_TRUE(LintFw_HasRule(Result, TEXT("long-wire")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, LintFramework, Scope_CsvStringStillWorks, UNTEST_TIMEOUTMS(60000))
{
	const FString Path = LintFw_MakeFixture();
	UNTEST_ASSERT_FALSE(Path.IsEmpty());

	TSharedPtr<FJsonObject> Args = LintFw_Args(Path);
	Args->SetStringField(TEXT("scope"), TEXT("graph, layout"));

	ClaireonTool_Lint Tool;
	const IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);

	const TArray<FString> Scopes = LintFw_ScopesRun(Result);
	UNTEST_EXPECT_TRUE(Scopes.Contains(TEXT("graph")));
	UNTEST_EXPECT_TRUE(Scopes.Contains(TEXT("layout")));
	UNTEST_EXPECT_FALSE(Scopes.Contains(TEXT("variables")));
	UNTEST_EXPECT_EQ(Scopes.Num(), 2);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, LintFramework, Scope_LayoutOnlyAndDuplicatesCollapse, UNTEST_TIMEOUTMS(60000))
{
	const FString Path = LintFw_MakeFixture();
	UNTEST_ASSERT_FALSE(Path.IsEmpty());

	TSharedPtr<FJsonObject> Args = LintFw_Args(Path);
	TArray<TSharedPtr<FJsonValue>> Scope;
	Scope.Add(MakeShared<FJsonValueString>(TEXT("layout")));
	Scope.Add(MakeShared<FJsonValueString>(TEXT("layout")));
	Args->SetArrayField(TEXT("scope"), Scope);

	ClaireonTool_Lint Tool;
	const IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);

	const TArray<FString> Scopes = LintFw_ScopesRun(Result);
	UNTEST_ASSERT_EQ(Scopes.Num(), 1);
	UNTEST_EXPECT_TRUE(Scopes[0] == TEXT("layout"));

	const TArray<FString> Order = LintFw_FindingOrder(Result);
	TSet<FString> Unique(Order);
	UNTEST_EXPECT_EQ(Unique.Num(), Order.Num());
	co_return;
}

// Reject unknown scopes.
UNTEST_UNIT_OPTS(Claireon, LintFramework, Scope_UnknownNameErrors, UNTEST_TIMEOUTMS(60000))
{
	const FString Path = LintFw_MakeFixture();
	UNTEST_ASSERT_FALSE(Path.IsEmpty());

	TSharedPtr<FJsonObject> Args = LintFw_Args(Path);
	Args->SetStringField(TEXT("scope"), TEXT("geometry"));

	ClaireonTool_Lint Tool;
	const IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_EXPECT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("Unknown scope")));
	co_return;
}

// Reject unusable scope values rather than selecting defaults.
UNTEST_UNIT_OPTS(Claireon, LintFramework, Scope_WrongTypeErrorsRatherThanDefaulting, UNTEST_TIMEOUTMS(60000))
{
	const FString Path = LintFw_MakeFixture();
	UNTEST_ASSERT_FALSE(Path.IsEmpty());

	TSharedPtr<FJsonObject> NumberScope = LintFw_Args(Path);
	NumberScope->SetNumberField(TEXT("scope"), 3);
	ClaireonTool_Lint Tool;
	IClaireonTool::FToolResult Result = Tool.Execute(NumberScope);
	UNTEST_EXPECT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("scope must be")));

	TSharedPtr<FJsonObject> MixedList = LintFw_Args(Path);
	TArray<TSharedPtr<FJsonValue>> Scope;
	Scope.Add(MakeShared<FJsonValueString>(TEXT("graph")));
	Scope.Add(MakeShared<FJsonValueNumber>(7));
	MixedList->SetArrayField(TEXT("scope"), Scope);
	Result = Tool.Execute(MixedList);
	UNTEST_EXPECT_TRUE(Result.bIsError);
	co_return;
}

// Accept both filter spellings and apply exclusions after the allowlist.
UNTEST_UNIT_OPTS(Claireon, LintFramework, Filters_AcceptBothSpellingsAndApplyInOrder, UNTEST_TIMEOUTMS(60000))
{
	const FString Path = LintFw_MakeFixture();
	UNTEST_ASSERT_FALSE(Path.IsEmpty());
	ClaireonTool_Lint Tool;

	TSharedPtr<FJsonObject> Allow = LintFw_Args(Path);
	Allow->SetStringField(TEXT("scope"), TEXT("all"));
	TArray<TSharedPtr<FJsonValue>> Rules;
	Rules.Add(MakeShared<FJsonValueString>(TEXT("long-wire")));
	Allow->SetArrayField(TEXT("rules"), Rules);
	IClaireonTool::FToolResult Result = Tool.Execute(Allow);
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_EXPECT_TRUE(LintFw_HasRule(Result, TEXT("long-wire")));
	UNTEST_EXPECT_FALSE(LintFw_HasRule(Result, TEXT("uncategorized")));

	TSharedPtr<FJsonObject> Both = LintFw_Args(Path);
	Both->SetStringField(TEXT("scope"), TEXT("all"));
	Both->SetStringField(TEXT("rules"), TEXT("long-wire"));
	Both->SetStringField(TEXT("exclude_rules"), TEXT("long-wire"));
	Result = Tool.Execute(Both);
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_EXPECT_EQ(LintFw_FindingOrder(Result).Num(), 0);

	TSharedPtr<FJsonObject> BadFilter = LintFw_Args(Path);
	BadFilter->SetNumberField(TEXT("rules"), 5);
	Result = Tool.Execute(BadFilter);
	UNTEST_EXPECT_TRUE(Result.bIsError);
	co_return;
}

// Require stable finding order.
UNTEST_UNIT_OPTS(Claireon, LintFramework, Determinism_FindingOrderIsStable, UNTEST_TIMEOUTMS(60000))
{
	const FString Path = LintFw_MakeFixture();
	UNTEST_ASSERT_FALSE(Path.IsEmpty());

	ClaireonTool_Lint Tool;
	TSharedPtr<FJsonObject> Args = LintFw_Args(Path);
	Args->SetStringField(TEXT("scope"), TEXT("all"));

	const TArray<FString> First = LintFw_FindingOrder(Tool.Execute(Args));
	const TArray<FString> Second = LintFw_FindingOrder(Tool.Execute(Args));
	UNTEST_ASSERT_TRUE(First.Num() > 0);
	UNTEST_ASSERT_EQ(Second.Num(), First.Num());
	for (int32 I = 0; I < First.Num(); ++I)
	{
		UNTEST_EXPECT_TRUE(First[I] == Second[I]);
	}

	const TArray<FString> Scopes = LintFw_ScopesRun(Tool.Execute(Args));
	TArray<FString> Sorted = Scopes;
	Sorted.Sort();
	for (int32 I = 0; I < Scopes.Num(); ++I)
	{
		UNTEST_EXPECT_TRUE(Scopes[I] == Sorted[I]);
	}
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, LintFramework, Envelope_TargetIsExactlyOne, UNTEST_TIMEOUTMS(60000))
{
	const FString Path = LintFw_MakeFixture();
	UNTEST_ASSERT_FALSE(Path.IsEmpty());
	ClaireonTool_Lint Tool;

	IClaireonTool::FToolResult Neither = Tool.Execute(MakeShared<FJsonObject>());
	UNTEST_EXPECT_TRUE(Neither.bIsError);

	TSharedPtr<FJsonObject> BothArgs = LintFw_Args(Path);
	BothArgs->SetStringField(TEXT("session_id"), TEXT("not-a-real-session"));
	IClaireonTool::FToolResult Both = Tool.Execute(BothArgs);
	UNTEST_EXPECT_TRUE(Both.bIsError);

	TSharedPtr<FJsonObject> Filtered = LintFw_Args(Path);
	Filtered->SetStringField(TEXT("rules"), TEXT("no-such-rule-id"));
	IClaireonTool::FToolResult Empty = Tool.Execute(Filtered);
	UNTEST_ASSERT_FALSE(Empty.bIsError);
	UNTEST_ASSERT_TRUE(Empty.Data.IsValid());
	UNTEST_EXPECT_TRUE(Empty.Data->HasField(TEXT("findings")));
	UNTEST_EXPECT_TRUE(Empty.Data->HasField(TEXT("counts_by_rule")));
	UNTEST_EXPECT_TRUE(Empty.Data->HasField(TEXT("scopes_run")));
	UNTEST_EXPECT_TRUE(Empty.Data->HasField(TEXT("blueprint_path")));
	co_return;
}

/** Verify the complete index_status passthrough, including first-search indexing cost. */
UNTEST_UNIT_OPTS(Claireon, LintFramework, Envelope_CarriesIndexStatusPayload, UNTEST_TIMEOUTMS(60000))
{
	const FString Path = LintFw_MakeFixture();
	UNTEST_ASSERT_FALSE(Path.IsEmpty());
	ClaireonTool_Lint Tool;

	const IClaireonTool::FToolResult Result = Tool.Execute(LintFw_Args(Path));
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());

	const TSharedPtr<FJsonObject>* IndexStatus = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetObjectField(TEXT("index_status"), IndexStatus));
	UNTEST_ASSERT_TRUE(IndexStatus != nullptr && IndexStatus->IsValid());

	for (const TCHAR* Field : {TEXT("ready"), TEXT("unindexed_assets"), TEXT("uncached_assets"),
	                           TEXT("cache_progress"), TEXT("cache_in_progress"),
	                           TEXT("asset_discovery_in_progress"),
	                           TEXT("first_search_may_index_full_corpus")})
	{
		UNTEST_EXPECT_TRUE((*IndexStatus)->HasField(Field));
	}

	ClaireonTool_SearchInBlueprintsIndexStatus StatusTool;
	const IClaireonTool::FToolResult Direct = StatusTool.Execute(MakeShared<FJsonObject>());
	UNTEST_ASSERT_FALSE(Direct.bIsError);
	UNTEST_ASSERT_TRUE(Direct.Data.IsValid());

	bool bViaLint = false;
	bool bDirect = true;
	UNTEST_EXPECT_TRUE((*IndexStatus)->TryGetBoolField(TEXT("first_search_may_index_full_corpus"), bViaLint));
	UNTEST_EXPECT_TRUE(Direct.Data->TryGetBoolField(TEXT("first_search_may_index_full_corpus"), bDirect));
	UNTEST_EXPECT_EQ(bViaLint, bDirect);
	co_return;
}

// Scope dispatch and graph-filter contracts.

namespace ClaireonLintFrameworkTestsNS
{
	static TArray<FString> LintFw_NotImplemented(const IClaireonTool::FToolResult& Result)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Result.Data.IsValid()
			&& Result.Data->TryGetArrayField(TEXT("scopes_not_implemented"), Values) && Values)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				FString Name;
				if (Value.IsValid() && Value->TryGetString(Name))
				{
					Out.Add(Name);
				}
			}
		}
		return Out;
	}

	/** The fixture's ubergraph name, read rather than assumed to be "EventGraph". */
	static FString LintFw_FirstGraphName(const FString& ObjectPath)
	{
		UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *ObjectPath);
		return (IsValid(BP) && BP->UbergraphPages.Num() > 0 && IsValid(BP->UbergraphPages[0]))
			? BP->UbergraphPages[0]->GetName()
			: FString();
	}
}

// The functions scope dispatches its declaration rules.
UNTEST_UNIT_OPTS(Claireon, LintFramework, Scope_FunctionsAloneNowRuns, UNTEST_TIMEOUTMS(60000))
{
	const FString Path = LintFw_MakeFixture();
	UNTEST_ASSERT_FALSE(Path.IsEmpty());

	TSharedPtr<FJsonObject> Args = LintFw_Args(Path);
	TArray<TSharedPtr<FJsonValue>> Scope;
	Scope.Add(MakeShared<FJsonValueString>(TEXT("functions")));
	Args->SetArrayField(TEXT("scope"), Scope);

	ClaireonTool_Lint Tool;
	const IClaireonTool::FToolResult Result = Tool.Execute(Args);

	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());

	UNTEST_EXPECT_TRUE(LintFw_ScopesRun(Result).Contains(TEXT("functions")));
	UNTEST_EXPECT_EQ(LintFw_NotImplemented(Result).Num(), 0);
	UNTEST_EXPECT_FALSE(Result.Summary.Contains(TEXT("no rules yet")));
	co_return;
}

// Keep unimplemented_scopes present and empty when every requested scope has a runner.
UNTEST_UNIT_OPTS(Claireon, LintFramework, Scope_DefaultRunHasNoUnimplementedScopes,
	UNTEST_TIMEOUTMS(60000))
{
	const FString Path = LintFw_MakeFixture();
	UNTEST_ASSERT_FALSE(Path.IsEmpty());

	ClaireonTool_Lint Tool;
	const IClaireonTool::FToolResult Result = Tool.Execute(LintFw_Args(Path));
	UNTEST_ASSERT_FALSE(Result.bIsError);

	UNTEST_ASSERT_TRUE(Result.Data.IsValid());

	UNTEST_EXPECT_TRUE(LintFw_ScopesRun(Result).Contains(TEXT("functions")));

	UNTEST_EXPECT_TRUE(Result.Data->HasField(TEXT("scopes_not_implemented")));
	UNTEST_EXPECT_EQ(LintFw_NotImplemented(Result).Num(), 0);
	UNTEST_EXPECT_FALSE(Result.Summary.Contains(TEXT("no rules yet")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, LintFramework, Scope_NotImplementedIsEmptyWhenEveryScopeHasRules,
	UNTEST_TIMEOUTMS(60000))
{
	const FString Path = LintFw_MakeFixture();
	UNTEST_ASSERT_FALSE(Path.IsEmpty());

	TSharedPtr<FJsonObject> Args = LintFw_Args(Path);
	TArray<TSharedPtr<FJsonValue>> Scope;
	Scope.Add(MakeShared<FJsonValueString>(TEXT("graph")));
	Args->SetArrayField(TEXT("scope"), Scope);

	ClaireonTool_Lint Tool;
	const IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());

	UNTEST_EXPECT_TRUE(Result.Data->HasField(TEXT("scopes_not_implemented")));
	UNTEST_EXPECT_EQ(LintFw_NotImplemented(Result).Num(), 0);
	UNTEST_EXPECT_FALSE(Result.Summary.Contains(TEXT("no rules yet")));
	co_return;
}

// Unknown graph names must fail and list available graphs.
UNTEST_UNIT_OPTS(Claireon, LintFramework, GraphName_UnmatchedIsAnErrorNotACleanReport,
	UNTEST_TIMEOUTMS(60000))
{
	const FString Path = LintFw_MakeFixture();
	UNTEST_ASSERT_FALSE(Path.IsEmpty());

	const FString RealGraph = LintFw_FirstGraphName(Path);
	UNTEST_ASSERT_FALSE(RealGraph.IsEmpty());

	TSharedPtr<FJsonObject> Args = LintFw_Args(Path);
	Args->SetStringField(TEXT("graph_name"), TEXT("NoSuchGraph_zz"));

	ClaireonTool_Lint Tool;
	const IClaireonTool::FToolResult Result = Tool.Execute(Args);

	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("NoSuchGraph_zz")));
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(RealGraph));
	co_return;
}

// Matched graph filters report how many graphs were examined.
UNTEST_UNIT_OPTS(Claireon, LintFramework, GraphName_MatchedNameReportsGraphsLinted,
	UNTEST_TIMEOUTMS(60000))
{
	const FString Path = LintFw_MakeFixture();
	UNTEST_ASSERT_FALSE(Path.IsEmpty());

	const FString RealGraph = LintFw_FirstGraphName(Path);
	UNTEST_ASSERT_FALSE(RealGraph.IsEmpty());

	TSharedPtr<FJsonObject> Args = LintFw_Args(Path);
	Args->SetStringField(TEXT("graph_name"), RealGraph);

	ClaireonTool_Lint Tool;
	const IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());

	double GraphsLinted = 0.0;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetNumberField(TEXT("graphs_linted"), GraphsLinted));
	UNTEST_EXPECT_EQ(static_cast<int32>(GraphsLinted), 1);

	TSharedPtr<FJsonObject> UpperArgs = LintFw_Args(Path);
	UpperArgs->SetStringField(TEXT("graph_name"), RealGraph.ToUpper());
	const IClaireonTool::FToolResult UpperResult = Tool.Execute(UpperArgs);
	UNTEST_EXPECT_FALSE(UpperResult.bIsError);
	co_return;
}


// Argument and output contracts.

// Explicit empty scopes must not select the default set.
UNTEST_UNIT_OPTS(Claireon, LintFramework, Scope_ExplicitlyEmptyListIsRefused, UNTEST_TIMEOUTMS(60000))
{
	const FString Path = LintFw_MakeFixture();
	UNTEST_ASSERT_FALSE(Path.IsEmpty());
	ClaireonTool_Lint Tool;

	TSharedPtr<FJsonObject> EmptyArray = LintFw_Args(Path);
	EmptyArray->SetArrayField(TEXT("scope"), TArray<TSharedPtr<FJsonValue>>());
	const IClaireonTool::FToolResult ArrayResult = Tool.Execute(EmptyArray);
	UNTEST_EXPECT_TRUE(ArrayResult.bIsError);
	UNTEST_EXPECT_TRUE(ArrayResult.ErrorMessage.Contains(TEXT("OMIT")));

	for (const TCHAR* Spelling : {TEXT(""), TEXT(","), TEXT("  ,  ")})
	{
		TSharedPtr<FJsonObject> Args = LintFw_Args(Path);
		Args->SetStringField(TEXT("scope"), Spelling);
		UNTEST_EXPECT_TRUE(Tool.Execute(Args).bIsError);
	}

	// Omitting scope still selects defaults.
	const IClaireonTool::FToolResult Omitted = Tool.Execute(LintFw_Args(Path));
	UNTEST_EXPECT_FALSE(Omitted.bIsError);
	UNTEST_EXPECT_TRUE(LintFw_ScopesRun(Omitted).Contains(TEXT("variables")));
	co_return;
}

// Reject unknown severities instead of silently widening results.
UNTEST_UNIT_OPTS(Claireon, LintFramework, SeverityMin_UnknownValueIsRefused, UNTEST_TIMEOUTMS(60000))
{
	const FString Path = LintFw_MakeFixture();
	UNTEST_ASSERT_FALSE(Path.IsEmpty());
	ClaireonTool_Lint Tool;

	for (const TCHAR* Bad : {TEXT("error"), TEXT("fatal"), TEXT("Warnings"), TEXT("none")})
	{
		TSharedPtr<FJsonObject> Args = LintFw_Args(Path);
		Args->SetStringField(TEXT("severity_min"), Bad);
		const IClaireonTool::FToolResult Result = Tool.Execute(Args);
		UNTEST_EXPECT_TRUE(Result.bIsError);
		UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("info")));
	}

	// Reject numeric severity values without coercion.
	TSharedPtr<FJsonObject> Numeric = LintFw_Args(Path);
	Numeric->SetNumberField(TEXT("severity_min"), 2);
	UNTEST_EXPECT_TRUE(Tool.Execute(Numeric).bIsError);

	TSharedPtr<FJsonObject> InfoArgs = LintFw_Args(Path);
	InfoArgs->SetStringField(TEXT("severity_min"), TEXT("info"));
	const IClaireonTool::FToolResult Info = Tool.Execute(InfoArgs);
	UNTEST_ASSERT_FALSE(Info.bIsError);

	TSharedPtr<FJsonObject> WarnArgs = LintFw_Args(Path);
	WarnArgs->SetStringField(TEXT("severity_min"), TEXT("warning"));
	const IClaireonTool::FToolResult Warn = Tool.Execute(WarnArgs);
	UNTEST_ASSERT_FALSE(Warn.bIsError);
	UNTEST_EXPECT_TRUE(LintFw_FindingOrder(Warn).Num() <= LintFw_FindingOrder(Info).Num());
	co_return;
}

// Schema and parser must both accept string and array filter forms.
UNTEST_UNIT_OPTS(Claireon, LintFramework, Schema_PluralArgsDeclareBothSpellings, UNTEST_TIMEOUTMS(60000))
{
	ClaireonTool_Lint Tool;
	const TSharedPtr<FJsonObject> Schema = Tool.GetInputSchema();
	UNTEST_ASSERT_TRUE(Schema.IsValid());
	const TSharedPtr<FJsonObject>* Props = nullptr;
	UNTEST_ASSERT_TRUE(Schema->TryGetObjectField(TEXT("properties"), Props) && Props);

	for (const TCHAR* Field : {TEXT("scope"), TEXT("rules"), TEXT("exclude_rules")})
	{
		const TSharedPtr<FJsonObject>* Prop = nullptr;
		UNTEST_ASSERT_TRUE((*Props)->TryGetObjectField(Field, Prop) && Prop);

		const TArray<TSharedPtr<FJsonValue>>* Types = nullptr;
		UNTEST_ASSERT_TRUE((*Prop)->TryGetArrayField(TEXT("type"), Types) && Types);
		TArray<FString> TypeNames;
		for (const TSharedPtr<FJsonValue>& V : *Types)
		{
			FString Name;
			if (V.IsValid() && V->TryGetString(Name)) { TypeNames.Add(Name); }
		}
		UNTEST_EXPECT_TRUE(TypeNames.Contains(TEXT("array")));
		UNTEST_EXPECT_TRUE(TypeNames.Contains(TEXT("string")));

		const TSharedPtr<FJsonObject>* Items = nullptr;
		UNTEST_EXPECT_TRUE((*Prop)->TryGetObjectField(TEXT("items"), Items) && Items);
	}

	const TSharedPtr<FJsonObject>* Sev = nullptr;
	UNTEST_ASSERT_TRUE((*Props)->TryGetObjectField(TEXT("severity_min"), Sev) && Sev);
	const TArray<TSharedPtr<FJsonValue>>* EnumValues = nullptr;
	UNTEST_ASSERT_TRUE((*Sev)->TryGetArrayField(TEXT("enum"), EnumValues) && EnumValues);
	UNTEST_EXPECT_EQ(EnumValues->Num(), 2);
	co_return;
}

// graph_name filters graph-local families; member-variable rules remain Blueprint-wide.
UNTEST_UNIT_OPTS(Claireon, LintFramework, GraphFilter_ScopeSubjectsAreStated, UNTEST_TIMEOUTMS(60000))
{
	const FString Path = LintFw_MakeFixture();
	UNTEST_ASSERT_FALSE(Path.IsEmpty());
	const FString RealGraph = LintFw_FirstGraphName(Path);
	UNTEST_ASSERT_FALSE(RealGraph.IsEmpty());

	ClaireonTool_Lint Tool;
	TSharedPtr<FJsonObject> Args = LintFw_Args(Path);
	Args->SetStringField(TEXT("graph_name"), RealGraph);
	const IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());

	FString Echoed;
	UNTEST_EXPECT_TRUE(Result.Data->TryGetStringField(TEXT("graph_filter"), Echoed));
	UNTEST_EXPECT_TRUE(Echoed == RealGraph);

	bool bAppliesToAll = true;
	UNTEST_EXPECT_TRUE(Result.Data->TryGetBoolField(TEXT("graph_filter_applies_to_all_scopes"), bAppliesToAll));
	UNTEST_EXPECT_FALSE(bAppliesToAll);

	const TSharedPtr<FJsonObject>* Subjects = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetObjectField(TEXT("scope_subjects"), Subjects) && Subjects);
	FString VariablesSubject, GraphSubject, FunctionsSubject;
	UNTEST_EXPECT_TRUE((*Subjects)->TryGetStringField(TEXT("variables"), VariablesSubject));
	UNTEST_EXPECT_TRUE(VariablesSubject == TEXT("blueprint"));
	UNTEST_EXPECT_TRUE((*Subjects)->TryGetStringField(TEXT("graph"), GraphSubject));
	UNTEST_EXPECT_TRUE(GraphSubject == TEXT("graph"));
	UNTEST_EXPECT_TRUE((*Subjects)->TryGetStringField(TEXT("functions"), FunctionsSubject));
	UNTEST_EXPECT_TRUE(FunctionsSubject == TEXT("graph"));

	// Unfiltered responses keep graph_filter present and empty.
	const IClaireonTool::FToolResult Unfiltered = Tool.Execute(LintFw_Args(Path));
	UNTEST_ASSERT_FALSE(Unfiltered.bIsError);
	FString NoFilter = TEXT("sentinel");
	UNTEST_EXPECT_TRUE(Unfiltered.Data->TryGetStringField(TEXT("graph_filter"), NoFilter));
	UNTEST_EXPECT_TRUE(NoFilter.IsEmpty());
	co_return;
}

// Require deterministic ordering, including findings with equal rule and target.
UNTEST_UNIT_OPTS(Claireon, LintFramework, Determinism_OrderIsTotalAcrossRepeatedRuns,
	UNTEST_TIMEOUTMS(90000))
{
	const FString Path = LintFw_MakeFixture();
	UNTEST_ASSERT_FALSE(Path.IsEmpty());
	ClaireonTool_Lint Tool;

	TSharedPtr<FJsonObject> Args = LintFw_Args(Path);
	TArray<TSharedPtr<FJsonValue>> Scope;
	Scope.Add(MakeShared<FJsonValueString>(TEXT("all")));
	Args->SetArrayField(TEXT("scope"), Scope);

	const TArray<FString> Baseline = LintFw_FindingOrder(Tool.Execute(Args));
	UNTEST_ASSERT_TRUE(Baseline.Num() > 0);

	for (int32 Run = 0; Run < 4; ++Run)
	{
		const TArray<FString> Repeat = LintFw_FindingOrder(Tool.Execute(Args));
		UNTEST_ASSERT_EQ(Repeat.Num(), Baseline.Num());
		for (int32 I = 0; I < Baseline.Num(); ++I)
		{
			UNTEST_EXPECT_TRUE(Repeat[I] == Baseline[I]);
		}
	}
	co_return;
}

#endif // WITH_UNTESTED
