// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Verify the keyed judgement-reference hint survives routing and spilling and resolves through live registries.

#if WITH_UNTESTED

#include "Untest.h"

#include "ClaireonModule.h"
#include "ClaireonOutputGate.h"
#include "ClaireonXmlFormatter.h"
#include "ClaireonServer.h"
#include "ClaireonSettings.h"
#include "IClaireonToolProvider.h"
#include "Tools/ClaireonBlueprintGraphTool_Extract.h"
#include "Tools/ClaireonBlueprintGraphTool_Format.h"
#include "Tools/ClaireonTool_Lint.h"
#include "Tools/IClaireonTool.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Features/IModularFeatures.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "K2Node_CallFunction.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SquidTasks/Task.h"
#include "UObject/Package.h"

namespace ClaireonLintJudgementRefTestsNS
{
	// Prefix helpers to avoid unity-build collisions.

	/** Expected resource and fallback addresses, checked against live registries. */
	static const TCHAR* JRef_ExpectedUri  = TEXT("claireon://instructions/blueprint-authoring");
	static const TCHAR* JRef_ExpectedTool = TEXT("instructions_read");

	/** Scale payload size with disconnected 5000-unit wire pairs. */
	static FString JRef_MakeFixture(int32 NumPairs)
	{
		static int32 Counter = 0;
		const FString AssetName = FString::Printf(TEXT("BP_LintJudgementRef_%d"), Counter++);
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

	/** A Blueprint with nothing wrong with it, for the zero-finding case. */
	static FString JRef_MakeCleanFixture()
	{
		return JRef_MakeFixture(0);
	}

	static TSharedPtr<FJsonObject> JRef_Args(const FString& ObjectPath)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), ObjectPath);
		return Args;
	}

	static IClaireonTool::FToolResult JRef_RunLint(const TSharedPtr<FJsonObject>& Args)
	{
		ClaireonTool_Lint Tool;
		return Tool.Execute(Args);
	}

	/** The judgement-reference hint payload from Result.Hints, or null when absent. */
	static TSharedPtr<FJsonObject> JRef_Field(const IClaireonTool::FToolResult& Result)
	{
		for (const TSharedPtr<FJsonObject>& Hint : Result.Hints)
		{
			FString Key;
			if (Hint.IsValid()
				&& Hint->TryGetStringField(TEXT("key"), Key)
				&& Key == TEXT("claireon.lint.judgement-reference"))
			{
				return Hint;
			}
		}
		return nullptr;
	}

	/** Require a valid resource hint with fallback instructions and the rising-count explanation. */
	static bool JRef_IsWellFormed(const IClaireonTool::FToolResult& Result, FString& OutWhyNot)
	{
		const TSharedPtr<FJsonObject> Hint = JRef_Field(Result);
		if (!Hint.IsValid())
		{
			OutWhyNot = TEXT("the judgement-reference hint is absent from Result.Hints");
			return false;
		}
		FString ShapeError;
		if (!IClaireonTool::ValidateHint(Hint, ShapeError))
		{
			OutWhyNot = FString::Printf(TEXT("hint fails ValidateHint: %s"), *ShapeError);
			return false;
		}
		FString Uri, Reason;
		if (!Hint->TryGetStringField(TEXT("resource"), Uri) || Uri != JRef_ExpectedUri)
		{
			OutWhyNot = FString::Printf(TEXT("hint resource is '%s', expected '%s'"), *Uri, JRef_ExpectedUri);
			return false;
		}
		if (!Hint->TryGetStringField(TEXT("reason"), Reason) || Reason.IsEmpty())
		{
			OutWhyNot = TEXT("hint reason missing or empty");
			return false;
		}
		if (!Reason.Contains(JRef_ExpectedTool))
		{
			OutWhyNot = FString::Printf(
				TEXT("reason does not name %s; a caller without resource access has no route"),
				JRef_ExpectedTool);
			return false;
		}
		if (!Reason.Contains(TEXT("bp_format")))
		{
			OutWhyNot = TEXT("reason does not mention bp_format; the rising-count clause is the load-bearing sentence");
			return false;
		}
		if (!Reason.Contains(TEXT("regression")))
		{
			OutWhyNot = TEXT("reason does not say a rising count is NOT a regression, which is the whole point");
			return false;
		}
		return true;
	}

	/** Serialized byte length of Result.Data, which is what the gate measures. */
	static int32 JRef_DataBytes(const IClaireonTool::FToolResult& Result)
	{
		if (!Result.Data.IsValid())
		{
			return 0;
		}
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Result.Data.ToSharedRef(), Writer);
		return Out.Len();
	}

	static int32 JRef_SpillThreshold()
	{
		const UClaireonSettings* Settings = UClaireonSettings::Get();
		return IsValid(Settings) ? Settings->ResultSpillThresholdBytes : 8192;
	}

	/** True when the gate spilled the generic data stream. */
	static bool JRef_DidSpill(const IClaireonTool::FToolResult& Routed)
	{
		bool bSpilled = false;
		return Routed.Data.IsValid()
			&& Routed.Data->TryGetBoolField(TEXT("__mcp_spilled__"), bSpilled)
			&& bSpilled;
	}

	/** Redirects the spill root into a throw-away directory and cleans it up. */
	struct FScopedSpillRoot
	{
		FString Root;
		FScopedSpillRoot()
		{
			Root = FPaths::ProjectIntermediateDir() / TEXT("ClaireonTests")
				/ TEXT("JudgementRef") / FGuid::NewGuid().ToString(EGuidFormats::Short);
			IFileManager::Get().MakeDirectory(*Root, /*Tree=*/true);
			FClaireonOutputGate::SetResultsRootOverrideForTests(Root);
		}
		~FScopedSpillRoot()
		{
			FClaireonOutputGate::SetResultsRootOverrideForTests(FString());
			if (!Root.IsEmpty())
			{
				IFileManager::Get().DeleteDirectory(*Root, /*bRequireExists=*/false, /*Tree=*/true);
			}
		}
	};

	/** Find tools through the modular-feature registry. */
	static bool JRef_RegistryHasTool(const FString& ToolName)
	{
		FClaireonModule::Get().EnsureServerForTest();
		const TArray<IClaireonToolProvider*> Providers = IModularFeatures::Get()
			.GetModularFeatureImplementations<IClaireonToolProvider>(IClaireonToolProvider::FeatureName);
		for (IClaireonToolProvider* Provider : Providers)
		{
			if (!Provider) { continue; }
			for (const TSharedPtr<IClaireonTool>& Tool : Provider->GetTools())
			{
				if (Tool.IsValid() && Tool->GetName() == ToolName)
				{
					return true;
				}
			}
		}
		return false;
	}
}

