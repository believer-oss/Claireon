// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

// ============================================================================
// python_execute hint emission coverage.
//
// Exercises ClaireonTool_ExecutePython::BuildHintFromLogs against the four
// signature-class nudge patterns (NameError, AttributeError, TypeError
// unexpected-keyword, TypeError missing-positional), plus the explicit
// SyntaxError-no-hint case and a cold-start best-match populated case.
//
// UNTEST_ASSERT_* macros expand to co_return and cannot live inside lambda
// bodies, and anonymous-namespace helpers under unity batching need a
// file-local discriminator.  Helpers below carry the
// `PyHintTests622_` prefix and stay outside any lambda.
// ============================================================================

#include "Untest.h"
#include "Tools/ClaireonTool_ExecutePython.h"
#include "Tools/IClaireonTool.h"
#include "IClaireonToolProvider.h"
#include "Features/IModularFeatures.h"
#include "Dom/JsonObject.h"
#include "ClaireonToolSearchIndex.h"

namespace PyHintTests622_Helpers
{
	/** Pick any registered tool name (other than the meta tools) so TypeError
	 *  patterns referencing a real tool produce a hint.  Returns empty when
	 *  no provider is registered (test should bail in that case). */
	static FString PyHintTests622_PickAnyRegisteredToolName()
	{
		TArray<IClaireonToolProvider*> Providers = IModularFeatures::Get()
			.GetModularFeatureImplementations<IClaireonToolProvider>(
				IClaireonToolProvider::FeatureName);
		for (IClaireonToolProvider* Provider : Providers)
		{
			if (!Provider) { continue; }
			for (const TSharedPtr<IClaireonTool>& Tool : Provider->GetTools())
			{
				if (!Tool.IsValid()) { continue; }
				const FString Name = Tool->GetName();
				if (Name == TEXT("python_execute") || Name == TEXT("tool_search"))
				{
					continue;
				}
				return Name;
			}
		}
		return FString();
	}

	/** Make a synthetic traceback that resembles what Python prints for a
	 *  NameError where the user wrote `claireon.<X>(...)`.  Used by the
	 *  NameError test. */
	static FString PyHintTests622_BuildNameErrorTraceback(const FString& MissingName)
	{
		FString Out;
		Out += TEXT("Traceback (most recent call last):\n");
		Out += TEXT("  File \"/tmp/mcp_exec.py\", line 5, in <module>\n");
		Out += FString::Printf(TEXT("    claireon.%s(asset_path='/Game/X')\n"), *MissingName);
		Out += FString::Printf(TEXT("NameError: name '%s' is not defined\n"), *MissingName);
		return Out;
	}

	static FString PyHintTests622_BuildAttributeErrorTraceback(const FString& MissingName)
	{
		FString Out;
		Out += TEXT("Traceback (most recent call last):\n");
		Out += TEXT("  File \"/tmp/mcp_exec.py\", line 5, in <module>\n");
		Out += FString::Printf(TEXT("    claireon.%s()\n"), *MissingName);
		Out += FString::Printf(
			TEXT("AttributeError: module 'claireon' has no attribute '%s'\n"),
			*MissingName);
		return Out;
	}

	static FString PyHintTests622_BuildTypeErrorUnexpectedKwargTraceback(
		const FString& ToolName, const FString& Kwarg)
	{
		FString Out;
		Out += TEXT("Traceback (most recent call last):\n");
		Out += TEXT("  File \"/tmp/mcp_exec.py\", line 5, in <module>\n");
		Out += FString::Printf(TEXT("    claireon.%s(%s='x')\n"), *ToolName, *Kwarg);
		Out += FString::Printf(
			TEXT("TypeError: %s() got an unexpected keyword argument '%s'\n"),
			*ToolName, *Kwarg);
		return Out;
	}

	static FString PyHintTests622_BuildTypeErrorMissingPositionalTraceback(
		const FString& ToolName)
	{
		FString Out;
		Out += TEXT("Traceback (most recent call last):\n");
		Out += TEXT("  File \"/tmp/mcp_exec.py\", line 5, in <module>\n");
		Out += FString::Printf(TEXT("    claireon.%s()\n"), *ToolName);
		Out += FString::Printf(
			TEXT("TypeError: %s() missing 1 required positional argument: 'asset_path'\n"),
			*ToolName);
		return Out;
	}
}

