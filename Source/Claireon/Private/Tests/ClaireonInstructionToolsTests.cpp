// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Compare instruction-tool text against the server accessors used by MCP.
// Initialize the test server explicitly in commandlets; unavailable registries must fail.

#if WITH_UNTESTED

#include "Untest.h"

#include "ClaireonModule.h"
#include "ClaireonServer.h"
#include "Tools/ClaireonTool_InstructionsList.h"
#include "Tools/ClaireonTool_InstructionsRead.h"
#include "Tools/IClaireonTool.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace ClaireonInstructionToolsTestsNS
{
	// Prefix helpers to avoid unity-build collisions.

	// Write probes in the loader's instruction directory and remove them on every exit.

	static const TCHAR* InstrProbe_Topic    = TEXT("__untest_instruction_probe");
	static const TCHAR* InstrProbe_FileName = TEXT("__untest_instruction_probe.md");
	static const TCHAR* InstrProbe_Uri      = TEXT("claireon://instructions/__untest_instruction_probe");

	static FString InstrProbe_Path()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Claireon"));
		if (!Plugin.IsValid())
		{
			return FString();
		}
		return FPaths::Combine(Plugin->GetBaseDir(), TEXT("Content"), TEXT("MCP"),
			TEXT("Instructions"), InstrProbe_FileName);
	}

	/** A resource instruction doc whose body is Body. */
	static FString InstrProbe_ValidDoc(const FString& Body)
	{
		return FString::Printf(
			TEXT("---\n")
			TEXT("name: untest-instruction-probe\n")
			TEXT("description: Transient probe written by ClaireonInstructionToolsTests.\n")
			TEXT("type: resource\n")
			TEXT("uri: %s\n")
			TEXT("---\n")
			TEXT("\n%s\n"),
			InstrProbe_Uri, *Body);
	}

	struct FInstructionProbeScope
	{
		FInstructionProbeScope() { Remove(); }
		~FInstructionProbeScope() { Remove(); }

		static void Remove()
		{
			const FString Path = InstrProbe_Path();
			if (!Path.IsEmpty() && IFileManager::Get().FileExists(*Path))
			{
				IFileManager::Get().Delete(*Path, /*bRequireExists=*/false, /*bEvenReadOnly=*/true);
			}
		}

		static bool Write(const FString& Contents)
		{
			const FString Path = InstrProbe_Path();
			return !Path.IsEmpty() && FFileHelper::SaveStringToFile(Contents, *Path);
		}
	};

	/** Require known topics by name while allowing additions. */
	static const TCHAR* InstrTools_ExpectedTopics[] = {
		TEXT("architecture-viz"),
		TEXT("begin-work"),
		TEXT("blueprint-authoring"),
		TEXT("push-branch"),
		TEXT("refine-proposal"),
		TEXT("sequencing"),
		TEXT("workflow"),
	};

	/** One row of instructions_list, flattened for assertion. */
	struct FInstrRow
	{
		FString Topic;
		FString Title;
		FString Summary;
		FString Kind;
		FString Uri;
		FString PromptName;
	};

	static IClaireonTool::FToolResult InstrTools_List()
	{
		ClaireonTool_InstructionsList Tool;
		return Tool.Execute(MakeShared<FJsonObject>());
	}

	static IClaireonTool::FToolResult InstrTools_Read(const FString& Topic)
	{
		ClaireonTool_InstructionsRead Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("topic"), Topic);
		return Tool.Execute(Args);
	}

	/** Parse instructions_list's payload into rows. False when the payload is malformed. */
	static bool InstrTools_ParseRows(const IClaireonTool::FToolResult& Result, TArray<FInstrRow>& OutRows)
	{
		OutRows.Reset();
		if (Result.bIsError || !Result.Data.IsValid())
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Topics = nullptr;
		if (!Result.Data->TryGetArrayField(TEXT("topics"), Topics) || !Topics)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Topics)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(Obj) || !Obj || !(*Obj).IsValid())
			{
				return false;
			}
			FInstrRow Row;
			// Require fields to exist; empty values remain valid.
			if (!(*Obj)->TryGetStringField(TEXT("topic"), Row.Topic)) { return false; }
			if (!(*Obj)->TryGetStringField(TEXT("title"), Row.Title)) { return false; }
			if (!(*Obj)->TryGetStringField(TEXT("summary"), Row.Summary)) { return false; }
			if (!(*Obj)->TryGetStringField(TEXT("kind"), Row.Kind)) { return false; }
			if (!(*Obj)->TryGetStringField(TEXT("uri"), Row.Uri)) { return false; }
			if (!(*Obj)->TryGetStringField(TEXT("prompt_name"), Row.PromptName)) { return false; }
			OutRows.Add(MoveTemp(Row));
		}
		return true;
	}

	static bool InstrTools_RowsContainTopic(const TArray<FInstrRow>& Rows, const FString& Topic)
	{
		return Rows.ContainsByPredicate([&Topic](const FInstrRow& Row) { return Row.Topic == Topic; });
	}
}