using namespace ClaireonLintJudgementRefTestsNS;

// Emit the hint across caller shapes, including zero findings.
UNTEST_UNIT_OPTS(Claireon, LintJudgementRef, FieldIsPresentUnderEveryCallerShape, UNTEST_TIMEOUTMS(90000))
{
	const FString Path = JRef_MakeFixture(3);
	UNTEST_ASSERT_FALSE(Path.IsEmpty());
	const FString CleanPath = JRef_MakeCleanFixture();
	UNTEST_ASSERT_FALSE(CleanPath.IsEmpty());

	TArray<FString> Failures;

	const auto Check = [&Failures](const TCHAR* Shape, const TSharedPtr<FJsonObject>& Args)
	{
		const IClaireonTool::FToolResult Result = JRef_RunLint(Args);
		if (Result.bIsError)
		{
			Failures.Add(FString::Printf(TEXT("[%s] lint errored: %s"), Shape, *Result.ErrorMessage));
			return;
		}
		FString WhyNot;
		if (!JRef_IsWellFormed(Result, WhyNot))
		{
			Failures.Add(FString::Printf(TEXT("[%s] %s"), Shape, *WhyNot));
		}
	};

	Check(TEXT("scope omitted"), JRef_Args(Path));

	{
		TSharedPtr<FJsonObject> Args = JRef_Args(Path);
		TArray<TSharedPtr<FJsonValue>> Scopes;
		Scopes.Add(MakeShared<FJsonValueString>(TEXT("all")));
		Args->SetArrayField(TEXT("scope"), Scopes);
		Check(TEXT("scope=all"), Args);
	}

	{
		TSharedPtr<FJsonObject> Args = JRef_Args(Path);
		Args->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
		Check(TEXT("graph_name=EventGraph"), Args);
	}

	// Restrict to a rule this fixture cannot trigger to produce a verified empty finding set.
	{
		TSharedPtr<FJsonObject> Args = JRef_Args(CleanPath);
		TArray<TSharedPtr<FJsonValue>> Rules;
		Rules.Add(MakeShared<FJsonValueString>(TEXT("exec-join")));
		Args->SetArrayField(TEXT("rules"), Rules);

		const IClaireonTool::FToolResult Result = JRef_RunLint(Args);
		if (Result.bIsError)
		{
			Failures.Add(FString::Printf(TEXT("[0 findings] lint errored: %s"), *Result.ErrorMessage));
		}
		else
		{
			const TArray<TSharedPtr<FJsonValue>>* Findings = nullptr;
			const bool bHasFindings = Result.Data.IsValid()
				&& Result.Data->TryGetArrayField(TEXT("findings"), Findings) && Findings;
			if (!bHasFindings || (*Findings).Num() != 0)
			{
				Failures.Add(FString::Printf(
					TEXT("[0 findings] fixture produced %d finding(s); the zero-finding shape was "
					     "not exercised"), bHasFindings ? (*Findings).Num() : -1));
			}
			FString WhyNot;
			if (!JRef_IsWellFormed(Result, WhyNot))
			{
				Failures.Add(FString::Printf(TEXT("[0 findings] %s"), *WhyNot));
			}
		}
	}

	// quiet belongs to python_execute; it must not suppress the inner lint hint.
	{
		TSharedPtr<FJsonObject> Args = JRef_Args(Path);
		Args->SetBoolField(TEXT("quiet"), true);
		Check(TEXT("quiet=true"), Args);
	}

	// response_mode is unsupported: refusal or an unchanged hint is acceptable, silent suppression is not.
	{
		TSharedPtr<FJsonObject> Args = JRef_Args(Path);
		Args->SetStringField(TEXT("response_mode"), TEXT("status"));
		const IClaireonTool::FToolResult Result = JRef_RunLint(Args);
		if (!Result.bIsError)
		{
			FString WhyNot;
			if (!JRef_IsWellFormed(Result, WhyNot))
			{
				Failures.Add(FString::Printf(
					TEXT("[response_mode=status] succeeded but %s -- if bp_lint has since gained a "
					     "verbosity axis, the field must be emitted at every verbosity"), *WhyNot));
			}
		}
	}

	if (Failures.Num() > 0)
	{
		UE_LOG(LogTemp, Error, TEXT("[LintJudgementRef] %d caller-shape failure(s):"), Failures.Num());
		for (const FString& F : Failures)
		{
			UE_LOG(LogTemp, Error, TEXT("[LintJudgementRef]   %s"), *F);
		}
	}
	UNTEST_EXPECT_TRUE(Failures.Num() == 0);
	co_return;
}