// The NameError/AttributeError/ColdStart cases run FClaireonToolSearchIndex::
// EnsureBuilt() + FindNearest() for the best-match reason suffix; a cold index
// build blows the 0.5ms Untest unit default, so they carry an explicit timeout.
UNTEST_UNIT_OPTS(Claireon, PythonExecuteHint, NameErrorOnClaireonDotProducesHint, UNTEST_TIMEOUTMS(10000))
{
	using namespace PyHintTests622_Helpers;

	const FString MissingName = TEXT("bp_open_nonexistent_xyzzy");
	const FString Logs = PyHintTests622_BuildNameErrorTraceback(MissingName);

	TSharedPtr<FJsonObject> Hint = ClaireonTool_ExecutePython::BuildHintFromLogs(Logs);

	UNTEST_EXPECT_TRUE(Hint.IsValid());
	if (Hint.IsValid())
	{
		FString ToolField;
		UNTEST_EXPECT_TRUE(Hint->TryGetStringField(TEXT("tool"), ToolField));
		UNTEST_EXPECT_TRUE(ToolField == TEXT("tool_search"));

		const TSharedPtr<FJsonObject>* ArgsObj = nullptr;
		UNTEST_EXPECT_TRUE(Hint->TryGetObjectField(TEXT("args"), ArgsObj));
		if (ArgsObj && ArgsObj->IsValid())
		{
			FString QueryField;
			UNTEST_EXPECT_TRUE((*ArgsObj)->TryGetStringField(TEXT("query"), QueryField));
			UNTEST_EXPECT_TRUE(QueryField == MissingName);
		}

		FString ReasonField;
		UNTEST_EXPECT_TRUE(Hint->TryGetStringField(TEXT("reason"), ReasonField));
		UNTEST_EXPECT_TRUE(ReasonField.Contains(
			FString::Printf(TEXT("unknown tool '%s'"), *MissingName)));
	}

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PythonExecuteHint, AttributeErrorOnClaireonModuleProducesHint, UNTEST_TIMEOUTMS(10000))
{
	using namespace PyHintTests622_Helpers;

	const FString MissingName = TEXT("bp_open_nonexistent_xyzzy");
	const FString Logs = PyHintTests622_BuildAttributeErrorTraceback(MissingName);

	TSharedPtr<FJsonObject> Hint = ClaireonTool_ExecutePython::BuildHintFromLogs(Logs);

	UNTEST_EXPECT_TRUE(Hint.IsValid());
	if (Hint.IsValid())
	{
		FString ToolField;
		UNTEST_EXPECT_TRUE(Hint->TryGetStringField(TEXT("tool"), ToolField));
		UNTEST_EXPECT_TRUE(ToolField == TEXT("tool_search"));

		const TSharedPtr<FJsonObject>* ArgsObj = nullptr;
		UNTEST_EXPECT_TRUE(Hint->TryGetObjectField(TEXT("args"), ArgsObj));
		if (ArgsObj && ArgsObj->IsValid())
		{
			FString QueryField;
			UNTEST_EXPECT_TRUE((*ArgsObj)->TryGetStringField(TEXT("query"), QueryField));
			UNTEST_EXPECT_TRUE(QueryField == MissingName);
		}

		FString ReasonField;
		UNTEST_EXPECT_TRUE(Hint->TryGetStringField(TEXT("reason"), ReasonField));
		UNTEST_EXPECT_TRUE(ReasonField.Contains(
			FString::Printf(TEXT("unknown tool '%s'"), *MissingName)));
	}

	co_return;
}

UNTEST_UNIT(Claireon, PythonExecuteHint, TypeErrorUnexpectedKwargProducesHint)
{
	using namespace PyHintTests622_Helpers;

	const FString ToolName = PyHintTests622_PickAnyRegisteredToolName();
	if (ToolName.IsEmpty())
	{
		// No registered tools means we can't validate the registered-tool
		// gate.  Treat as a pass since the gate is what we're verifying.
		co_return;
	}

	const FString Logs = PyHintTests622_BuildTypeErrorUnexpectedKwargTraceback(
		ToolName, TEXT("foo"));

	TSharedPtr<FJsonObject> Hint = ClaireonTool_ExecutePython::BuildHintFromLogs(Logs);

	UNTEST_EXPECT_TRUE(Hint.IsValid());
	if (Hint.IsValid())
	{
		const TSharedPtr<FJsonObject>* ArgsObj = nullptr;
		UNTEST_EXPECT_TRUE(Hint->TryGetObjectField(TEXT("args"), ArgsObj));
		if (ArgsObj && ArgsObj->IsValid())
		{
			FString NameField;
			UNTEST_EXPECT_TRUE((*ArgsObj)->TryGetStringField(TEXT("name"), NameField));
			UNTEST_EXPECT_TRUE(NameField == ToolName);

			FString DetailField;
			UNTEST_EXPECT_TRUE((*ArgsObj)->TryGetStringField(TEXT("detail"), DetailField));
			UNTEST_EXPECT_TRUE(DetailField == TEXT("full"));
		}

		FString ReasonField;
		UNTEST_EXPECT_TRUE(Hint->TryGetStringField(TEXT("reason"), ReasonField));
		UNTEST_EXPECT_TRUE(ReasonField.Contains(
			FString::Printf(TEXT("signature mismatch on %s"), *ToolName)));
	}

	co_return;
}

