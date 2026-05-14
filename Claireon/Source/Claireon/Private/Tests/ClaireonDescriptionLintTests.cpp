// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Lint tests for tool descriptions across the four P5 categories
// (claireon.blueprint_graph_*, claireon.anim_*, claireon.statetree_*,
// claireon.widgetbp_*). Validates the P5 audit acceptance criteria:
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
#include "Tools/IClaireonTool.h"
#include "IClaireonToolProvider.h"
#include "Features/IModularFeatures.h"

namespace ClaireonDescriptionLintHelpers
{
	/**
	 * Walk the modular-feature provider list and collect all registered
	 * IClaireonTool instances. Mirrors the discovery pattern used by
	 * ClaireonApplySpecHelpTests.
	 */
	void CollectAllRegisteredTools(TArray<TSharedPtr<IClaireonTool>>& OutTools)
	{
		OutTools.Reset();
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
	 * Identify whether the tool name belongs to one of the four P5-scoped
	 * categories. Tools outside these prefixes are intentionally not
	 * subject to the audit (they remain free to use any phrasing).
	 */
	bool IsP5ScopedTool(const FString& ToolName)
	{
		return ToolName.StartsWith(TEXT("claireon.blueprint_graph_"))
			|| ToolName.StartsWith(TEXT("claireon.anim_"))
			|| ToolName.StartsWith(TEXT("claireon.blendspace_"))
			|| ToolName.StartsWith(TEXT("claireon.statetree_"))
			|| ToolName.StartsWith(TEXT("claireon.widgetbp_"));
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
			TEXT("Step")
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
UNTEST_UNIT(Claireon, DescriptionLint, AllP5CategoriesConformToTemplate)
{
	using namespace ClaireonDescriptionLintHelpers;

	TArray<TSharedPtr<IClaireonTool>> AllTools;
	CollectAllRegisteredTools(AllTools);

	// Commandlet test runners may execute before any IClaireonToolProvider has
	// registered, in which case the modular-feature list is empty. Treat this
	// as a skip-with-pass: the lint cannot run, but the absence of tool
	// registration is an environment property, not a description regression.
	if (AllTools.Num() == 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[DescriptionLint] No IClaireonToolProvider tools registered in this run; skipping lint."));
		UNTEST_EXPECT_TRUE(true);
		co_return;
	}

	int32 ScopedCount = 0;
	int32 LengthFailures = 0;
	int32 VerbOpenerFailures = 0;
	int32 KeywordFailures = 0;
	TArray<FString> FailureMessages;

	for (const TSharedPtr<IClaireonTool>& Tool : AllTools)
	{
		const FString Name = Tool->GetName();
		if (!IsP5ScopedTool(Name)) { continue; }
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
		UE_LOG(LogTemp, Error, TEXT("[DescriptionLint] %d offending tool(s) across %d P5-scoped tools:"),
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
	// ScopedCount may legitimately be 0 if the registered tools all live
	// outside the four P5-scoped prefixes (this is not a test failure).
	co_return;
}

#endif // WITH_UNTESTED