// Preserve the hint in both routed results and wire output.
UNTEST_UNIT_OPTS(Claireon, LintJudgementRef, HintSurvivesTheOutputGate, UNTEST_TIMEOUTMS(90000))
{
	FScopedSpillRoot SpillRoot;

	const FString Path = JRef_MakeFixture(40);
	UNTEST_ASSERT_FALSE(Path.IsEmpty());

	TArray<FString> Failures;

	const auto CheckRouted = [&Failures](const TCHAR* Shape, const TSharedPtr<FJsonObject>& Args)
	{
		const IClaireonTool::FToolResult Result = JRef_RunLint(Args);
		if (Result.bIsError)
		{
			Failures.Add(FString::Printf(TEXT("[%s] lint errored: %s"), Shape, *Result.ErrorMessage));
			return;
		}

		const IClaireonTool::FToolResult Routed = FClaireonOutputGate::RouteResult(
			Result, TEXT("bp_lint"), Args, TEXT("untest-judgement-ref"),
			EClaireonSpillStreamSet::GenericData);

		FString WhyNot;
		if (!JRef_IsWellFormed(Routed, WhyNot))
		{
			Failures.Add(FString::Printf(TEXT("[%s] post-gate: %s"), Shape, *WhyNot));
			return;
		}

		const FString Xml = FClaireonXmlFormatter::FormatExecuteResult(Routed);
		if (!Xml.Contains(TEXT("key=\"claireon.lint.judgement-reference\"")))
		{
			Failures.Add(FString::Printf(
				TEXT("[%s] wire XML has no keyed judgement-reference <hint> element"), Shape));
		}
		else if (!Xml.Contains(JRef_ExpectedUri))
		{
			Failures.Add(FString::Printf(
				TEXT("[%s] wire XML hint does not carry the resource URI"), Shape));
		}
	};

	CheckRouted(TEXT("plain"), JRef_Args(Path));

	{
		TSharedPtr<FJsonObject> Args = JRef_Args(Path);
		Args->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
		CheckRouted(TEXT("graph_name"), Args);
	}

	// force_inline still bounds metadata and must preserve hints.
	{
		TSharedPtr<FJsonObject> Args = JRef_Args(Path);
		Args->SetBoolField(TEXT("force_inline"), true);
		CheckRouted(TEXT("force_inline"), Args);
	}

	if (Failures.Num() > 0)
	{
		UE_LOG(LogTemp, Error, TEXT("[LintJudgementRef] %d gate-survival failure(s):"), Failures.Num());
		for (const FString& F : Failures)
		{
			UE_LOG(LogTemp, Error, TEXT("[LintJudgementRef]   %s"), *F);
		}
	}
	UNTEST_EXPECT_TRUE(Failures.Num() == 0);
	co_return;
}

