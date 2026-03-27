// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonModule.h"
#include "ClaireonLog.h"
#include "ClaireonPIEManager.h"
#include "ClaireonRichTextStyle.h"
#include "ClaireonServer.h"
#include "ClaireonBridge.h"
#include "ClaireonDiagnosticsWidget.h"
#include "ClaireonSettings.h"
#include "Modules/ModuleManager.h"
#include "ToolMenus.h"
#include "Styling/AppStyle.h"
#include "Framework/Docking/TabManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Containers/Ticker.h"
#include "Tools/ClaireonTool_AssetReferences.h"
#include "Tools/ClaireonTool_AssetSearch.h"
#include "Tools/ClaireonTool_ExecutePython.h"
#include "Tools/ClaireonTool_GetBlueprintProperties.h"
#include "Tools/ClaireonTool_GetBlueprintGraph.h"
#include "Tools/ClaireonTool_FormatBlueprintGraph.h"
#include "Tools/ClaireonTool_EditBlueprintGraph.h"
#include "Tools/ClaireonTool_SearchInBlueprints.h"

// New tools (stage 001 stubs)
#include "Tools/ClaireonTool_PythonAuditLog.h"
#include "Tools/ClaireonTool_ProjectInfo.h"
#include "Tools/ClaireonTool_EngineInfo.h"
#include "Tools/ClaireonTool_LiveCodingReload.h"
#include "Tools/ClaireonTool_MapOpen.h"
#include "Tools/ClaireonTool_MapStatus.h"
#include "Tools/ClaireonTool_PIEStart.h"
#include "Tools/ClaireonTool_PIEStop.h"
#include "Tools/ClaireonTool_PIEStatus.h"
#include "Tools/ClaireonTool_PIEGetPlayerPawn.h"
#include "Tools/ClaireonTool_PIEGetActor.h"
#include "Tools/ClaireonTool_PIECheckInitState.h"
#include "Tools/ClaireonTool_PIEWaitFor.h"
#include "Tools/ClaireonTool_PIESpawnEnemy.h"
#include "Tools/ClaireonTool_PIEGetComponent.h"
#include "Tools/ClaireonTool_PIERegisterDamageListener.h"
#include "Tools/ClaireonTool_PIEGetDamageEvents.h"
#include "Tools/ClaireonTool_PIEUnregisterDamageListener.h"
#include "Tools/ClaireonTool_PIEAITargetInfo.h"
#include "Tools/ClaireonTool_PIETestAbility.h"
#include "Tools/ClaireonTool_ConsoleExecute.h"
#include "Tools/ClaireonTool_AssetList.h"
#include "Tools/ClaireonTool_AssetValidate.h"
#include "Tools/ClaireonTool_AssetFixupRedirectors.h"
#include "Tools/ClaireonTool_BlueprintCompile.h"
#include "Tools/ClaireonTool_CommandletRun.h"
#include "Tools/ClaireonTool_AssetResave.h"
#include "Tools/ClaireonTool_AssetCook.h"
#include "Tools/ClaireonTool_LogTail.h"
#include "Tools/ClaireonTool_LogSearch.h"
#include "Tools/ClaireonTool_TestRun.h"
#include "Tools/ClaireonTool_TestList.h"
// State Tree MCP tools
#include "Tools/ClaireonTool_StateTreeInspect.h"
#include "Tools/ClaireonTool_StateTreeListNodeTypes.h"
#include "Tools/ClaireonTool_StateTreeEdit.h"
#include "Tools/ClaireonTool_StateTreeRuntimeInspect.h"
#include "Tools/ClaireonTool_StateTreeRuntimeSendEvent.h"

// Diff MCP tools
#include "Tools/ClaireonTool_AssetDiffProperties.h"
#include "Tools/ClaireonTool_BlueprintDiff.h"
#include "Tools/ClaireonTool_StateTreeDiff.h"

// Trace analysis MCP tools
#include "Tools/ClaireonTool_TraceOpen.h"
#include "Tools/ClaireonTool_TraceClose.h"
#include "Tools/ClaireonTool_TraceGetSessionInfo.h"
#include "Tools/ClaireonTool_TraceGetFrameStats.h"
#include "Tools/ClaireonTool_TraceGetTopScopes.h"
#include "Tools/ClaireonTool_TraceGetScopeDetails.h"
#include "Tools/ClaireonTool_TraceGetThreads.h"

