// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Tests for advisory identity, aggregation, and bridge capture lifecycle.

#if WITH_UNTESTED

#include "Untest.h"

#include "ClaireonAdvisory.h"
#include "ClaireonAdvisoryCoalesce.h"
#include "ClaireonBridge.h"
#include "ClaireonHintRateLimiter.h"
#include "ClaireonSessionManager.h"
#include "ClaireonXmlFormatter.h"
#include "Tools/ClaireonBlueprintGraphTool_Format.h"
#include "Tools/ClaireonTool_BlueprintCompileBatch.h"
#include "Tools/ClaireonTool_Lint.h"
#include "Tools/IClaireonTool.h"

#include "Dom/JsonObject.h"

namespace ClaireonAdvisoryCoreTestsNS
{
	// File-local prefix: anonymous namespaces are not isolation under unity batching.

	static FClaireonAdvisory AdvCore_Make(
		EClaireonAdvisoryKind Kind,
		const TCHAR* Tool,
		const TCHAR* Target,
		const TCHAR* Text,
		FName HintKey = NAME_None)
	{
		FClaireonAdvisory Advisory;
		Advisory.Kind = Kind;
		Advisory.SourceTool = Tool;
		Advisory.Target = Target;
		Advisory.Text = Text;
		Advisory.HintKey = HintKey;
		return Advisory;
	}

	static TSharedPtr<FJsonObject> AdvCore_Args(const TCHAR* AssetPath, const TCHAR* GraphName)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		if (AssetPath)
		{
			Args->SetStringField(TEXT("asset_path"), AssetPath);
		}
		if (GraphName)
		{
			Args->SetStringField(TEXT("graph_name"), GraphName);
		}
		return Args;
	}
}

// Summary identity is (tool, target); last text wins and count reports calls.

UNTEST_UNIT(Claireon, AdvisoryCore, AdvisoryCore_SameTargetSummariesCollapseToLast)
{
	using namespace ClaireonAdvisoryCoreTestsNS;

	TArray<FClaireonAdvisory> Raw;
	for (int32 Index = 0; Index < 5; ++Index)
	{
		Raw.Add(AdvCore_Make(EClaireonAdvisoryKind::Summary, TEXT("bp_format"),
			TEXT("/Game/Fixtures/BP_AdvisoryTarget:EventGraph"),
			*FString::Printf(TEXT("pass %d: %d moved"), Index, 140 - Index * 30)));
	}

	const TArray<FClaireonAdvisory> Out = ClaireonAdvisoryCoalesce::Coalesce(Raw);
	UNTEST_ASSERT_EQ(Out.Num(), 1);
	UNTEST_EXPECT_EQ(Out[0].OccurrenceCount, 5);
	// Last text wins -- a repeated call is a converging operation; reporting the first
	// (or summing) manufactures a confident-wrong number.
	UNTEST_EXPECT_STREQ(*Out[0].Text, TEXT("pass 4: 20 moved"));
	co_return;
}

UNTEST_UNIT(Claireon, AdvisoryCore, AdvisoryCore_DistinctTargetsStaySeparateInOrder)
{
	using namespace ClaireonAdvisoryCoreTestsNS;

	TArray<FClaireonAdvisory> Raw;
	Raw.Add(AdvCore_Make(EClaireonAdvisoryKind::Summary, TEXT("bp_format"), TEXT("/Game/A"), TEXT("a")));
	Raw.Add(AdvCore_Make(EClaireonAdvisoryKind::Summary, TEXT("bp_format"), TEXT("/Game/B"), TEXT("b")));
	Raw.Add(AdvCore_Make(EClaireonAdvisoryKind::Summary, TEXT("bp_format"), TEXT("/Game/C"), TEXT("c")));

	const TArray<FClaireonAdvisory> Out = ClaireonAdvisoryCoalesce::Coalesce(Raw);
	UNTEST_ASSERT_EQ(Out.Num(), 3);
	UNTEST_EXPECT_EQ(Out[0].OccurrenceCount, 1);
	UNTEST_EXPECT_STREQ(*Out[0].Target, TEXT("/Game/A"));
	UNTEST_EXPECT_STREQ(*Out[1].Target, TEXT("/Game/B"));
	UNTEST_EXPECT_STREQ(*Out[2].Target, TEXT("/Game/C"));
	co_return;
}

// Warning identity is (tool, target, text).

UNTEST_UNIT(Claireon, AdvisoryCore, AdvisoryCore_IdenticalWarningsCountForty)
{
	using namespace ClaireonAdvisoryCoreTestsNS;

	TArray<FClaireonAdvisory> Raw;
	for (int32 Index = 0; Index < 40; ++Index)
	{
		Raw.Add(AdvCore_Make(EClaireonAdvisoryKind::Warning, TEXT("bp_remove_node"),
			TEXT("/Game/Fixtures/BP_AdvisoryTarget"),
			TEXT("response_mode=changed: no affected nodes recorded")));
	}
	Raw.Add(AdvCore_Make(EClaireonAdvisoryKind::Warning, TEXT("bp_remove_node"),
		TEXT("/Game/Fixtures/BP_AdvisoryTarget"), TEXT("a DIFFERENT warning")));

	const TArray<FClaireonAdvisory> Out = ClaireonAdvisoryCoalesce::Coalesce(Raw);
	UNTEST_ASSERT_EQ(Out.Num(), 2);
	UNTEST_EXPECT_EQ(Out[0].OccurrenceCount, 40);
	UNTEST_EXPECT_EQ(Out[1].OccurrenceCount, 1);
	// Warning text is identity, not payload: the different warning did not merge.
	UNTEST_EXPECT_STREQ(*Out[1].Text, TEXT("a DIFFERENT warning"));
	co_return;
}

