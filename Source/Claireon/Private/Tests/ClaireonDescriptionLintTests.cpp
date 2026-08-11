// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Lint tests for tool descriptions across ALL registered Claireon tools.
// The scope is the full tool registry so future tool additions that drift
// from the bar are caught at test time. Validates the description acceptance
// criteria:
//
//   1. Length: 80 <= len(description) <= 400 characters.
//   2. Verb-opener: the first word is a recognized authoring verb (Add,
//      Remove, Set, Get, Move, Open, Close, Save, Compile, ...). Bare
//      adjectives like "Stateless" or noun-leading phrasings are rejected.
//   3. Session-model keyword: contains at least one substring (case-
//      insensitive) from {session, open, transactional, immediate,
//      read-only, non-session, stateless}. Stateless tools cover the
//      requirement via "stateless"/"non-session"/"read-only".
//
// On failure the test logs every offending tool/category in a single
// pass so future regressions surface as a batch instead of one at a time.

#if WITH_UNTESTED

#include "Untest.h"
#include "ClaireonModule.h"
#include "ClaireonServer.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Internationalization/Regex.h"
#include "Tools/IClaireonTool.h"
#include "IClaireonToolProvider.h"
#include "Features/IModularFeatures.h"
#include "SquidTasks/Task.h"

namespace ClaireonDescriptionLintHelpers
{
	/**
	 * Walk the modular-feature provider list and collect all registered
	 * IClaireonTool instances. Mirrors the discovery pattern used by
	 * ClaireonApplySpecHelpTests.
	 *
	 * FClaireonModule::StartupModule() early-returns under IsRunningCommandlet(),
	 * and Invoke-UntestTests.ps1 uses -run=UntestRunTests in BOTH of its modes,
	 * so the IClaireonToolProvider modular feature is NEVER registered by normal
	 * startup in any supported test run. Without the EnsureServerForTest() seam
	 * the provider list is empty unless some earlier test in the same process
	 * happened to call the seam first -- i.e. this lint was order-dependent and
	 * audited nothing on its own. The seam registers
	 * FClaireonBuiltinToolProvider unconditionally, so call it before reading
	 * the modular-feature list. StartServer() is NOT a substitute: it refuses to
	 * construct the registry when StartupModule() was skipped.
	 */
	void CollectAllRegisteredTools(TArray<TSharedPtr<IClaireonTool>>& OutTools)
	{
		OutTools.Reset();
		FClaireonModule::Get().EnsureServerForTest();
		TArray<IClaireonToolProvider*> Providers = IModularFeatures::Get()
			.GetModularFeatureImplementations<IClaireonToolProvider>(IClaireonToolProvider::FeatureName);
		for (IClaireonToolProvider* Provider : Providers)
		{
			if (!Provider) { continue; }
			for (const TSharedPtr<IClaireonTool>& Tool : Provider->GetTools())
			{
				if (Tool.IsValid())
				{
					OutTools.Add(Tool);
				}
			}
		}
	}

	/**
	 * Every registered Claireon tool must clear the description-quality bar.
	 * This helper is preserved as a deliberate no-op so callers can treat
	 * its result as "no tool is exempt". Add an exemption here only with
	 * a documented justification.
	 */
	bool IsP5ExemptTool(const FString& /*ToolName*/)
	{
		return false;
	}

	/** Return the first whitespace-delimited token in a string. */
	FString FirstWord(const FString& Sentence)
	{
		int32 SpaceIdx = INDEX_NONE;
		Sentence.FindChar(TEXT(' '), SpaceIdx);
		if (SpaceIdx == INDEX_NONE)
		{
			return Sentence;
		}
		return Sentence.Left(SpaceIdx);
	}