// Level tools
#include "Tools/ClaireonTool_ListActors.h"
#include "Tools/ClaireonTool_LevelSetActorProperty.h"
#include "Tools/ClaireonTool_PlaceActor.h"
#include "Tools/ClaireonTool_MapDuplicate.h"
#include "Tools/ClaireonTool_SetSplinePoints.h"

// Blueprint CDO tools
#include "Tools/ClaireonTool_SetBlueprintCDOProperty.h"

// Meta tools
#include "Tools/ClaireonTool_SearchTools.h"
#include "Tools/ClaireonTool_FeedbackSubmit.h"

// Widget Blueprint MCP tools
#include "Tools/ClaireonTool_GetWidgetBPTree.h"
#include "Tools/ClaireonTool_EditWidgetBP.h"

// Behavior Tree + EQS MCP tools
#include "Tools/ClaireonTool_BehaviorTreeInspect.h"
#include "Tools/ClaireonTool_BehaviorTreeInspectBlackboard.h"
#include "Tools/ClaireonTool_BehaviorTreeEdit.h"
#include "Tools/ClaireonTool_BlackboardEdit.h"
#include "Tools/ClaireonTool_EQSInspect.h"
#include "Tools/ClaireonTool_EQSEdit.h"
#include "Tools/ClaireonTool_NiagaraInspect.h"
#include "Tools/ClaireonTool_NiagaraEdit.h"

// Data Table MCP tools
#include "Tools/ClaireonTool_DataTableSearch.h"
#include "Tools/ClaireonTool_DataTableGetInfo.h"
#include "Tools/ClaireonTool_DataTableGetRows.h"
#include "Tools/ClaireonTool_DataTableGetRow.h"
#include "Tools/ClaireonTool_DataTableFindRows.h"
#include "Tools/ClaireonTool_DataTableAddRow.h"
#include "Tools/ClaireonTool_DataTableRemoveRow.h"
#include "Tools/ClaireonTool_DataTableDuplicateRow.h"
#include "Tools/ClaireonTool_DataTableRenameRow.h"
#include "Tools/ClaireonTool_DataTableMoveRow.h"
#include "Tools/ClaireonTool_DataTableSetRowValues.h"
#include "Tools/ClaireonTool_DataTableExportJson.h"
#include "Tools/ClaireonTool_DataTableImportJson.h"
#include "Tools/ClaireonTool_DataTableExportCsv.h"
#include "Tools/ClaireonTool_DataTableImportCsv.h"

// Flythrough camera MCP tools
#include "Tools/ClaireonTool_FlythroughStart.h"
#include "Tools/ClaireonTool_FlythroughStop.h"
#include "Tools/ClaireonTool_FlythroughStatus.h"
#include "Tools/ClaireonTool_PIEScreenshot.h"
#include "Tools/ClaireonTool_PIETraceStart.h"
#include "Tools/ClaireonTool_PIETraceStop.h"

// Asset editor tool
#include "Tools/ClaireonTool_OpenAssetEditor.h"

// Session management MCP tools
#include "Tools/ClaireonTool_ListSessions.h"
#include "Tools/ClaireonTool_ReleaseSessions.h"

// PCG Graph MCP tools
#include "Tools/ClaireonTool_PCGGraphInspect.h"
#include "Tools/ClaireonTool_PCGGraphEdit.h"

DEFINE_LOG_CATEGORY(LogClaireon);

#define LOCTEXT_NAMESPACE "ClaireonModule"

void FClaireonModule::StartupModule()
{
	if (!GIsEditor || IsRunningCommandlet())
	{
		return;
	}

	SClaireonDiagnosticsWidget::RegisterTabSpawner();

	// Settings registration is handled automatically by UDeveloperSettings via
	// GetCategoryName() -> "Plugins" and meta=(DisplayName="MCP REPL").
	// Manual RegisterSettings was removed — it caused duplicate entries.

	FClaireonRichTextStyle::Initialize();

	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FClaireonModule::RegisterMenus));

	// Bind PIE manager to editor delegates so it detects all PIE sessions
	FClaireonPIEManager::Get().BindEditorDelegates();

	// Check for -StartMCPServer command-line flag
	if (FParse::Param(FCommandLine::Get(), TEXT("StartMCPServer")))
	{
		UE_LOG(LogClaireon, Display, TEXT("[MCP] Auto-starting server (command-line flag)"));
		StartServer();
	}
}

void FClaireonModule::ShutdownModule()
{
	FClaireonPIEManager::Get().UnbindEditorDelegates();
	FClaireonRichTextStyle::Shutdown();

	StopServer();
	SClaireonDiagnosticsWidget::UnregisterTabSpawner();
}

