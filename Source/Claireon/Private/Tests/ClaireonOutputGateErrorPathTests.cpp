// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

// ============================================================================
// WI-14 coverage: python_execute error-path spill and hint noise.
//
// 1. Output-gate error path: a failing result whose ErrorMessage exceeds the
//    inline spill threshold must (a) keep the wire payload under the cap,
//    (b) name the spill path inline, (c) write the full text to disk exactly
//    once, and (d) carry the spill manifest so the error envelope can surface
//    it (previously the manifest check lived only on the success branch).
// 2. Hint policy seam (ClaireonPyExec_ComputeSessionHint): the script-content
//    get_editor_property nudge fires at most once per editor session, and
//    quiet=true suppresses all hints without consuming the session latch.
//
// UNTEST_ASSERT_*/UNTEST_EXPECT_* expand to co_return and cannot live inside
// lambdas; helpers below are plain bool/value-returning functions carried in
// the named namespace ClOGErrPathTestsHelpers with a `ClOGErr_` discriminator
// prefix (unity-batching safety).
// ============================================================================

#include "Untest.h"
#include "ClaireonOutputGate.h"
#include "ClaireonSettings.h"
#include "ClaireonXmlFormatter.h"
#include "Tools/ClaireonTool_ExecutePython.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "SquidTasks/Task.h"

// WI-14 hint-policy seam, defined with external linkage in
// ClaireonTool_ExecutePython.cpp (that work item owns only .cpp files, so the
// seam is not declared in the public header).
extern TSharedPtr<FJsonObject> ClaireonPyExec_ComputeSessionHint(
	const FString& Logs, const FString& Code, bool bQuiet);
extern void ClaireonPyExec_ResetHintSessionStateForTests();

namespace ClOGErrPathTestsHelpers
{
	/** Build a per-test unique root path under ProjectIntermediateDir. */
	static FString ClOGErr_MakeUniqueTestRoot(const TCHAR* Case)
	{
		const FString ShortGuid = FGuid::NewGuid().ToString(EGuidFormats::Short);
		return FPaths::ProjectIntermediateDir()
			/ TEXT("ClaireonTests")
			/ TEXT("OutputGateErrorPath")
			/ FString(Case)
			/ ShortGuid;
	}

	/** RAII scope guard that installs a test results-root override and clears it on destruction. */
	struct FClOGErrScopedTestRoot
	{
		FString Root;
		explicit FClOGErrScopedTestRoot(const TCHAR* Case)
		{
			Root = ClOGErr_MakeUniqueTestRoot(Case);
			IFileManager::Get().MakeDirectory(*Root, /*Tree*/ true);
			FClaireonOutputGate::SetResultsRootOverrideForTests(Root);
		}
		~FClOGErrScopedTestRoot()
		{
			FClaireonOutputGate::SetResultsRootOverrideForTests(FString());
			if (!Root.IsEmpty())
			{
				IFileManager::Get().DeleteDirectory(*Root, /*bRequireExists*/ false, /*Tree*/ true);
			}
		}
	};

	/** UTF-8 encoded byte length of a string. */
	static int32 ClOGErr_Utf8Len(const FString& In)
	{
		FTCHARToUTF8 Converter(*In);
		return Converter.Length();
	}