	/**
	 * Recognized verb-openers. Curated rather than open-ended so authors
	 * have to pick a verb the audit understands; expand this set as the
	 * tool surface grows.
	 */
	const TArray<FString>& GetVerbWhitelist()
	{
		static const TArray<FString> Verbs = {
			TEXT("Add"), TEXT("Remove"), TEXT("Set"), TEXT("Get"), TEXT("List"),
			TEXT("Move"), TEXT("Open"), TEXT("Close"), TEXT("Save"), TEXT("Load"),
			TEXT("Compile"), TEXT("Format"), TEXT("Inspect"), TEXT("Read"), TEXT("Return"),
			TEXT("Create"), TEXT("Delete"), TEXT("Duplicate"), TEXT("Rename"), TEXT("Reorder"),
			TEXT("Reparent"), TEXT("Reconstruct"), TEXT("Recombine"), TEXT("Reset"),
			TEXT("Replace"), TEXT("Send"), TEXT("Switch"), TEXT("Split"), TEXT("Connect"),
			TEXT("Disconnect"), TEXT("Apply"), TEXT("Revert"), TEXT("Modify"), TEXT("Edit"),
			TEXT("Bind"), TEXT("Configure"), TEXT("Designate"), TEXT("Diff"), TEXT("Enable"),
			TEXT("Disable"), TEXT("Enumerate"), TEXT("Export"), TEXT("Import"), TEXT("Find"),
			TEXT("Focus"), TEXT("Implement"), TEXT("Mark"), TEXT("Navigate"), TEXT("Pin"),
			TEXT("Query"), TEXT("Refresh"), TEXT("Register"), TEXT("Retime"), TEXT("Rotate"),
			TEXT("Run"), TEXT("Scan"), TEXT("Schedule"), TEXT("Select"), TEXT("Spawn"),
			TEXT("Start"), TEXT("Stop"), TEXT("Suggest"), TEXT("Test"), TEXT("Trigger"),
			TEXT("Update"), TEXT("Validate"), TEXT("Verify"), TEXT("Walk"), TEXT("Write"),
			TEXT("Check"), TEXT("Adjust"), TEXT("Author"), TEXT("Compare"),
			TEXT("Copy"), TEXT("Patch"), TEXT("Promote"), TEXT("Drag"), TEXT("Drop"),
			TEXT("Place"), TEXT("Show"), TEXT("Hide"), TEXT("Toggle"), TEXT("Batch"),
			TEXT("Step"),
			// Legitimate authoring verbs surfaced when the lint scope expanded
			// to all registered tools.
			TEXT("Snapshot"), TEXT("Capture"), TEXT("Wait"), TEXT("Begin"),
			TEXT("End"), TEXT("Poll"), TEXT("Cancel"), TEXT("Redo"), TEXT("Undo"),
			TEXT("Resave"), TEXT("Recompile"), TEXT("Wire"), TEXT("Append"),
			TEXT("Sculpt"), TEXT("Paint"), TEXT("Pop"), TEXT("Clear"),
			TEXT("Unregister"), TEXT("Retrieve"), TEXT("Look"), TEXT("Search"),
			TEXT("Execute"), TEXT("Request"), TEXT("Force"), TEXT("Assign")
		};
		return Verbs;
	}

