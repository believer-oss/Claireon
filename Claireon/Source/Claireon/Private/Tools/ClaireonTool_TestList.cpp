// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_TestList.h"
#include "ClaireonLog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"

FString ClaireonTool_TestList::GetCategory() const { return TEXT("test"); }
FString ClaireonTool_TestList::GetOperation() const { return TEXT("list"); }

FString ClaireonTool_TestList::GetDescription() const
{
	return TEXT("List available automation tests registered in the editor");
}

TSharedPtr<FJsonObject> ClaireonTool_TestList::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// testFilter (optional)
	TSharedPtr<FJsonObject> FilterProp = MakeShared<FJsonObject>();
	FilterProp->SetStringField(TEXT("type"), TEXT("string"));
	FilterProp->SetStringField(TEXT("description"),
		TEXT("Filter pattern to narrow the list of tests (e.g. 'Combat', 'Ability')"));
	Properties->SetObjectField(TEXT("testFilter"), FilterProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_TestList::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	// Parse optional arguments
	FString TestFilter;
	if (Arguments.IsValid())
	{
		Arguments->TryGetStringField(TEXT("testFilter"), TestFilter);
	}

	UE_LOG(LogClaireon, Display,
		TEXT("[MCP] editor.test.list: testFilter='%s'"), *TestFilter);

	// Get all valid tests from the local automation framework
	TArray<FAutomationTestInfo> AllTestInfos;
	FAutomationTestFramework::Get().GetValidTestNames(AllTestInfos);

	// Filter and collect test names
	TArray<FString> MatchingTests;
	for (const FAutomationTestInfo& TestInfo : AllTestInfos)
	{
		const FString& DisplayName = TestInfo.GetDisplayName();
		const FString& FullPath = TestInfo.GetFullTestPath();

		if (!TestFilter.IsEmpty())
		{
			// Match against both display name and full test path
			if (!DisplayName.Contains(TestFilter) && !FullPath.Contains(TestFilter))
			{
				continue;
			}
		}

		MatchingTests.Add(FullPath);
	}

	// Build result
	FString Result;
	Result += TEXT("Automation Test List\n");
	if (!TestFilter.IsEmpty())
	{
		Result += FString::Printf(TEXT("Filter: %s\n"), *TestFilter);
	}
	Result += FString::Printf(TEXT("Total registered tests: %d\n"), AllTestInfos.Num());
	Result += FString::Printf(TEXT("Matching tests: %d\n"), MatchingTests.Num());
	Result += TEXT("\n");

	if (MatchingTests.Num() == 0)
	{
		Result += TEXT("No tests found matching the specified criteria.\n");
	}
	else
	{
		for (int32 i = 0; i < MatchingTests.Num(); i++)
		{
			Result += FString::Printf(TEXT("%d. %s\n"), i + 1, *MatchingTests[i]);
		}
	}

	return MakeSuccessResult(nullptr, Result);
}
