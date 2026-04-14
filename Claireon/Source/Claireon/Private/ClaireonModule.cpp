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
#include "IClaireonToolProvider.h"
#include "Modules/ModuleManager.h"
#include "Features/IModularFeatures.h"
#include "ToolMenus.h"
#include "Styling/AppStyle.h"
#include "Framework/Docking/TabManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Containers/Ticker.h"
#include "Async/Async.h"
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
#include "Tools/ClaireonTool_AssetImportFile.h"
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
#include "Tools/ClaireonTool_SetBlueprintMetadata.h"

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

// Animation MCP tools
#include "Tools/ClaireonTool_AnimInspect.h"
#include "Tools/ClaireonAnimTools_Session.h"
#include "Tools/ClaireonAnimTools_Create.h"
#include "Tools/ClaireonAnimTools_Notify.h"
#include "Tools/ClaireonAnimTools_Curve.h"
#include "Tools/ClaireonAnimTools_Montage.h"
#include "Tools/ClaireonAnimTools_DataOps.h"
#include "Tools/ClaireonAnimTools_BlendSpace.h"

// Session management MCP tools
#include "Tools/ClaireonTool_ListSessions.h"
#include "Tools/ClaireonTool_ReleaseSessions.h"

// PCG Graph MCP tools
#include "Tools/ClaireonTool_PCGGraphInspect.h"
#include "Tools/ClaireonTool_PCGGraphEdit.h"

// Enhanced Input MCP tools
#include "Tools/ClaireonTool_InputInspect.h"
#include "Tools/ClaireonTool_InputEdit.h"

// Landscape and foliage MCP tools
#include "Tools/ClaireonTool_LandscapeInspect.h"
#include "Tools/ClaireonTool_LandscapeEdit.h"
#include "Tools/ClaireonTool_LandscapeSplineEdit.h"
#include "Tools/ClaireonTool_FoliageEdit.h"
#include "Tools/ClaireonTool_LandscapeImport.h"

// Transaction management
#include "Tools/ClaireonTool_Transaction.h"

// Chooser / Proxy Table MCP tools
#include "Tools/ClaireonTool_ChooserInspect.h"
#include "Tools/ClaireonChooserTools_Lifecycle.h"
#include "Tools/ClaireonChooserTools_Edit.h"
#include "Tools/ClaireonTool_ProxyTableInspect.h"
#include "Tools/ClaireonTool_ProxyAssetInspect.h"
#include "Tools/ClaireonProxyTools_Lifecycle.h"
#include "Tools/ClaireonProxyTools_Edit.h"

DEFINE_LOG_CATEGORY(LogClaireon);

// Define the static FeatureName for IClaireonToolProvider
const FName IClaireonToolProvider::FeatureName = TEXT("ClaireonToolProvider");

#define LOCTEXT_NAMESPACE "ClaireonModule"

// ---------------------------------------------------------------------------
// FClaireonBuiltinToolProvider -- private class that provides all built-in tools
// ---------------------------------------------------------------------------

class FClaireonBuiltinToolProvider : public IClaireonToolProvider
{
public:
	virtual TArray<TSharedPtr<IClaireonTool>> GetTools() const override;
	virtual FName GetProviderName() const override { return TEXT("Claireon Built-in"); }
};

