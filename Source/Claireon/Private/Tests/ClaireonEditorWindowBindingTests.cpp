// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Headless tests for editor availability and binding refusals.
// Successful widget resolution and exact instance identity are covered by Claireon.BPEditor.Binding tests.

#if WITH_UNTESTED

#include "Untest.h"

#include "ClaireonBlueprintHelpers.h"
#include "ClaireonScopedAssetEditor.h"
#include "ClaireonSessionManager.h"
#include "ClaireonTestAssetDeletion.h"
#include "Tools/ClaireonBlueprintGraphTool_Create.h"
#include "Tools/ClaireonBlueprintGraphTool_Open.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

namespace ClaireonEditorWindowBindingTestsInternal
{
	// Prefix helpers to avoid unity-build collisions.

	static void EWB_Cleanup(const FString& AssetPath)
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

	/** Create a stock Actor Blueprint and open a session on it. Returns the session id. */
	static FString EWB_CreateAndOpen(const TCHAR* AssetPath)
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
}

using namespace ClaireonEditorWindowBindingTestsInternal;


// Report the missing availability precondition in a commandlet.
UNTEST_UNIT_OPTS(Claireon, EditorWindow, EditorWindow_GateNamesTheMissingPrecondition,
	UNTEST_TIMEOUTMS(60000))
{
	// Require the headless premise: Slate must be uninitialized.
	UNTEST_ASSERT_FALSE(FSlateApplication::IsInitialized());

	const EClaireonEditorAvailability Availability = ClaireonAssetEditorWindow::CheckAvailability();
	UNTEST_EXPECT_TRUE(Availability == EClaireonEditorAvailability::NoSlateApplication);
	UNTEST_EXPECT_TRUE(FString(ClaireonAssetEditorWindow::ToWireString(Availability))
		== TEXT("no_slate_application"));

	const FString Description = ClaireonAssetEditorWindow::DescribeMissingPrecondition(Availability);
	UNTEST_EXPECT_TRUE(Description.Contains(TEXT("Slate")));
	UNTEST_EXPECT_TRUE(ClaireonAssetEditorWindow::DescribeMissingPrecondition(
		EClaireonEditorAvailability::Available).IsEmpty());

	UNTEST_EXPECT_TRUE(FString(ClaireonAssetEditorWindow::ToWireString(
		EClaireonEditorWindowState::NotOpened)) == TEXT("not_opened"));
	UNTEST_EXPECT_TRUE(FString(ClaireonAssetEditorWindow::ToWireString(
		EClaireonEditorWindowState::Opened)) == TEXT("opened"));
	UNTEST_EXPECT_TRUE(FString(ClaireonAssetEditorWindow::ToWireString(
		EClaireonEditorWindowState::AlreadyOpen)) == TEXT("already_open"));

	co_return;
}


// Headless sessions remain usable without an editor binding.
UNTEST_UNIT_OPTS(Claireon, EditorWindow, EditorWindow_CommandletSessionOpensNoWindow,
	UNTEST_TIMEOUTMS(120000))
{
	const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_EWB_NoWindow");
	EWB_Cleanup(AssetPath);

	ClaireonBlueprintGraphTool_Create CreateTool;
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetStringField(TEXT("parent_class"), TEXT("Actor"));
		UNTEST_ASSERT_FALSE(CreateTool.Execute(Args).bIsError);
	}

	ClaireonBlueprintGraphTool_Open OpenTool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	IClaireonTool::FToolResult R = OpenTool.Execute(Args);

	UNTEST_ASSERT_FALSE(R.bIsError);
	UNTEST_ASSERT_TRUE(R.Data.IsValid());

	const TSharedPtr<FJsonObject>* Window = nullptr;
	UNTEST_ASSERT_TRUE(R.Data->TryGetObjectField(TEXT("editor_window"), Window));
	UNTEST_ASSERT_TRUE(Window && Window->IsValid());

	FString State;
	UNTEST_ASSERT_TRUE((*Window)->TryGetStringField(TEXT("state"), State));
	UNTEST_EXPECT_TRUE(State == TEXT("not_opened"));

	FString Reason;
	UNTEST_ASSERT_TRUE((*Window)->TryGetStringField(TEXT("reason"), Reason));
	UNTEST_EXPECT_TRUE(Reason.Contains(TEXT("Slate")));

	FString Binding;
	UNTEST_ASSERT_TRUE((*Window)->TryGetStringField(TEXT("binding"), Binding));
	UNTEST_EXPECT_TRUE(Binding == TEXT("not_bound"));

	FString SessionId;
	UNTEST_ASSERT_TRUE(R.Data->TryGetStringField(TEXT("session_id"), SessionId));
	FBlueprintEditToolData* Data = ClaireonBlueprintGraphEditToolBase::FindToolData(SessionId);
	UNTEST_ASSERT_TRUE(Data != nullptr);
	UNTEST_EXPECT_TRUE(Data->EditorWindow.State == EClaireonEditorWindowState::NotOpened);
	UNTEST_EXPECT_FALSE(Data->EditorBinding.IsBound());

	EWB_Cleanup(AssetPath);
	co_return;
}


