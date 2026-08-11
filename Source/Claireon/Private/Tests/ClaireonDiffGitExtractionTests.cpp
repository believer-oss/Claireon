// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
//
// P1-3: bp_diff / asset_diff git-revision extraction returned silent wrong
// answers for `^` revisions.
//
// The revision was interpolated UNQUOTED into a cmd.exe command string, and `^`
// is cmd's escape character. `git show HEAD^:<path>` therefore ran as
// `git show HEAD:<path>`: bp_diff(revision_a='HEAD^') compared HEAD against
// HEAD and reported "no differences". Nothing errored.
//
// The oracle used here is `rev-parse`, not file bytes: HEAD^ always resolves to
// a different commit than HEAD, whereas a file's bytes may legitimately be
// identical across two revisions, which would make a bytes-based test pass for
// the wrong reason.
//
// These tests require a git checkout with at least two commits, which every
// working copy of this repo has.

#if WITH_UNTESTED

#include "Untest.h"

#include "Tools/ClaireonDiffHelpers.h"
#include "Misc/Paths.h"

// File-local namespace (NOT raw `namespace { ... }`) to avoid unity-batched
// symbol collisions across other Tests TUs.
namespace ClaireonDiffGitExtractionTests
{
FString RunRevParse(const FString& Revision, bool& bOutSucceeded)
{
	const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const TArray<FString> Args = { TEXT("-C"), ProjectDir, TEXT("rev-parse"), Revision };

	const ClaireonDiffHelpers::FGitCommandResult Result =
		ClaireonDiffHelpers::RunGitCommand(Args, nullptr, 30.0);

	bOutSucceeded = Result.Succeeded();
	if (!bOutSucceeded)
	{
		return FString();
	}

	TArray<uint8> Bytes = Result.StdOut;
	Bytes.Add(0);
	FString Out = FString(UTF8_TO_TCHAR(reinterpret_cast<const char*>(Bytes.GetData())));
	Out.TrimStartAndEndInline();
	return Out;
}

bool LooksLikeSha(const FString& Value)
{
	if (Value.Len() != 40)
	{
		return false;
	}
	for (const TCHAR Ch : Value)
	{
		if (!FChar::IsHexDigit(Ch))
		{
			return false;
		}
	}
	return true;
}
} // namespace ClaireonDiffGitExtractionTests

// ---------------------------------------------------------------------------
// The regression: `^` reaches git instead of being eaten by a shell.
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, DiffGitExtraction, CaretRevisionResolvesToTheParent, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonDiffGitExtractionTests;

	bool bHeadOk = false;
	const FString Head = RunRevParse(TEXT("HEAD"), bHeadOk);
	UNTEST_ASSERT_TRUE(bHeadOk);
	UNTEST_ASSERT_TRUE(LooksLikeSha(Head));

	bool bParentOk = false;
	const FString Parent = RunRevParse(TEXT("HEAD^"), bParentOk);
	UNTEST_ASSERT_TRUE(bParentOk);
	UNTEST_ASSERT_TRUE(LooksLikeSha(Parent));

	// This is the whole bug: through cmd.exe these two came back equal.
	UNTEST_ASSERT_TRUE(Parent != Head);
	co_return;
}

// ---------------------------------------------------------------------------
// `HEAD^{commit}` used to degrade to a hard "Needed a single revision" failure,
// because cmd.exe ate the caret and then split on the braces.
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, DiffGitExtraction, PeelSyntaxRevisionSucceeds, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonDiffGitExtractionTests;

	bool bHeadOk = false;
	const FString Head = RunRevParse(TEXT("HEAD"), bHeadOk);
	UNTEST_ASSERT_TRUE(bHeadOk);

	bool bPeeledOk = false;
	const FString Peeled = RunRevParse(TEXT("HEAD^{commit}"), bPeeledOk);
	UNTEST_ASSERT_TRUE(bPeeledOk);
	UNTEST_ASSERT_STREQ(*Peeled, *Head);
	co_return;
}