// Compare every listed topic with the server accessor.
UNTEST_UNIT_OPTS(Claireon, InstructionTools, ToolServedTextIsByteIdenticalToWireServedText, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonInstructionToolsTestsNS;

	FClaireonServer* Server = FClaireonModule::Get().EnsureServerForTest();
	UNTEST_ASSERT_TRUE(Server != nullptr);
	Server->ReloadMCPContent();

	const IClaireonTool::FToolResult ListResult = InstrTools_List();
	UNTEST_ASSERT_FALSE(ListResult.bIsError);

	TArray<FInstrRow> Rows;
	UNTEST_ASSERT_TRUE(InstrTools_ParseRows(ListResult, Rows));
	UNTEST_ASSERT_TRUE(Rows.Num() > 0);

	TArray<FString> Mismatches;
	for (const FInstrRow& Row : Rows)
	{
		const IClaireonTool::FToolResult ReadResult = InstrTools_Read(Row.Topic);
		if (ReadResult.bIsError || !ReadResult.Data.IsValid())
		{
			Mismatches.Add(FString::Printf(TEXT("[%s] instructions_read failed: %s"),
				*Row.Topic, *ReadResult.ErrorMessage));
			continue;
		}

		FString ToolText;
		if (!ReadResult.Data->TryGetStringField(TEXT("text"), ToolText))
		{
			Mismatches.Add(FString::Printf(TEXT("[%s] read payload has no `text` field"), *Row.Topic));
			continue;
		}

		FString WireText;
		const bool bWireServed = (Row.Kind == TEXT("resource"))
			? Server->TryGetResourceText(Row.Uri, WireText)
			: Server->TryGetPromptText(Row.PromptName, WireText);

		if (!bWireServed)
		{
			Mismatches.Add(FString::Printf(
				TEXT("[%s] listed as kind=%s but the wire accessor refused it (uri='%s', prompt_name='%s')"),
				*Row.Topic, *Row.Kind, *Row.Uri, *Row.PromptName));
			continue;
		}

		// Require exact case-sensitive text equality.
		if (!ToolText.Equals(WireText, ESearchCase::CaseSensitive))
		{
			Mismatches.Add(FString::Printf(
				TEXT("[%s] tool text (%d chars) differs from wire text (%d chars)"),
				*Row.Topic, ToolText.Len(), WireText.Len()));
			continue;
		}

		if (ToolText.IsEmpty())
		{
			Mismatches.Add(FString::Printf(TEXT("[%s] served text is empty"), *Row.Topic));
		}
	}

	if (Mismatches.Num() > 0)
	{
		UE_LOG(LogTemp, Error, TEXT("[InstructionTools] %d transport mismatch(es) across %d topic(s):"),
			Mismatches.Num(), Rows.Num());
		for (const FString& Msg : Mismatches)
		{
			UE_LOG(LogTemp, Error, TEXT("[InstructionTools]   %s"), *Msg);
		}
	}
	UNTEST_EXPECT_TRUE(Mismatches.Num() == 0);
	co_return;
}