	/** Count non-overlapping occurrences of Needle in Haystack (case-sensitive). */
	static int32 ClOGErr_CountOccurrences(const FString& Haystack, const FString& Needle)
	{
		if (Needle.IsEmpty())
		{
			return 0;
		}
		int32 Count = 0;
		int32 SearchFrom = 0;
		while (true)
		{
			const int32 FoundAt = Haystack.Find(
				Needle, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
			if (FoundAt < 0)
			{
				break;
			}
			++Count;
			SearchFrom = FoundAt + Needle.Len();
		}
		return Count;
	}

	/** Lookup a spilled_streams entry by name on the manifest. Returns null if absent. */
	static TSharedPtr<FJsonObject> ClOGErr_FindStream(
		const TSharedPtr<FJsonObject>& Manifest, const FString& Name)
	{
		if (!Manifest.IsValid())
		{
			return nullptr;
		}
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Manifest->TryGetArrayField(TEXT("spilled_streams"), Arr) || !Arr)
		{
			return nullptr;
		}
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (V.IsValid() && V->TryGetObject(Obj) && Obj && (*Obj).IsValid())
			{
				FString N;
				if ((*Obj)->TryGetStringField(TEXT("name"), N) && N == Name)
				{
					return *Obj;
				}
			}
		}
		return nullptr;
	}

	/** True iff the manifest's inline_omitted array contains Field. */
	static bool ClOGErr_InlineOmittedContains(
		const TSharedPtr<FJsonObject>& Manifest, const FString& Field)
	{
		if (!Manifest.IsValid())
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Manifest->TryGetArrayField(TEXT("inline_omitted"), Arr) || !Arr)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			if (V.IsValid() && V->AsString() == Field)
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * Build an ASCII-only fake Python traceback of at least MinBytes bytes
	 * whose LAST line carries TailSentinel (the exception line -- the part the
	 * gate must keep inline).  ASCII-only so FString::Len() == UTF-8 bytes.
	 */
	static FString ClOGErr_BuildBigTraceback(int32 MinBytes, const FString& TailSentinel)
	{
		FString Out = TEXT("Traceback (most recent call last):\n");
		int32 LineIdx = 0;
		while (Out.Len() < MinBytes)
		{
			Out += FString::Printf(
				TEXT("  File \"/tmp/mcp_exec.py\", line %d, in <module>  # padding padding padding padding\n"),
				++LineIdx);
		}
		Out += TEXT("TimeoutError: boom ") + TailSentinel + TEXT("\n");
		return Out;
	}

	/** Fabricate the python_execute failure shape: full stdout mirrored into ErrorMessage. */
	static IClaireonTool::FToolResult ClOGErr_MakeFailingPythonResult(const FString& BigTraceback)
	{
		IClaireonTool::FToolResult R;
		R.bIsError = true;
		R.Logs = BigTraceback;
		R.ErrorMessage = TEXT("Python execution failed. Output:\n") + BigTraceback;
		return R;
	}
}

// ===========================================================================
// Error path: over-cap ErrorMessage spills; wire payload under the cap,
// spill path named inline, full text on disk exactly once, manifest present.
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, OutputGateErrorPath, ErrorMessageOverCapSpillsAndKeepsTailInline, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClOGErrPathTestsHelpers;
	FClOGErrScopedTestRoot Scope(TEXT("ErrorMessageOverCapSpillsAndKeepsTailInline"));

	const UClaireonSettings* Settings = UClaireonSettings::Get();
	const int32 Threshold = IsValid(Settings) ? Settings->ResultSpillThresholdBytes : 8192;

	const FString Sentinel = TEXT("CLAIREON_WI14_TAIL_SENTINEL");
	const FString Big = ClOGErr_BuildBigTraceback(Threshold * 2, Sentinel);

	IClaireonTool::FToolResult R = ClOGErr_MakeFailingPythonResult(Big);
	const FString FullError = R.ErrorMessage;

	IClaireonTool::FToolResult Routed = FClaireonOutputGate::RouteResult(
		MoveTemp(R), TEXT("python_execute"), TEXT("test_conv"),
		EClaireonSpillStreamSet::PythonStdoutAndUELog);

	// Still an error result.
	UNTEST_ASSERT_TRUE(Routed.bIsError);

	// (a) Wire payload is under the inline cap.
	UNTEST_EXPECT_TRUE(ClOGErr_Utf8Len(Routed.ErrorMessage) <= Threshold);

	// The exception/traceback TAIL stays inline.
	UNTEST_EXPECT_TRUE(Routed.ErrorMessage.Contains(Sentinel));

	// Manifest present on the error result with an "error" stream.
	UNTEST_ASSERT_TRUE(Routed.Data.IsValid());
	bool bSpilled = false;
	UNTEST_ASSERT_TRUE(Routed.Data->TryGetBoolField(TEXT("__mcp_spilled__"), bSpilled) && bSpilled);

	TSharedPtr<FJsonObject> ErrStream = ClOGErr_FindStream(Routed.Data, TEXT("error"));
	UNTEST_ASSERT_TRUE(ErrStream.IsValid());

	FString SpillPath;
	UNTEST_ASSERT_TRUE(ErrStream->TryGetStringField(TEXT("absolute_path"), SpillPath));
	UNTEST_ASSERT_TRUE(!SpillPath.IsEmpty());
	UNTEST_ASSERT_TRUE(FPaths::FileExists(SpillPath));

	// (b) The inline error names the spill path.
	UNTEST_EXPECT_TRUE(Routed.ErrorMessage.Contains(SpillPath));

	// (c) The spill file contains the full text exactly once.
	FString FileText;
	UNTEST_ASSERT_TRUE(FFileHelper::LoadFileToString(FileText, *SpillPath));
	UNTEST_EXPECT_TRUE(FileText == FullError);
	UNTEST_EXPECT_EQ(ClOGErr_CountOccurrences(FileText, Sentinel), 1);

	// The stripped inline field is declared on the manifest.
	UNTEST_EXPECT_TRUE(ClOGErr_InlineOmittedContains(Routed.Data, TEXT("error_message")));

	// stdout also spilled (existing behavior): inline Logs cleared, so the big
	// text is no longer duplicated onto the wire via <logs>.
	UNTEST_EXPECT_TRUE(Routed.Logs.IsEmpty());

	co_return;
}