// Preserve the hint when real data spills.
UNTEST_UNIT_OPTS(Claireon, LintJudgementRef, HintSurvivesASpilledPayload, UNTEST_TIMEOUTMS(120000))
{
	FScopedSpillRoot SpillRoot;

	const int32 Threshold = JRef_SpillThreshold();
	UNTEST_ASSERT_TRUE(Threshold > 0);

	// Size the fixture from the effective spill threshold, with a bounded node count.
	const int32 NumPairs = FMath::Clamp(Threshold / 200 + 20, 40, 600);
	const FString Path = JRef_MakeFixture(NumPairs);
	UNTEST_ASSERT_FALSE(Path.IsEmpty());

	TSharedPtr<FJsonObject> Args = JRef_Args(Path);
	TArray<TSharedPtr<FJsonValue>> Scopes;
	Scopes.Add(MakeShared<FJsonValueString>(TEXT("all")));
	Args->SetArrayField(TEXT("scope"), Scopes);

	const IClaireonTool::FToolResult Result = JRef_RunLint(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);

	FString WhyNot;
	UNTEST_ASSERT_TRUE(JRef_IsWellFormed(Result, WhyNot));

	// Require an actual spill rather than skipping when the fixture is too small.
	const int32 DataBytes = JRef_DataBytes(Result);
	if (DataBytes <= Threshold)
	{
		UE_LOG(LogTemp, Error,
			TEXT("[LintJudgementRef] fixture produced %d bytes of Data against a %d-byte spill "
			     "threshold with %d pairs; the spill case was not exercised"),
			DataBytes, Threshold, NumPairs);
	}
	UNTEST_ASSERT_TRUE(DataBytes > Threshold);

	const IClaireonTool::FToolResult Routed = FClaireonOutputGate::RouteResult(
		Result, TEXT("bp_lint"), Args, TEXT("untest-judgement-ref-spill"),
		EClaireonSpillStreamSet::GenericData);

	UNTEST_ASSERT_TRUE(JRef_DidSpill(Routed));

	FString WhyNotRouted;
	UNTEST_EXPECT_TRUE(JRef_IsWellFormed(Routed, WhyNotRouted));
	if (!WhyNotRouted.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("[LintJudgementRef] post-spill: %s"), *WhyNotRouted);
	}

	const FString Xml = FClaireonXmlFormatter::FormatExecuteResult(Routed);
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("key=\"claireon.lint.judgement-reference\"")));
	UNTEST_EXPECT_TRUE(Xml.Contains(JRef_ExpectedUri));

	UE_LOG(LogTemp, Display,
		TEXT("[LintJudgementRef] spill case: %d finding pair(s) -> %d bytes of Data against a "
		     "%d-byte threshold; hint survived the spill on struct and wire"),
		NumPairs, DataBytes, Threshold);
	co_return;
}

