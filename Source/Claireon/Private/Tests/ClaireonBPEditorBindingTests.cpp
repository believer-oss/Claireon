// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Test binding to one of two distinct widgets over the same graph.
// Normal editor opening reuses the existing instance, so candidates use the binding seam.
// Separate fixture creation and editor opening across latent updates to avoid reentrant loading.

#include "Misc/AutomationTest.h"

#if WITH_CLAIREON_TESTS && WITH_EDITOR

#include "Tests/ClaireonBPEditorFixtures.h"

#include "BlueprintEditor.h"
#include "ClaireonBlueprintHelpers.h"
#include "ClaireonScopedAssetEditor.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "GraphEditor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Tools/ClaireonBlueprintGraphTool_Open.h"
#include "Widgets/Docking/SDockTab.h"

namespace ClaireonBPEditorBindingInternal
{
	// Prefix helpers to avoid unity-build collisions.

	/** bp_open on AssetPath, returning the whole envelope. */
	static IClaireonTool::FToolResult BND_Open(const FString& AssetPath)
	{
		ClaireonBlueprintGraphTool_Open Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		return Tool.Execute(Args);
	}

	/** The editor_window sub-object's string field, or empty with a test error. */
	static FString BND_WindowField(FAutomationTestBase& Test,
		const IClaireonTool::FToolResult& Result, const TCHAR* Field)
	{
		const TSharedPtr<FJsonObject>* Window = nullptr;
		if (!Result.Data.IsValid() || !Result.Data->TryGetObjectField(TEXT("editor_window"), Window)
			|| !Window || !Window->IsValid())
		{
			Test.AddError(TEXT("the bp_open envelope carried no editor_window object."));
			return FString();
		}
		FString Value;
		if (!(*Window)->TryGetStringField(Field, Value))
		{
			Test.AddError(FString::Printf(TEXT("editor_window omitted '%s'."), Field));
			return FString();
		}
		return Value;
	}

	static FString BND_SessionId(const IClaireonTool::FToolResult& Result)
	{
		FString SessionId;
		if (Result.Data.IsValid())
		{
			Result.Data->TryGetStringField(TEXT("session_id"), SessionId);
		}
		return SessionId;
	}

	/** Create, open, execute, and tear down in separate latent updates. */
	class FBND_FixtureCommand : public IAutomationLatentCommand
	{
	public:
		FBND_FixtureCommand(FAutomationTestBase* InTest, const FString& InAssetPath)
			: Test(InTest)
			, AssetPath(InAssetPath)
		{
		}

