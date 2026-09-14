// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Headless selection-refusal tests first establish valid sessions and graphs.
// Successful widget and atomicity behavior is covered by the editor suite.

#if WITH_UNTESTED

#include "Untest.h"

#include "ClaireonBlueprintHelpers.h"
#include "ClaireonSessionManager.h"
#include "ClaireonTestAssetDeletion.h"
#include "Tools/ClaireonBlueprintGraphTool_Create.h"
#include "Tools/ClaireonBlueprintGraphTool_Open.h"
#include "Tools/ClaireonBlueprintGraphTool_Selection.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Misc/PackageName.h"
#include "UObject/SoftObjectPath.h"

namespace ClaireonSelectionRefusalTestsInternal
{
	// Prefix helpers to avoid unity-build collisions.

	static void SELR_Cleanup(const FString& AssetPath)
	{
		FClaireonSessionManager::Get().ReleaseByAssetPath(AssetPath);
		const FString ObjectPath = AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
		if (UObject* Asset = FSoftObjectPath(ObjectPath).TryLoad(); IsValid(Asset))
		{
			TArray<UObject*> ToDelete;
			ToDelete.Add(Asset);
			ClaireonTestAssetDeletion::DeleteObjectsForTest(ToDelete);
		}
	}

	static FString SELR_CreateAndOpen(const TCHAR* AssetPath)
	{
		{
			ClaireonBlueprintGraphTool_Create CreateTool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("asset_path"), AssetPath);
			Args->SetStringField(TEXT("parent_class"), TEXT("Actor"));
			if (CreateTool.Execute(Args).bIsError)
			{
				return FString();
			}
		}
		ClaireonBlueprintGraphTool_Open OpenTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		IClaireonTool::FToolResult R = OpenTool.Execute(Args);
		if (R.bIsError || !R.Data.IsValid())
		{
			return FString();
		}
		FString SessionId;
		R.Data->TryGetStringField(TEXT("session_id"), SessionId);
		return SessionId;
	}

	static FString SELR_Field(const IClaireonTool::FToolResult& R, const TCHAR* Field)
	{
		FString Value;
		if (R.Data.IsValid())
		{
			R.Data->TryGetStringField(Field, Value);
		}
		return Value;
	}

	static FString SELR_HintTool(const IClaireonTool::FToolResult& R)
	{
		FString Value;
		if (R.Hints.Num() > 0)
		{
			R.Hints[0]->TryGetStringField(TEXT("tool"), Value);
		}
		return Value;
	}
}

using namespace ClaireonSelectionRefusalTestsInternal;


// Selection tools report a consistent no-window reason and recovery.
UNTEST_UNIT_OPTS(Claireon, Selection, Selection_NoWindowRefusalNamesTheWayOut,
	UNTEST_TIMEOUTMS(120000))
{
	const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_SELR_NoWindow");
	SELR_Cleanup(AssetPath);

	const FString SessionId = SELR_CreateAndOpen(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	FBlueprintEditToolData* Data = ClaireonBlueprintGraphEditToolBase::FindToolData(SessionId);
	UNTEST_ASSERT_TRUE(Data != nullptr);
	UEdGraph* Graph = Data->Graph.Get();
	UNTEST_ASSERT_TRUE(IsValid(Graph));
	UNTEST_ASSERT_TRUE(Graph->Nodes.Num() > 0);
	UNTEST_ASSERT_FALSE(Data->EditorBinding.IsBound());

	const FString RealGuid = Graph->Nodes[0]->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);

	{
		ClaireonBlueprintGraphTool_SelectionGet Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		IClaireonTool::FToolResult R = Tool.Execute(Args);

		UNTEST_EXPECT_TRUE(R.bIsError);
		UNTEST_EXPECT_TRUE(SELR_Field(R, TEXT("refusal_reason")) == TEXT("no_editor_window"));
		UNTEST_EXPECT_TRUE(SELR_Field(R, TEXT("binding")) == TEXT("not_bound"));
		UNTEST_EXPECT_TRUE(SELR_HintTool(R) == TEXT("bp_open"));
		UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("Slate")));
	}

	// Use a valid node GUID to isolate the window refusal.
	{
		ClaireonBlueprintGraphTool_SelectionSet Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		TArray<TSharedPtr<FJsonValue>> Guids;
		Guids.Add(MakeShared<FJsonValueString>(RealGuid));
		Args->SetArrayField(TEXT("node_guids"), Guids);
		IClaireonTool::FToolResult R = Tool.Execute(Args);

		UNTEST_EXPECT_TRUE(R.bIsError);
		UNTEST_EXPECT_TRUE(SELR_Field(R, TEXT("refusal_reason")) == TEXT("no_editor_window"));
		UNTEST_EXPECT_TRUE(SELR_Field(R, TEXT("binding")) == TEXT("not_bound"));
		UNTEST_EXPECT_TRUE(SELR_HintTool(R) == TEXT("bp_open"));
	}

	{
		ClaireonBlueprintGraphTool_SelectionClear Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		IClaireonTool::FToolResult R = Tool.Execute(Args);

		UNTEST_EXPECT_TRUE(R.bIsError);
		UNTEST_EXPECT_TRUE(SELR_Field(R, TEXT("refusal_reason")) == TEXT("no_editor_window"));
		UNTEST_EXPECT_TRUE(SELR_HintTool(R) == TEXT("bp_open"));
	}

	SELR_Cleanup(AssetPath);
	co_return;
}


