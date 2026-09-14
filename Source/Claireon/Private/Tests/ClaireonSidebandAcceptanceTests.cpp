// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Run Python that discards lint results and verify advisories still reach the top-level result.

#if WITH_UNTESTED

#include "Untest.h"

#include "ClaireonAdvisory.h"
#include "ClaireonModule.h"
#include "ClaireonXmlFormatter.h"
#include "Tools/ClaireonTool_ExecutePython.h"
#include "Tools/IClaireonTool.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "SquidTasks/Task.h"
#include "UObject/Package.h"

namespace ClaireonSidebandAcceptanceTestsNS
{
	// Prefix helpers to avoid unity-build collisions.

	/** Create disconnected long-wire pairs to produce real lint findings. */
	static FString SbAcc_MakeFixture(int32 NumPairs)
	{
		static int32 Counter = 0;
		const FString AssetName = FString::Printf(TEXT("BP_SidebandAcceptance_%d"), Counter++);
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

		const auto AddCall = [Graph](int32 X, int32 Y) -> UK2Node_CallFunction*
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

		const auto FirstExec = [](UEdGraphNode* Node, EEdGraphPinDirection Dir) -> UEdGraphPin*
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

		for (int32 Index = 0; Index < NumPairs; ++Index)
		{
			const int32 Row = Index * 400;
			UK2Node_CallFunction* A = AddCall(0, Row);
			UK2Node_CallFunction* B = AddCall(5000, Row);
			UEdGraphPin* From = FirstExec(A, EGPD_Output);
			UEdGraphPin* To = FirstExec(B, EGPD_Input);
			if (From && To)
			{
				From->LinkedTo.AddUnique(To);
				To->LinkedTo.AddUnique(From);
			}
		}

		return PackagePath + TEXT(".") + AssetName;
	}

	/** Run Python with quiet=true. */
	static IClaireonTool::FToolResult SbAcc_RunPythonQuiet(const FString& Code)
	{
		ClaireonTool_ExecutePython PyTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("code"), Code);
		Args->SetBoolField(TEXT("quiet"), true);
		Args->SetNumberField(TEXT("timeout_ms"), 60000);
		return PyTool.Execute(Args);
	}

	/** Count occurrences of a substring. */
	static int32 SbAcc_CountOccurrences(const FString& Haystack, const FString& Needle)
	{
		int32 Count = 0;
		int32 From = 0;
		while (true)
		{
			const int32 At = Haystack.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromStart, From);
			if (At == INDEX_NONE)
			{
				break;
			}
			++Count;
			From = At + Needle.Len();
		}
		return Count;
	}
}

// Discard one lint result and preserve its top-level advisories.

UNTEST_UNIT_OPTS(Claireon, SidebandAcceptance, DiscardingScriptStillSurfacesLint, UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonSidebandAcceptanceTestsNS;

	// Initialize the headless registry so the bridge can dispatch lint.
	FClaireonServer* Server = FClaireonModule::Get().EnsureServerForTest();
	UNTEST_ASSERT_PTR(Server);

	const FString Path = SbAcc_MakeFixture(3);
	UNTEST_ASSERT_FALSE(Path.IsEmpty());

	// Read only sliced findings, discarding the per-call envelope.
	const FString Code = FString::Printf(TEXT(
		"r = claireon.bp_lint(asset_path='%s')\n"
		"findings = r['data']['findings']\n"
		"print(str(findings)[:120])\n"), *Path);

	const IClaireonTool::FToolResult Result = SbAcc_RunPythonQuiet(Code);
	UNTEST_ASSERT_FALSE(Result.bIsError);

	UNTEST_EXPECT_TRUE(Result.Summary.Contains(TEXT("bp_lint")));
	UNTEST_EXPECT_TRUE(Result.Summary.Contains(TEXT("BP_SidebandAcceptance")));
	UNTEST_EXPECT_TRUE(Result.Summary.Contains(TEXT("finding")));

	const FString Xml = FClaireonXmlFormatter::FormatExecuteResult(Result);
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("bp_lint")));

	bool bHintSurfaced = false;
	for (const FClaireonAdvisory& Advisory : Result.InnerAdvisories)
	{
		if (Advisory.Kind == EClaireonAdvisoryKind::Hint
			&& Advisory.HintKey == FName(TEXT("claireon.lint.judgement-reference")))
		{
			bHintSurfaced = true;
		}
	}
	UNTEST_EXPECT_TRUE(bHintSurfaced);
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<hint tool=\"bp_lint\"")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("claireon://instructions/blueprint-authoring")));
	co_return;
}

// Repeated calls to one target coalesce into one counted rollup.

UNTEST_UNIT_OPTS(Claireon, SidebandAcceptance, RepeatedCallsCoalesceToOneRow, UNTEST_TIMEOUTMS(180000))
{
	using namespace ClaireonSidebandAcceptanceTestsNS;

	FClaireonServer* Server = FClaireonModule::Get().EnsureServerForTest();
	UNTEST_ASSERT_PTR(Server);

	const FString Path = SbAcc_MakeFixture(2);
	UNTEST_ASSERT_FALSE(Path.IsEmpty());

	// Force stdout spill so assertions cover post-gate rollup survival.
	const FString Code = FString::Printf(TEXT(
		"for _ in range(5):\n"
		"    r = claireon.bp_lint(asset_path='%s')\n"
		"    findings = r['data']['findings']\n"
		"print('x' * 250000)\n"), *Path);

	const IClaireonTool::FToolResult Result = SbAcc_RunPythonQuiet(Code);
	UNTEST_ASSERT_FALSE(Result.bIsError);

	UNTEST_EXPECT_TRUE(Result.Summary.Contains(TEXT("x5")));
	UNTEST_EXPECT_EQ(SbAcc_CountOccurrences(Result.Summary, TEXT("bp_lint")), 1);

	// Coalescing and rate limiting must emit one judgement hint.
	const FString Xml = FClaireonXmlFormatter::FormatExecuteResult(Result);
	UNTEST_EXPECT_EQ(SbAcc_CountOccurrences(Xml, TEXT("key=\"claireon.lint.judgement-reference\"")), 1);
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<hint tool=\"bp_lint\" count=\"5\" key=\"claireon.lint.judgement-reference\">")));
	co_return;
}

#endif // WITH_UNTESTED