// ---------------------------------------------------------------------------
// One argv entry per Args entry: a value containing a space arrives whole.
// `rev-parse --sq-quote` echoes its arguments back, shell-quoted, which makes
// argument boundaries directly observable.
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, DiffGitExtraction, ArgumentWithASpaceSurvivesIntact, UNTEST_TIMEOUTMS(60000))
{
	const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const TArray<FString> Args = {
		TEXT("-C"), ProjectDir, TEXT("rev-parse"), TEXT("--sq-quote"), TEXT("Content/With Space.uasset")
	};

	const ClaireonDiffHelpers::FGitCommandResult Result =
		ClaireonDiffHelpers::RunGitCommand(Args, nullptr, 30.0);
	UNTEST_ASSERT_TRUE(Result.Succeeded());

	TArray<uint8> Bytes = Result.StdOut;
	Bytes.Add(0);
	const FString Echoed = FString(UTF8_TO_TCHAR(reinterpret_cast<const char*>(Bytes.GetData())));

	// Both halves in one quoted token means git saw ONE argument, not two.
	UNTEST_ASSERT_TRUE(Echoed.Contains(TEXT("'Content/With Space.uasset'")));
	co_return;
}

// ---------------------------------------------------------------------------
// A bad revision must still be classified as a bad REVISION, not reported as a
// missing asset or as raw git text.
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, DiffGitExtraction, BadRevisionIsClassifiedAsBadRevision, UNTEST_TIMEOUTMS(60000))
{
	FString Error;
	const FString TempFile = ClaireonDiffHelpers::ExtractAssetFromGitRevision(
		TEXT("Content/DoesNotMatter.uasset"), TEXT("no-such-rev-claireon"), Error);

	UNTEST_ASSERT_TRUE(TempFile.IsEmpty());
	UNTEST_ASSERT_TRUE(Error.Contains(TEXT("Bad git revision")));
	// The message must name the revision the caller passed.
	UNTEST_ASSERT_TRUE(Error.Contains(TEXT("no-such-rev-claireon")));
	co_return;
}

// ---------------------------------------------------------------------------
// SanitizeRevision keeps rejecting what it always rejected. `^` stays ALLOWED:
// the fix is that it now reaches git, not that it is forbidden.
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, DiffGitExtraction, SanitizeRevisionStillAllowsCaret, UNTEST_TIMEOUTMS(5000))
{
	FString Error;
	UNTEST_ASSERT_TRUE(ClaireonDiffHelpers::SanitizeRevision(TEXT("HEAD^"), Error));
	UNTEST_ASSERT_TRUE(ClaireonDiffHelpers::SanitizeRevision(TEXT("HEAD~2"), Error));

	Error.Empty();
	UNTEST_ASSERT_FALSE(ClaireonDiffHelpers::SanitizeRevision(TEXT("HEAD; rm -rf /"), Error));
	UNTEST_ASSERT_FALSE(Error.IsEmpty());
	co_return;
}

// ---------------------------------------------------------------------------
// Path conversion: the object-name suffix is stripped and /Game maps to Content.
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, DiffGitExtraction, PathConversionStripsObjectName, UNTEST_TIMEOUTMS(5000))
{
	const FString FromPackage = ClaireonDiffHelpers::ConvertAssetPathToGitRelativePath(TEXT("/Game/Claireon/NoSuchAsset"));
	const FString FromObject = ClaireonDiffHelpers::ConvertAssetPathToGitRelativePath(TEXT("/Game/Claireon/NoSuchAsset.NoSuchAsset"));

	// The object form and the package form must name the same file, or half the
	// callers silently look up a path that cannot exist.
	UNTEST_ASSERT_STREQ(*FromObject, *FromPackage);
	UNTEST_ASSERT_TRUE(FromPackage.StartsWith(TEXT("Content/")));
	UNTEST_ASSERT_TRUE(FromPackage.EndsWith(TEXT(".uasset")));
	co_return;
}

#endif // WITH_UNTESTED
