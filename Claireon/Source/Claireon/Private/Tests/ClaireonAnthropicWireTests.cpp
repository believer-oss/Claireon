// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "ClaireonOutputGate.h"
#include "ClaireonXmlFormatter.h"
#include "ClaireonSettings.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "SquidTasks/Task.h"

// ===========================================================================
// Anthropic wire tests (ClaireonAnthropicWireTests)
// ===========================================================================
// Cases 4a-4e per CLAIREON_DISK_RESULTS/test-plan.md section 4.
//
// FClaireonXmlFormatter::FormatExecuteResult is the rendering chokepoint used by
// the server path to build the XML envelope that ships as tool_result content
// text.  The REPL path builds a separate plain-text wire, but the <spilled-
// result> / <stream> / <path> / <size-bytes> / <preview> / <truncated> markers
// called out in the plan are emitted by FormatExecuteResult when the gate has
// flagged __mcp_spilled__ on Data.  These tests assert that contract end-to-end
// by routing a synthetic FToolResult through FClaireonOutputGate::RouteResult
// (exact same side that the server and REPL use) and then through
// FClaireonXmlFormatter::FormatExecuteResult.
// ===========================================================================

namespace ClaireonAnthropicWireTestsHelpers
{
	static FString MakeUniqueTestRoot(const TCHAR* Case)
	{
		const FString ShortGuid = FGuid::NewGuid().ToString(EGuidFormats::Short);
		return FPaths::ProjectIntermediateDir()
			/ TEXT("ClaireonTests")
			/ TEXT("AnthropicWire")
			/ FString(Case)
			/ ShortGuid;
	}

	struct FScopedTestRoot
	{
		FString Root;
		explicit FScopedTestRoot(const TCHAR* Case)
		{
			Root = MakeUniqueTestRoot(Case);
			IFileManager::Get().MakeDirectory(*Root, /*Tree*/ true);
			FClaireonOutputGate::SetResultsRootOverrideForTests(Root);
		}
		~FScopedTestRoot()
		{
			FClaireonOutputGate::SetResultsRootOverrideForTests(FString());
			if (!Root.IsEmpty())
			{
				IFileManager::Get().DeleteDirectory(*Root, /*bRequireExists*/ false, /*Tree*/ true);
			}
		}
	};

	static IClaireonTool::FToolResult MakeGenericDataResult(int32 TargetBytes)
	{
		IClaireonTool::FToolResult R;
		R.Summary = TEXT("generic result");
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("payload"), FString::ChrN(FMath::Max(1, TargetBytes), TEXT('g')));
		R.Data = Data;
		return R;
	}

	static IClaireonTool::FToolResult MakePythonResult(const FString& Stdout, const FString& UELog)
	{
		IClaireonTool::FToolResult R;
		R.Summary = TEXT("python result");
		R.Logs = Stdout;
		R.UELog = UELog;
		return R;
	}
}

// ===========================================================================
// Case 4a: large generic-tool result spills; XML carries path/size/preview
// ===========================================================================

UNTEST_UNIT(Claireon, AnthropicWire, GenericSpillRendersPathAndPreview)
{
	using namespace ClaireonAnthropicWireTestsHelpers;
	FScopedTestRoot Scope(TEXT("GenericSpillRendersPathAndPreview"));

	const UClaireonSettings* S = UClaireonSettings::Get();
	const int32 Threshold = S ? S->ResultSpillThresholdBytes : 8192;

	IClaireonTool::FToolResult R = MakeGenericDataResult(Threshold * 2);
	IClaireonTool::FToolResult Routed = FClaireonOutputGate::RouteResult(
		MoveTemp(R), TEXT("claireon.asset_search"), TEXT("conv_4a"),
		EClaireonSpillStreamSet::GenericData);

	const FString Xml = FClaireonXmlFormatter::FormatExecuteResult(Routed);

	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<spilled-result>")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<stream name=\"data\">")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<path>")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<size-bytes>")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<preview>")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<truncated>true</truncated>")));

	// Does NOT contain the full padded payload -- the spilled envelope rewrites
	// Data so that no "payload" JSON field appears in the rendered XML body.
	UNTEST_EXPECT_FALSE(Xml.Contains(TEXT("\"payload\"")));

	co_return;
}

// ===========================================================================
// Case 4b: python_execute with only stdout spilled
// ===========================================================================