TArray<TSharedPtr<IClaireonTool>> FClaireonBuiltinToolProvider::GetTools() const
{
	TArray<TSharedPtr<IClaireonTool>> Tools;

	Tools.Add(MakeShared<ClaireonTool_AssetReferences>());
	Tools.Add(MakeShared<ClaireonTool_AssetSearch>());
	Tools.Add(MakeShared<ClaireonTool_ExecutePython>());

	// Blueprint MCP tools
	Tools.Add(MakeShared<ClaireonTool_GetBlueprintProperties>());
	Tools.Add(MakeShared<ClaireonTool_GetBlueprintGraph>());
	Tools.Add(MakeShared<ClaireonTool_FormatBlueprintGraph>());
	Tools.Add(MakeShared<ClaireonTool_EditBlueprintGraph>());
	Tools.Add(MakeShared<ClaireonTool_SearchInBlueprints>());

	// New tools (stage 001 stubs -- implementations filled in later stages)
	Tools.Add(MakeShared<ClaireonTool_PythonAuditLog>());
	Tools.Add(MakeShared<ClaireonTool_ProjectInfo>());
	Tools.Add(MakeShared<ClaireonTool_EngineInfo>());
	Tools.Add(MakeShared<ClaireonTool_LiveCodingReload>());
	Tools.Add(MakeShared<ClaireonTool_MapOpen>());
	Tools.Add(MakeShared<ClaireonTool_MapStatus>());
	Tools.Add(MakeShared<ClaireonTool_PIEStart>());
	Tools.Add(MakeShared<ClaireonTool_PIEStop>());
	Tools.Add(MakeShared<ClaireonTool_PIEStatus>());
	Tools.Add(MakeShared<ClaireonTool_PIEGetPlayerPawn>());
	Tools.Add(MakeShared<ClaireonTool_PIEGetActor>());
	Tools.Add(MakeShared<ClaireonTool_PIECheckInitState>());
	Tools.Add(MakeShared<ClaireonTool_PIEWaitFor>());
	Tools.Add(MakeShared<ClaireonTool_PIESpawnEnemy>());
	Tools.Add(MakeShared<ClaireonTool_PIEGetComponent>());
	Tools.Add(MakeShared<ClaireonTool_PIERegisterDamageListener>());
	Tools.Add(MakeShared<ClaireonTool_PIEGetDamageEvents>());
	Tools.Add(MakeShared<ClaireonTool_PIEUnregisterDamageListener>());
	Tools.Add(MakeShared<ClaireonTool_PIEAITargetInfo>());
	Tools.Add(MakeShared<ClaireonTool_PIETestAbility>());
	// Flythrough camera tools
	Tools.Add(MakeShared<ClaireonTool_FlythroughStart>());
	Tools.Add(MakeShared<ClaireonTool_FlythroughStop>());
	Tools.Add(MakeShared<ClaireonTool_FlythroughStatus>());
	Tools.Add(MakeShared<ClaireonTool_PIEScreenshot>());
	Tools.Add(MakeShared<ClaireonTool_PIETraceStart>());
	Tools.Add(MakeShared<ClaireonTool_PIETraceStop>());
	Tools.Add(MakeShared<ClaireonTool_ConsoleExecute>());
	Tools.Add(MakeShared<ClaireonTool_AssetList>());
	Tools.Add(MakeShared<ClaireonTool_AssetValidate>());
	Tools.Add(MakeShared<ClaireonTool_AssetFixupRedirectors>());
	Tools.Add(MakeShared<ClaireonTool_OpenAssetEditor>());
	Tools.Add(MakeShared<ClaireonTool_AnimInspect>());
	// Animation editing tools (individual operations with focused inputSchema)
	Tools.Add(MakeShared<ClaireonAnimTool_Open>());
	Tools.Add(MakeShared<ClaireonAnimTool_Close>());
	Tools.Add(MakeShared<ClaireonAnimTool_GetState>());
	Tools.Add(MakeShared<ClaireonAnimTool_Save>());
	Tools.Add(MakeShared<ClaireonAnimTool_CreateMontage>());
	Tools.Add(MakeShared<ClaireonAnimTool_CreateComposite>());
	Tools.Add(MakeShared<ClaireonAnimTool_DuplicateAsset>());
	Tools.Add(MakeShared<ClaireonAnimTool_AddNotify>());
	Tools.Add(MakeShared<ClaireonAnimTool_RemoveNotify>());
	Tools.Add(MakeShared<ClaireonAnimTool_MoveNotify>());
	Tools.Add(MakeShared<ClaireonAnimTool_DuplicateNotify>());
	Tools.Add(MakeShared<ClaireonAnimTool_SetNotifyProperty>());
	Tools.Add(MakeShared<ClaireonAnimTool_GetNotifyProperty>());
	Tools.Add(MakeShared<ClaireonAnimTool_ListNotifyProperties>());
	Tools.Add(MakeShared<ClaireonAnimTool_AddNotifyTrack>());
	Tools.Add(MakeShared<ClaireonAnimTool_RemoveNotifyTrack>());
	Tools.Add(MakeShared<ClaireonAnimTool_RenameNotifyTrack>());
	Tools.Add(MakeShared<ClaireonAnimTool_ReorderNotifyTrack>());
	Tools.Add(MakeShared<ClaireonAnimTool_AddCurve>());
	Tools.Add(MakeShared<ClaireonAnimTool_RemoveCurve>());
	Tools.Add(MakeShared<ClaireonAnimTool_AddCurveKey>());
	Tools.Add(MakeShared<ClaireonAnimTool_RemoveCurveKey>());
	Tools.Add(MakeShared<ClaireonAnimTool_SetCurveKeyProperty>());
	Tools.Add(MakeShared<ClaireonAnimTool_AddSection>());
	Tools.Add(MakeShared<ClaireonAnimTool_RemoveSection>());
	Tools.Add(MakeShared<ClaireonAnimTool_SetSectionLink>());
	Tools.Add(MakeShared<ClaireonAnimTool_SetSectionLinkMethod>());
	Tools.Add(MakeShared<ClaireonAnimTool_AddSegment>());
	Tools.Add(MakeShared<ClaireonAnimTool_RemoveSegment>());
	Tools.Add(MakeShared<ClaireonAnimTool_SetSegmentProperty>());
	Tools.Add(MakeShared<ClaireonAnimTool_AddSlot>());
	Tools.Add(MakeShared<ClaireonAnimTool_RemoveSlot>());
	Tools.Add(MakeShared<ClaireonAnimTool_SetSlotProperty>());
	Tools.Add(MakeShared<ClaireonAnimTool_InspectSegment>());
	Tools.Add(MakeShared<ClaireonAnimTool_RetimeSegment>());
	Tools.Add(MakeShared<ClaireonAnimTool_BatchRetimeAnimation>());
	Tools.Add(MakeShared<ClaireonAnimTool_ListModifiers>());
	Tools.Add(MakeShared<ClaireonAnimTool_AddModifier>());
	Tools.Add(MakeShared<ClaireonAnimTool_RemoveModifier>());
	Tools.Add(MakeShared<ClaireonAnimTool_ApplyModifier>());
	Tools.Add(MakeShared<ClaireonAnimTool_RevertModifier>());
	Tools.Add(MakeShared<ClaireonAnimTool_ListMetadata>());
	Tools.Add(MakeShared<ClaireonAnimTool_AddMetadata>());
	Tools.Add(MakeShared<ClaireonAnimTool_RemoveMetadata>());
	Tools.Add(MakeShared<ClaireonAnimTool_SetMetadataProperty>());
	Tools.Add(MakeShared<ClaireonAnimTool_SetProperty>());
	// Blend Space / Aim Offset tools
	Tools.Add(MakeShared<ClaireonAnimTool_BlendSpaceCreate>());
	Tools.Add(MakeShared<ClaireonAnimTool_BlendSpaceDuplicate>());
	Tools.Add(MakeShared<ClaireonAnimTool_BlendSpaceDelete>());
	Tools.Add(MakeShared<ClaireonAnimTool_BlendSpaceInspect>());
	Tools.Add(MakeShared<ClaireonAnimTool_BlendSpaceAddSample>());
	Tools.Add(MakeShared<ClaireonAnimTool_BlendSpaceRemoveSample>());
	Tools.Add(MakeShared<ClaireonAnimTool_BlendSpaceEditSample>());
	Tools.Add(MakeShared<ClaireonAnimTool_BlendSpaceSetAxis>());
	Tools.Add(MakeShared<ClaireonAnimTool_BlendSpaceSetInterpolation>());
	Tools.Add(MakeShared<ClaireonAnimTool_BlendSpaceSetProperty>());
	Tools.Add(MakeShared<ClaireonAnimTool_BlendSpaceAddMetadata>());
	Tools.Add(MakeShared<ClaireonAnimTool_BlendSpaceRemoveMetadata>());
	Tools.Add(MakeShared<ClaireonTool_BlueprintCompile>());
	Tools.Add(MakeShared<ClaireonTool_CommandletRun>());
	Tools.Add(MakeShared<ClaireonTool_AssetResave>());
	Tools.Add(MakeShared<ClaireonTool_AssetCook>());
	Tools.Add(MakeShared<ClaireonTool_AssetImportFile>());
	Tools.Add(MakeShared<ClaireonTool_LogTail>());
	Tools.Add(MakeShared<ClaireonTool_LogSearch>());
	Tools.Add(MakeShared<ClaireonTool_TestRun>());
	Tools.Add(MakeShared<ClaireonTool_TestList>());
	// State Tree MCP tools
	Tools.Add(MakeShared<ClaireonTool_StateTreeInspect>());
	Tools.Add(MakeShared<ClaireonTool_StateTreeListNodeTypes>());
	Tools.Add(MakeShared<ClaireonTool_StateTreeEdit>());
	Tools.Add(MakeShared<ClaireonTool_StateTreeRuntimeInspect>());
	Tools.Add(MakeShared<ClaireonTool_StateTreeRuntimeSendEvent>());

	// Diff MCP tools
	Tools.Add(MakeShared<ClaireonTool_AssetDiffProperties>());
	Tools.Add(MakeShared<ClaireonTool_BlueprintDiff>());
	Tools.Add(MakeShared<ClaireonTool_StateTreeDiff>());

	// Trace analysis MCP tools
	Tools.Add(MakeShared<ClaireonTool_TraceOpen>());
	Tools.Add(MakeShared<ClaireonTool_TraceClose>());
	Tools.Add(MakeShared<ClaireonTool_TraceGetSessionInfo>());
	Tools.Add(MakeShared<ClaireonTool_TraceGetFrameStats>());
	Tools.Add(MakeShared<ClaireonTool_TraceGetTopScopes>());
	Tools.Add(MakeShared<ClaireonTool_TraceGetScopeDetails>());
	Tools.Add(MakeShared<ClaireonTool_TraceGetThreads>());

	// Widget Blueprint MCP tools
	Tools.Add(MakeShared<ClaireonTool_GetWidgetBPTree>());
	Tools.Add(MakeShared<ClaireonTool_EditWidgetBP>());

	// Behavior Tree + EQS MCP tools
	Tools.Add(MakeShared<ClaireonTool_BehaviorTreeInspect>());
	Tools.Add(MakeShared<ClaireonTool_BehaviorTreeInspectBlackboard>());
	Tools.Add(MakeShared<ClaireonTool_BehaviorTreeEdit>());
	Tools.Add(MakeShared<ClaireonTool_BlackboardEdit>());
	Tools.Add(MakeShared<ClaireonTool_EQSInspect>());
	Tools.Add(MakeShared<ClaireonTool_EQSEdit>());

	// Niagara MCP tools
	Tools.Add(MakeShared<ClaireonTool_NiagaraInspect>());
	Tools.Add(MakeShared<ClaireonTool_NiagaraEdit>());

	// PCG Graph MCP tools
	Tools.Add(MakeShared<ClaireonTool_PCGGraphInspect>());
	Tools.Add(MakeShared<ClaireonTool_PCGGraphEdit>());

	// Enhanced Input MCP tools
	Tools.Add(MakeShared<ClaireonTool_InputInspect>());
	Tools.Add(MakeShared<ClaireonTool_InputEdit>());

	// Landscape and foliage MCP tools
	Tools.Add(MakeShared<ClaireonTool_LandscapeInspect>());
	Tools.Add(MakeShared<ClaireonTool_LandscapeEdit>());
	Tools.Add(MakeShared<ClaireonTool_LandscapeSplineEdit>());
	Tools.Add(MakeShared<ClaireonTool_FoliageEdit>());
	Tools.Add(MakeShared<ClaireonTool_LandscapeImport>());

	// Session management tools
	Tools.Add(MakeShared<ClaireonTool_ListSessions>());
	Tools.Add(MakeShared<ClaireonTool_ReleaseSessions>());

	// Level tools
	Tools.Add(MakeShared<ClaireonTool_ListActors>());
	Tools.Add(MakeShared<ClaireonTool_LevelSetActorProperty>());
	Tools.Add(MakeShared<ClaireonTool_PlaceActor>());
	Tools.Add(MakeShared<ClaireonTool_MapDuplicate>());
	Tools.Add(MakeShared<ClaireonTool_SetSplinePoints>());

	// Blueprint CDO tools
	Tools.Add(MakeShared<ClaireonTool_SetBlueprintCDOProperty>());
	Tools.Add(MakeShared<ClaireonTool_SetBlueprintMetadata>());

	// Meta tools
	Tools.Add(MakeShared<ClaireonTool_SearchTools>());
	Tools.Add(MakeShared<ClaireonTool_FeedbackSubmit>());

	// Transaction management
	Tools.Add(MakeShared<ClaireonTool_Transaction>());

	// Data Table MCP tools
	Tools.Add(MakeShared<ClaireonTool_DataTableSearch>());
	Tools.Add(MakeShared<ClaireonTool_DataTableGetInfo>());
	Tools.Add(MakeShared<ClaireonTool_DataTableGetRows>());
	Tools.Add(MakeShared<ClaireonTool_DataTableGetRow>());
	Tools.Add(MakeShared<ClaireonTool_DataTableFindRows>());
	Tools.Add(MakeShared<ClaireonTool_DataTableAddRow>());
	Tools.Add(MakeShared<ClaireonTool_DataTableRemoveRow>());
	Tools.Add(MakeShared<ClaireonTool_DataTableDuplicateRow>());
	Tools.Add(MakeShared<ClaireonTool_DataTableRenameRow>());
	Tools.Add(MakeShared<ClaireonTool_DataTableMoveRow>());
	Tools.Add(MakeShared<ClaireonTool_DataTableSetRowValues>());
	Tools.Add(MakeShared<ClaireonTool_DataTableExportJson>());
	Tools.Add(MakeShared<ClaireonTool_DataTableImportJson>());
	Tools.Add(MakeShared<ClaireonTool_DataTableExportCsv>());
	Tools.Add(MakeShared<ClaireonTool_DataTableImportCsv>());

	// Chooser Table MCP tools
	Tools.Add(MakeShared<ClaireonTool_ChooserInspect>());
	Tools.Add(MakeShared<ClaireonTool_ChooserCreate>());
	Tools.Add(MakeShared<ClaireonTool_ChooserDuplicate>());
	Tools.Add(MakeShared<ClaireonTool_ChooserEdit>());
	Tools.Add(MakeShared<ClaireonTool_ChooserAddRow>());
	Tools.Add(MakeShared<ClaireonTool_ChooserRemoveRow>());
	Tools.Add(MakeShared<ClaireonTool_ChooserSetRowResult>());
	Tools.Add(MakeShared<ClaireonTool_ChooserSetColumnValue>());
	Tools.Add(MakeShared<ClaireonTool_ChooserAddColumn>());
	Tools.Add(MakeShared<ClaireonTool_ChooserRemoveColumn>());

	// Proxy Table / Proxy Asset MCP tools
	Tools.Add(MakeShared<ClaireonTool_ProxyTableInspect>());
	Tools.Add(MakeShared<ClaireonTool_ProxyAssetInspect>());
	Tools.Add(MakeShared<ClaireonTool_ProxyTableCreate>());
	Tools.Add(MakeShared<ClaireonTool_ProxyTableDuplicate>());
	Tools.Add(MakeShared<ClaireonTool_ProxyAssetCreate>());
	Tools.Add(MakeShared<ClaireonTool_ProxyAssetDuplicate>());
	Tools.Add(MakeShared<ClaireonTool_ProxyTableEdit>());
	Tools.Add(MakeShared<ClaireonTool_ProxyAssetEdit>());
	Tools.Add(MakeShared<ClaireonTool_ProxyTableAddEntry>());
	Tools.Add(MakeShared<ClaireonTool_ProxyTableRemoveEntry>());
	Tools.Add(MakeShared<ClaireonTool_ProxyTableSetEntryValue>());

	return Tools;
}