// Require known topics and well-formed rows without fixing the total count.
UNTEST_UNIT_OPTS(Claireon, InstructionTools, ListEnumeratesEveryKnownTopicAndIsWellFormed, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonInstructionToolsTestsNS;

	FClaireonServer* Server = FClaireonModule::Get().EnsureServerForTest();
	UNTEST_ASSERT_TRUE(Server != nullptr);
	Server->ReloadMCPContent();

	TArray<FInstrRow> Rows;
	UNTEST_ASSERT_TRUE(InstrTools_ParseRows(InstrTools_List(), Rows));

	TArray<FString> Failures;

	for (const TCHAR* Expected : InstrTools_ExpectedTopics)
	{
		if (!InstrTools_RowsContainTopic(Rows, Expected))
		{
			Failures.Add(FString::Printf(TEXT("topic '%s' is no longer enumerated"), Expected));
		}
	}

	for (const FInstrRow& Row : Rows)
	{
		if (Row.Topic.IsEmpty()) { Failures.Add(TEXT("a row has an empty `topic`")); }
		if (Row.Title.IsEmpty()) { Failures.Add(FString::Printf(TEXT("[%s] empty `title`"), *Row.Topic)); }
		if (Row.Kind != TEXT("resource") && Row.Kind != TEXT("prompt"))
		{
			Failures.Add(FString::Printf(TEXT("[%s] `kind` is '%s', expected resource|prompt"),
				*Row.Topic, *Row.Kind));
		}
		const bool bHasUri = !Row.Uri.IsEmpty();
		const bool bHasPromptName = !Row.PromptName.IsEmpty();
		if (bHasUri == bHasPromptName)
		{
			Failures.Add(FString::Printf(
				TEXT("[%s] expected exactly one of uri/prompt_name populated (uri='%s', prompt_name='%s')"),
				*Row.Topic, *Row.Uri, *Row.PromptName));
		}
		if (Row.Kind == TEXT("resource") && !bHasUri)
		{
			Failures.Add(FString::Printf(TEXT("[%s] kind=resource with no uri"), *Row.Topic));
		}
		if (Row.Kind == TEXT("prompt") && !bHasPromptName)
		{
			Failures.Add(FString::Printf(TEXT("[%s] kind=prompt with no prompt_name"), *Row.Topic));
		}
		// Verify listed resource URIs resolve.
		if (Row.Kind == TEXT("resource"))
		{
			FString Ignored;
			if (!Server->TryGetResourceText(Row.Uri, Ignored))
			{
				Failures.Add(FString::Printf(TEXT("[%s] uri '%s' does not resolve"), *Row.Topic, *Row.Uri));
			}
		}
	}

	// workflow is a prompt and has no resource URI.
	const FInstrRow* WorkflowRow = Rows.FindByPredicate(
		[](const FInstrRow& Row) { return Row.Topic == TEXT("workflow"); });
	if (WorkflowRow)
	{
		if (WorkflowRow->Kind != TEXT("prompt"))
		{
			Failures.Add(FString::Printf(
				TEXT("[workflow] kind is '%s'; it is loaded from frontmatter type: prompt, so "
				     "prompt was expected. If the doc was deliberately retyped, update this "
				     "assertion and the byte-identity test's accessor selection together."),
				*WorkflowRow->Kind));
		}
	}

	if (Failures.Num() > 0)
	{
		UE_LOG(LogTemp, Error, TEXT("[InstructionTools] %d listing failure(s):"), Failures.Num());
		for (const FString& F : Failures)
		{
			UE_LOG(LogTemp, Error, TEXT("[InstructionTools]   %s"), *F);
		}
	}
	UNTEST_EXPECT_TRUE(Failures.Num() == 0);
	co_return;
}

// Use a probe to exercise placeholder substitution even if shipped docs contain no placeholders.
UNTEST_UNIT_OPTS(Claireon, InstructionTools, ReadSubstitutesPlaceholdersLikeTheWirePath, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonInstructionToolsTestsNS;

	FClaireonServer* Server = FClaireonModule::Get().EnsureServerForTest();
	UNTEST_ASSERT_TRUE(Server != nullptr);

	FInstructionProbeScope Probe;
	UNTEST_ASSERT_FALSE(InstrProbe_Path().IsEmpty());

	// Substitute a known runtime variable and preserve an unknown placeholder.
	UNTEST_ASSERT_TRUE(FInstructionProbeScope::Write(InstrProbe_ValidDoc(
		TEXT("PROJECT={{project.name}} UNKNOWN={{claireon.no.such.variable}}"))));
	Server->ReloadMCPContent();

	const IClaireonTool::FToolResult ReadResult = InstrTools_Read(InstrProbe_Topic);
	UNTEST_ASSERT_FALSE(ReadResult.bIsError);
	UNTEST_ASSERT_TRUE(ReadResult.Data.IsValid());

	FString ToolText;
	UNTEST_ASSERT_TRUE(ReadResult.Data->TryGetStringField(TEXT("text"), ToolText));

	UNTEST_EXPECT_FALSE(ToolText.Contains(TEXT("{{project.name}}")));
	UNTEST_EXPECT_TRUE(ToolText.Contains(TEXT("PROJECT=")));
	UNTEST_EXPECT_TRUE(ToolText.Contains(TEXT("{{claireon.no.such.variable}}")));

	FString WireText;
	UNTEST_ASSERT_TRUE(Server->TryGetResourceText(InstrProbe_Uri, WireText));
	UNTEST_EXPECT_TRUE(ToolText.Equals(WireText, ESearchCase::CaseSensitive));

	UNTEST_EXPECT_FALSE(ToolText.StartsWith(TEXT("---")));
	co_return;
}