void FClaireonModule::StartServer()
{
	if (Server.IsValid() && Server->IsRunning())
	{
		UE_LOG(LogClaireon, Warning, TEXT("[MCP] Server is already running"));
		return;
	}

	Server = MakeShared<FClaireonServer>();

	// Set the bridge's tool registry pointer so Python can dispatch tool calls
	FClaireonBridge::SetToolRegistry(Server.Get());

	// Defer bridge registration + Python import pre-warming to next tick.
	// Python may not be fully initialized yet during StartupModule, and calling
	// CPython C API too early causes access violations. Deferring to next tick
	// ensures Python is ready, and pre-warming avoids blocking the game thread
	// for >6s during the first execute call (which triggers UE's audio mixer abort).
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
											 [](float) -> bool
	{
		FClaireonBridge::EnsureRegistered();
		return false; // one-shot
	}),
		1.0f);

	Server->RegisterTool(MakeShared<ClaireonTool_AssetReferences>());
	Server->RegisterTool(MakeShared<ClaireonTool_AssetSearch>());
	Server->RegisterTool(MakeShared<ClaireonTool_ExecutePython>());

	// Blueprint MCP tools
	Server->RegisterTool(MakeShared<ClaireonTool_GetBlueprintProperties>());
	Server->RegisterTool(MakeShared<ClaireonTool_GetBlueprintGraph>());
	Server->RegisterTool(MakeShared<ClaireonTool_FormatBlueprintGraph>());
	Server->RegisterTool(MakeShared<ClaireonTool_EditBlueprintGraph>());
	Server->RegisterTool(MakeShared<ClaireonTool_SearchInBlueprints>());

	// New tools (stage 001 stubs — implementations filled in later stages)
	Server->RegisterTool(MakeShared<ClaireonTool_PythonAuditLog>());
	Server->RegisterTool(MakeShared<ClaireonTool_ProjectInfo>());
	Server->RegisterTool(MakeShared<ClaireonTool_EngineInfo>());
	Server->RegisterTool(MakeShared<ClaireonTool_LiveCodingReload>());
	Server->RegisterTool(MakeShared<ClaireonTool_MapOpen>());
	Server->RegisterTool(MakeShared<ClaireonTool_MapStatus>());
	Server->RegisterTool(MakeShared<ClaireonTool_PIEStart>());
	Server->RegisterTool(MakeShared<ClaireonTool_PIEStop>());
	Server->RegisterTool(MakeShared<ClaireonTool_PIEStatus>());
	Server->RegisterTool(MakeShared<ClaireonTool_PIEGetPlayerPawn>());
	Server->RegisterTool(MakeShared<ClaireonTool_PIEGetActor>());
	Server->RegisterTool(MakeShared<ClaireonTool_PIECheckInitState>());
	Server->RegisterTool(MakeShared<ClaireonTool_PIEWaitFor>());
	Server->RegisterTool(MakeShared<ClaireonTool_PIESpawnEnemy>());
	Server->RegisterTool(MakeShared<ClaireonTool_PIEGetComponent>());
	Server->RegisterTool(MakeShared<ClaireonTool_PIERegisterDamageListener>());
	Server->RegisterTool(MakeShared<ClaireonTool_PIEGetDamageEvents>());
	Server->RegisterTool(MakeShared<ClaireonTool_PIEUnregisterDamageListener>());
	Server->RegisterTool(MakeShared<ClaireonTool_PIEAITargetInfo>());
	Server->RegisterTool(MakeShared<ClaireonTool_PIETestAbility>());
	// Flythrough camera tools
	Server->RegisterTool(MakeShared<ClaireonTool_FlythroughStart>());
	Server->RegisterTool(MakeShared<ClaireonTool_FlythroughStop>());
	Server->RegisterTool(MakeShared<ClaireonTool_FlythroughStatus>());
	Server->RegisterTool(MakeShared<ClaireonTool_PIEScreenshot>());
	Server->RegisterTool(MakeShared<ClaireonTool_PIETraceStart>());
	Server->RegisterTool(MakeShared<ClaireonTool_PIETraceStop>());
	Server->RegisterTool(MakeShared<ClaireonTool_ConsoleExecute>());
	Server->RegisterTool(MakeShared<ClaireonTool_AssetList>());
	Server->RegisterTool(MakeShared<ClaireonTool_AssetValidate>());
	Server->RegisterTool(MakeShared<ClaireonTool_AssetFixupRedirectors>());
	Server->RegisterTool(MakeShared<ClaireonTool_OpenAssetEditor>());
	Server->RegisterTool(MakeShared<ClaireonTool_BlueprintCompile>());
	Server->RegisterTool(MakeShared<ClaireonTool_CommandletRun>());
	Server->RegisterTool(MakeShared<ClaireonTool_AssetResave>());
	Server->RegisterTool(MakeShared<ClaireonTool_AssetCook>());
	Server->RegisterTool(MakeShared<ClaireonTool_LogTail>());
	Server->RegisterTool(MakeShared<ClaireonTool_LogSearch>());
	Server->RegisterTool(MakeShared<ClaireonTool_TestRun>());
	Server->RegisterTool(MakeShared<ClaireonTool_TestList>());
	// State Tree MCP tools
	Server->RegisterTool(MakeShared<ClaireonTool_StateTreeInspect>());
	Server->RegisterTool(MakeShared<ClaireonTool_StateTreeListNodeTypes>());
	Server->RegisterTool(MakeShared<ClaireonTool_StateTreeEdit>());
	Server->RegisterTool(MakeShared<ClaireonTool_StateTreeRuntimeInspect>());
	Server->RegisterTool(MakeShared<ClaireonTool_StateTreeRuntimeSendEvent>());

	// Diff MCP tools
	Server->RegisterTool(MakeShared<ClaireonTool_AssetDiffProperties>());
	Server->RegisterTool(MakeShared<ClaireonTool_BlueprintDiff>());
	Server->RegisterTool(MakeShared<ClaireonTool_StateTreeDiff>());

	// Trace analysis MCP tools
	Server->RegisterTool(MakeShared<ClaireonTool_TraceOpen>());
	Server->RegisterTool(MakeShared<ClaireonTool_TraceClose>());
	Server->RegisterTool(MakeShared<ClaireonTool_TraceGetSessionInfo>());
	Server->RegisterTool(MakeShared<ClaireonTool_TraceGetFrameStats>());
	Server->RegisterTool(MakeShared<ClaireonTool_TraceGetTopScopes>());
	Server->RegisterTool(MakeShared<ClaireonTool_TraceGetScopeDetails>());
	Server->RegisterTool(MakeShared<ClaireonTool_TraceGetThreads>());

	// Widget Blueprint MCP tools
	Server->RegisterTool(MakeShared<ClaireonTool_GetWidgetBPTree>());
	Server->RegisterTool(MakeShared<ClaireonTool_EditWidgetBP>());

	// Behavior Tree + EQS MCP tools
	Server->RegisterTool(MakeShared<ClaireonTool_BehaviorTreeInspect>());
	Server->RegisterTool(MakeShared<ClaireonTool_BehaviorTreeInspectBlackboard>());
	Server->RegisterTool(MakeShared<ClaireonTool_BehaviorTreeEdit>());
	Server->RegisterTool(MakeShared<ClaireonTool_BlackboardEdit>());
	Server->RegisterTool(MakeShared<ClaireonTool_EQSInspect>());
	Server->RegisterTool(MakeShared<ClaireonTool_EQSEdit>());

	// Niagara MCP tools
	Server->RegisterTool(MakeShared<ClaireonTool_NiagaraInspect>());
	Server->RegisterTool(MakeShared<ClaireonTool_NiagaraEdit>());

	// PCG Graph MCP tools
	Server->RegisterTool(MakeShared<ClaireonTool_PCGGraphInspect>());
	Server->RegisterTool(MakeShared<ClaireonTool_PCGGraphEdit>());

	// Session management tools
	Server->RegisterTool(MakeShared<ClaireonTool_ListSessions>());
	Server->RegisterTool(MakeShared<ClaireonTool_ReleaseSessions>());

	// Level tools
	Server->RegisterTool(MakeShared<ClaireonTool_ListActors>());
	Server->RegisterTool(MakeShared<ClaireonTool_LevelSetActorProperty>());
	Server->RegisterTool(MakeShared<ClaireonTool_PlaceActor>());
	Server->RegisterTool(MakeShared<ClaireonTool_MapDuplicate>());
	Server->RegisterTool(MakeShared<ClaireonTool_SetSplinePoints>());

	// Blueprint CDO tools
	Server->RegisterTool(MakeShared<ClaireonTool_SetBlueprintCDOProperty>());

	// Meta tools
	{
		auto SearchTool = MakeShared<ClaireonTool_SearchTools>();
		SearchTool->SetServer(Server.Get());
		Server->RegisterTool(SearchTool);
	}
	Server->RegisterTool(MakeShared<ClaireonTool_FeedbackSubmit>());

	// Data Table MCP tools
	Server->RegisterTool(MakeShared<ClaireonTool_DataTableSearch>());
	Server->RegisterTool(MakeShared<ClaireonTool_DataTableGetInfo>());
	Server->RegisterTool(MakeShared<ClaireonTool_DataTableGetRows>());
	Server->RegisterTool(MakeShared<ClaireonTool_DataTableGetRow>());
	Server->RegisterTool(MakeShared<ClaireonTool_DataTableFindRows>());
	Server->RegisterTool(MakeShared<ClaireonTool_DataTableAddRow>());
	Server->RegisterTool(MakeShared<ClaireonTool_DataTableRemoveRow>());
	Server->RegisterTool(MakeShared<ClaireonTool_DataTableDuplicateRow>());
	Server->RegisterTool(MakeShared<ClaireonTool_DataTableRenameRow>());
	Server->RegisterTool(MakeShared<ClaireonTool_DataTableMoveRow>());
	Server->RegisterTool(MakeShared<ClaireonTool_DataTableSetRowValues>());
	Server->RegisterTool(MakeShared<ClaireonTool_DataTableExportJson>());
	Server->RegisterTool(MakeShared<ClaireonTool_DataTableImportJson>());
	Server->RegisterTool(MakeShared<ClaireonTool_DataTableExportCsv>());
	Server->RegisterTool(MakeShared<ClaireonTool_DataTableImportCsv>());

	// Parse optional port override from command line
	uint32 Port = 8017;
	FString PortStr;
	if (FParse::Value(FCommandLine::Get(), TEXT("-MCPServerPort="), PortStr))
	{
		Port = FCString::Atoi(*PortStr);
		if (Port == 0)
		{
			UE_LOG(LogClaireon, Warning, TEXT("[MCP] Invalid port from command line, using default 8017"));
			Port = 8017;
		}
	}

	if (!Server->Start(Port))
	{
		UE_LOG(LogClaireon, Error, TEXT("[MCP] Failed to start server"));
		Server.Reset();
		return;
	}

	// Register any tools that were queued before the server started
	FlushPendingExternalTools();

	// Notify external modules so they can register their own tools
	FClaireonServer::OnServerStarted().Broadcast(*Server);
}