// ---------------------------------------------------------------------------
// FClaireonModule
// ---------------------------------------------------------------------------

void FClaireonModule::StartupModule()
{
	if (!GIsEditor || IsRunningCommandlet())
	{
		return;
	}

	SClaireonDiagnosticsWidget::RegisterTabSpawner();

	// Settings registration is handled automatically by UDeveloperSettings via
	// GetCategoryName() -> "Plugins" and meta=(DisplayName="Claireon").
	// Manual RegisterSettings was removed — it caused duplicate entries.

	FClaireonRichTextStyle::Initialize();

	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FClaireonModule::RegisterMenus));

	// Bind PIE manager to editor delegates so it detects all PIE sessions
	FClaireonPIEManager::Get().BindEditorDelegates();

	// Register built-in tool provider
	BuiltinToolProvider = MakeUnique<FClaireonBuiltinToolProvider>();
	IModularFeatures::Get().RegisterModularFeature(
		IClaireonToolProvider::FeatureName, BuiltinToolProvider.Get());

	// Listen for modular feature registration/unregistration
	IModularFeatures::Get().OnModularFeatureRegistered().AddRaw(
		this, &FClaireonModule::OnModularFeatureRegistered);
	IModularFeatures::Get().OnModularFeatureUnregistered().AddRaw(
		this, &FClaireonModule::OnModularFeatureUnregistered);

	// Check for -StartMCPServer command-line flag
	if (FParse::Param(FCommandLine::Get(), TEXT("StartMCPServer")))
	{
		UE_LOG(LogClaireon, Display, TEXT("[MCP] Auto-starting server (command-line flag)"));
		StartServer();
	}
}