// Errors must not return empty success.
UNTEST_UNIT_OPTS(Claireon, InstructionTools, ErrorPathsRefuseCleanlyAndNameWhatExists, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonInstructionToolsTestsNS;

	FClaireonServer* Server = FClaireonModule::Get().EnsureServerForTest();
	UNTEST_ASSERT_TRUE(Server != nullptr);
	Server->ReloadMCPContent();

	// Unknown-topic errors list available topics.
	{
		const IClaireonTool::FToolResult Result = InstrTools_Read(TEXT("nope-not-a-topic"));
		UNTEST_EXPECT_TRUE(Result.bIsError);
		UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("nope-not-a-topic")));
		UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("blueprint-authoring")));
	}

	{
		ClaireonTool_InstructionsRead Tool;
		const IClaireonTool::FToolResult Result = Tool.Execute(MakeShared<FJsonObject>());
		UNTEST_EXPECT_TRUE(Result.bIsError);
		UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("topic")));
	}

	{
		const IClaireonTool::FToolResult Result = InstrTools_Read(FString());
		UNTEST_EXPECT_TRUE(Result.bIsError);
	}

	{
		ClaireonTool_InstructionsList Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("topic"), TEXT("blueprint-authoring"));
		const IClaireonTool::FToolResult Result = Tool.Execute(Args);
		UNTEST_EXPECT_TRUE(Result.bIsError);
		UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("topic")));
	}
	{
		ClaireonTool_InstructionsRead Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("topic"), TEXT("blueprint-authoring"));
		Args->SetBoolField(TEXT("include_frontmatter"), true);
		const IClaireonTool::FToolResult Result = Tool.Execute(Args);
		UNTEST_EXPECT_TRUE(Result.bIsError);
		UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("include_frontmatter")));
	}
	co_return;
}

// Reload must update both tool and server-accessor output.
UNTEST_UNIT_OPTS(Claireon, InstructionTools, ReloadKeepsToolAndWireOnTheSameCopy, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonInstructionToolsTestsNS;

	FClaireonServer* Server = FClaireonModule::Get().EnsureServerForTest();
	UNTEST_ASSERT_TRUE(Server != nullptr);

	FInstructionProbeScope Probe;
	UNTEST_ASSERT_FALSE(InstrProbe_Path().IsEmpty());

	Server->ReloadMCPContent();
	UNTEST_EXPECT_TRUE(InstrTools_Read(InstrProbe_Topic).bIsError);

	UNTEST_ASSERT_TRUE(FInstructionProbeScope::Write(InstrProbe_ValidDoc(TEXT("BODY ONE"))));
	Server->ReloadMCPContent();

	TArray<FInstrRow> Rows;
	UNTEST_ASSERT_TRUE(InstrTools_ParseRows(InstrTools_List(), Rows));
	UNTEST_EXPECT_TRUE(InstrTools_RowsContainTopic(Rows, InstrProbe_Topic));

	const auto ToolTextFor = [](const FString& Topic, FString& OutText) -> bool
	{
		const IClaireonTool::FToolResult Result = InstrTools_Read(Topic);
		return !Result.bIsError && Result.Data.IsValid()
			&& Result.Data->TryGetStringField(TEXT("text"), OutText);
	};

	FString ToolText;
	FString WireText;
	UNTEST_ASSERT_TRUE(ToolTextFor(InstrProbe_Topic, ToolText));
	UNTEST_ASSERT_TRUE(Server->TryGetResourceText(InstrProbe_Uri, WireText));
	UNTEST_EXPECT_TRUE(ToolText.Contains(TEXT("BODY ONE")));
	UNTEST_EXPECT_TRUE(ToolText.Equals(WireText, ESearchCase::CaseSensitive));

	UNTEST_ASSERT_TRUE(FInstructionProbeScope::Write(InstrProbe_ValidDoc(TEXT("BODY TWO"))));
	Server->ReloadMCPContent();
	UNTEST_ASSERT_TRUE(ToolTextFor(InstrProbe_Topic, ToolText));
	UNTEST_ASSERT_TRUE(Server->TryGetResourceText(InstrProbe_Uri, WireText));
	UNTEST_EXPECT_TRUE(ToolText.Contains(TEXT("BODY TWO")));
	UNTEST_EXPECT_FALSE(ToolText.Contains(TEXT("BODY ONE")));
	UNTEST_EXPECT_TRUE(ToolText.Equals(WireText, ESearchCase::CaseSensitive));

	FInstructionProbeScope::Remove();
	Server->ReloadMCPContent();
	UNTEST_EXPECT_TRUE(InstrTools_Read(InstrProbe_Topic).bIsError);
	UNTEST_EXPECT_FALSE(Server->TryGetResourceText(InstrProbe_Uri, WireText));
	co_return;
}