		virtual bool Update() override
		{
			switch (Phase)
			{
			case EPhase::Create:
			{
				FString CreateError;
				UBlueprint* Blueprint = ClaireonBPEditorFixtures::Create(AssetPath, CreateError);
				if (!IsValid(Blueprint))
				{
					Test->AddError(FString::Printf(TEXT("fixture creation failed at %s: %s"),
						*AssetPath, *CreateError));
					return true;
				}
				if (!IsValid(ClaireonBPEditorFixtures::FirstUbergraph(Blueprint)))
				{
					Test->AddError(TEXT("the fixture has no ubergraph to bind to."));
					Phase = EPhase::Teardown;
					return false;
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				Phase = EPhase::Settle;
				return false;
			}

			case EPhase::Settle:
				if (++SettleUpdates < 3)
				{
					return false;
				}
				Phase = EPhase::Body;
				return false;

			case EPhase::Body:
			{
				UBlueprint* Blueprint = Cast<UBlueprint>(ClaireonBPEditorFixtures::Resolve(AssetPath));
				UEdGraph* Graph = ClaireonBPEditorFixtures::FirstUbergraph(Blueprint);
				if (!IsValid(Blueprint) || !IsValid(Graph))
				{
					Test->AddError(TEXT("the fixture went away before the body could run."));
					return true;
				}
				RunBody(*Blueprint, *Graph);

				// Rebuild the open editor's widgets over whatever the body left, so a later
				// test's Slate pump cannot paint a stale SGraphPanel.
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				Phase = EPhase::Teardown;
				return false;
			}

			case EPhase::Teardown:
			default:
				if (IsValid(GEditor))
				{
					if (UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
						IsValid(Subsystem))
					{
						if (UObject* Asset = ClaireonBPEditorFixtures::Resolve(AssetPath); IsValid(Asset))
						{
							Subsystem->CloseAllEditorsForAsset(Asset);
						}
					}
				}
				ClaireonBPEditorFixtures::Teardown(AssetPath);
				return true;
			}
		}

	protected:
		virtual void RunBody(UBlueprint& Blueprint, UEdGraph& Graph) = 0;

		FAutomationTestBase* Test = nullptr;
		FString AssetPath;

	private:
		enum class EPhase : uint8 { Create, Settle, Body, Teardown };
		EPhase Phase = EPhase::Create;
		int32 SettleUpdates = 0;
	};
}

using namespace ClaireonBPEditorBindingInternal;


// Opening a session binds to one editor and reuses it on subsequent opens.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorBindingSessionOpenShowsTheAsset,
	"Claireon.BPEditor.Binding.SessionOpenShowsTheAssetAndBinds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorBindingInternal
{
	class FBND_ShowsTheAssetCommand : public FBND_FixtureCommand
	{
	public:
		using FBND_FixtureCommand::FBND_FixtureCommand;

	protected:
		virtual void RunBody(UBlueprint& Blueprint, UEdGraph& Graph) override
		{
			UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
			if (!IsValid(Subsystem))
			{
				Test->AddError(TEXT("no asset-editor subsystem in an editor run."));
				return;
			}
			Subsystem->CloseAllEditorsForAsset(&Blueprint);
			Test->TestEqual(TEXT("the fixture starts with no editor open"),
				Subsystem->FindEditorsForAsset(&Blueprint).Num(), 0);

			const IClaireonTool::FToolResult First = BND_Open(AssetPath);
			if (First.bIsError)
			{
				Test->AddError(FString::Printf(TEXT("bp_open failed: %s"), *First.ErrorMessage));
				return;
			}
			Test->TestEqual(TEXT("the first open reports it opened the window"),
				BND_WindowField(*Test, First, TEXT("state")), FString(TEXT("opened")));
			Test->TestEqual(TEXT("and binds to it"),
				BND_WindowField(*Test, First, TEXT("binding")), FString(TEXT("valid")));

			const TArray<IAssetEditorInstance*> Instances = Subsystem->FindEditorsForAsset(&Blueprint);
			Test->TestEqual(TEXT("exactly one editor instance exists"), Instances.Num(), 1);

			const FString SessionId = BND_SessionId(First);
			FBlueprintEditToolData* Data = ClaireonBlueprintGraphEditToolBase::FindToolData(SessionId);
			if (!Data)
			{
				Test->AddError(TEXT("the session reported an id it does not have tool data for."));
				return;
			}
			Test->TestTrue(TEXT("the session recorded a binding"), Data->EditorBinding.IsBound());

			EClaireonEditorBindingStatus Status = EClaireonEditorBindingStatus::NotBound;
			const TSharedPtr<SGraphEditor> Widget = Data->EditorBinding.ResolveGraphEditor(&Graph, Status);
			Test->TestEqual(TEXT("revalidation says valid"),
				FString(ClaireonEditorBindingStatusToWireString(Status)), FString(TEXT("valid")));
			if (!Widget.IsValid())
			{
				Test->AddError(TEXT("a valid binding resolved no widget."));
				return;
			}
			Test->TestTrue(TEXT("the resolved widget is showing the session's graph"),
				Widget->GetCurrentGraph() == &Graph);

			const IClaireonTool::FToolResult Second = BND_Open(AssetPath);
			Test->TestFalse(TEXT("the second open succeeds"), Second.bIsError);
			Test->TestEqual(TEXT("the second open reports the window was already open"),
				BND_WindowField(*Test, Second, TEXT("state")), FString(TEXT("already_open")));
			Test->TestEqual(TEXT("and still exactly one instance exists"),
				Subsystem->FindEditorsForAsset(&Blueprint).Num(), 1);

			Test->TestTrue(TEXT("the window is still open after both calls"),
				Subsystem->FindEditorForAsset(&Blueprint, /*bFocusIfOpen=*/false) != nullptr);
		}
	};
}

bool FClaireonBPEditorBindingSessionOpenShowsTheAsset::RunTest(const FString& /*Parameters*/)
{
	ADD_LATENT_AUTOMATION_COMMAND(FBND_ShowsTheAssetCommand(
		this, ClaireonBPEditorFixtures::UniquePath(TEXT("BND_Shows"))));
	return true;
}


// Two widgets share a graph; only the recorded editor instance can disambiguate them.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorBindingResolvesTheBoundWidget,
	"Claireon.BPEditor.Binding.ResolvesTheBoundWidgetNotTheOtherCandidate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorBindingInternal
{
	class FBND_TwoCandidatesCommand : public FBND_FixtureCommand
	{
	public:
		using FBND_FixtureCommand::FBND_FixtureCommand;

	protected:
		virtual void RunBody(UBlueprint& Blueprint, UEdGraph& Graph) override
		{
			ClaireonBlueprintEditorBindingSeam::FScopedSeam Seam;

			const TSharedRef<SGraphEditor> WidgetA = SNew(SGraphEditor).GraphToEdit(&Graph);
			const TSharedRef<SGraphEditor> WidgetB = SNew(SGraphEditor).GraphToEdit(&Graph);
			if (WidgetA == WidgetB)
			{
				Test->AddError(TEXT("the two candidates are the same widget, so this test "
				                    "cannot distinguish the bound one from the other."));
				return;
			}

			const FGuid InstanceA = ClaireonBlueprintEditorBindingSeam::RegisterInstance(&Graph, WidgetA);
			const FGuid InstanceB = ClaireonBlueprintEditorBindingSeam::RegisterInstance(&Graph, WidgetB);
			Test->TestTrue(TEXT("the two candidates have distinct identities"), InstanceA != InstanceB);

			FClaireonBlueprintEditorBinding Binding;
			Binding.BindToSeamInstance(&Blueprint, &Graph, InstanceA);

			EClaireonEditorBindingStatus Status = EClaireonEditorBindingStatus::NotBound;
			const TSharedPtr<SGraphEditor> Resolved = Binding.ResolveGraphEditor(&Graph, Status);
			Test->TestEqual(TEXT("the binding is valid"),
				FString(ClaireonEditorBindingStatusToWireString(Status)), FString(TEXT("valid")));

			Test->TestTrue(TEXT("the BOUND candidate's widget is what resolved"),
				Resolved == WidgetA);
			Test->TestFalse(TEXT("the other candidate's widget is NOT what resolved"),
				Resolved == WidgetB);

			// Reverse the binding to rule out registration-order selection.
			FClaireonBlueprintEditorBinding OtherBinding;
			OtherBinding.BindToSeamInstance(&Blueprint, &Graph, InstanceB);
			const TSharedPtr<SGraphEditor> ResolvedB = OtherBinding.ResolveGraphEditor(&Graph, Status);
			Test->TestTrue(TEXT("binding to B resolves B"), ResolvedB == WidgetB);
			Test->TestFalse(TEXT("binding to B does not resolve A"), ResolvedB == WidgetA);
		}
	};
}

bool FClaireonBPEditorBindingResolvesTheBoundWidget::RunTest(const FString& /*Parameters*/)
{
	ADD_LATENT_AUTOMATION_COMMAND(FBND_TwoCandidatesCommand(
		this, ClaireonBPEditorFixtures::UniquePath(TEXT("BND_TwoCandidates"))));
	return true;
}


// A session must not adopt a replacement editor after its bound instance closes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorBindingClosedEditorRefuses,
	"Claireon.BPEditor.Binding.ClosedEditorRefusesAndDoesNotRebind",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorBindingInternal
{
	class FBND_ClosedEditorCommand : public FBND_FixtureCommand
	{
	public:
		using FBND_FixtureCommand::FBND_FixtureCommand;

	protected:
		virtual void RunBody(UBlueprint& Blueprint, UEdGraph& Graph) override
		{
			UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
			if (!IsValid(Subsystem))
			{
				Test->AddError(TEXT("no asset-editor subsystem in an editor run."));
				return;
			}
			Subsystem->CloseAllEditorsForAsset(&Blueprint);

			const IClaireonTool::FToolResult Opened = BND_Open(AssetPath);
			if (Opened.bIsError)
			{
				Test->AddError(FString::Printf(TEXT("bp_open failed: %s"), *Opened.ErrorMessage));
				return;
			}
			FBlueprintEditToolData* Data =
				ClaireonBlueprintGraphEditToolBase::FindToolData(BND_SessionId(Opened));
			if (!Data)
			{
				Test->AddError(TEXT("the session reported an id it does not have tool data for."));
				return;
			}

			Test->TestEqual(TEXT("the binding starts valid"),
				FString(ClaireonEditorBindingStatusToWireString(Data->EditorBinding.Revalidate(&Graph))),
				FString(TEXT("valid")));

			Subsystem->CloseAllEditorsForAsset(&Blueprint);
			Test->TestEqual(TEXT("the editor really did close"),
				Subsystem->FindEditorsForAsset(&Blueprint).Num(), 0);

			EClaireonEditorBindingStatus Status = EClaireonEditorBindingStatus::Valid;
			const TSharedPtr<SGraphEditor> Widget = Data->EditorBinding.ResolveGraphEditor(&Graph, Status);
			Test->TestEqual(TEXT("a dead instance reports bound_editor_closed"),
				FString(ClaireonEditorBindingStatusToWireString(Status)),
				FString(TEXT("bound_editor_closed")));
			Test->TestFalse(TEXT("and resolves no widget"), Widget.IsValid());

			// Now open a window on the same asset. The session must NOT adopt it.
			Subsystem->OpenEditorForAsset(&Blueprint);
			Test->TestTrue(TEXT("a new editor is open on the asset"),
				Subsystem->FindEditorForAsset(&Blueprint, /*bFocusIfOpen=*/false) != nullptr);
			Test->TestEqual(TEXT("the session STILL refuses -- it did not rebind"),
				FString(ClaireonEditorBindingStatusToWireString(Data->EditorBinding.Revalidate(&Graph))),
				FString(TEXT("bound_editor_closed")));
		}
	};
}

bool FClaireonBPEditorBindingClosedEditorRefuses::RunTest(const FString& /*Parameters*/)
{
	ADD_LATENT_AUTOMATION_COMMAND(FBND_ClosedEditorCommand(
		this, ClaireonBPEditorFixtures::UniquePath(TEXT("BND_ClosedEditor"))));
	return true;
}


// Closing a graph tab must remain distinct from closing its editor.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorBindingClosedTabIsItsOwnReason,
	"Claireon.BPEditor.Binding.ClosedGraphTabIsItsOwnReason",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorBindingInternal
{
	class FBND_ClosedTabCommand : public FBND_FixtureCommand
	{
	public:
		using FBND_FixtureCommand::FBND_FixtureCommand;

	protected:
		virtual void RunBody(UBlueprint& Blueprint, UEdGraph& Graph) override
		{
			UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
			if (!IsValid(Subsystem))
			{
				Test->AddError(TEXT("no asset-editor subsystem in an editor run."));
				return;
			}
			Subsystem->CloseAllEditorsForAsset(&Blueprint);

			const IClaireonTool::FToolResult Opened = BND_Open(AssetPath);
			if (Opened.bIsError)
			{
				Test->AddError(FString::Printf(TEXT("bp_open failed: %s"), *Opened.ErrorMessage));
				return;
			}
			FBlueprintEditToolData* Data =
				ClaireonBlueprintGraphEditToolBase::FindToolData(BND_SessionId(Opened));
			if (!Data)
			{
				Test->AddError(TEXT("the session reported an id it does not have tool data for."));
				return;
			}
			Test->TestEqual(TEXT("the binding starts valid"),
				FString(ClaireonEditorBindingStatusToWireString(Data->EditorBinding.Revalidate(&Graph))),
				FString(TEXT("valid")));

			TSharedPtr<FBlueprintEditor> Editor = Data->EditorBinding.Editor.Pin();
			if (!Editor.IsValid())
			{
				Test->AddError(TEXT("the session bound to no Blueprint editor."));
				return;
			}

			Editor->CloseDocumentTab(&Graph);

			TArray<TSharedPtr<SDockTab>> OpenTabs;
			Editor->FindOpenTabsContainingDocument(&Graph, OpenTabs);
			if (OpenTabs.Num() != 0)
			{
				Test->AddError(TEXT("the graph tab did not close, so graph_tab_closed is not "
				                    "the condition under test here."));
				return;
			}
			Test->TestTrue(TEXT("the editor instance is still registered"),
				Subsystem->FindEditorForAsset(&Blueprint, /*bFocusIfOpen=*/false) != nullptr);

			Test->TestEqual(TEXT("a closed tab reports graph_tab_closed, not bound_editor_closed"),
				FString(ClaireonEditorBindingStatusToWireString(Data->EditorBinding.Revalidate(&Graph))),
				FString(TEXT("graph_tab_closed")));
		}
	};
}

bool FClaireonBPEditorBindingClosedTabIsItsOwnReason::RunTest(const FString& /*Parameters*/)
{
	ADD_LATENT_AUTOMATION_COMMAND(FBND_ClosedTabCommand(
		this, ClaireonBPEditorFixtures::UniquePath(TEXT("BND_ClosedTab"))));
	return true;
}

// A scoped open owns only the editor instance it created.
// Register a stub second instance because OpenEditorForAsset reuses the primary editor.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorBindingScopeClosesOnlyItsOwnEditor,
	"Claireon.BPEditor.Binding.ScopedEditorClosesOnlyTheInstanceItOpened",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorBindingInternal
{
	/** A second registered editor instance for the fixture asset that records close requests. */
	class FBND_StubEditorInstance : public IAssetEditorInstance
	{
	public:
		int32 CloseRequests = 0;

		virtual FName GetEditorName() const override { return TEXT("ClaireonBindingStubEditor"); }
		virtual void FocusWindow(UObject* /*ObjectToFocusOn*/) override {}
		virtual bool CloseWindow() override { ++CloseRequests; return true; }
		virtual bool CloseWindow(EAssetEditorCloseReason /*InCloseReason*/) override { ++CloseRequests; return true; }
		virtual bool IsPrimaryEditor() const override { return false; }
		virtual void InvokeTab(const FTabId& /*TabId*/) override {}
		virtual TSharedPtr<FTabManager> GetAssociatedTabManager() override { return nullptr; }
		virtual double GetLastActivationTime() override { return 0.0; }
		virtual void RemoveEditingAsset(UObject* /*Asset*/) override {}
	};

	class FBND_ScopedOwnershipCommand : public FBND_FixtureCommand
	{
	public:
		using FBND_FixtureCommand::FBND_FixtureCommand;

	protected:
		virtual void RunBody(UBlueprint& Blueprint, UEdGraph& /*Graph*/) override
		{
			UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
			if (!IsValid(Subsystem))
			{
				Test->AddError(TEXT("no asset-editor subsystem in an editor run."));
				return;
			}
			Subsystem->CloseAllEditorsForAsset(&Blueprint);
			Test->TestEqual(TEXT("the fixture starts with no editor open"),
				Subsystem->FindEditorsForAsset(&Blueprint).Num(), 0);

			// 1. An editor open before the scope is not the scope's to close.
			Subsystem->OpenEditorForAsset(&Blueprint);
			Test->TestEqual(TEXT("the pre-existing editor opened"),
				Subsystem->FindEditorsForAsset(&Blueprint).Num(), 1);
			{
				FClaireonScopedAssetEditor Scope(&Blueprint, /*bInCloseOnDestroy=*/true);
				Test->TestTrue(TEXT("the scope saw the editor as already open"), Scope.WasAlreadyOpen());
			}
			Test->TestEqual(TEXT("the pre-existing editor survives the scope"),
				Subsystem->FindEditorsForAsset(&Blueprint).Num(), 1);
			Subsystem->CloseAllEditorsForAsset(&Blueprint);
			Test->TestEqual(TEXT("reset between cases"), Subsystem->FindEditorsForAsset(&Blueprint).Num(), 0);

			// 2. The scope closes what it opened.
			{
				FClaireonScopedAssetEditor Scope(&Blueprint, /*bInCloseOnDestroy=*/true);
				Test->TestEqual(TEXT("the scope opened the editor"),
					FString(ClaireonAssetEditorWindow::ToWireString(Scope.GetOpenOutcome().State)),
					FString(TEXT("opened")));
				Test->TestTrue(TEXT("and recorded its toolkit"), Scope.GetToolkit().IsValid());
				Test->TestEqual(TEXT("exactly one instance exists inside the scope"),
					Subsystem->FindEditorsForAsset(&Blueprint).Num(), 1);
			}
			Test->TestEqual(TEXT("the scope closed the editor it opened"),
				Subsystem->FindEditorsForAsset(&Blueprint).Num(), 0);

			// 3. A same-asset instance registered during the scope is not the scope's either.
			{
				FBND_StubEditorInstance Stub;
				{
					FClaireonScopedAssetEditor Scope(&Blueprint, /*bInCloseOnDestroy=*/true);
					Test->TestTrue(TEXT("the scope opened its own instance"), Scope.GetToolkit().IsValid());

					Subsystem->NotifyAssetOpened(&Blueprint, &Stub);
					Test->TestEqual(TEXT("two instances are registered inside the scope"),
						Subsystem->FindEditorsForAsset(&Blueprint).Num(), 2);
				}
				Test->TestEqual(TEXT("the other instance was never asked to close"), Stub.CloseRequests, 0);
				const TArray<IAssetEditorInstance*> Remaining = Subsystem->FindEditorsForAsset(&Blueprint);
				Test->TestEqual(TEXT("only the other instance remains registered"), Remaining.Num(), 1);
				Test->TestTrue(TEXT("and it is the other instance"),
					Remaining.Num() == 1 && Remaining[0] == static_cast<IAssetEditorInstance*>(&Stub));

				// The stub is stack-owned: unregister it before it goes out of scope.
				Subsystem->NotifyAssetClosed(&Blueprint, &Stub);
				Test->TestEqual(TEXT("the stub unregistered cleanly"),
					Subsystem->FindEditorsForAsset(&Blueprint).Num(), 0);
			}

			// Editor closure is deferred; scope exit must tolerate a pending close without reopening the asset.
			{
				FClaireonScopedAssetEditor Scope(&Blueprint, /*bInCloseOnDestroy=*/true);
				Test->TestTrue(TEXT("the scope opened an instance to be closed by the user"),
					Scope.GetToolkit().IsValid());
				Subsystem->CloseAllEditorsForAsset(&Blueprint);
				Test->AddInfo(FString::Printf(TEXT("instances still registered right after the user's close "
					"request (deferred close): %d"), Subsystem->FindEditorsForAsset(&Blueprint).Num()));
			}
			Test->TestTrue(TEXT("the scope's exit over a user-closed instance reopened nothing"),
				Subsystem->FindEditorsForAsset(&Blueprint).Num() <= 1);
			Subsystem->CloseAllEditorsForAsset(&Blueprint);
		}
	};
}

bool FClaireonBPEditorBindingScopeClosesOnlyItsOwnEditor::RunTest(const FString& /*Parameters*/)
{
	ADD_LATENT_AUTOMATION_COMMAND(FBND_ScopedOwnershipCommand(
		this, ClaireonBPEditorFixtures::UniquePath(TEXT("BND_ScopedOwner"))));
	return true;
}

#endif // WITH_CLAIREON_TESTS && WITH_EDITOR