void FClaireonModule::ShutdownModule()
{
	// Reset transaction group state (safety net for module shutdown)
	ClaireonTool_Transaction::ResetGroupState();

	FClaireonPIEManager::Get().UnbindEditorDelegates();
	FClaireonRichTextStyle::Shutdown();

	// Unsubscribe from modular feature events
	IModularFeatures::Get().OnModularFeatureRegistered().RemoveAll(this);
	IModularFeatures::Get().OnModularFeatureUnregistered().RemoveAll(this);

	// Unsubscribe from settings changes
	if (SettingsChangedHandle.IsValid())
	{
		if (UObjectInitialized())
		{
			if (UClaireonSettings* MutableSettings = GetMutableDefault<UClaireonSettings>())
			{
				MutableSettings->OnSettingsChanged.Remove(SettingsChangedHandle);
			}
		}
		SettingsChangedHandle.Reset();
	}

	StopServer();
	SClaireonDiagnosticsWidget::UnregisterTabSpawner();

	// Unregister built-in tool provider
	if (BuiltinToolProvider)
	{
		IModularFeatures::Get().UnregisterModularFeature(
			IClaireonToolProvider::FeatureName, BuiltinToolProvider.Get());
		BuiltinToolProvider.Reset();
	}
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

	// Collect tools from all registered providers
	CollectToolsFromProviders();

	// Determine port: command-line override > settings > hardcoded default
	uint32 Port = 8017;
	FString PortStr;
	if (FParse::Value(FCommandLine::Get(), TEXT("-MCPServerPort="), PortStr))
	{
		Port = FCString::Atoi(*PortStr);
		if (Port == 0)
		{
			UE_LOG(LogClaireon, Warning, TEXT("[MCP] Invalid port from command line, using default"));
			Port = 8017;
		}
		else
		{
			bPortOverriddenByCommandLine = true;
		}
	}
	else if (const UClaireonSettings* Settings = UClaireonSettings::Get())
	{
		Port = Settings->ServerPort;
	}

	if (!Server->Start(Port))
	{
		UE_LOG(LogClaireon, Error, TEXT("[MCP] Failed to start server"));
		Server.Reset();
		return;
	}

	// Subscribe to settings changes for runtime port reconfiguration
	if (!SettingsChangedHandle.IsValid())
	{
		UClaireonSettings* MutableSettings = GetMutableDefault<UClaireonSettings>();
		SettingsChangedHandle = MutableSettings->OnSettingsChanged.AddLambda([this]()
		{
			if (bPortOverriddenByCommandLine)
			{
				UE_LOG(LogClaireon, Warning,
					TEXT("[MCP] Port change in settings ignored -- overridden by -MCPServerPort command line"));
				return;
			}

			const UClaireonSettings* Settings = UClaireonSettings::Get();
			if (!Settings || !Server.IsValid() || !Server->IsRunning())
			{
				return;
			}

			const uint32 NewPort = Settings->ServerPort;
			if (NewPort != Server->GetPort())
			{
				UE_LOG(LogClaireon, Display,
					TEXT("[MCP] Port changed from %u to %u -- restarting server"), Server->GetPort(), NewPort);
				StopServer();
				StartServer();
			}
		});
	}
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

void FClaireonModule::CollectToolsFromProviders()
{
	if (!Server.IsValid())
	{
		return;
	}

	TArray<IClaireonToolProvider*> Providers =
		IModularFeatures::Get().GetModularFeatureImplementations<IClaireonToolProvider>(
			IClaireonToolProvider::FeatureName);

	for (IClaireonToolProvider* Provider : Providers)
	{
		CollectToolsFromProvider(Provider);
	}
}

void FClaireonModule::CollectToolsFromProvider(IClaireonToolProvider* Provider)
{
	if (!Server.IsValid() || !Provider)
	{
		return;
	}

	const FName ProviderName = Provider->GetProviderName();
	TArray<TSharedPtr<IClaireonTool>> Tools = Provider->GetTools();

	for (const TSharedPtr<IClaireonTool>& Tool : Tools)
	{
		if (!Tool.IsValid())
		{
			continue;
		}

		const FString ToolName = Tool->GetName();

		// Check for name collision and log a warning
		const TMap<FString, FName>& SourceMap = Server->GetToolSourceMap();
		if (const FName* ExistingSource = SourceMap.Find(ToolName))
		{
			UE_LOG(LogClaireon, Warning,
				TEXT("[MCP] Tool name collision: '%s' from provider '%s' overrides existing from '%s'"),
				*ToolName, *ProviderName.ToString(), *ExistingSource->ToString());
		}

		Server->RegisterTool(Tool, ProviderName);
	}
}

void FClaireonModule::OnModularFeatureRegistered(const FName& Type, IModularFeature* ModularFeature)
{
	if (Type != IClaireonToolProvider::FeatureName)
	{
		return;
	}

	IClaireonToolProvider* Provider = static_cast<IClaireonToolProvider*>(ModularFeature);

	if (!IsInGameThread())
	{
		AsyncTask(ENamedThreads::GameThread, [this, Provider]()
		{
			if (Server.IsValid())
			{
				CollectToolsFromProvider(Provider);
			}
		});
		return;
	}

	if (Server.IsValid())
	{
		CollectToolsFromProvider(Provider);
	}
}

void FClaireonModule::OnModularFeatureUnregistered(const FName& Type, IModularFeature* ModularFeature)
{
	if (Type != IClaireonToolProvider::FeatureName)
	{
		return;
	}

	IClaireonToolProvider* Provider = static_cast<IClaireonToolProvider*>(ModularFeature);

	if (!IsInGameThread())
	{
		FName ProviderName = Provider->GetProviderName();
		AsyncTask(ENamedThreads::GameThread, [this, ProviderName]()
		{
			if (Server.IsValid())
			{
				Server->UnregisterToolsBySource(ProviderName);
			}
		});
		return;
	}

	if (Server.IsValid())
	{
		Server->UnregisterToolsBySource(Provider->GetProviderName());
	}
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