// Hint identity is (tool, key, target, text).

UNTEST_UNIT(Claireon, AdvisoryCore, AdvisoryCore_SameKeyHintsCollapse)
{
	using namespace ClaireonAdvisoryCoreTestsNS;

	TArray<FClaireonAdvisory> Raw;
	for (int32 Index = 0; Index < 5; ++Index)
	{
		Raw.Add(AdvCore_Make(EClaireonAdvisoryKind::Hint, TEXT("bp_lint"),
			TEXT("/Game/Fixtures/BP_AdvisoryTarget"), TEXT("read the authoring doc"),
			FName(TEXT("claireon.lint.judgement-reference"))));
	}

	const TArray<FClaireonAdvisory> Out = ClaireonAdvisoryCoalesce::Coalesce(Raw);
	UNTEST_ASSERT_EQ(Out.Num(), 1);
	UNTEST_EXPECT_EQ(Out[0].OccurrenceCount, 5);
	UNTEST_EXPECT_TRUE(Out[0].HintKey == FName(TEXT("claireon.lint.judgement-reference")));
	co_return;
}

UNTEST_UNIT(Claireon, AdvisoryCore, AdvisoryCore_KindDiscriminatesIdentity)
{
	using namespace ClaireonAdvisoryCoreTestsNS;

	// Same tool, target, and text -- but different kinds. Must NOT merge: a warning and a
	// summary that happen to share a string are different information.
	TArray<FClaireonAdvisory> Raw;
	Raw.Add(AdvCore_Make(EClaireonAdvisoryKind::Warning, TEXT("bp_lint"), TEXT("/Game/A"), TEXT("same text")));
	Raw.Add(AdvCore_Make(EClaireonAdvisoryKind::Summary, TEXT("bp_lint"), TEXT("/Game/A"), TEXT("same text")));

	const TArray<FClaireonAdvisory> Out = ClaireonAdvisoryCoalesce::Coalesce(Raw);
	UNTEST_ASSERT_EQ(Out.Num(), 2);
	co_return;
}

// Target extraction.

UNTEST_UNIT(Claireon, AdvisoryCore, AdvisoryCore_TargetExtractionRule)
{
	using namespace ClaireonAdvisoryCoreTestsNS;

	UNTEST_EXPECT_STREQ(
		*ClaireonAdvisoryCoalesce::ExtractTarget(AdvCore_Args(TEXT("/Game/BP_X"), TEXT("EventGraph"))),
		TEXT("/Game/BP_X:EventGraph"));
	UNTEST_EXPECT_STREQ(
		*ClaireonAdvisoryCoalesce::ExtractTarget(AdvCore_Args(TEXT("/Game/BP_X"), nullptr)),
		TEXT("/Game/BP_X"));
	UNTEST_EXPECT_STREQ(
		*ClaireonAdvisoryCoalesce::ExtractTarget(AdvCore_Args(nullptr, TEXT("EventGraph"))),
		TEXT("EventGraph"));
	UNTEST_EXPECT_TRUE(ClaireonAdvisoryCoalesce::ExtractTarget(AdvCore_Args(nullptr, nullptr)).IsEmpty());
	UNTEST_EXPECT_TRUE(ClaireonAdvisoryCoalesce::ExtractTarget(nullptr).IsEmpty());
	co_return;
}

UNTEST_UNIT(Claireon, AdvisoryCore, AdvisoryCore_SessionAddressedCallsGetDistinctTargets)
{
	using namespace ClaireonAdvisoryCoreTestsNS;

	// Session-addressed calls need distinct targets to preserve each asset in the rollup.
	TSharedPtr<FJsonObject> SessionA = MakeShared<FJsonObject>();
	SessionA->SetStringField(TEXT("session_id"), TEXT("bp-open-aaa"));
	TSharedPtr<FJsonObject> SessionB = MakeShared<FJsonObject>();
	SessionB->SetStringField(TEXT("session_id"), TEXT("bp-open-bbb"));
	TSharedPtr<FJsonObject> SessionWithGraph = MakeShared<FJsonObject>();
	SessionWithGraph->SetStringField(TEXT("session_id"), TEXT("bp-open-aaa"));
	SessionWithGraph->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));

	const FString TargetA = ClaireonAdvisoryCoalesce::ExtractTarget(SessionA);
	const FString TargetB = ClaireonAdvisoryCoalesce::ExtractTarget(SessionB);
	UNTEST_EXPECT_STREQ(*TargetA, TEXT("session:bp-open-aaa"));
	UNTEST_EXPECT_STREQ(*TargetB, TEXT("session:bp-open-bbb"));
	UNTEST_EXPECT_FALSE(TargetA == TargetB);
	UNTEST_EXPECT_STREQ(
		*ClaireonAdvisoryCoalesce::ExtractTarget(SessionWithGraph),
		TEXT("session:bp-open-aaa:EventGraph"));

	// asset_path still wins when both are present.
	TSharedPtr<FJsonObject> Both = AdvCore_Args(TEXT("/Game/BP_X"), nullptr);
	Both->SetStringField(TEXT("session_id"), TEXT("bp-open-aaa"));
	UNTEST_EXPECT_STREQ(*ClaireonAdvisoryCoalesce::ExtractTarget(Both), TEXT("/Game/BP_X"));
	co_return;
}