UNTEST_UNIT(Claireon, AnthropicWire, PythonStdoutOnlySpillXml)
{
	using namespace ClaireonAnthropicWireTestsHelpers;
	FScopedTestRoot Scope(TEXT("PythonStdoutOnlySpillXml"));

	const UClaireonSettings* S = UClaireonSettings::Get();
	const int32 Threshold = S ? S->ResultSpillThresholdBytes : 8192;

	const FString Big = FString::ChrN(Threshold * 2, TEXT('s'));
	const FString SmallUELog = TEXT("small uelog line");

	IClaireonTool::FToolResult R = MakePythonResult(Big, SmallUELog);
	IClaireonTool::FToolResult Routed = FClaireonOutputGate::RouteResult(
		MoveTemp(R), TEXT("claireon.python_execute"), TEXT("conv_4b"),
		EClaireonSpillStreamSet::PythonStdoutAndUELog);

	const FString Xml = FClaireonXmlFormatter::FormatExecuteResult(Routed);

	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<stream name=\"stdout\">")));
	UNTEST_EXPECT_FALSE(Xml.Contains(TEXT("<stream name=\"data\">")));
	UNTEST_EXPECT_FALSE(Xml.Contains(TEXT("<stream name=\"uelog\">"))); // uelog stayed inline

	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<path>")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<size-bytes>")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<preview>")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<truncated>true</truncated>")));

	// The full spilled stdout text (many 's' chars) must not appear in the XML.
	UNTEST_EXPECT_FALSE(Xml.Contains(Big));

	co_return;
}

// ===========================================================================
// Case 4c: python_execute with both stdout and uelog spilled
// ===========================================================================

UNTEST_UNIT(Claireon, AnthropicWire, PythonBothStreamsSpillXml)
{
	using namespace ClaireonAnthropicWireTestsHelpers;
	FScopedTestRoot Scope(TEXT("PythonBothStreamsSpillXml"));

	const UClaireonSettings* S = UClaireonSettings::Get();
	const int32 Threshold = S ? S->ResultSpillThresholdBytes : 8192;

	const FString BigStdout = FString::ChrN(Threshold * 2, TEXT('s'));
	const FString BigUELog  = FString::ChrN(Threshold * 2, TEXT('u'));

	IClaireonTool::FToolResult R = MakePythonResult(BigStdout, BigUELog);
	IClaireonTool::FToolResult Routed = FClaireonOutputGate::RouteResult(
		MoveTemp(R), TEXT("claireon.python_execute"), TEXT("conv_4c"),
		EClaireonSpillStreamSet::PythonStdoutAndUELog);

	const FString Xml = FClaireonXmlFormatter::FormatExecuteResult(Routed);

	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<stream name=\"stdout\">")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<stream name=\"uelog\">")));
	UNTEST_EXPECT_FALSE(Xml.Contains(TEXT("<stream name=\"data\">")));

	UNTEST_EXPECT_FALSE(Xml.Contains(BigStdout));
	UNTEST_EXPECT_FALSE(Xml.Contains(BigUELog));

	co_return;
}

// ===========================================================================
// Case 4d: small inline result -- no <spilled-result> envelope appears
// ===========================================================================

UNTEST_UNIT(Claireon, AnthropicWire, SmallPayloadStaysInline)
{
	using namespace ClaireonAnthropicWireTestsHelpers;
	FScopedTestRoot Scope(TEXT("SmallPayloadStaysInline"));

	IClaireonTool::FToolResult R = MakeGenericDataResult(/*TargetBytes=*/64);
	IClaireonTool::FToolResult Routed = FClaireonOutputGate::RouteResult(
		MoveTemp(R), TEXT("claireon.asset_search"), TEXT("conv_4d"),
		EClaireonSpillStreamSet::GenericData);

	const FString Xml = FClaireonXmlFormatter::FormatExecuteResult(Routed);

	UNTEST_EXPECT_FALSE(Xml.Contains(TEXT("<spilled-result>")));
	// Inline payload appears as today: <data> with the JSON body.
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<data>")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("\"payload\"")));

	co_return;
}

// ===========================================================================
// Case 4e: truncation at 2048 chars still yields a well-formed summary + path
// ===========================================================================

UNTEST_UNIT(Claireon, AnthropicWire, TruncatedDiagnosticsBodyContainsPath)
{
	using namespace ClaireonAnthropicWireTestsHelpers;
	FScopedTestRoot Scope(TEXT("TruncatedDiagnosticsBodyContainsPath"));

	const UClaireonSettings* S = UClaireonSettings::Get();
	const int32 Threshold = S ? S->ResultSpillThresholdBytes : 8192;

	IClaireonTool::FToolResult R = MakeGenericDataResult(Threshold * 2);
	IClaireonTool::FToolResult Routed = FClaireonOutputGate::RouteResult(
		MoveTemp(R), TEXT("claireon.asset_search"), TEXT("conv_4e"),
		EClaireonSpillStreamSet::GenericData);

	const FString Xml = FClaireonXmlFormatter::FormatExecuteResult(Routed);

	// MCPDiagnosticsEntry::ResponseBody is truncated to 2048 today.  A spilled
	// envelope that renders <summary> and <path> inside the first 2048 bytes
	// proves that the truncation boundary does not cut a load-bearing element
	// off the top of the stream.
	const FString Head = Xml.Left(2048);
	UNTEST_EXPECT_TRUE(Head.Contains(TEXT("<summary>")));
	UNTEST_EXPECT_TRUE(Head.Contains(TEXT("<path>")));

	co_return;
}

#endif // WITH_UNTESTED