// ===========================================================================
// Error path: under-cap ErrorMessage keeps its exact current wire shape.
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, OutputGateErrorPath, SmallErrorMessageStaysInlineUnchanged, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClOGErrPathTestsHelpers;
	FClOGErrScopedTestRoot Scope(TEXT("SmallErrorMessageStaysInlineUnchanged"));

	IClaireonTool::FToolResult R;
	R.bIsError = true;
	R.ErrorMessage = TEXT("NameError: name 'bogus_tool' is not defined");
	const FString OriginalError = R.ErrorMessage;

	IClaireonTool::FToolResult Routed = FClaireonOutputGate::RouteResult(
		MoveTemp(R), TEXT("python_execute"), TEXT("test_conv"),
		EClaireonSpillStreamSet::PythonStdoutAndUELog);

	UNTEST_ASSERT_TRUE(Routed.bIsError);
	UNTEST_EXPECT_STREQ(*Routed.ErrorMessage, *OriginalError);

	// No spill manifest injected.
	bool bSpilled = false;
	if (Routed.Data.IsValid())
	{
		Routed.Data->TryGetBoolField(TEXT("__mcp_spilled__"), bSpilled);
	}
	UNTEST_EXPECT_FALSE(bSpilled);

	co_return;
}

// ===========================================================================
// Error envelope: the XML error branch surfaces the spill manifest (the
// __mcp_spilled__ check previously lived only in the success branch).
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, OutputGateErrorPath, ErrorSpillManifestSurfacesInXmlErrorEnvelope, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClOGErrPathTestsHelpers;
	FClOGErrScopedTestRoot Scope(TEXT("ErrorSpillManifestSurfacesInXmlErrorEnvelope"));

	const UClaireonSettings* Settings = UClaireonSettings::Get();
	const int32 Threshold = IsValid(Settings) ? Settings->ResultSpillThresholdBytes : 8192;

	const FString Sentinel = TEXT("CLAIREON_WI14_XML_SENTINEL");
	const FString Big = ClOGErr_BuildBigTraceback(Threshold * 2, Sentinel);

	IClaireonTool::FToolResult R = ClOGErr_MakeFailingPythonResult(Big);

	IClaireonTool::FToolResult Routed = FClaireonOutputGate::RouteResult(
		MoveTemp(R), TEXT("python_execute"), TEXT("test_conv"),
		EClaireonSpillStreamSet::PythonStdoutAndUELog);

	TSharedPtr<FJsonObject> ErrStream = ClOGErr_FindStream(Routed.Data, TEXT("error"));
	UNTEST_ASSERT_TRUE(ErrStream.IsValid());
	FString SpillPath;
	UNTEST_ASSERT_TRUE(ErrStream->TryGetStringField(TEXT("absolute_path"), SpillPath));

	const FString Xml = FClaireonXmlFormatter::FormatExecuteResult(Routed);

	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("status=\"error\"")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<spilled-result>")));
	UNTEST_EXPECT_TRUE(Xml.Contains(SpillPath));

	// The inline cap is not defeated: the rendered envelope is far smaller
	// than the raw traceback it replaced (previously the whole stdout rode
	// the wire twice -- once as <logs>, once inside <error>).
	UNTEST_EXPECT_TRUE(Xml.Len() < Big.Len());

	co_return;
}

// ===========================================================================
// Hint policy: the get_editor_property script nudge fires at most once per
// editor session.
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, OutputGateErrorPath, ScriptHintFiresAtMostOncePerSession, UNTEST_TIMEOUTMS(10000))
{
	ClaireonPyExec_ResetHintSessionStateForTests();

	const FString Code = TEXT(
		"import unreal\n"
		"a = unreal.load_asset('/Game/Foo')\n"
		"v = a.get_editor_property('bHidden')\n");

	TSharedPtr<FJsonObject> First = ClaireonPyExec_ComputeSessionHint(
		FString(), Code, /*bQuiet=*/false);
	UNTEST_EXPECT_TRUE(First.IsValid());

	TSharedPtr<FJsonObject> Second = ClaireonPyExec_ComputeSessionHint(
		FString(), Code, /*bQuiet=*/false);
	UNTEST_EXPECT_FALSE(Second.IsValid());

	// Leave the latch clear so unrelated tests are order-independent.
	ClaireonPyExec_ResetHintSessionStateForTests();
	co_return;
}