UNTEST_UNIT(Claireon, AdvisoryCore, AdvisoryCore_BridgeResolvesSessionTargetToAssetPath)
{
	using namespace ClaireonAdvisoryCoreTestsNS;

	// An OPEN session resolves to its asset path at capture, so session-addressed rows
	// group with path-addressed rows on the same asset.
	const FString AssetPath = TEXT("/Game/__MCPTests/BP_AdvisorySessionTarget");
	const FMCPOpenSessionResult Opened =
		FClaireonSessionManager::Get().OpenSession(AssetPath, TEXT("bp_open"));
	UNTEST_ASSERT_TRUE(Opened.Result == EOpenSessionResult::Success);

	FClaireonBridge::ResetInnerToolAdvisories();

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), Opened.SessionId);
	IClaireonTool::FToolResult Result;
	Result.Summary = TEXT("7 of 9 island(s) formatted");
	FClaireonBridge::AppendInnerToolAdvisories(TEXT("bp_format"), Args, Result);

	// A session that is GONE by capture time still keeps per-session distinctness.
	FClaireonSessionManager::Get().CloseSession(Opened.SessionId);
	TSharedPtr<FJsonObject> StaleArgs = MakeShared<FJsonObject>();
	StaleArgs->SetStringField(TEXT("session_id"), Opened.SessionId);
	FClaireonBridge::AppendInnerToolAdvisories(TEXT("bp_format"), StaleArgs, Result);

	const TArray<FClaireonAdvisory> Drained = FClaireonBridge::DrainInnerToolAdvisories();
	UNTEST_ASSERT_EQ(Drained.Num(), 2);
	UNTEST_EXPECT_STREQ(*Drained[0].Target, *AssetPath);
	UNTEST_EXPECT_STREQ(*Drained[1].Target, *(TEXT("session:") + Opened.SessionId));
	co_return;
}

UNTEST_UNIT(Claireon, AdvisoryCore, AdvisoryCore_WireBytesAreUtf8AndIncludePayloads)
{
	using namespace ClaireonAdvisoryCoreTestsNS;

	// Three CJK characters occupy nine UTF-8 bytes.
	const FString Multibyte = FString(TEXT("テスト"));
	FClaireonAdvisory Warning = AdvCore_Make(EClaireonAdvisoryKind::Warning, TEXT(""), TEXT(""), *Multibyte);
	TArray<FClaireonAdvisory> JustWarning = { Warning };
	const int32 WarningBytes = ClaireonAdvisoryCoalesce::MeasureWireAdvisoryBytes(JustWarning);
	UNTEST_EXPECT_EQ(WarningBytes, 9);
	UNTEST_EXPECT_TRUE(WarningBytes > Multibyte.Len());

	// A hint's serialized payload is counted -- it is most of what ships.
	FClaireonAdvisory Hint = AdvCore_Make(EClaireonAdvisoryKind::Hint, TEXT("bp_lint"), TEXT(""), TEXT("r"));
	Hint.Payload = IClaireonTool::MakeResourceHint(
		TEXT("claireon://instructions/blueprint-authoring"), TEXT("judgement rules"),
		FName(TEXT("claireon.lint.judgement-reference")));
	TArray<FClaireonAdvisory> WithPayload = { Hint };
	FClaireonAdvisory Bare = Hint;
	Bare.Payload = nullptr;
	TArray<FClaireonAdvisory> WithoutPayload = { Bare };
	UNTEST_EXPECT_TRUE(
		ClaireonAdvisoryCoalesce::MeasureWireAdvisoryBytes(WithPayload)
		> ClaireonAdvisoryCoalesce::MeasureWireAdvisoryBytes(WithoutPayload) + 60);
	co_return;
}

// Bridge capture and drain without CPython.

namespace ClaireonAdvisoryCoreTestsNS
{
	static IClaireonTool::FToolResult AdvCore_ResultWithAllKinds()
	{
		IClaireonTool::FToolResult Result;
		Result.Summary = TEXT("45 finding(s) across 10 rule(s)");
		Result.Warnings.Add(TEXT("warning one"));
		Result.Warnings.Add(TEXT("warning two"));

		TSharedPtr<FJsonObject> Hint = MakeShared<FJsonObject>();
		Hint->SetStringField(TEXT("resource"), TEXT("claireon://instructions/blueprint-authoring"));
		Hint->SetStringField(TEXT("reason"), TEXT("judgement rules live here"));
		Result.AddHint(Hint);
		return Result;
	}
}