// Resolve the resource and fallback tool through live registries.
UNTEST_UNIT_OPTS(Claireon, LintJudgementRef, PointerResolvesAgainstTheLiveRegistries, UNTEST_TIMEOUTMS(60000))
{
	FClaireonServer* Server = FClaireonModule::Get().EnsureServerForTest();
	UNTEST_ASSERT_TRUE(Server != nullptr);
	Server->ReloadMCPContent();

	const FString Path = JRef_MakeFixture(2);
	UNTEST_ASSERT_FALSE(Path.IsEmpty());

	const IClaireonTool::FToolResult Result = JRef_RunLint(JRef_Args(Path));
	UNTEST_ASSERT_FALSE(Result.bIsError);

	const TSharedPtr<FJsonObject> Hint = JRef_Field(Result);
	UNTEST_ASSERT_TRUE(Hint.IsValid());

	FString Uri;
	FString Reason;
	UNTEST_ASSERT_TRUE(Hint->TryGetStringField(TEXT("resource"), Uri));
	UNTEST_ASSERT_TRUE(Hint->TryGetStringField(TEXT("reason"), Reason));

	FString Served;
	UNTEST_EXPECT_TRUE(Server->TryGetResourceText(Uri, Served));
	UNTEST_EXPECT_TRUE(Served.Len() > 1000);

	UNTEST_EXPECT_TRUE(Reason.Contains(JRef_ExpectedTool));
	UNTEST_EXPECT_TRUE(JRef_RegistryHasTool(JRef_ExpectedTool));

	// Require the URI and fallback topic to identify the same document.
	const FString Slug = Uri.RightChop(FCString::Strlen(TEXT("claireon://instructions/")));
	UNTEST_EXPECT_FALSE(Slug.IsEmpty());
	UNTEST_EXPECT_TRUE(Reason.Contains(Slug));
	co_return;
}

// Check judgement pointers in full tool descriptions.
UNTEST_UNIT_OPTS(Claireon, LintJudgementRef, RestructuringToolsPointAtTheJudgementDoc, UNTEST_TIMEOUTMS(60000))
{
	TArray<FString> Failures;

	const auto CheckTool = [&Failures](const IClaireonTool& Tool)
	{
		const FString Name = Tool.GetName();
		const FString FullDesc = Tool.GetFullDescription();
		const FString StdDesc = Tool.GetDescription();

		if (!FullDesc.Contains(JRef_ExpectedTool))
		{
			Failures.Add(FString::Printf(
				TEXT("[%s] GetFullDescription does not name %s"), *Name, JRef_ExpectedTool));
		}
		if (!FullDesc.Contains(TEXT("blueprint-authoring")))
		{
			Failures.Add(FString::Printf(
				TEXT("[%s] GetFullDescription does not name the blueprint-authoring topic"), *Name));
		}
		if (FullDesc.Len() < 200)
		{
			Failures.Add(FString::Printf(
				TEXT("[%s] GetFullDescription is %d chars, expected >= 200"), *Name, FullDesc.Len()));
		}
		if (FullDesc == StdDesc)
		{
			Failures.Add(FString::Printf(
				TEXT("[%s] GetFullDescription is identical to GetDescription"), *Name));
		}
		// Keep standard descriptions within their length limit.
		if (StdDesc.Len() < 80 || StdDesc.Len() > 400)
		{
			Failures.Add(FString::Printf(
				TEXT("[%s] GetDescription is %d chars, expected [80,400]"), *Name, StdDesc.Len()));
		}
		if (!JRef_RegistryHasTool(Name))
		{
			Failures.Add(FString::Printf(TEXT("[%s] is not in the live tool registry"), *Name));
		}
	};

	{
		ClaireonBlueprintGraphTool_Format Tool;
		CheckTool(Tool);
		const FString FullDesc = Tool.GetFullDescription();
		if (!FullDesc.Contains(TEXT("reroute")))
		{
			Failures.Add(TEXT("[bp_format] GetFullDescription does not mention reroutes rising"));
		}
	}
	{
		ClaireonBlueprintGraphTool_ExtractFunction Tool;
		CheckTool(Tool);
	}
	{
		ClaireonBlueprintGraphTool_ExtractMacro Tool;
		CheckTool(Tool);
	}
	{
		ClaireonBlueprintGraphTool_ExtractComposite Tool;
		CheckTool(Tool);
	}
	{
		ClaireonBlueprintGraphTool_ExtractEvent Tool;
		CheckTool(Tool);
	}

	if (Failures.Num() > 0)
	{
		UE_LOG(LogTemp, Error, TEXT("[LintJudgementRef] %d description-pointer failure(s):"),
			Failures.Num());
		for (const FString& F : Failures)
		{
			UE_LOG(LogTemp, Error, TEXT("[LintJudgementRef]   %s"), *F);
		}
	}
	UNTEST_EXPECT_TRUE(Failures.Num() == 0);
	co_return;
}

#endif // WITH_UNTESTED