void FClaireonModule::StopServer()
{
	if (Server.IsValid())
	{
		Server->Stop();
		Server.Reset();
	}
}

bool FClaireonModule::IsServerRunning() const
{
	return Server.IsValid() && Server->IsRunning();
}

FClaireonModule& FClaireonModule::Get()
{
	return FModuleManager::GetModuleChecked<FClaireonModule>(TEXT("Claireon"));
}

void FClaireonModule::RegisterExternalTool(TSharedPtr<IClaireonTool> Tool)
{
	if (Server.IsValid())
	{
		Server->RegisterTool(Tool);
	}
	else
	{
		PendingExternalTools.Add(Tool);
	}
}

void FClaireonModule::FlushPendingExternalTools()
{
	if (!Server.IsValid())
	{
		return;
	}

	for (const TSharedPtr<IClaireonTool>& Tool : PendingExternalTools)
	{
		Server->RegisterTool(Tool);
	}
	PendingExternalTools.Empty();
}

void FClaireonModule::RegisterMenus()
{
	// Register toolbar button in the editor toolbar User section.
	static const FName ToolbarSection(TEXT("LevelEditor.LevelEditorToolBar.User"));
	static const FName AIChatSection(TEXT("LevelEditor.LevelEditorToolBar.User.AIChat"));

	UToolMenu* Toolbar = UToolMenus::Get()->ExtendMenu(ToolbarSection);
	if (!Toolbar)
	{
		return;
	}

	FToolMenuSection& Section = Toolbar->AddSection(AIChatSection);
	Section.AddSeparator(NAME_None);

	FToolMenuEntry AIChatEntry = FToolMenuEntry::InitToolBarButton(
		TEXT("AIChat"),
		FUIAction(
			FExecuteAction::CreateLambda([]()
	{
		FGlobalTabmanager::Get()->TryInvokeTab(SClaireonDiagnosticsWidget::TabId);
	})),
		LOCTEXT("AIChatLabel", "AI Chat"), LOCTEXT("AIChatTooltip", "Open the AI Chat assistant (Claude REPL)"), FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Comment")), EUserInterfaceActionType::Button);
	AIChatEntry.StyleNameOverride = "CalloutToolbar";
	Section.AddEntry(AIChatEntry);
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FClaireonModule, Claireon)