// ===========================================================================
// Hint policy: quiet=true suppresses all hints and does NOT consume the
// once-per-session latch.
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, OutputGateErrorPath, QuietSuppressesAllHintsWithoutConsumingLatch, UNTEST_TIMEOUTMS(10000))
{
	ClaireonPyExec_ResetHintSessionStateForTests();

	const FString Code = TEXT(
		"import unreal\n"
		"a = unreal.load_asset('/Game/Foo')\n"
		"v = a.get_editor_property('bHidden')\n");

	// quiet suppresses the script-content channel...
	TSharedPtr<FJsonObject> Quiet = ClaireonPyExec_ComputeSessionHint(
		FString(), Code, /*bQuiet=*/true);
	UNTEST_EXPECT_FALSE(Quiet.IsValid());

	// ...and the error-derived channel (traceback with a claireon.* NameError).
	const FString ErrorLogs = TEXT(
		"Traceback (most recent call last):\n"
		"  File \"/tmp/mcp_exec.py\", line 5, in <module>\n"
		"    claireon.bp_open_nonexistent_xyzzy(asset_path='/Game/X')\n"
		"NameError: name 'bp_open_nonexistent_xyzzy' is not defined\n");
	TSharedPtr<FJsonObject> QuietError = ClaireonPyExec_ComputeSessionHint(
		ErrorLogs, FString(), /*bQuiet=*/true);
	UNTEST_EXPECT_FALSE(QuietError.IsValid());

	// The quiet calls did not burn the session latch: a later non-quiet
	// invocation still gets its one script hint.
	TSharedPtr<FJsonObject> AfterQuiet = ClaireonPyExec_ComputeSessionHint(
		FString(), Code, /*bQuiet=*/false);
	UNTEST_EXPECT_TRUE(AfterQuiet.IsValid());

	ClaireonPyExec_ResetHintSessionStateForTests();
	co_return;
}

// ===========================================================================
// Contract: the quiet parameter is schema-documented, and the existing
// required 'code' contract is untouched.
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, OutputGateErrorPath, QuietParamIsSchemaDocumented, UNTEST_TIMEOUTMS(10000))
{
	ClaireonTool_ExecutePython Tool;
	TSharedPtr<FJsonObject> Schema = Tool.GetInputSchema();
	UNTEST_ASSERT_TRUE(Schema.IsValid());

	const TSharedPtr<FJsonObject>* Properties = nullptr;
	UNTEST_ASSERT_TRUE(Schema->TryGetObjectField(TEXT("properties"), Properties)
		&& Properties && (*Properties).IsValid());

	// quiet declared as boolean.
	const TSharedPtr<FJsonObject>* QuietProp = nullptr;
	UNTEST_ASSERT_TRUE((*Properties)->TryGetObjectField(TEXT("quiet"), QuietProp)
		&& QuietProp && (*QuietProp).IsValid());
	FString QuietType;
	UNTEST_EXPECT_TRUE((*QuietProp)->TryGetStringField(TEXT("type"), QuietType));
	UNTEST_EXPECT_STREQ(*QuietType, TEXT("boolean"));

	// Backward compatibility: 'code' is still declared and still required.
	UNTEST_EXPECT_TRUE((*Properties)->HasField(TEXT("code")));
	const TArray<TSharedPtr<FJsonValue>>* Required = nullptr;
	UNTEST_ASSERT_TRUE(Schema->TryGetArrayField(TEXT("required"), Required) && Required);
	bool bCodeRequired = false;
	for (const TSharedPtr<FJsonValue>& V : *Required)
	{
		if (V.IsValid() && V->AsString() == TEXT("code"))
		{
			bCodeRequired = true;
			break;
		}
	}
	UNTEST_EXPECT_TRUE(bCodeRequired);
	// quiet must NOT be required.
	bool bQuietRequired = false;
	for (const TSharedPtr<FJsonValue>& V : *Required)
	{
		if (V.IsValid() && V->AsString() == TEXT("quiet"))
		{
			bQuietRequired = true;
			break;
		}
	}
	UNTEST_EXPECT_FALSE(bQuietRequired);

	co_return;
}

#endif // WITH_UNTESTED