	bool IsRecognizedVerb(const FString& Word)
	{
		const TArray<FString>& Verbs = GetVerbWhitelist();
		for (const FString& Verb : Verbs)
		{
			if (Word.Equals(Verb, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * Session-model keywords. Presence of any one (case-insensitive,
	 * substring match) is sufficient. Stateless tools satisfy via
	 * "stateless"/"non-session"/"read-only".
	 */
	bool ContainsSessionModelKeyword(const FString& Description)
	{
		static const TArray<FString> Keywords = {
			TEXT("session"),
			TEXT("open"),
			TEXT("transactional"),
			TEXT("immediate"),
			TEXT("read-only"),
			TEXT("non-session"),
			TEXT("stateless")
		};
		const FString Lower = Description.ToLower();
		for (const FString& Keyword : Keywords)
		{
			if (Lower.Contains(Keyword))
			{
				return true;
			}
		}
		return false;
	}
}

// ---------------------------------------------------------------------------
// 1: Length, verb-opener, and session-keyword lint across all P5 categories.
//
// Failure mode: log every offending tool, then assert at the end. This
// guarantees that adding/regressing many descriptions surfaces as a single
// readable batch report instead of one-at-a-time test failures.
// ---------------------------------------------------------------------------
// Budget: the bare UNTEST_UNIT default is 0.50ms (FUntestUnitFixture::DefaultTimeoutMs),
// which is not a deliberate perf assertion. This test sweeps the fully-populated
// ~717-tool registry, so give it real headroom instead of restoring the default.
UNTEST_UNIT_OPTS(Claireon, DescriptionLint, AllP5CategoriesConformToTemplate, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonDescriptionLintHelpers;

	TArray<TSharedPtr<IClaireonTool>> AllTools;
	CollectAllRegisteredTools(AllTools);

	// HARD FAILURE, not a skip. CollectAllRegisteredTools() calls
	// EnsureServerForTest(), which registers FClaireonBuiltinToolProvider
	// unconditionally, so an empty list now means the seam itself broke --
	// which would silently retire the ONLY automated check on every tool
	// description. The old form logged a warning and then did
	// UNTEST_EXPECT_TRUE(true) + co_return: Untest has no skip primitive, so the
	// runner scored that as a PASS. That assertion could not fail by
	// construction, and the "skip" branch was the normal path in commandlet
	// runs, meaning this lint reported green while auditing zero tools.
	UNTEST_ASSERT_TRUE(AllTools.Num() > 0);

	int32 ScopedCount = 0;
	int32 LengthFailures = 0;
	int32 VerbOpenerFailures = 0;
	int32 KeywordFailures = 0;
	TArray<FString> FailureMessages;

	for (const TSharedPtr<IClaireonTool>& Tool : AllTools)
	{
		const FString Name = Tool->GetName();
		if (IsP5ExemptTool(Name)) { continue; }
		++ScopedCount;

		const FString Description = Tool->GetDescription();
		const int32 Len = Description.Len();
		if (Len < 80 || Len > 400)
		{
			++LengthFailures;
			FailureMessages.Add(FString::Printf(TEXT("[len=%d] %s -- expected [80,400]"), Len, *Name));
		}

		const FString First = FirstWord(Description);
		if (First.IsEmpty() || !IsRecognizedVerb(First))
		{
			++VerbOpenerFailures;
			FailureMessages.Add(FString::Printf(TEXT("[verb='%s'] %s -- first word not in audit verb whitelist"), *First, *Name));
		}

		if (!ContainsSessionModelKeyword(Description))
		{
			++KeywordFailures;
			FailureMessages.Add(FString::Printf(TEXT("[no-session-keyword] %s"), *Name));
		}
	}

	if (FailureMessages.Num() > 0)
	{
		UE_LOG(LogTemp, Error, TEXT("[DescriptionLint] %d offending tool(s) across %d audited tools:"),
			FailureMessages.Num(), ScopedCount);
		for (const FString& Msg : FailureMessages)
		{
			UE_LOG(LogTemp, Error, TEXT("[DescriptionLint]   %s"), *Msg);
		}
		UE_LOG(LogTemp, Error,
			TEXT("[DescriptionLint] breakdown: length=%d, verb-opener=%d, session-keyword=%d"),
			LengthFailures, VerbOpenerFailures, KeywordFailures);
	}

	UNTEST_EXPECT_TRUE(FailureMessages.Num() == 0);
	co_return;
}

// ---------------------------------------------------------------------------
// GetPatterns() output lint sweep.
//
// Every tool that overrides GetPatterns() with a non-empty body must:
//   - return only ASCII code points (reject em/en dashes + NBSP).
//   - omit the project shibboleths "delightful", "thoughtful", "elegant".
// Empty returns (the default) are skipped.
// ---------------------------------------------------------------------------
// Budget: the bare UNTEST_UNIT default is 0.50ms (FUntestUnitFixture::DefaultTimeoutMs),
// which is not a deliberate perf assertion. This test sweeps the fully-populated
// registry (~727 tools) and calls GetPatterns() on each, so give it real headroom.
UNTEST_UNIT_OPTS(Claireon, DescriptionLint, AllGetPatternsAreAsciiAndShibbolethFree, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonDescriptionLintHelpers;

	TArray<TSharedPtr<IClaireonTool>> AllTools;
	CollectAllRegisteredTools(AllTools);

	// HARD FAILURE, not a skip -- see the note on the sibling test above. The
	// old UNTEST_EXPECT_TRUE(true) + co_return was scored as a PASS by the
	// runner (Untest has no skip primitive) and was the normal path in
	// commandlet runs, so this sweep reported green over zero tools.
	UNTEST_ASSERT_TRUE(AllTools.Num() > 0);

	TArray<FString> Failures;
	int32 EvaluatedCount = 0;
	for (const TSharedPtr<IClaireonTool>& Tool : AllTools)
	{
		if (!Tool.IsValid()) { continue; }
		const FString Patterns = Tool->GetPatterns();
		if (Patterns.IsEmpty()) { continue; }
		++EvaluatedCount;

		const FString Name = Tool->GetName();
		// ASCII-only sweep.
		for (int32 I = 0; I < Patterns.Len(); ++I)
		{
			const TCHAR C = Patterns[I];
			if (C == TCHAR(0x2013) || C == TCHAR(0x2014) || C == TCHAR(0x00A0))
			{
				Failures.Add(FString::Printf(
					TEXT("[%s] GetPatterns() contains non-ASCII code point U+%04X at index %d"),
					*Name, static_cast<uint32>(C), I));
				break;
			}
		}
		// Shibboleth sweep.
		const FString Lower = Patterns.ToLower();
		const TArray<FString> Shibboleths = {
			TEXT("delightful"), TEXT("thoughtful"), TEXT("elegant")
		};
		for (const FString& S : Shibboleths)
		{
			if (Lower.Contains(S))
			{
				Failures.Add(FString::Printf(
					TEXT("[%s] GetPatterns() contains shibboleth '%s'"), *Name, *S));
			}
		}
	}

	if (Failures.Num() > 0)
	{
		UE_LOG(LogTemp, Error,
			TEXT("[DescriptionLint] %d GetPatterns() lint failure(s) across %d evaluated tool(s):"),
			Failures.Num(), EvaluatedCount);
		for (const FString& F : Failures)
		{
			UE_LOG(LogTemp, Error, TEXT("[DescriptionLint]   %s"), *F);
		}
	}

	UNTEST_EXPECT_TRUE(Failures.Num() == 0);
	co_return;
}

// ---------------------------------------------------------------------------
// Retired dotted tool names must not appear in any description.
//
// Wire names are sealed as GetCategory() + "_" + GetOperation()
// (IClaireonTool.h), so `trace_open` is the real name and `editor.trace.open`
// is a name from a retired scheme. Ten trace-family schemas still told callers
// "The session ID returned by editor.trace.open" -- a name that resolves to
// nothing. An agent reading a description copies the name verbatim, so this is
// not cosmetic drift: it produces a call that cannot succeed.
//
// Scans tool descriptions AND input-schema property descriptions, because the
// worst offenders were per-parameter descriptions rather than tool ones.
//
// Deliberately NOT scanned: error strings and UE_LOG text. Several still carry
// retired names (ClaireonTool_Flythrough*, PIEAITargetInfo, PIECheckInitState,
// ClaireonServer.cpp) and cleaning those up is P2-19 scope, tracked separately.
// Widening this lint to them without doing that work would just make it fail.
// ---------------------------------------------------------------------------
UNTEST_UNIT_OPTS(Claireon, DescriptionLint, NoRetiredDottedToolNamesInDescriptions, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonDescriptionLintHelpers;

	TArray<TSharedPtr<IClaireonTool>> AllTools;
	CollectAllRegisteredTools(AllTools);

	UNTEST_ASSERT_TRUE(AllTools.Num() > 0);

	// Matches the retired `editor.<category>.<operation>` shape specifically,
	// not any dotted string -- prose legitimately contains "e.g." and file
	// names, and a lint that fires on those would be turned off rather than
	// obeyed.
	const FRegexPattern RetiredNamePattern(TEXT("editor\\.[a-zA-Z]+\\.[a-zA-Z]+"));

	TArray<FString> Failures;

	for (const TSharedPtr<IClaireonTool>& Tool : AllTools)
	{
		if (!Tool.IsValid()) { continue; }

		const FString Name = Tool->GetName();

		TArray<TPair<FString, FString>> TextsToScan;
		TextsToScan.Emplace(TEXT("GetDescription()"), Tool->GetDescription());

		const FString FullDescription = Tool->GetFullDescription();
		if (FullDescription != Tool->GetDescription())
		{
			TextsToScan.Emplace(TEXT("GetFullDescription()"), FullDescription);
		}

		// Per-parameter descriptions: where the trace family's drift actually lived.
		const TSharedPtr<FJsonObject> Schema = Tool->GetInputSchema();
		if (Schema.IsValid())
		{
			const TSharedPtr<FJsonObject>* Properties = nullptr;
			if (Schema->TryGetObjectField(TEXT("properties"), Properties) && Properties && Properties->IsValid())
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Properties)->Values)
				{
					const TSharedPtr<FJsonObject>* PropObj = nullptr;
					FString PropDescription;
					if (Pair.Value.IsValid() && Pair.Value->TryGetObject(PropObj) && PropObj
						&& (*PropObj)->TryGetStringField(TEXT("description"), PropDescription))
					{
						TextsToScan.Emplace(
							FString::Printf(TEXT("schema.properties.%s.description"), *Pair.Key),
							PropDescription);
					}
				}
			}
		}

		for (const TPair<FString, FString>& Entry : TextsToScan)
		{
			if (Entry.Value.IsEmpty()) { continue; }

			FRegexMatcher Matcher(RetiredNamePattern, Entry.Value);
			while (Matcher.FindNext())
			{
				Failures.Add(FString::Printf(
					TEXT("[%s] %s names retired tool '%s'; wire names are category_operation "
					     "(e.g. trace_open), so this name resolves to nothing"),
					*Name, *Entry.Key, *Matcher.GetCaptureGroup(0)));
			}
		}
	}

	if (Failures.Num() > 0)
	{
		UE_LOG(LogTemp, Error,
			TEXT("[DescriptionLint] %d retired dotted tool name(s) in descriptions:"), Failures.Num());
		for (const FString& F : Failures)
		{
			UE_LOG(LogTemp, Error, TEXT("[DescriptionLint]   %s"), *F);
		}
	}

	UNTEST_EXPECT_TRUE(Failures.Num() == 0);
	co_return;
}

#endif // WITH_UNTESTED