// Distinguish malformed arguments from unavailable windows.
UNTEST_UNIT_OPTS(Claireon, Selection, Selection_BadArgumentIsItsOwnRefusal,
	UNTEST_TIMEOUTMS(120000))
{
	const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_SELR_BadArg");
	SELR_Cleanup(AssetPath);

	const FString SessionId = SELR_CreateAndOpen(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	{
		ClaireonBlueprintGraphTool_SelectionSet Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		IClaireonTool::FToolResult R = Tool.Execute(Args);

		UNTEST_EXPECT_TRUE(R.bIsError);
		UNTEST_EXPECT_TRUE(SELR_Field(R, TEXT("refusal_reason")) == TEXT("bad_argument"));
		UNTEST_EXPECT_FALSE(SELR_Field(R, TEXT("refusal_reason")) == TEXT("no_editor_window"));
	}

	// Use a non-string node_guids entry to exercise argument validation.
	{
		ClaireonBlueprintGraphTool_SelectionSet Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		TArray<TSharedPtr<FJsonValue>> Guids;
		Guids.Add(MakeShared<FJsonValueNumber>(7));
		Args->SetArrayField(TEXT("node_guids"), Guids);
		IClaireonTool::FToolResult R = Tool.Execute(Args);

		UNTEST_EXPECT_TRUE(R.bIsError);
		// A missing window may take precedence; its reason must remain distinct from bad_argument.
		const FString Reason = SELR_Field(R, TEXT("refusal_reason"));
		UNTEST_EXPECT_TRUE(Reason == TEXT("no_editor_window") || Reason == TEXT("bad_argument"));
	}

	SELR_Cleanup(AssetPath);
	co_return;
}


// Refusals preserve the session and cursor.
UNTEST_UNIT_OPTS(Claireon, Selection, Selection_RefusalLeavesTheSessionUsable,
	UNTEST_TIMEOUTMS(120000))
{
	const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_SELR_Survives");
	SELR_Cleanup(AssetPath);

	const FString SessionId = SELR_CreateAndOpen(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	FBlueprintEditToolData* Data = ClaireonBlueprintGraphEditToolBase::FindToolData(SessionId);
	UNTEST_ASSERT_TRUE(Data != nullptr);
	UEdGraph* Graph = Data->Graph.Get();
	UNTEST_ASSERT_TRUE(IsValid(Graph));
	const int32 NodeCountBefore = Graph->Nodes.Num();
	const FGuid CursorBefore = Data->Cursor.FocusedNodeGuid;

	{
		ClaireonBlueprintGraphTool_SelectionSet Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		TArray<TSharedPtr<FJsonValue>> Guids;
		Guids.Add(MakeShared<FJsonValueString>(TEXT("DEADBEEFDEADBEEFDEADBEEFDEADBEEF")));
		Args->SetArrayField(TEXT("node_guids"), Guids);
		UNTEST_EXPECT_TRUE(Tool.Execute(Args).bIsError);
	}

	FBlueprintEditToolData* After = ClaireonBlueprintGraphEditToolBase::FindToolData(SessionId);
	UNTEST_ASSERT_TRUE(After != nullptr);
	UNTEST_EXPECT_TRUE(After->Graph.Get() == Graph);
	UNTEST_EXPECT_TRUE(Graph->Nodes.Num() == NodeCountBefore);
	UNTEST_EXPECT_TRUE(After->Cursor.FocusedNodeGuid == CursorBefore);

	SELR_Cleanup(AssetPath);
	co_return;
}

#endif // WITH_UNTESTED