// Refuse unavailable editor access and null assets separately.
UNTEST_UNIT_OPTS(Claireon, EditorWindow, EditorWindow_OpenForSessionRefusesCleanly,
	UNTEST_TIMEOUTMS(60000))
{
	const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_EWB_Refuse");
	EWB_Cleanup(AssetPath);

	const FString SessionId = EWB_CreateAndOpen(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	FBlueprintEditToolData* Data = ClaireonBlueprintGraphEditToolBase::FindToolData(SessionId);
	UNTEST_ASSERT_TRUE(Data != nullptr);
	UBlueprint* Blueprint = Data->Blueprint.Get();
	UNTEST_ASSERT_TRUE(IsValid(Blueprint));

	const FClaireonEditorOpenOutcome Outcome = ClaireonAssetEditorWindow::OpenForSession(Blueprint);
	UNTEST_EXPECT_TRUE(Outcome.State == EClaireonEditorWindowState::NotOpened);
	UNTEST_EXPECT_FALSE(Outcome.WasOpenedOrAlreadyOpen());
	UNTEST_EXPECT_TRUE(Outcome.Availability == EClaireonEditorAvailability::NoSlateApplication);
	UNTEST_EXPECT_FALSE(Outcome.Reason.IsEmpty());

	UNTEST_EXPECT_FALSE(ClaireonAssetEditorWindow::FindToolkitForAsset(Blueprint).IsValid());
	UNTEST_EXPECT_FALSE(ClaireonAssetEditorWindow::IsInstanceRegistered(Blueprint, nullptr));

	// A null asset does not imply editor unavailability.
	const FClaireonEditorOpenOutcome NullOutcome = ClaireonAssetEditorWindow::OpenForSession(nullptr);
	UNTEST_EXPECT_TRUE(NullOutcome.State == EClaireonEditorWindowState::NotOpened);
	UNTEST_EXPECT_TRUE(NullOutcome.Availability == EClaireonEditorAvailability::Available);
	UNTEST_EXPECT_TRUE(NullOutcome.Reason.Contains(TEXT("no asset")));

	{
		FScopedBlueprintEditor Scoped(Blueprint, /*bInSilent=*/false, /*bInCloseOnDestroy=*/true);
		UNTEST_EXPECT_FALSE(Scoped.IsValid());
		UNTEST_EXPECT_FALSE(Scoped.IsEditorOpen());
		UNTEST_EXPECT_FALSE(Scoped.WasAlreadyOpen());
		UNTEST_EXPECT_TRUE(Scoped.GetOpenOutcome().State == EClaireonEditorWindowState::NotOpened);
		UNTEST_EXPECT_TRUE(Scoped.GetGraphEditor(Data->Graph.Get()) == nullptr);
	}

	EWB_Cleanup(AssetPath);
	co_return;
}


// Remove one binding dependency at a time and verify distinct recovery reasons.
UNTEST_UNIT_OPTS(Claireon, EditorWindow, EditorWindow_EveryBindingRefusalIsDistinct,
	UNTEST_TIMEOUTMS(120000))
{
	const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_EWB_Binding");
	EWB_Cleanup(AssetPath);

	const FString SessionId = EWB_CreateAndOpen(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	FBlueprintEditToolData* Data = ClaireonBlueprintGraphEditToolBase::FindToolData(SessionId);
	UNTEST_ASSERT_TRUE(Data != nullptr);
	UBlueprint* Blueprint = Data->Blueprint.Get();
	UEdGraph* Graph = Data->Graph.Get();
	UNTEST_ASSERT_TRUE(IsValid(Blueprint));
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	{
		FClaireonBlueprintEditorBinding Unbound;
		UNTEST_EXPECT_FALSE(Unbound.IsBound());
		UNTEST_EXPECT_TRUE(Unbound.Revalidate(Graph) == EClaireonEditorBindingStatus::NotBound);
	}

	ClaireonBlueprintEditorBindingSeam::FScopedSeam Seam;

	{
		FClaireonBlueprintEditorBinding Binding;
		const FGuid InstanceId = ClaireonBlueprintEditorBindingSeam::RegisterInstance(Graph, nullptr);
		Binding.BindToSeamInstance(Blueprint, Graph, InstanceId);

		UNTEST_EXPECT_TRUE(Binding.IsBound());
		UNTEST_EXPECT_TRUE(ClaireonBlueprintEditorBindingSeam::IsInstanceRegistered(InstanceId));
		UNTEST_EXPECT_TRUE(Binding.Revalidate(Graph) == EClaireonEditorBindingStatus::GraphTabClosed);

		EClaireonEditorBindingStatus Status = EClaireonEditorBindingStatus::Valid;
		UNTEST_EXPECT_TRUE(Binding.ResolveGraphEditor(Graph, Status) == nullptr);
		UNTEST_EXPECT_TRUE(Status == EClaireonEditorBindingStatus::GraphTabClosed);
	}

	// Report graph removal before a missing tab.
	{
		FClaireonBlueprintEditorBinding Binding;
		const FGuid InstanceId = ClaireonBlueprintEditorBindingSeam::RegisterInstance(Graph, nullptr);
		Binding.BindToSeamInstance(Blueprint, Graph, InstanceId);
		UNTEST_EXPECT_TRUE(Binding.Revalidate(nullptr) == EClaireonEditorBindingStatus::GraphRemoved);
	}

	{
		FClaireonBlueprintEditorBinding Binding;
		const FGuid InstanceId = ClaireonBlueprintEditorBindingSeam::RegisterInstance(Graph, nullptr);
		Binding.BindToSeamInstance(Blueprint, Graph, InstanceId);
		UNTEST_ASSERT_TRUE(Binding.Revalidate(Graph) == EClaireonEditorBindingStatus::GraphTabClosed);

		ClaireonBlueprintEditorBindingSeam::CloseInstance(InstanceId);
		UNTEST_EXPECT_FALSE(ClaireonBlueprintEditorBindingSeam::IsInstanceRegistered(InstanceId));
		UNTEST_EXPECT_TRUE(Binding.Revalidate(Graph) == EClaireonEditorBindingStatus::BoundEditorClosed);

		// Register another candidate to catch unintended rebinding.
		const FGuid OtherId = ClaireonBlueprintEditorBindingSeam::RegisterInstance(Graph, nullptr);
		UNTEST_EXPECT_TRUE(ClaireonBlueprintEditorBindingSeam::IsInstanceRegistered(OtherId));
		UNTEST_EXPECT_TRUE(Binding.Revalidate(Graph) == EClaireonEditorBindingStatus::BoundEditorClosed);
	}

	{
		TSet<FString> Spellings;
		Spellings.Add(ClaireonEditorBindingStatusToWireString(EClaireonEditorBindingStatus::Valid));
		Spellings.Add(ClaireonEditorBindingStatusToWireString(EClaireonEditorBindingStatus::NotBound));
		Spellings.Add(ClaireonEditorBindingStatusToWireString(EClaireonEditorBindingStatus::BoundEditorClosed));
		Spellings.Add(ClaireonEditorBindingStatusToWireString(EClaireonEditorBindingStatus::GraphTabClosed));
		Spellings.Add(ClaireonEditorBindingStatusToWireString(EClaireonEditorBindingStatus::GraphRemoved));
		UNTEST_EXPECT_TRUE(Spellings.Num() == 5);
		UNTEST_EXPECT_TRUE(Spellings.Contains(TEXT("bound_editor_closed")));
		UNTEST_EXPECT_TRUE(Spellings.Contains(TEXT("graph_tab_closed")));
		UNTEST_EXPECT_TRUE(Spellings.Contains(TEXT("graph_removed")));
	}

	EWB_Cleanup(AssetPath);
	co_return;
}


// Clearing a binding restores not_bound.
UNTEST_UNIT_OPTS(Claireon, EditorWindow, EditorWindow_ClearedBindingIsNotBound,
	UNTEST_TIMEOUTMS(120000))
{
	const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_EWB_Clear");
	EWB_Cleanup(AssetPath);

	const FString SessionId = EWB_CreateAndOpen(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	FBlueprintEditToolData* Data = ClaireonBlueprintGraphEditToolBase::FindToolData(SessionId);
	UNTEST_ASSERT_TRUE(Data != nullptr);
	UEdGraph* Graph = Data->Graph.Get();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	ClaireonBlueprintEditorBindingSeam::FScopedSeam Seam;

	FClaireonBlueprintEditorBinding Binding;
	const FGuid InstanceId = ClaireonBlueprintEditorBindingSeam::RegisterInstance(Graph, nullptr);
	Binding.BindToSeamInstance(Data->Blueprint.Get(), Graph, InstanceId);
	UNTEST_ASSERT_TRUE(Binding.IsBound());

	Binding.Clear();
	UNTEST_EXPECT_FALSE(Binding.IsBound());
	UNTEST_EXPECT_TRUE(Binding.Revalidate(Graph) == EClaireonEditorBindingStatus::NotBound);

	// A failed editor open must not create a binding identity.
	Binding.BindTo(Data->Blueprint.Get(), Graph, nullptr, nullptr);
	UNTEST_EXPECT_FALSE(Binding.IsBound());
	UNTEST_EXPECT_TRUE(Binding.Revalidate(Graph) == EClaireonEditorBindingStatus::NotBound);

	EWB_Cleanup(AssetPath);
	co_return;
}

#endif // WITH_UNTESTED