UNTEST_UNIT(Claireon, AdvisoryCore, AdvisoryCore_BridgeCapturesAllThreeKinds)
{
	using namespace ClaireonAdvisoryCoreTestsNS;

	FClaireonBridge::ResetInnerToolAdvisories();
	FClaireonBridge::AppendInnerToolAdvisories(
		TEXT("bp_lint"),
		AdvCore_Args(TEXT("/Game/Fixtures/BP_AdvisoryTarget"), TEXT("EventGraph")),
		AdvCore_ResultWithAllKinds());

	const TArray<FClaireonAdvisory> Drained = FClaireonBridge::DrainInnerToolAdvisories();
	UNTEST_ASSERT_EQ(Drained.Num(), 4);

	// Summary first, warnings in order, hint last (capture order is per-kind).
	UNTEST_EXPECT_TRUE(Drained[0].Kind == EClaireonAdvisoryKind::Summary);
	UNTEST_EXPECT_STREQ(*Drained[0].SourceTool, TEXT("bp_lint"));
	UNTEST_EXPECT_STREQ(*Drained[0].Target, TEXT("/Game/Fixtures/BP_AdvisoryTarget:EventGraph"));
	UNTEST_EXPECT_STREQ(*Drained[0].Text, TEXT("45 finding(s) across 10 rule(s)"));

	UNTEST_EXPECT_TRUE(Drained[1].Kind == EClaireonAdvisoryKind::Warning);
	UNTEST_EXPECT_STREQ(*Drained[1].Text, TEXT("warning one"));
	UNTEST_EXPECT_TRUE(Drained[2].Kind == EClaireonAdvisoryKind::Warning);
	UNTEST_EXPECT_STREQ(*Drained[2].Text, TEXT("warning two"));

	UNTEST_EXPECT_TRUE(Drained[3].Kind == EClaireonAdvisoryKind::Hint);
	UNTEST_EXPECT_TRUE(Drained[3].Payload.IsValid());
	UNTEST_EXPECT_STREQ(*Drained[3].Text, TEXT("judgement rules live here"));
	UNTEST_EXPECT_TRUE(Drained[3].HintKey.IsNone()); // no "key" field on this hint
	co_return;
}

UNTEST_UNIT(Claireon, AdvisoryCore, AdvisoryCore_BridgeSkipsMalformedHint)
{
	using namespace ClaireonAdvisoryCoreTestsNS;

	FClaireonBridge::ResetInnerToolAdvisories();

	IClaireonTool::FToolResult Result;
	Result.Summary = TEXT("done");
	TSharedPtr<FJsonObject> Malformed = MakeShared<FJsonObject>();
	Malformed->SetStringField(TEXT("tool"), TEXT("bp_open"));
	Malformed->SetStringField(TEXT("resource"), TEXT("claireon://x")); // tool XOR resource -> invalid
	Malformed->SetStringField(TEXT("reason"), TEXT("both at once"));
	Result.AddHint(Malformed);

	FClaireonBridge::AppendInnerToolAdvisories(TEXT("bp_open"), nullptr, Result);

	const TArray<FClaireonAdvisory> Drained = FClaireonBridge::DrainInnerToolAdvisories();
	UNTEST_ASSERT_EQ(Drained.Num(), 1);
	UNTEST_EXPECT_TRUE(Drained[0].Kind == EClaireonAdvisoryKind::Summary);
	co_return;
}

UNTEST_UNIT(Claireon, AdvisoryCore, AdvisoryCore_BridgeCapturesErrorResultWarnings)
{
	using namespace ClaireonAdvisoryCoreTestsNS;

	FClaireonBridge::ResetInnerToolAdvisories();

	// A failing tool's advisories are exactly what a script's `except` handler discards.
	IClaireonTool::FToolResult Result = IClaireonTool::MakeErrorResult(TEXT("it broke"));
	Result.Warnings.Add(TEXT("half the mutation applied before the failure"));

	FClaireonBridge::AppendInnerToolAdvisories(
		TEXT("bp_remove_node"), AdvCore_Args(TEXT("/Game/BP_X"), nullptr), Result);

	const TArray<FClaireonAdvisory> Drained = FClaireonBridge::DrainInnerToolAdvisories();
	UNTEST_ASSERT_EQ(Drained.Num(), 1);
	UNTEST_EXPECT_TRUE(Drained[0].Kind == EClaireonAdvisoryKind::Warning);
	UNTEST_EXPECT_STREQ(*Drained[0].Target, TEXT("/Game/BP_X"));
	// Error results carry no Summary; ErrorMessage reaches the script as a RuntimeError
	// and is deliberately not side-banded.
	co_return;
}