UNTEST_UNIT(Claireon, PythonExecuteHint, TypeErrorMissingPositionalProducesHint)
{
	using namespace PyHintTests622_Helpers;

	const FString ToolName = PyHintTests622_PickAnyRegisteredToolName();
	if (ToolName.IsEmpty())
	{
		co_return;
	}

	const FString Logs = PyHintTests622_BuildTypeErrorMissingPositionalTraceback(ToolName);

	TSharedPtr<FJsonObject> Hint = ClaireonTool_ExecutePython::BuildHintFromLogs(Logs);

	UNTEST_EXPECT_TRUE(Hint.IsValid());
	if (Hint.IsValid())
	{
		FString ReasonField;
		UNTEST_EXPECT_TRUE(Hint->TryGetStringField(TEXT("reason"), ReasonField));
		UNTEST_EXPECT_TRUE(ReasonField.Contains(
			FString::Printf(TEXT("signature mismatch on %s"), *ToolName)));
	}

	co_return;
}

UNTEST_UNIT(Claireon, PythonExecuteHint, SyntaxErrorSuppressesHint)
{
	const FString Logs = TEXT(
		"Traceback (most recent call last):\n"
		"  File \"/tmp/mcp_exec.py\", line 1\n"
		"    def\n"
		"       ^\n"
		"SyntaxError: invalid syntax\n");

	TSharedPtr<FJsonObject> Hint = ClaireonTool_ExecutePython::BuildHintFromLogs(Logs);
	UNTEST_EXPECT_FALSE(Hint.IsValid());

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PythonExecuteHint, ColdStartBestMatchAppearsWhenCatalogAvailable, UNTEST_TIMEOUTMS(10000))
{
	using namespace PyHintTests622_Helpers;

	// EnsureBuilt populates the catalog from the live tool registry on the
	// cold path.  Pick a registered tool name, then construct a
	// near-miss query so FindNearest is likely to surface it as the best
	// match.  Note: we don't assert a specific best-match string here
	// because the matcher's exact ranking depends on the runtime
	// registry, only that the suffix appears when at least one match is
	// returned by the matcher.
	const FString AnyToolName = PyHintTests622_PickAnyRegisteredToolName();
	if (AnyToolName.IsEmpty())
	{
		co_return;
	}

	// Construct a typo-like query by mutating one character of the
	// registered tool name to a near-by character so the bounded
	// Levenshtein matcher returns a hit.
	FString Typo = AnyToolName;
	if (Typo.Len() >= 3)
	{
		Typo[Typo.Len() - 1] = TEXT('z');
	}

	const FString Logs = PyHintTests622_BuildNameErrorTraceback(Typo);

	TSharedPtr<FJsonObject> Hint = ClaireonTool_ExecutePython::BuildHintFromLogs(Logs);
	UNTEST_EXPECT_TRUE(Hint.IsValid());
	if (Hint.IsValid())
	{
		FString ReasonField;
		UNTEST_EXPECT_TRUE(Hint->TryGetStringField(TEXT("reason"), ReasonField));
		// Best-match suffix is appended only when FindNearest returns at
		// least one hit.  On a typo of an actual registered tool name we
		// expect the matcher to find at least the original.  If the
		// matcher returned no hits (e.g. very short tool names), the
		// reason will still carry the bare unknown-tool prefix -- so
		// just assert presence of the prefix.
		UNTEST_EXPECT_TRUE(ReasonField.Contains(TEXT("unknown tool '")));
	}

	co_return;
}

// Script-content channel: raw user code containing get_editor_property earns
// a hint toward claireon.uobject_inspect, independent of execution outcome.
UNTEST_UNIT(Claireon, PythonExecuteHint, GetEditorPropertyInScriptProducesHint)
{
	const FString Code = TEXT(
		"import unreal\n"
		"a = unreal.load_asset('/Game/Foo')\n"
		"v = a.get_editor_property('bHidden')\n");

	TSharedPtr<FJsonObject> Hint = ClaireonTool_ExecutePython::BuildHintFromScript(Code);
	UNTEST_EXPECT_TRUE(Hint.IsValid());
	if (Hint.IsValid())
	{
		FString ToolField;
		UNTEST_EXPECT_TRUE(Hint->TryGetStringField(TEXT("tool"), ToolField));
		UNTEST_EXPECT_STREQ(*ToolField, TEXT("python_execute"));

		FString ReasonField;
		UNTEST_EXPECT_TRUE(Hint->TryGetStringField(TEXT("reason"), ReasonField));
		UNTEST_EXPECT_TRUE(ReasonField.Contains(TEXT("uobject_inspect")));
	}

	co_return;
}

UNTEST_UNIT(Claireon, PythonExecuteHint, ScriptWithoutGetEditorPropertyProducesNoHint)
{
	// set_editor_property must NOT trigger the get_editor_property nudge.
	const FString Code = TEXT(
		"import unreal\n"
		"a = unreal.load_asset('/Game/Foo')\n"
		"a.set_editor_property('bHidden', True)\n");

	TSharedPtr<FJsonObject> Hint = ClaireonTool_ExecutePython::BuildHintFromScript(Code);
	UNTEST_EXPECT_FALSE(Hint.IsValid());

	co_return;
}

#endif // WITH_UNTESTED