// Listings must be deterministic regardless of filesystem scan order.
UNTEST_UNIT_OPTS(Claireon, InstructionTools, ListOrderIsStableAndSorted, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonInstructionToolsTestsNS;

	FClaireonServer* Server = FClaireonModule::Get().EnsureServerForTest();
	UNTEST_ASSERT_TRUE(Server != nullptr);
	Server->ReloadMCPContent();

	TArray<FInstrRow> First;
	TArray<FInstrRow> Second;
	UNTEST_ASSERT_TRUE(InstrTools_ParseRows(InstrTools_List(), First));
	UNTEST_ASSERT_TRUE(InstrTools_ParseRows(InstrTools_List(), Second));
	UNTEST_ASSERT_EQ(Second.Num(), First.Num());

	for (int32 Index = 0; Index < First.Num(); ++Index)
	{
		UNTEST_EXPECT_EQ(Second[Index].Topic, First[Index].Topic);
	}

	for (int32 Index = 1; Index < First.Num(); ++Index)
	{
		UNTEST_EXPECT_TRUE(First[Index - 1].Topic <= First[Index].Topic);
	}
	co_return;
}

// Check full descriptions and examples not covered by the registry-wide description sweep.
UNTEST_UNIT_OPTS(Claireon, InstructionTools, BothToolsMeetTheMetadataBar, UNTEST_TIMEOUTMS(20000))
{
	TArray<FString> Failures;

	const auto CheckTool = [&Failures](const IClaireonTool& Tool, const TCHAR* ExpectedName)
	{
		const FString Name = Tool.GetName();
		if (Name != ExpectedName)
		{
			Failures.Add(FString::Printf(TEXT("wire name is '%s', expected '%s'"), *Name, ExpectedName));
		}
		const FString StdDesc = Tool.GetDescription();
		if (StdDesc.Len() < 80 || StdDesc.Len() > 400)
		{
			Failures.Add(FString::Printf(TEXT("[%s] GetDescription is %d chars, expected [80,400]"),
				*Name, StdDesc.Len()));
		}
		const FString FullDesc = Tool.GetFullDescription();
		if (FullDesc.Len() < 200)
		{
			Failures.Add(FString::Printf(TEXT("[%s] GetFullDescription is %d chars, expected >= 200"),
				*Name, FullDesc.Len()));
		}
		if (FullDesc == StdDesc)
		{
			Failures.Add(FString::Printf(TEXT("[%s] GetFullDescription is identical to GetDescription"), *Name));
		}
		if (Tool.GetExampleUsage().IsEmpty())
		{
			Failures.Add(FString::Printf(TEXT("[%s] GetExampleUsage is empty"), *Name));
		}
		if (Tool.GetSearchKeywords().Num() < 3)
		{
			Failures.Add(FString::Printf(TEXT("[%s] GetSearchKeywords returns %d, expected >= 3"),
				*Name, Tool.GetSearchKeywords().Num()));
		}
		if (Tool.GetSessionMode() != EClaireonToolSessionMode::ReadOnly)
		{
			Failures.Add(FString::Printf(TEXT("[%s] GetSessionMode is not ReadOnly"), *Name));
		}
	};

	{
		ClaireonTool_InstructionsList Tool;
		CheckTool(Tool, TEXT("instructions_list"));
	}
	{
		ClaireonTool_InstructionsRead Tool;
		CheckTool(Tool, TEXT("instructions_read"));
	}

	if (Failures.Num() > 0)
	{
		UE_LOG(LogTemp, Error, TEXT("[InstructionTools] %d metadata failure(s):"), Failures.Num());
		for (const FString& F : Failures)
		{
			UE_LOG(LogTemp, Error, TEXT("[InstructionTools]   %s"), *F);
		}
	}
	UNTEST_EXPECT_TRUE(Failures.Num() == 0);
	co_return;
}

#endif // WITH_UNTESTED