UNTEST_UNIT(Claireon, AdvisoryCore, AdvisoryCore_DrainEmptiesAndCyclesAreIndependent)
{
	using namespace ClaireonAdvisoryCoreTestsNS;

	FClaireonBridge::ResetInnerToolAdvisories();
	FClaireonBridge::AppendInnerToolAdvisories(
		TEXT("bp_lint"), nullptr, AdvCore_ResultWithAllKinds());

	UNTEST_EXPECT_EQ(FClaireonBridge::DrainInnerToolAdvisories().Num(), 4);
	UNTEST_EXPECT_EQ(FClaireonBridge::DrainInnerToolAdvisories().Num(), 0); // drain twice: empty

	// Second reset/append/drain cycle is unaffected by the first.
	FClaireonBridge::ResetInnerToolAdvisories();
	IClaireonTool::FToolResult Small;
	Small.Summary = TEXT("only a summary");
	FClaireonBridge::AppendInnerToolAdvisories(TEXT("log_tail"), nullptr, Small);
	UNTEST_EXPECT_EQ(FClaireonBridge::DrainInnerToolAdvisories().Num(), 1);
	co_return;
}

UNTEST_UNIT(Claireon, AdvisoryCore, AdvisoryCore_InvocationScopeSavesAndRestores)
{
	using namespace ClaireonAdvisoryCoreTestsNS;

	// Outer invocation state: 2 advisories accumulated, one limiter key consumed.
	FClaireonBridge::ResetInnerToolAdvisories();
	ClaireonHintRateLimiter::ResetScope();
	IClaireonTool::FToolResult Outer;
	Outer.Summary = TEXT("outer summary");
	Outer.Warnings.Add(TEXT("outer warning"));
	FClaireonBridge::AppendInnerToolAdvisories(TEXT("bp_lint"), nullptr, Outer);
	const FName Key(TEXT("claireon.lint.judgement-reference"));
	UNTEST_EXPECT_TRUE(ClaireonHintRateLimiter::ShouldEmit(Key, TEXT("default")));

	{
		// A nested python_execute runs with a FRESH scope: empty accumulator, open
		// limiter -- the outer script's consumption must not blind the nested run.
		FClaireonBridgeInvocationScope Nested;
		UNTEST_EXPECT_EQ(FClaireonBridge::DrainInnerToolAdvisories().Num(), 0);
		UNTEST_EXPECT_TRUE(ClaireonHintRateLimiter::ShouldEmit(Key, TEXT("default")));

		IClaireonTool::FToolResult Inner;
		Inner.Summary = TEXT("inner summary");
		FClaireonBridge::AppendInnerToolAdvisories(TEXT("bp_format"), nullptr, Inner);
		UNTEST_EXPECT_EQ(FClaireonBridge::DrainInnerToolAdvisories().Num(), 1);
	}

	// Nested scope exit must restore the outer queue and consumed keys.
	UNTEST_EXPECT_FALSE(ClaireonHintRateLimiter::ShouldEmit(Key, TEXT("default")));
	const TArray<FClaireonAdvisory> Restored = FClaireonBridge::DrainInnerToolAdvisories();
	UNTEST_ASSERT_EQ(Restored.Num(), 2);
	UNTEST_EXPECT_STREQ(*Restored[0].Text, TEXT("outer summary"));

	ClaireonHintRateLimiter::ResetScope();
	co_return;
}

// Summary rollup rendering.

UNTEST_UNIT(Claireon, AdvisoryCore, AdvisoryCore_RollupRendersGroupsAndCounts)
{
	using namespace ClaireonAdvisoryCoreTestsNS;

	TArray<FClaireonAdvisory> Coalesced;
	FClaireonAdvisory Format = AdvCore_Make(EClaireonAdvisoryKind::Summary, TEXT("bp_format"),
		TEXT("/Game/Fixtures/BP_AdvisoryTarget:EventGraph"), TEXT("partial / applied_clean, 4 moved, 3 added"));
	Format.OccurrenceCount = 12;
	Coalesced.Add(Format);
	FClaireonAdvisory Lint = AdvCore_Make(EClaireonAdvisoryKind::Summary, TEXT("bp_lint"),
		TEXT("/Game/Fixtures/BP_AdvisoryTarget"), TEXT("45 finding(s) across 10 rule(s)"));
	Lint.OccurrenceCount = 5;
	Coalesced.Add(Lint);
	// Count of 1 and no target: both decorations omitted.
	Coalesced.Add(AdvCore_Make(EClaireonAdvisoryKind::Summary, TEXT("log_tail"), TEXT(""), TEXT("120 lines")));
	// Non-Summary records are the wire's concern, never the rollup's.
	Coalesced.Add(AdvCore_Make(EClaireonAdvisoryKind::Warning, TEXT("bp_remove_node"), TEXT("/Game/X"), TEXT("w")));

	const FString Rollup = ClaireonAdvisoryCoalesce::RenderSummaryRollup(Coalesced);
	UNTEST_EXPECT_TRUE(Rollup.Contains(TEXT("\n  bp_format x12 on /Game/Fixtures/BP_AdvisoryTarget:EventGraph -- partial / applied_clean, 4 moved, 3 added")));
	UNTEST_EXPECT_TRUE(Rollup.Contains(TEXT("\n  bp_lint x5 on /Game/Fixtures/BP_AdvisoryTarget -- 45 finding(s) across 10 rule(s)")));
	UNTEST_EXPECT_TRUE(Rollup.Contains(TEXT("\n  log_tail -- 120 lines")));
	// Count of 1 renders no xN marker (" x1 " as a token; "x12" above legitimately
	// contains the substring "x1").
	UNTEST_EXPECT_FALSE(Rollup.Contains(TEXT(" x1 ")));
	UNTEST_EXPECT_FALSE(Rollup.Contains(TEXT("bp_remove_node")));
	co_return;
}

UNTEST_UNIT(Claireon, AdvisoryCore, AdvisoryCore_RollupBudgetElidesNeverOverflows)
{
	using namespace ClaireonAdvisoryCoreTestsNS;

	// Overflow rows must fit behind an elision marker within the summary budget.
	const FString LongText = FString::ChrN(100, TEXT('m'));
	TArray<FClaireonAdvisory> Coalesced;
	for (int32 Index = 0; Index < 200; ++Index)
	{
		Coalesced.Add(AdvCore_Make(EClaireonAdvisoryKind::Summary, TEXT("bp_format"),
			*FString::Printf(TEXT("/Game/Target_%03d"), Index), *LongText));
	}

	const FString Rollup = ClaireonAdvisoryCoalesce::RenderSummaryRollup(Coalesced, 1400);
	UNTEST_EXPECT_TRUE(FTCHARToUTF8(*Rollup).Length() <= 1400);
	UNTEST_EXPECT_TRUE(Rollup.Contains(TEXT("more")));
	UNTEST_EXPECT_TRUE(Rollup.Contains(TEXT("/Game/Target_000"))); // first rows survive
	co_return;
}

// Inner-advisory wire attributes.

UNTEST_UNIT(Claireon, AdvisoryCore, AdvisoryCore_FormatterEmitsAdvisoryAttributes)
{
	using namespace ClaireonAdvisoryCoreTestsNS;

	IClaireonTool::FToolResult Result;
	Result.Summary = TEXT("Execution completed. (8 tool call(s) made)");

	FClaireonAdvisory Warning = AdvCore_Make(EClaireonAdvisoryKind::Warning, TEXT("bp_remove_node"),
		TEXT("/Game/Fixtures/BP_AdvisoryTarget"), TEXT("response_mode=changed: no affected nodes recorded"));
	Warning.OccurrenceCount = 3;
	Result.InnerAdvisories.Add(Warning);

	FClaireonAdvisory Hint = AdvCore_Make(EClaireonAdvisoryKind::Hint, TEXT("bp_lint"),
		TEXT("/Game/Fixtures/BP_AdvisoryTarget"), TEXT("judgement rules"),
		FName(TEXT("claireon.lint.judgement-reference")));
	Hint.OccurrenceCount = 5;
	Hint.Payload = IClaireonTool::MakeResourceHint(
		TEXT("claireon://instructions/blueprint-authoring"), TEXT("judgement rules"),
		FName(TEXT("claireon.lint.judgement-reference")));
	Result.InnerAdvisories.Add(Hint);

	const FString Xml = FClaireonXmlFormatter::FormatExecuteResult(Result);
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<warning tool=\"bp_remove_node\" count=\"3\">")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<hint tool=\"bp_lint\" count=\"5\" key=\"claireon.lint.judgement-reference\">")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("claireon://instructions/blueprint-authoring")));

	// The advisory block sits INSIDE the result element, before the final closing tag.
	const int32 CloseAt = Xml.Find(TEXT("</execute-result>"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	const int32 HintAt = Xml.Find(TEXT("<hint tool="), ESearchCase::CaseSensitive);
	UNTEST_EXPECT_TRUE(CloseAt > HintAt && HintAt != INDEX_NONE);

	// A tool's OWN hint stays attribute-free apart from payload-driven key promotion.
	IClaireonTool::FToolResult Own;
	Own.Summary = TEXT("ok");
	Own.AddHint(IClaireonTool::MakeGuidanceHint(TEXT("bp_open"), TEXT("open it first"), nullptr,
		FName(TEXT("bp_get_graph_detail_omissions"))));
	const FString OwnXml = FClaireonXmlFormatter::FormatExecuteResult(Own);
	UNTEST_EXPECT_TRUE(OwnXml.Contains(TEXT("<hint key=\"bp_get_graph_detail_omissions\">")));
	UNTEST_EXPECT_FALSE(OwnXml.Contains(TEXT("<hint tool=")));
	co_return;
}

// Repeated observations of one target use the last value, not their sum.

namespace ClaireonAdvisoryCoreTestsNS
{
	static FClaireonAdvisory AdvCore_Tier1Record(
		const TCHAR* Tool, const TCHAR* Target,
		const TArray<FClaireonFieldAggregation>& Spec,
		TFunctionRef<void(FJsonObject&)> FillData)
	{
		FClaireonAdvisory Advisory = AdvCore_Make(EClaireonAdvisoryKind::Summary, Tool, Target, TEXT("per-call text"));
		Advisory.AggregationSpec = Spec;
		Advisory.Data = MakeShared<FJsonObject>();
		FillData(*Advisory.Data);
		return Advisory;
	}
}

UNTEST_UNIT(Claireon, AdvisoryCore, AdvisoryCore_Tier1IncidentChurnNeverSums)
{
	using namespace ClaireonAdvisoryCoreTestsNS;

	const TArray<FClaireonFieldAggregation> Spec = {
		{ FName(TEXT("nodes_moved")), EClaireonAggregationKind::Sum },
	};

	// The incident's exact series on ONE target: 140, 7, 10, 4, 4, 4, 4, 4.
	const int32 Series[] = { 140, 7, 10, 4, 4, 4, 4, 4 };
	TArray<FClaireonAdvisory> Raw;
	for (int32 Value : Series)
	{
		Raw.Add(AdvCore_Tier1Record(TEXT("bp_format"), TEXT("/Game/Fixtures/BP_AdvisoryTarget:EventGraph"), Spec,
			[Value](FJsonObject& Data) { Data.SetNumberField(TEXT("nodes_moved"), Value); }));
	}

	const FString Rollup = ClaireonAdvisoryCoalesce::RenderTier1Rollup(Raw);
	UNTEST_EXPECT_TRUE(Rollup.Contains(TEXT("bp_format x8 on /Game/Fixtures/BP_AdvisoryTarget:EventGraph")));
	UNTEST_EXPECT_TRUE(Rollup.Contains(TEXT("nodes_moved: 4")));
	// "177 nodes moved" in a 218-node graph is the confident-wrong number this exists to stop.
	UNTEST_EXPECT_FALSE(Rollup.Contains(TEXT("177")));
	co_return;
}

UNTEST_UNIT(Claireon, AdvisoryCore, AdvisoryCore_Tier1SumsAcrossDistinctTargetsOnly)
{
	using namespace ClaireonAdvisoryCoreTestsNS;

	const TArray<FClaireonFieldAggregation> Spec = {
		{ FName(TEXT("nodes_moved")), EClaireonAggregationKind::Sum },
		{ FName(TEXT("target")), EClaireonAggregationKind::DistinctCount },
	};

	TArray<FClaireonAdvisory> Raw;
	// Target A converges 50 -> 10 (last-within-target = 10); B and C once each.
	Raw.Add(AdvCore_Tier1Record(TEXT("bp_format"), TEXT("/Game/A"), Spec,
		[](FJsonObject& Data) { Data.SetNumberField(TEXT("nodes_moved"), 50); }));
	Raw.Add(AdvCore_Tier1Record(TEXT("bp_format"), TEXT("/Game/A"), Spec,
		[](FJsonObject& Data) { Data.SetNumberField(TEXT("nodes_moved"), 10); }));
	Raw.Add(AdvCore_Tier1Record(TEXT("bp_format"), TEXT("/Game/B"), Spec,
		[](FJsonObject& Data) { Data.SetNumberField(TEXT("nodes_moved"), 20); }));
	Raw.Add(AdvCore_Tier1Record(TEXT("bp_format"), TEXT("/Game/C"), Spec,
		[](FJsonObject& Data) { Data.SetNumberField(TEXT("nodes_moved"), 30); }));

	const FString Rollup = ClaireonAdvisoryCoalesce::RenderTier1Rollup(Raw);
	UNTEST_EXPECT_TRUE(Rollup.Contains(TEXT("bp_format x4 across 3 targets")));
	UNTEST_EXPECT_TRUE(Rollup.Contains(TEXT("nodes_moved: 60"))); // 10 + 20 + 30, never 110
	UNTEST_EXPECT_TRUE(Rollup.Contains(TEXT("target: 3 distinct")));
	co_return;
}

UNTEST_UNIT(Claireon, AdvisoryCore, AdvisoryCore_Tier1HistogramAndInvariantDivergence)
{
	using namespace ClaireonAdvisoryCoreTestsNS;

	const TArray<FClaireonFieldAggregation> Spec = {
		{ FName(TEXT("format_status")), EClaireonAggregationKind::Histogram },
		{ FName(TEXT("islands_total")), EClaireonAggregationKind::Invariant },
	};

	TArray<FClaireonAdvisory> Raw;
	const TCHAR* Statuses[] = { TEXT("partial"), TEXT("partial"), TEXT("complete") };
	const int32 Islands[] = { 9, 9, 7 }; // diverges on the third pass
	for (int32 Index = 0; Index < 3; ++Index)
	{
		const TCHAR* Status = Statuses[Index];
		const int32 IslandCount = Islands[Index];
		Raw.Add(AdvCore_Tier1Record(TEXT("bp_format"), TEXT("/Game/A"), Spec,
			[Status, IslandCount](FJsonObject& Data)
			{
				Data.SetStringField(TEXT("format_status"), Status);
				Data.SetNumberField(TEXT("islands_total"), IslandCount);
			}));
	}

	const FString Rollup = ClaireonAdvisoryCoalesce::RenderTier1Rollup(Raw);
	UNTEST_EXPECT_TRUE(Rollup.Contains(TEXT("format_status: 2 partial/1 complete")));
	// A violated invariant is information, not a silent merge.
	UNTEST_EXPECT_TRUE(Rollup.Contains(TEXT("islands_total DIVERGED 9 -> 7")));
	co_return;
}

UNTEST_UNIT(Claireon, AdvisoryCore, AdvisoryCore_Tier1SessionsDoNotCollapse)
{
	using namespace ClaireonAdvisoryCoreTestsNS;

	// Different sessions must contribute separate asset totals.
	const TArray<FClaireonFieldAggregation> Spec = {
		{ FName(TEXT("nodes_moved")), EClaireonAggregationKind::Sum },
	};
	TArray<FClaireonAdvisory> Raw;
	Raw.Add(AdvCore_Tier1Record(TEXT("bp_format"), TEXT("session:bp-open-aaa"), Spec,
		[](FJsonObject& Data) { Data.SetNumberField(TEXT("nodes_moved"), 10); }));
	Raw.Add(AdvCore_Tier1Record(TEXT("bp_format"), TEXT("session:bp-open-bbb"), Spec,
		[](FJsonObject& Data) { Data.SetNumberField(TEXT("nodes_moved"), 20); }));

	const FString Rollup = ClaireonAdvisoryCoalesce::RenderTier1Rollup(Raw);
	UNTEST_EXPECT_TRUE(Rollup.Contains(TEXT("across 2 targets")));
	UNTEST_EXPECT_TRUE(Rollup.Contains(TEXT("nodes_moved: 30")));
	co_return;
}

UNTEST_UNIT(Claireon, AdvisoryCore, AdvisoryCore_Tier0RendererSkipsTier1Records)
{
	using namespace ClaireonAdvisoryCoreTestsNS;

	const TArray<FClaireonFieldAggregation> Spec = {
		{ FName(TEXT("nodes_moved")), EClaireonAggregationKind::Sum },
	};

	TArray<FClaireonAdvisory> Records;
	Records.Add(AdvCore_Tier1Record(TEXT("bp_format"), TEXT("/Game/A"), Spec,
		[](FJsonObject& Data) { Data.SetNumberField(TEXT("nodes_moved"), 4); }));
	Records.Add(AdvCore_Make(EClaireonAdvisoryKind::Summary, TEXT("log_tail"), TEXT(""), TEXT("120 lines")));

	// The two renderers partition the rows: no double reporting.
	const FString Tier0 = ClaireonAdvisoryCoalesce::RenderSummaryRollup(Records);
	UNTEST_EXPECT_FALSE(Tier0.Contains(TEXT("bp_format")));
	UNTEST_EXPECT_TRUE(Tier0.Contains(TEXT("log_tail")));
	const FString Tier1 = ClaireonAdvisoryCoalesce::RenderTier1Rollup(Records);
	UNTEST_EXPECT_TRUE(Tier1.Contains(TEXT("bp_format")));
	UNTEST_EXPECT_FALSE(Tier1.Contains(TEXT("log_tail")));
	co_return;
}

// Aggregation fields must match the real tools' Data keys.

UNTEST_UNIT(Claireon, AdvisoryCore, AdvisoryCore_RealToolSpecsNameRealFields)
{
	ClaireonBlueprintGraphTool_Format FormatTool;
	TArray<FClaireonFieldAggregation> FormatSpec;
	UNTEST_ASSERT_TRUE(FormatTool.GetSummaryAggregationSpec(FormatSpec));
	bool bHasMoved = false;
	bool bHasStatus = false;
	for (const FClaireonFieldAggregation& Field : FormatSpec)
	{
		bHasMoved |= (Field.Field == FName(TEXT("nodes_moved")) && Field.Kind == EClaireonAggregationKind::Sum);
		bHasStatus |= (Field.Field == FName(TEXT("format_status")) && Field.Kind == EClaireonAggregationKind::Histogram);
	}
	UNTEST_EXPECT_TRUE(bHasMoved);
	UNTEST_EXPECT_TRUE(bHasStatus);

	ClaireonTool_BlueprintCompileBatch CompileTool;
	TArray<FClaireonFieldAggregation> CompileSpec;
	UNTEST_ASSERT_TRUE(CompileTool.GetSummaryAggregationSpec(CompileSpec));
	bool bSucceededIsLast = false;
	for (const FClaireonFieldAggregation& Field : CompileSpec)
	{
		// Last, not Sum: summing across repeated batch calls double-counts a retried
		// batch -- see the spec's comment.
		bSucceededIsLast |= (Field.Field == FName(TEXT("succeeded")) && Field.Kind == EClaireonAggregationKind::Last);
	}
	UNTEST_EXPECT_TRUE(bSucceededIsLast);

	// Lint uses text summaries because its Data has no scalar finding count.
	ClaireonTool_Lint LintTool;
	TArray<FClaireonFieldAggregation> LintSpec;
	UNTEST_EXPECT_FALSE(LintTool.GetSummaryAggregationSpec(LintSpec));
	co_return;
}

#endif // WITH_UNTESTED
