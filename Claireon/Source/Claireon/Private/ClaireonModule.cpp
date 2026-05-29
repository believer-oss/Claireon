// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonModule.h"
#include "ClaireonLog.h"
#include "ClaireonPIEManager.h"
#include "ClaireonRichTextStyle.h"
#include "ClaireonToolbarStyle.h"
#include "Styling/SlateStyle.h" // Full definition of FSlateStyleSet (ClaireonToolbarStyle.h forward-declares only)
#include "ClaireonServer.h"
#include "ClaireonProxyClient.h"
#include "ClaireonPortDerivation.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SToolTip.h"
#include "ClaireonBridge.h"
#include "ClaireonDiagnosticsWidget.h"
#include "ClaireonSettings.h"
#include "IClaireonToolProvider.h"
#include "Modules/ModuleManager.h"
#include "Features/IModularFeatures.h"
#include "ToolMenus.h"
#include "Styling/AppStyle.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformMisc.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Containers/Ticker.h"
#include "Async/Async.h"
#include "Tools/ClaireonTool_AssetReferences.h"
#include "Tools/ClaireonTool_AssetSearch.h"
#include "Tools/ClaireonTool_ExecutePython.h"
#include "Tools/ClaireonTool_GetBlueprintProperties.h"
#include "Tools/ClaireonTool_GetBlueprintGraph.h"
#include "Tools/ClaireonTool_SearchInBlueprints.h"
#include "Tools/ClaireonTool_ApplyBlueprintDelta.h"
#include "Tools/ClaireonTool_GameplayTagsList.h"
#include "Tools/ClaireonTool_StructInspect.h"
#include "Tools/ClaireonTool_UObjectInspect.h"
#include "Tools/ClaireonTool_ReplaceStructUsage.h"
#include "Tools/ClaireonAnimGraphTools_CopyGraph.h"

// Blueprint graph editing (decomposed -- one tool per operation, stage 016)
#include "Tools/ClaireonBlueprintGraphTool_Open.h"
#include "Tools/ClaireonBlueprintGraphTool_Create.h"
#include "Tools/ClaireonBlueprintGraphTool_ListGraphs.h"
#include "Tools/ClaireonBlueprintGraphTool_AddNode.h"
#include "Tools/ClaireonBlueprintGraphTool_RemoveNode.h"
#include "Tools/ClaireonBlueprintGraphTool_ReconstructNode.h"
#include "Tools/ClaireonBlueprintGraphTool_SetGameplayTags.h"
#include "Tools/ClaireonBlueprintGraphTool_SuggestNode.h"
#include "Tools/ClaireonBlueprintGraphTool_ConnectPins.h"
#include "Tools/ClaireonBlueprintGraphTool_DisconnectPin.h"
#include "Tools/ClaireonBlueprintGraphTool_SetPinValue.h"
#include "Tools/ClaireonBlueprintGraphTool_AddPin.h"
#include "Tools/ClaireonBlueprintGraphTool_RemovePin.h"
#include "Tools/ClaireonBlueprintGraphTool_SplitPin.h"
#include "Tools/ClaireonBlueprintGraphTool_RecombinePin.h"
#include "Tools/ClaireonBlueprintGraphTool_AddVariable.h"
#include "Tools/ClaireonBlueprintGraphTool_SetVariableProperties.h"
#include "Tools/ClaireonBlueprintGraphTool_RemoveVariable.h"
#include "Tools/ClaireonBlueprintGraphTool_AddComponent.h"
#include "Tools/ClaireonBlueprintGraphTool_SetProperty.h"
#include "Tools/ClaireonBlueprintGraphTool_RemoveComponent.h"
#include "Tools/ClaireonBlueprintGraphTool_ReparentComponent.h"
#include "Tools/ClaireonBlueprintGraphTool_RenameComponent.h"
#include "Tools/ClaireonBlueprintGraphTool_SetRootComponent.h"
#include "Tools/ClaireonBlueprintGraphTool_GetComponentDetails.h"
#include "Tools/ClaireonBlueprintGraphTool_MoveCursor.h"
#include "Tools/ClaireonBlueprintGraphTool_CursorBack.h"
#include "Tools/ClaireonBlueprintGraphTool_SwitchGraph.h"
#include "Tools/ClaireonBlueprintGraphTool_InspectNode.h"
#include "Tools/ClaireonBlueprintGraphTool_SelectNode.h"
#include "Tools/ClaireonBlueprintGraphTool_SelectPin.h"
#include "Tools/ClaireonBlueprintGraphTool_SelectNearestNode.h"
#include "Tools/ClaireonBlueprintGraphTool_GetState.h"
#include "Tools/ClaireonBlueprintGraphTool_ImportNodes.h"
#include "Tools/ClaireonBlueprintGraphTool_Compile.h"
#include "Tools/ClaireonBlueprintGraphTool_Save.h"
#include "Tools/ClaireonBlueprintGraphTool_Format.h"
#include "Tools/ClaireonBlueprintGraphTool_Close.h"
#include "Tools/ClaireonBlueprintGraphTool_MoveNode.h"
#include "Tools/ClaireonBlueprintGraphTool_AddFunction.h"
#include "Tools/ClaireonBlueprintGraphTool_AddFunctionOverride.h"
#include "Tools/ClaireonBlueprintGraphTool_AddInterface.h"
#include "Tools/ClaireonBlueprintGraphTool_ImplementInterface.h"
#include "Tools/ClaireonBlueprintGraphTool_RemoveInterface.h"
#include "Tools/ClaireonBlueprintGraphTool_ApplySpec.h"

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
#include "Tools/ClaireonTool_BlueprintCompileBatch.h"
#include "Tools/ClaireonTool_BlueprintDuplicate.h"
#include "Tools/ClaireonTool_CommandletRun.h"
#include "Tools/ClaireonTool_AssetResave.h"
#include "Tools/ClaireonTool_AssetCook.h"
#include "Tools/ClaireonTool_LogTail.h"
#include "Tools/ClaireonTool_LogSearch.h"
#include "Tools/ClaireonTool_MessageLogGet.h"
#include "Tools/ClaireonTool_TestRun.h"
#include "Tools/ClaireonTool_TestList.h"
// State Tree MCP tools
#include "Tools/ClaireonTool_StateTreeInspect.h"
#include "Tools/ClaireonTool_StateTreeListNodeTypes.h"
#include "Tools/ClaireonTool_StateTreeGetSchema.h"
#include "Tools/ClaireonTool_StateTreeListBindingSources.h"
#include "Tools/ClaireonTool_StateTreeRuntimeInspect.h"
#include "Tools/ClaireonTool_StateTreeRuntimeSendEvent.h"
#include "Tools/ClaireonStateTreeTool_Open.h"
#include "Tools/ClaireonStateTreeTool_Close.h"
#include "Tools/ClaireonStateTreeTool_Status.h"
#include "Tools/ClaireonStateTreeTool_AddState.h"
#include "Tools/ClaireonStateTreeTool_RemoveState.h"
#include "Tools/ClaireonStateTreeTool_RenameState.h"
#include "Tools/ClaireonStateTreeTool_MoveState.h"
#include "Tools/ClaireonStateTreeTool_SetStateType.h"
#include "Tools/ClaireonStateTreeTool_SetStateSelectionBehavior.h"
#include "Tools/ClaireonStateTreeTool_SetStateEnabled.h"
#include "Tools/ClaireonStateTreeTool_AddTask.h"
#include "Tools/ClaireonStateTreeTool_RemoveTask.h"
#include "Tools/ClaireonStateTreeTool_AddEnterCondition.h"
#include "Tools/ClaireonStateTreeTool_RemoveEnterCondition.h"
#include "Tools/ClaireonStateTreeTool_AddConsideration.h"
#include "Tools/ClaireonStateTreeTool_RemoveConsideration.h"
#include "Tools/ClaireonStateTreeTool_AddTransition.h"
#include "Tools/ClaireonStateTreeTool_RemoveTransition.h"
#include "Tools/ClaireonStateTreeTool_ModifyTransition.h"
#include "Tools/ClaireonStateTreeTool_AddTransitionCondition.h"
#include "Tools/ClaireonStateTreeTool_RemoveTransitionCondition.h"
#include "Tools/ClaireonStateTreeTool_AddEvaluator.h"
#include "Tools/ClaireonStateTreeTool_RemoveEvaluator.h"
#include "Tools/ClaireonStateTreeTool_AddGlobalTask.h"
#include "Tools/ClaireonStateTreeTool_RemoveGlobalTask.h"
#include "Tools/ClaireonStateTreeTool_AddBinding.h"
#include "Tools/ClaireonStateTreeTool_AddPropertyFunction.h"
#include "Tools/ClaireonStateTreeTool_RemoveBinding.h"
#include "Tools/ClaireonStateTreeTool_SetNodeProperty.h"
#include "Tools/ClaireonStateTreeTool_SetStateProperty.h"
#include "Tools/ClaireonStateTreeTool_SetTransitionProperty.h"
#include "Tools/ClaireonStateTreeTool_SetSchemaProperty.h"
#include "Tools/ClaireonStateTreeTool_Compile.h"
#include "Tools/ClaireonStateTreeTool_Save.h"
#include "Tools/ClaireonStateTreeTool_ApplySpec.h"

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
#include "Tools/ClaireonTool_ApplySpecHelp.h"

// Widget Blueprint MCP tools
#include "Tools/ClaireonTool_GetWidgetBPTree.h"
// Widget Blueprint editing (decomposed -- one tool per operation)
#include "Tools/ClaireonWidgetBPTool_Open.h"
#include "Tools/ClaireonWidgetBPTool_Create.h"
#include "Tools/ClaireonWidgetBPTool_GetState.h"
#include "Tools/ClaireonWidgetBPTool_Focus.h"
#include "Tools/ClaireonWidgetBPTool_Compile.h"
#include "Tools/ClaireonWidgetBPTool_Save.h"
#include "Tools/ClaireonWidgetBPTool_Close.h"
#include "Tools/ClaireonWidgetBPTool_AddWidget.h"
#include "Tools/ClaireonWidgetBPTool_RemoveWidget.h"
#include "Tools/ClaireonWidgetBPTool_MoveWidget.h"
#include "Tools/ClaireonWidgetBPTool_ReplaceWidget.h"
#include "Tools/ClaireonWidgetBPTool_RenameWidget.h"
#include "Tools/ClaireonWidgetBPTool_SetWidgetProperty.h"
#include "Tools/ClaireonWidgetBPTool_SetSlotProperty.h"
#include "Tools/ClaireonWidgetBPTool_GetWidgetDetails.h"
#include "Tools/ClaireonWidgetBPTool_AddBinding.h"
#include "Tools/ClaireonWidgetBPTool_RemoveBinding.h"
#include "Tools/ClaireonWidgetBPTool_ListAnimations.h"
#include "Tools/ClaireonWidgetBPTool_ImportWidgets.h"
#include "Tools/ClaireonWidgetBPTool_ExportWidgets.h"
#include "Tools/ClaireonWidgetBPTool_ListWidgetClasses.h"
#include "Tools/ClaireonWidgetBPTool_ListMVVMViewModels.h"
#include "Tools/ClaireonWidgetBPTool_AddMVVMViewModel.h"
#include "Tools/ClaireonWidgetBPTool_RemoveMVVMViewModel.h"
#include "Tools/ClaireonWidgetBPTool_ListMVVMBindings.h"
#include "Tools/ClaireonWidgetBPTool_AddMVVMBinding.h"
#include "Tools/ClaireonWidgetBPTool_EditMVVMBinding.h"
#include "Tools/ClaireonWidgetBPTool_RemoveMVVMBinding.h"
#include "Tools/ClaireonWidgetBPTool_ApplySpec.h"
// Widget Blueprint animation extras (stubbed pending dispatch backlog; see stage 017 commit)
#include "Tools/ClaireonWidgetBPTool_CreateAnimation.h"
#include "Tools/ClaireonWidgetBPTool_DeleteAnimation.h"
#include "Tools/ClaireonWidgetBPTool_RenameAnimation.h"
#include "Tools/ClaireonWidgetBPTool_DuplicateAnimation.h"
#include "Tools/ClaireonWidgetBPTool_GetAnimationDetails.h"
#include "Tools/ClaireonWidgetBPTool_AddAnimationBinding.h"
#include "Tools/ClaireonWidgetBPTool_AddAnimationTrack.h"
#include "Tools/ClaireonWidgetBPTool_AddAnimationKeyframe.h"
#include "Tools/ClaireonWidgetBPTool_RemoveAnimationKeyframe.h"
#include "Tools/ClaireonWidgetBPTool_SetAnimationProperty.h"

// Behavior Tree + EQS MCP tools
#include "Tools/ClaireonTool_BehaviorTreeInspect.h"
#include "Tools/ClaireonTool_BehaviorTreeInspectBlackboard.h"
// Behavior Tree editing (decomposed -- one tool per operation)
#include "Tools/ClaireonBehaviorTreeTool_Open.h"
#include "Tools/ClaireonBehaviorTreeTool_Close.h"
#include "Tools/ClaireonBehaviorTreeTool_Status.h"
#include "Tools/ClaireonBehaviorTreeTool_AddNode.h"
#include "Tools/ClaireonBehaviorTreeTool_RemoveNode.h"
#include "Tools/ClaireonBehaviorTreeTool_MoveNode.h"
#include "Tools/ClaireonBehaviorTreeTool_SetNodeProperty.h"
#include "Tools/ClaireonBehaviorTreeTool_AddDecorator.h"
#include "Tools/ClaireonBehaviorTreeTool_RemoveDecorator.h"
#include "Tools/ClaireonBehaviorTreeTool_AddService.h"
#include "Tools/ClaireonBehaviorTreeTool_RemoveService.h"
#include "Tools/ClaireonBehaviorTreeTool_SetSubtreeAsset.h"
#include "Tools/ClaireonBehaviorTreeTool_UpdateAsset.h"
#include "Tools/ClaireonBehaviorTreeTool_Save.h"
#include "Tools/ClaireonBehaviorTreeTool_ListNodeTypes.h"
#include "Tools/ClaireonBehaviorTreeTool_ApplySpec.h"
// Blackboard editing (decomposed -- one tool per operation)
#include "Tools/ClaireonBlackboardTool_Open.h"
#include "Tools/ClaireonBlackboardTool_Close.h"
#include "Tools/ClaireonBlackboardTool_Status.h"
#include "Tools/ClaireonBlackboardTool_AddKey.h"
#include "Tools/ClaireonBlackboardTool_RemoveKey.h"
#include "Tools/ClaireonBlackboardTool_RenameKey.h"
#include "Tools/ClaireonBlackboardTool_SetKeyType.h"
#include "Tools/ClaireonBlackboardTool_SetParent.h"
#include "Tools/ClaireonBlackboardTool_Save.h"
#include "Tools/ClaireonBlackboardTool_ApplySpec.h"
#include "Tools/ClaireonTool_EQSInspect.h"
// EQS editing (decomposed -- one tool per operation)
#include "Tools/ClaireonEQSTool_Open.h"
#include "Tools/ClaireonEQSTool_CreateNew.h"
#include "Tools/ClaireonEQSTool_Close.h"
#include "Tools/ClaireonEQSTool_Status.h"
#include "Tools/ClaireonEQSTool_Save.h"
#include "Tools/ClaireonEQSTool_ApplySpec.h"
#include "Tools/ClaireonEQSTool_AddOption.h"
#include "Tools/ClaireonEQSTool_RemoveOption.h"
#include "Tools/ClaireonEQSTool_SetGenerator.h"
#include "Tools/ClaireonEQSTool_AddTest.h"
#include "Tools/ClaireonEQSTool_RemoveTest.h"
#include "Tools/ClaireonEQSTool_ReorderTests.h"
#include "Tools/ClaireonEQSTool_SetNodeProperty.h"
#include "Tools/ClaireonTool_NiagaraInspect.h"
#include "Tools/ClaireonNiagaraTool_Open.h"
#include "Tools/ClaireonNiagaraTool_Close.h"
#include "Tools/ClaireonNiagaraTool_Status.h"
#include "Tools/ClaireonNiagaraTool_FocusEmitter.h"
#include "Tools/ClaireonNiagaraTool_Create.h"
#include "Tools/ClaireonNiagaraTool_AddEmitter.h"
#include "Tools/ClaireonNiagaraTool_RemoveEmitter.h"
#include "Tools/ClaireonNiagaraTool_RenameEmitter.h"
#include "Tools/ClaireonNiagaraTool_SetEmitterEnabled.h"
#include "Tools/ClaireonNiagaraTool_AddRenderer.h"
#include "Tools/ClaireonNiagaraTool_RemoveRenderer.h"
#include "Tools/ClaireonNiagaraTool_SetRendererProperty.h"
#include "Tools/ClaireonNiagaraTool_SetEmitterProperty.h"
#include "Tools/ClaireonNiagaraTool_ListModules.h"
#include "Tools/ClaireonNiagaraTool_AddModule.h"
#include "Tools/ClaireonNiagaraTool_RemoveModule.h"
#include "Tools/ClaireonNiagaraTool_GetModuleInputs.h"
#include "Tools/ClaireonNiagaraTool_SetModuleInput.h"
#include "Tools/ClaireonNiagaraTool_SetSystemProperty.h"
#include "Tools/ClaireonNiagaraTool_AddParameter.h"
#include "Tools/ClaireonNiagaraTool_RemoveParameter.h"
#include "Tools/ClaireonNiagaraTool_SetParameterValue.h"
#include "Tools/ClaireonNiagaraTool_Compile.h"
#include "Tools/ClaireonNiagaraTool_Save.h"
#include "Tools/ClaireonNiagaraTool_ApplySpec.h"

// Level Sequence MCP tools
#include "Tools/ClaireonTool_SequenceInspect.h"
#include "Tools/ClaireonTool_SequenceListTrackTypes.h"
#include "Tools/ClaireonTool_SequenceActorPlace.h"

// Level Sequence editing (decomposed -- one tool per operation)
#include "Tools/ClaireonLevelSequenceTool_Open.h"
#include "Tools/ClaireonLevelSequenceTool_ApplySpec.h"
#include "Tools/ClaireonLevelSequenceTool_Close.h"
#include "Tools/ClaireonLevelSequenceTool_GetState.h"
#include "Tools/ClaireonLevelSequenceTool_Save.h"
#include "Tools/ClaireonLevelSequenceTool_FocusBinding.h"
#include "Tools/ClaireonLevelSequenceTool_FocusTrack.h"
#include "Tools/ClaireonLevelSequenceTool_AddPossessable.h"
#include "Tools/ClaireonLevelSequenceTool_RemovePossessable.h"
#include "Tools/ClaireonLevelSequenceTool_RebindActor.h"
#include "Tools/ClaireonLevelSequenceTool_AddSpawnable.h"
#include "Tools/ClaireonLevelSequenceTool_AddTrack.h"
#include "Tools/ClaireonLevelSequenceTool_RemoveTrack.h"
#include "Tools/ClaireonLevelSequenceTool_SetTrackProperty.h"
#include "Tools/ClaireonLevelSequenceTool_AddSection.h"
#include "Tools/ClaireonLevelSequenceTool_RemoveSection.h"
#include "Tools/ClaireonLevelSequenceTool_AddKeyframe.h"
#include "Tools/ClaireonLevelSequenceTool_RemoveKeyframe.h"
#include "Tools/ClaireonLevelSequenceTool_SetPlaybackRange.h"
#include "Tools/ClaireonLevelSequenceTool_AddEventKey.h"
#include "Tools/ClaireonLevelSequenceTool_CreateEventEndpoint.h"

// Material MCP tools
#include "Tools/ClaireonTool_MaterialInspect.h"
#include "Tools/ClaireonTool_MaterialInstanceInspect.h"
#include "Tools/ClaireonTool_MaterialApply.h"

// Material editing (decomposed -- one tool per operation)
#include "Tools/ClaireonMaterialTool_Open.h"
#include "Tools/ClaireonMaterialTool_Create.h"
#include "Tools/ClaireonMaterialTool_ApplySpec.h"
#include "Tools/ClaireonMaterialTool_Close.h"
#include "Tools/ClaireonMaterialTool_Status.h"
#include "Tools/ClaireonMaterialTool_Save.h"
#include "Tools/ClaireonMaterialTool_Compile.h"
#include "Tools/ClaireonMaterialTool_AddExpression.h"
#include "Tools/ClaireonMaterialTool_RemoveExpression.h"
#include "Tools/ClaireonMaterialTool_ConnectExpressions.h"
#include "Tools/ClaireonMaterialTool_DisconnectExpressionInput.h"
#include "Tools/ClaireonMaterialTool_ConnectToMaterialOutput.h"
#include "Tools/ClaireonMaterialTool_SetExpressionProperty.h"
#include "Tools/ClaireonMaterialTool_SetParameterDefault.h"
#include "Tools/ClaireonMaterialTool_SetShadingModel.h"
#include "Tools/ClaireonMaterialTool_SetBlendMode.h"

// Material instance editing (decomposed -- one tool per operation)
#include "Tools/ClaireonMaterialInstanceTool_Open.h"
#include "Tools/ClaireonMaterialInstanceTool_Create.h"
#include "Tools/ClaireonMaterialInstanceTool_ApplySpec.h"
#include "Tools/ClaireonMaterialInstanceTool_Close.h"
#include "Tools/ClaireonMaterialInstanceTool_Status.h"
#include "Tools/ClaireonMaterialInstanceTool_Save.h"
#include "Tools/ClaireonMaterialInstanceTool_SetParent.h"
#include "Tools/ClaireonMaterialInstanceTool_SetScalarParameter.h"
#include "Tools/ClaireonMaterialInstanceTool_SetVectorParameter.h"
#include "Tools/ClaireonMaterialInstanceTool_SetTextureParameter.h"
#include "Tools/ClaireonMaterialInstanceTool_SetStaticSwitchParameter.h"
#include "Tools/ClaireonMaterialInstanceTool_SetStaticComponentMaskParameter.h"
#include "Tools/ClaireonMaterialInstanceTool_ClearParameterOverride.h"

// Audio MCP tools
#include "Tools/ClaireonTool_AudioInspect.h"
#include "Tools/ClaireonTool_AudioApply.h"

// Audio decomposed tools (42)
#include "Tools/ClaireonSoundCueTool_Open.h"
#include "Tools/ClaireonSoundCueTool_Close.h"
#include "Tools/ClaireonSoundCueTool_Status.h"
#include "Tools/ClaireonSoundCueTool_Save.h"
#include "Tools/ClaireonSoundCueTool_AddNode.h"
#include "Tools/ClaireonSoundCueTool_RemoveNode.h"
#include "Tools/ClaireonSoundCueTool_ConnectNodes.h"
#include "Tools/ClaireonSoundCueTool_DisconnectNodes.h"
#include "Tools/ClaireonSoundCueTool_SetNodeProperty.h"
#include "Tools/ClaireonSoundCueTool_SetNodePosition.h"
#include "Tools/ClaireonSoundCueTool_SetFocusedNode.h"
#include "Tools/ClaireonSoundCueTool_Create.h"
#include "Tools/ClaireonSoundCueTool_ListNodeTypes.h"
#include "Tools/ClaireonSoundCueTool_ApplySpec.h"
#include "Tools/ClaireonMetaSoundTool_Open.h"
#include "Tools/ClaireonMetaSoundTool_Close.h"
#include "Tools/ClaireonMetaSoundTool_Status.h"
#include "Tools/ClaireonMetaSoundTool_Save.h"
#include "Tools/ClaireonMetaSoundTool_AddInput.h"
#include "Tools/ClaireonMetaSoundTool_AddOutput.h"
#include "Tools/ClaireonMetaSoundTool_AddNode.h"
#include "Tools/ClaireonMetaSoundTool_SetDefault.h"
#include "Tools/ClaireonMetaSoundTool_ConnectPins.h"
#include "Tools/ClaireonMetaSoundTool_Create.h"
#include "Tools/ClaireonMetaSoundTool_ApplySpec.h"
#include "Tools/ClaireonCameraAssetTool_Create.h"
#include "Tools/ClaireonCameraAssetTool_Duplicate.h"
#include "Tools/ClaireonCameraAssetTool_Save.h"
#include "Tools/ClaireonCameraAssetTool_Compile.h"
#include "Tools/ClaireonCameraAssetTool_ListRigs.h"
#include "Tools/ClaireonCameraAssetTool_AddRig.h"
#include "Tools/ClaireonCameraAssetTool_ListNodes.h"
#include "Tools/ClaireonCameraAssetTool_ListNodeClasses.h"
#include "Tools/ClaireonCameraAssetTool_AddNode.h"
#include "Tools/ClaireonCameraAssetTool_RemoveNode.h"
#include "Tools/ClaireonCameraAssetTool_MoveNode.h"
#include "Tools/ClaireonCameraAssetTool_GetNodeProperty.h"
#include "Tools/ClaireonCameraAssetTool_SetNodeProperty.h"
#include "Tools/ClaireonSoundClassTool_SetProperty.h"
#include "Tools/ClaireonSoundClassTool_AddChild.h"
#include "Tools/ClaireonSoundClassTool_RemoveChild.h"
#include "Tools/ClaireonSoundClassTool_Create.h"
#include "Tools/ClaireonSoundClassTool_ApplySpec.h"
#include "Tools/ClaireonSoundMixTool_AddClassAdjuster.h"
#include "Tools/ClaireonSoundMixTool_RemoveClassAdjuster.h"
#include "Tools/ClaireonSoundMixTool_SetClassAdjusterProperty.h"
#include "Tools/ClaireonSoundMixTool_SetEnvelope.h"
#include "Tools/ClaireonSoundMixTool_Create.h"
#include "Tools/ClaireonSoundMixTool_ApplySpec.h"
#include "Tools/ClaireonAttenuationTool_SetProperty.h"
#include "Tools/ClaireonAttenuationTool_Create.h"
#include "Tools/ClaireonAttenuationTool_ApplySpec.h"
#include "Tools/ClaireonConcurrencyTool_SetProperty.h"
#include "Tools/ClaireonConcurrencyTool_Create.h"
#include "Tools/ClaireonConcurrencyTool_ApplySpec.h"

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

// Runtime Diagnostics (snapshot CMC / anim / motion-warp / tick state on a paused-PIE pawn)
#include "Tools/ClaireonAnimInspectTool.h"
#include "Tools/ClaireonCMCInspectTool.h"
#include "Tools/ClaireonComponentTickInspectTool.h"

// Animation MCP tools
#include "Tools/ClaireonTool_AnimInspect.h"
#include "Tools/ClaireonTool_AnimInvariantsCheck.h"
#include "Tools/ClaireonTool_AssetCheckInnerNameInvariant.h"
#include "Tools/ClaireonAnimTools_Session.h"
#include "Tools/ClaireonAnimTools_Create.h"
#include "Tools/ClaireonAnimTools_Notify.h"
#include "Tools/ClaireonAnimTools_Curve.h"
#include "Tools/ClaireonAnimTools_Montage.h"
#include "Tools/ClaireonAnimTools_DataOps.h"
#include "Tools/ClaireonAnimTools_BlendSpace.h"

// Skeleton MCP tools
#include "Tools/ClaireonSkeletonTools_Inspect.h"
#include "Tools/ClaireonSkeletonTools_VirtualBones.h"
#include "Tools/ClaireonSkeletonTools_Sockets.h"
#include "Tools/ClaireonSkeletonTools_Metadata.h"
#include "Tools/ClaireonSkeletonTools_BlendProfiles.h"

// Session management MCP tools
#include "Tools/ClaireonTool_ListSessions.h"
#include "Tools/ClaireonTool_ReleaseSessions.h"

// PCG Graph MCP tools
#include "Tools/ClaireonTool_PCGGraphInspect.h"

// PCG Graph editing (decomposed -- one tool per operation)
#include "Tools/ClaireonPCGGraphTool_Open.h"
#include "Tools/ClaireonPCGGraphTool_Close.h"
#include "Tools/ClaireonPCGGraphTool_GetState.h"
#include "Tools/ClaireonPCGGraphTool_AddNode.h"
#include "Tools/ClaireonPCGGraphTool_RemoveNode.h"
#include "Tools/ClaireonPCGGraphTool_Connect.h"
#include "Tools/ClaireonPCGGraphTool_Disconnect.h"
#include "Tools/ClaireonPCGGraphTool_DisconnectAll.h"
#include "Tools/ClaireonPCGGraphTool_SetNodeProperty.h"
#include "Tools/ClaireonPCGGraphTool_GetNodeProperties.h"
#include "Tools/ClaireonPCGGraphTool_ListNodeTypes.h"
#include "Tools/ClaireonPCGGraphTool_Focus.h"
#include "Tools/ClaireonPCGGraphTool_CursorBack.h"
#include "Tools/ClaireonPCGGraphTool_Save.h"
#include "Tools/ClaireonPCGGraphTool_ApplySpec.h"

// Enhanced Input MCP tools
#include "Tools/ClaireonTool_InputInspect.h"

// Enhanced Input editing (decomposed -- one tool per operation)
#include "Tools/ClaireonInputTool_Open.h"
#include "Tools/ClaireonInputTool_Create.h"
#include "Tools/ClaireonInputTool_Close.h"
#include "Tools/ClaireonInputTool_Status.h"
#include "Tools/ClaireonInputTool_Save.h"
#include "Tools/ClaireonInputTool_SetValueType.h"
#include "Tools/ClaireonInputTool_SetActionProperty.h"
#include "Tools/ClaireonInputTool_AddActionTrigger.h"
#include "Tools/ClaireonInputTool_RemoveActionTrigger.h"
#include "Tools/ClaireonInputTool_SetActionTriggerProperty.h"
#include "Tools/ClaireonInputTool_AddActionModifier.h"
#include "Tools/ClaireonInputTool_RemoveActionModifier.h"
#include "Tools/ClaireonInputTool_SetActionModifierProperty.h"
#include "Tools/ClaireonInputTool_AddMapping.h"
#include "Tools/ClaireonInputTool_RemoveMapping.h"
#include "Tools/ClaireonInputTool_SetMappingKey.h"
#include "Tools/ClaireonInputTool_SetMappingAction.h"
#include "Tools/ClaireonInputTool_AddMappingTrigger.h"
#include "Tools/ClaireonInputTool_RemoveMappingTrigger.h"
#include "Tools/ClaireonInputTool_AddMappingModifier.h"
#include "Tools/ClaireonInputTool_RemoveMappingModifier.h"

// Landscape and foliage MCP tools
#include "Tools/ClaireonTool_LandscapeInspect.h"
#include "Tools/ClaireonTool_LandscapeImport.h"

// Landscape editing (decomposed -- one tool per operation)
#include "Tools/ClaireonLandscapeTool_Open.h"
#include "Tools/ClaireonLandscapeTool_Create.h"
#include "Tools/ClaireonLandscapeTool_Close.h"
#include "Tools/ClaireonLandscapeTool_Status.h"
#include "Tools/ClaireonLandscapeTool_Sculpt.h"
#include "Tools/ClaireonLandscapeTool_PaintLayer.h"
#include "Tools/ClaireonLandscapeTool_PunchHole.h"
#include "Tools/ClaireonLandscapeTool_SetMaterial.h"
#include "Tools/ClaireonLandscapeTool_AddLayer.h"
#include "Tools/ClaireonLandscapeTool_Save.h"

// Landscape spline editing (decomposed -- one tool per operation)
#include "Tools/ClaireonLandscapeSplineTool_Open.h"
#include "Tools/ClaireonLandscapeSplineTool_Close.h"
#include "Tools/ClaireonLandscapeSplineTool_Status.h"
#include "Tools/ClaireonLandscapeSplineTool_AddControlPoint.h"
#include "Tools/ClaireonLandscapeSplineTool_RemoveControlPoint.h"
#include "Tools/ClaireonLandscapeSplineTool_SetControlPoint.h"
#include "Tools/ClaireonLandscapeSplineTool_AddSegment.h"
#include "Tools/ClaireonLandscapeSplineTool_RemoveSegment.h"
#include "Tools/ClaireonLandscapeSplineTool_SetSegmentProperty.h"
#include "Tools/ClaireonLandscapeSplineTool_ApplyToLandscape.h"
#include "Tools/ClaireonLandscapeSplineTool_Save.h"

// Foliage editing (decomposed -- one tool per operation)
#include "Tools/ClaireonFoliageTool_Open.h"
#include "Tools/ClaireonFoliageTool_Close.h"
#include "Tools/ClaireonFoliageTool_Status.h"
#include "Tools/ClaireonFoliageTool_AddFoliageType.h"
#include "Tools/ClaireonFoliageTool_RemoveFoliageType.h"
#include "Tools/ClaireonFoliageTool_Paint.h"
#include "Tools/ClaireonFoliageTool_Erase.h"
#include "Tools/ClaireonFoliageTool_SetDensity.h"
#include "Tools/ClaireonFoliageTool_Scatter.h"
#include "Tools/ClaireonFoliageTool_Save.h"

// Transaction management (decomposed -- one tool per operation)
#include "Tools/ClaireonTransactionGroupState.h"
#include "Tools/ClaireonTool_TransactionUndo.h"
#include "Tools/ClaireonTool_TransactionRedo.h"
#include "Tools/ClaireonTool_TransactionHistory.h"
#include "Tools/ClaireonTool_TransactionBeginGroup.h"
#include "Tools/ClaireonTool_TransactionEndGroup.h"
#include "Tools/ClaireonTool_TransactionRollbackGroup.h"

// Chooser / Proxy Table MCP tools
#include "Tools/ClaireonTool_ChooserInspect.h"
#include "Tools/ClaireonChooserTools_Lifecycle.h"
// Decomposed chooser edit tools (#0000)
#include "Tools/ClaireonChooserTool_SetResultType.h"
#include "Tools/ClaireonChooserTool_SetOutputClass.h"
#include "Tools/ClaireonChooserTool_AddContextParameter.h"
#include "Tools/ClaireonChooserTool_RemoveContextParameter.h"
#include "Tools/ClaireonChooserTool_SetContextParameterDirection.h"
#include "Tools/ClaireonChooserTool_SetFallbackResult.h"
#include "Tools/ClaireonChooserTool_AddRow.h"
#include "Tools/ClaireonChooserTool_RemoveRow.h"
#include "Tools/ClaireonChooserTool_SetRowResult.h"
#include "Tools/ClaireonChooserTool_SetColumnValue.h"
#include "Tools/ClaireonChooserTool_AddColumn.h"
#include "Tools/ClaireonChooserTool_RemoveColumn.h"
#include "Tools/ClaireonTool_ProxyTableInspect.h"
#include "Tools/ClaireonTool_ProxyAssetInspect.h"
#include "Tools/ClaireonTool_EnumInspect.h"
#include "Tools/ClaireonTool_ChooserWalk.h"
#include "Tools/ClaireonTool_ChooserRowDescend.h"
#include "Tools/ClaireonTool_ChooserFindRows.h"
#include "Tools/ClaireonTool_ChooserTraverse.h"
#include "Tools/ClaireonProxyTools_Lifecycle.h"
// Decomposed proxyasset edit tools (#0000)
#include "Tools/ClaireonProxyAssetTool_SetType.h"
#include "Tools/ClaireonProxyAssetTool_SetResultType.h"
#include "Tools/ClaireonProxyAssetTool_AddContextParameter.h"
#include "Tools/ClaireonProxyAssetTool_RemoveContextParameter.h"
#include "Tools/ClaireonProxyAssetTool_SetContextParameterDirection.h"
// Decomposed proxytable edit tools (#0000)
#include "Tools/ClaireonProxyTableTool_AddInherit.h"
#include "Tools/ClaireonProxyTableTool_RemoveInherit.h"
#include "Tools/ClaireonProxyTableTool_AddEntry.h"
#include "Tools/ClaireonProxyTableTool_RemoveEntry.h"
#include "Tools/ClaireonProxyTableTool_SetEntryValue.h"

// Animation Graph inspection MCP tools
#include "Tools/ClaireonTool_AnimGraphInspect.h"
#include "Tools/ClaireonTool_AnimGraphGetGraph.h"
#include "Tools/ClaireonTool_AnimGraphGetNode.h"
#include "Tools/ClaireonTool_AnimGraphGetStateMachine.h"
#include "Tools/ClaireonTool_AnimGraphGetTransition.h"
#include "Tools/ClaireonTool_AnimGraphAnalyze.h"

// Animation Graph editing MCP tools
#include "Tools/ClaireonAnimGraphTools_Lifecycle.h"
#include "Tools/ClaireonAnimGraphTools_Session.h"
#include "Tools/ClaireonAnimGraphTools_Node.h"
#include "Tools/ClaireonAnimGraphTools_Pin.h"
#include "Tools/ClaireonAnimGraphTools_StateMachine.h"
#include "Tools/ClaireonAnimGraphTools_Variable.h"
#include "Tools/ClaireonAnimGraphTools_Batch.h"

// Blueprint-to-C++ translation tools
#include "Tools/ClaireonTool_BlueprintTranslateScaffold.h"
#include "Tools/ClaireonTool_BlueprintTranslateImplement.h"
#include "Tools/ClaireonTool_BlueprintTranslateStatus.h"

DEFINE_LOG_CATEGORY(LogClaireon);

// Define the static FeatureName for IClaireonToolProvider
const FName IClaireonToolProvider::FeatureName = TEXT("ClaireonToolProvider");

#define LOCTEXT_NAMESPACE "ClaireonModule"

// ---------------------------------------------------------------------------
// Launch Claude Code helper -- spawns a terminal at the project root with
// `claude` running against the live MCP server. Engineers click this to
// drop into a Claude Code session that already knows about Claireon's tools.
// ---------------------------------------------------------------------------
namespace ClaireonLaunch
{
	/** Search PATH for an executable. Returns full path if found, empty otherwise. */
	static FString FindOnPath(const FString& ExeName)
	{
		const FString PathEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("PATH"));
		if (PathEnv.IsEmpty())
		{
			return FString();
		}

		TArray<FString> Dirs;
		PathEnv.ParseIntoArray(Dirs, TEXT(";"), /*InCullEmpty=*/true);

		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		for (const FString& RawDir : Dirs)
		{
			const FString Dir = RawDir.TrimStartAndEnd();
			if (Dir.IsEmpty())
			{
				continue;
			}
			FString Candidate = FPaths::Combine(Dir, ExeName);
			if (PF.FileExists(*Candidate))
			{
				return Candidate;
			}
		}
		return FString();
	}

	/**
	 * Resolve a PowerShell executable using canonical Windows install paths first,
	 * with a PATH-search fallback. Canonical paths are reliable even when the editor
	 * process inherits a stripped or non-default PATH (which can happen when launched
	 * by external tooling or as a child of a non-shell parent).
	 *
	 * Order: PowerShell 7 (preferred) -> Windows PowerShell 5.1 (always in-box on Win10+).
	 */
	static FString ResolveShell()
	{
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();

		// 1. PowerShell 7 in its default install location (system-wide).
		FString ProgramFiles = FPlatformMisc::GetEnvironmentVariable(TEXT("ProgramFiles"));
		if (ProgramFiles.IsEmpty())
		{
			ProgramFiles = TEXT("C:\\Program Files");
		}
		const FString Pwsh7 = FPaths::Combine(ProgramFiles, TEXT("PowerShell"), TEXT("7"), TEXT("pwsh.exe"));
		if (PF.FileExists(*Pwsh7))
		{
			return Pwsh7;
		}

		// 2. PATH search for `pwsh.exe` (per-user installs, scoop, etc.).
		FString FromPathPwsh = FindOnPath(TEXT("pwsh.exe"));
		if (!FromPathPwsh.IsEmpty())
		{
			return FromPathPwsh;
		}

		// 3. In-box Windows PowerShell 5.1 -- always present on Win10+, fixed path.
		FString SystemRoot = FPlatformMisc::GetEnvironmentVariable(TEXT("SystemRoot"));
		if (SystemRoot.IsEmpty())
		{
			SystemRoot = TEXT("C:\\Windows");
		}
		const FString WinPS = FPaths::Combine(
			SystemRoot, TEXT("System32"), TEXT("WindowsPowerShell"), TEXT("v1.0"), TEXT("powershell.exe"));
		if (PF.FileExists(*WinPS))
		{
			return WinPS;
		}

		// 4. PATH search for `powershell.exe` as a last resort.
		return FindOnPath(TEXT("powershell.exe"));
	}

	/** Show a transient editor notification. */
	static void Notify(const FText& Message, float Duration = 5.0f)
	{
		FNotificationInfo Info(Message);
		Info.ExpireDuration = Duration;
		FSlateNotificationManager::Get().AddNotification(Info);
	}

	/** Escape a value for safe interpolation into a PowerShell single-quoted string. */
	static FString EscapeForPwshSingle(const FString& In)
	{
		return In.Replace(TEXT("'"), TEXT("''"));
	}

	/**
	 * Resolve the live MCP port: prefer the in-process server, fall back to
	 * Saved/MCPServer.json (in case the diagnostics tab hasn't auto-started yet),
	 * fall back to the default 8017.
	 */
	static uint32 ResolveLivePort()
	{
		FClaireonModule& Module = FClaireonModule::Get();
		if (Module.IsServerRunning())
		{
			if (FClaireonServer* Server = Module.GetServer())
			{
				return Server->GetPort();
			}
		}

		const FString PortFile = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MCPServer.json"));
		FString Contents;
		if (FFileHelper::LoadFileToString(Contents, *PortFile))
		{
			TSharedPtr<FJsonObject> Json;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Contents);
			if (FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid())
			{
				int32 ParsedPort = 0;
				if (Json->TryGetNumberField(TEXT("port"), ParsedPort) && ParsedPort > 0)
				{
					return static_cast<uint32>(ParsedPort);
				}
			}
		}

		return 8017;
	}

	/**
	 * Write a per-launch MCP config to Saved/Claireon/claude-code-mcp.json so we
	 * pin Claude Code to the live port without touching the committed .mcp.json.
	 * Returns the absolute path on success, empty on failure.
	 */
	static FString WriteTransientMcpConfig(uint32 Port)
	{
		const FString McpUrl = FString::Printf(TEXT("http://127.0.0.1:%u/mcp"), Port);

		const TSharedRef<FJsonObject> UnrealServer = MakeShared<FJsonObject>();
		UnrealServer->SetStringField(TEXT("type"), TEXT("http"));
		UnrealServer->SetStringField(TEXT("url"), McpUrl);

		const TSharedRef<FJsonObject> Servers = MakeShared<FJsonObject>();
		Servers->SetObjectField(TEXT("unreal-editor"), UnrealServer);

		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetObjectField(TEXT("mcpServers"), Servers);

		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		if (!FJsonSerializer::Serialize(Root, Writer))
		{
			return FString();
		}

		const FString ConfigPath = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Claireon"), TEXT("claude-code-mcp.json")));

		if (!FFileHelper::SaveStringToFile(Out, *ConfigPath))
		{
			UE_LOG(LogClaireon, Warning, TEXT("[MCP] Failed to write transient MCP config to %s"), *ConfigPath);
			return FString();
		}

		return ConfigPath;
	}

	/**
	 * Launch Claude Code in a terminal at the project root, configured to
	 * talk to the running MCP server. Tries Windows Terminal first, falls
	 * back to PowerShell. Surfaces failures via Slate notification.
	 */
	static bool LaunchClaudeCodeFromProjectDir()
	{
		const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

		// External-launcher-spawned editors don't pass -StartMCPServer, so the MCP
		// server is dormant until the user explicitly asks for Claude. Starting
		// it here means clicking "Claude Code" is sufficient -- the user does not
		// need to first open the diagnostics tab to bring the server up.
		FClaireonModule& Module = FClaireonModule::Get();
		if (!Module.IsServerRunning())
		{
			Module.StartServer();
		}

		const uint32 Port = ResolveLivePort();
		const FString ConfigPath = WriteTransientMcpConfig(Port);
		if (ConfigPath.IsEmpty())
		{
			Notify(LOCTEXT("ClaireonLaunchConfigFailed",
				"Could not write MCP config under Saved/Claireon/. Check filesystem permissions."));
			return false;
		}

		const FString ProjectDirEsc = EscapeForPwshSingle(ProjectDir);
		const FString ConfigPathEsc = EscapeForPwshSingle(ConfigPath);

		// Optional flags driven by UClaireonSettings (configurable per-user via
		// Editor Preferences > Plugins > Claireon > Claude Code Launch, or per-project
		// via Config/DefaultEditorPerProjectUserSettings.ini).
		FString ExtraFlags;
		FString InitialPromptArg;
		if (const UClaireonSettings* LaunchSettings = UClaireonSettings::Get())
		{
			if (LaunchSettings->bLaunchSkipPermissions)
			{
				ExtraFlags += TEXT(" --dangerously-skip-permissions");
			}

			const FString Trimmed = LaunchSettings->LaunchInitialPrompt.TrimStartAndEnd();
			if (!Trimmed.IsEmpty())
			{
				InitialPromptArg = FString::Printf(TEXT(" '%s'"), *EscapeForPwshSingle(Trimmed));
			}
		}

		// Inner PowerShell command. Using `claude` from PATH; if it isn't installed
		// the engineer sees a clear "claude : The term ... is not recognized" message
		// in the terminal we just opened -- a better UX than a silent failure here.
		const FString PwshInner = FString::Printf(
			TEXT("Set-Location '%s'; claude --mcp-config '%s'%s%s"),
			*ProjectDirEsc, *ConfigPathEsc, *ExtraFlags, *InitialPromptArg);

		// Resolve a shell via canonical Windows install paths (with PATH fallback).
		// We deliberately do NOT shell through `wt.exe`: when Windows Terminal
		// launches a child shell name that isn't installed, it surfaces a cryptic
		// "file not found" referencing the inner command line. Launching the shell
		// directly is reliable, and on Windows 11 with Terminal set as the default
		// console host the user still gets the Terminal UI.
		const FString Shell = ResolveShell();
		if (Shell.IsEmpty())
		{
			Notify(LOCTEXT("ClaireonLaunchNoShell",
				"Could not locate PowerShell. Install PowerShell 7, or verify that Windows PowerShell exists at %SystemRoot%\\System32\\WindowsPowerShell\\v1.0\\powershell.exe."));
			return false;
		}

		const FString Exe = Shell;
		// -NoProfile skips the user's PowerShell profile (e.g.
		// %USERPROFILE%\Documents\WindowsPowerShell\Microsoft.PowerShell_profile.ps1).
		// User profiles routinely define a `claude` function or alias whose internals
		// (claude.ps1 path, npm prefix, etc.) drift over time and break this launch.
		// Skipping the profile keeps the shim invocation reproducible across machines.
		const FString Args = FString::Printf(TEXT("-NoProfile -NoExit -Command \"%s\""), *PwshInner);

		UE_LOG(LogClaireon, Display,
			TEXT("[MCP] LaunchClaudeCode resolved shell: %s"), *Exe);

		// bLaunchDetached MUST be false here. Setting it true causes UE to OR together
		// DETACHED_PROCESS | CREATE_NEW_CONSOLE in the Windows CreateProcess flags --
		// which are mutually exclusive per the Win32 docs, and Windows picks
		// DETACHED_PROCESS, so PowerShell spawns with no console and runs invisibly.
		// With bLaunchDetached=false the child gets a proper new console (rendered
		// via Windows Terminal on Win11 if that's the default console host).
		// We CloseProc the handle immediately, so the editor never waits on the child.
		const bool bLaunchDetached = false;
		const bool bLaunchHidden = false;
		const bool bLaunchReallyHidden = false;
		FProcHandle Handle = FPlatformProcess::CreateProc(
			*Exe, *Args, bLaunchDetached, bLaunchHidden, bLaunchReallyHidden,
			/*OutProcessID=*/nullptr, /*PriorityModifier=*/0,
			*ProjectDir, /*PipeWriteChild=*/nullptr, /*PipeReadChild=*/nullptr);

		if (!Handle.IsValid())
		{
			Notify(FText::Format(
				LOCTEXT("ClaireonLaunchSpawnFailed", "Failed to launch terminal: {0}"),
				FText::FromString(Exe)));
			return false;
		}
		FPlatformProcess::CloseProc(Handle);

		UE_LOG(LogClaireon, Display,
			TEXT("[MCP] Launched Claude Code (exe=%s, cwd=%s, port=%u)"),
			*Exe, *ProjectDir, Port);

		return true;
	}
}

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

	Tools.Add(MakeShared<ClaireonTool_ApplyBlueprintDelta>());
	Tools.Add(MakeShared<ClaireonTool_AssetReferences>());
	Tools.Add(MakeShared<ClaireonTool_AssetSearch>());
	Tools.Add(MakeShared<ClaireonTool_ExecutePython>());
	Tools.Add(MakeShared<ClaireonTool_GameplayTagsList>());
	Tools.Add(MakeShared<ClaireonTool_StructInspect>());
	Tools.Add(MakeShared<ClaireonTool_UObjectInspect>());
	Tools.Add(MakeShared<ClaireonTool_ReplaceStructUsage>());

	// Blueprint MCP tools
	Tools.Add(MakeShared<ClaireonTool_GetBlueprintProperties>());
	Tools.Add(MakeShared<ClaireonTool_GetBlueprintGraph>());
	Tools.Add(MakeShared<ClaireonTool_SearchInBlueprints>());

	// Blueprint graph editing (decomposed -- one tool per operation, stage 016)
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_Open>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_Create>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_ListGraphs>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_AddNode>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_RemoveNode>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_ReconstructNode>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_SetGameplayTags>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_SuggestNode>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_ConnectPins>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_DisconnectPin>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_SetPinValue>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_AddPin>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_RemovePin>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_SplitPin>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_RecombinePin>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_AddVariable>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_SetVariableProperties>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_RemoveVariable>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_AddComponent>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_SetProperty>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_RemoveComponent>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_ReparentComponent>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_RenameComponent>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_SetRootComponent>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_GetComponentDetails>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_MoveCursor>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_CursorBack>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_SwitchGraph>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_InspectNode>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_SelectNode>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_SelectPin>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_SelectNearestNode>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_GetState>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_ImportNodes>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_Compile>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_Save>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_Format>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_Close>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_MoveNode>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_AddFunction>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_AddFunctionOverride>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_AddInterface>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_ImplementInterface>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_RemoveInterface>());
	Tools.Add(MakeShared<ClaireonBlueprintGraphTool_ApplySpec>());

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

	// Runtime Diagnostics (snapshot CMC / anim / motion-warp / tick state on a paused-PIE pawn)
	Tools.Add(MakeShared<ClaireonTool_CMCInspectState>());
	Tools.Add(MakeShared<ClaireonTool_CMCInspectRootMotion>());
	Tools.Add(MakeShared<ClaireonTool_CMCInspectPredictionData>());
	Tools.Add(MakeShared<ClaireonTool_AnimInspectMontages>());
	Tools.Add(MakeShared<ClaireonTool_AnimInspectMotionWarping>());
	Tools.Add(MakeShared<ClaireonTool_ComponentTickInspect>());

	Tools.Add(MakeShared<ClaireonTool_PIETraceStart>());
	Tools.Add(MakeShared<ClaireonTool_PIETraceStop>());
	Tools.Add(MakeShared<ClaireonTool_ConsoleExecute>());
	Tools.Add(MakeShared<ClaireonTool_AssetList>());
	Tools.Add(MakeShared<ClaireonTool_AssetValidate>());
	Tools.Add(MakeShared<ClaireonTool_AssetFixupRedirectors>());
	Tools.Add(MakeShared<ClaireonTool_AssetCheckInnerNameInvariant>());
	Tools.Add(MakeShared<ClaireonTool_OpenAssetEditor>());
	Tools.Add(MakeShared<ClaireonTool_AnimInspect>());
	Tools.Add(MakeShared<ClaireonTool_AnimInvariantsCheck>());
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
	// Skeleton MCP tools (stateless; operate on USkeleton assets)
	Tools.Add(MakeShared<ClaireonSkeletonTool_Inspect>());
	Tools.Add(MakeShared<ClaireonSkeletonTool_AddVirtualBone>());
	Tools.Add(MakeShared<ClaireonSkeletonTool_RemoveVirtualBones>());
	Tools.Add(MakeShared<ClaireonSkeletonTool_RenameVirtualBone>());
	Tools.Add(MakeShared<ClaireonSkeletonTool_AddSocket>());
	Tools.Add(MakeShared<ClaireonSkeletonTool_RemoveSocket>());
	Tools.Add(MakeShared<ClaireonSkeletonTool_RenameSocket>());
	Tools.Add(MakeShared<ClaireonSkeletonTool_ModifySocket>());
	Tools.Add(MakeShared<ClaireonSkeletonTool_AddAnimationNotify>());
	Tools.Add(MakeShared<ClaireonSkeletonTool_RemoveAnimationNotify>());
	Tools.Add(MakeShared<ClaireonSkeletonTool_RenameAnimationNotify>());
	Tools.Add(MakeShared<ClaireonSkeletonTool_AddCurveMetadata>());
	Tools.Add(MakeShared<ClaireonSkeletonTool_RemoveCurveMetadata>());
	Tools.Add(MakeShared<ClaireonSkeletonTool_RenameCurveMetadata>());
	Tools.Add(MakeShared<ClaireonSkeletonTool_SetCurveMetadataFlags>());
	Tools.Add(MakeShared<ClaireonSkeletonTool_AddBlendProfile>());
	Tools.Add(MakeShared<ClaireonSkeletonTool_RemoveBlendProfile>());
	Tools.Add(MakeShared<ClaireonSkeletonTool_RenameBlendProfile>());
	Tools.Add(MakeShared<ClaireonSkeletonTool_SetBlendProfileMode>());
	Tools.Add(MakeShared<ClaireonSkeletonTool_SetBlendProfileBoneScale>());
	Tools.Add(MakeShared<ClaireonSkeletonTool_ClearBlendProfileBoneScale>());
	Tools.Add(MakeShared<ClaireonSkeletonTool_AddBlendMask>());
	Tools.Add(MakeShared<ClaireonSkeletonTool_RenameBlendMask>());
	Tools.Add(MakeShared<ClaireonSkeletonTool_SetBlendMaskBoneWeight>());
	Tools.Add(MakeShared<ClaireonSkeletonTool_ClearBlendMaskBoneWeight>());
	Tools.Add(MakeShared<ClaireonSkeletonTool_RemoveBlendMask>()); // Intentional stub — always errors (engine bug)
	Tools.Add(MakeShared<ClaireonTool_BlueprintCompileBatch>());
	Tools.Add(MakeShared<ClaireonTool_BlueprintDuplicate>());
	Tools.Add(MakeShared<ClaireonTool_CommandletRun>());
	Tools.Add(MakeShared<ClaireonTool_AssetResave>());
	Tools.Add(MakeShared<ClaireonTool_AssetCook>());
	Tools.Add(MakeShared<ClaireonTool_LogTail>());
	Tools.Add(MakeShared<ClaireonTool_LogSearch>());
	Tools.Add(MakeShared<ClaireonTool_MessageLogGet>());
	Tools.Add(MakeShared<ClaireonTool_TestRun>());
	Tools.Add(MakeShared<ClaireonTool_TestList>());
	// State Tree MCP tools
	Tools.Add(MakeShared<ClaireonTool_StateTreeInspect>());
	Tools.Add(MakeShared<ClaireonTool_StateTreeListNodeTypes>());
	Tools.Add(MakeShared<ClaireonTool_StateTreeGetSchema>());
	Tools.Add(MakeShared<ClaireonTool_StateTreeListBindingSources>());
	Tools.Add(MakeShared<ClaireonTool_StateTreeRuntimeInspect>());
	Tools.Add(MakeShared<ClaireonTool_StateTreeRuntimeSendEvent>());
	// State Tree edit (decomposed per-operation tools)
	Tools.Add(MakeShared<ClaireonStateTreeTool_Open>());
	Tools.Add(MakeShared<ClaireonStateTreeTool_Close>());
	Tools.Add(MakeShared<ClaireonStateTreeTool_Status>());
	Tools.Add(MakeShared<ClaireonStateTreeTool_AddState>());
	Tools.Add(MakeShared<ClaireonStateTreeTool_RemoveState>());
	Tools.Add(MakeShared<ClaireonStateTreeTool_RenameState>());
	Tools.Add(MakeShared<ClaireonStateTreeTool_MoveState>());
	Tools.Add(MakeShared<ClaireonStateTreeTool_SetStateType>());
	Tools.Add(MakeShared<ClaireonStateTreeTool_SetStateSelectionBehavior>());
	Tools.Add(MakeShared<ClaireonStateTreeTool_SetStateEnabled>());
	Tools.Add(MakeShared<ClaireonStateTreeTool_AddTask>());
	Tools.Add(MakeShared<ClaireonStateTreeTool_RemoveTask>());
	Tools.Add(MakeShared<ClaireonStateTreeTool_AddEnterCondition>());
	Tools.Add(MakeShared<ClaireonStateTreeTool_RemoveEnterCondition>());
	Tools.Add(MakeShared<ClaireonStateTreeTool_AddConsideration>());
	Tools.Add(MakeShared<ClaireonStateTreeTool_RemoveConsideration>());
	Tools.Add(MakeShared<ClaireonStateTreeTool_AddTransition>());
	Tools.Add(MakeShared<ClaireonStateTreeTool_RemoveTransition>());
	Tools.Add(MakeShared<ClaireonStateTreeTool_ModifyTransition>());
	Tools.Add(MakeShared<ClaireonStateTreeTool_AddTransitionCondition>());
	Tools.Add(MakeShared<ClaireonStateTreeTool_RemoveTransitionCondition>());
	Tools.Add(MakeShared<ClaireonStateTreeTool_AddEvaluator>());
	Tools.Add(MakeShared<ClaireonStateTreeTool_RemoveEvaluator>());
	Tools.Add(MakeShared<ClaireonStateTreeTool_AddGlobalTask>());
	Tools.Add(MakeShared<ClaireonStateTreeTool_RemoveGlobalTask>());
	Tools.Add(MakeShared<ClaireonStateTreeTool_AddBinding>());
	Tools.Add(MakeShared<ClaireonStateTreeTool_AddPropertyFunction>());
	Tools.Add(MakeShared<ClaireonStateTreeTool_RemoveBinding>());
	Tools.Add(MakeShared<ClaireonStateTreeTool_SetNodeProperty>());
	Tools.Add(MakeShared<ClaireonStateTreeTool_SetStateProperty>());
	Tools.Add(MakeShared<ClaireonStateTreeTool_SetTransitionProperty>());
	Tools.Add(MakeShared<ClaireonStateTreeTool_SetSchemaProperty>());
	Tools.Add(MakeShared<ClaireonStateTreeTool_Compile>());
	Tools.Add(MakeShared<ClaireonStateTreeTool_Save>());
	Tools.Add(MakeShared<ClaireonStateTreeTool_ApplySpec>());

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

	// Widget Blueprint editing (decomposed -- one tool per operation)
	Tools.Add(MakeShared<ClaireonWidgetBPTool_Open>());
	Tools.Add(MakeShared<ClaireonWidgetBPTool_Create>());
	Tools.Add(MakeShared<ClaireonWidgetBPTool_GetState>());
	Tools.Add(MakeShared<ClaireonWidgetBPTool_Focus>());
	Tools.Add(MakeShared<ClaireonWidgetBPTool_Compile>());
	Tools.Add(MakeShared<ClaireonWidgetBPTool_Save>());
	Tools.Add(MakeShared<ClaireonWidgetBPTool_Close>());
	Tools.Add(MakeShared<ClaireonWidgetBPTool_AddWidget>());
	Tools.Add(MakeShared<ClaireonWidgetBPTool_RemoveWidget>());
	Tools.Add(MakeShared<ClaireonWidgetBPTool_MoveWidget>());
	Tools.Add(MakeShared<ClaireonWidgetBPTool_ReplaceWidget>());
	Tools.Add(MakeShared<ClaireonWidgetBPTool_RenameWidget>());
	Tools.Add(MakeShared<ClaireonWidgetBPTool_SetWidgetProperty>());
	Tools.Add(MakeShared<ClaireonWidgetBPTool_SetSlotProperty>());
	Tools.Add(MakeShared<ClaireonWidgetBPTool_GetWidgetDetails>());
	Tools.Add(MakeShared<ClaireonWidgetBPTool_AddBinding>());
	Tools.Add(MakeShared<ClaireonWidgetBPTool_RemoveBinding>());
	Tools.Add(MakeShared<ClaireonWidgetBPTool_ListAnimations>());
	Tools.Add(MakeShared<ClaireonWidgetBPTool_ImportWidgets>());
	Tools.Add(MakeShared<ClaireonWidgetBPTool_ExportWidgets>());
	Tools.Add(MakeShared<ClaireonWidgetBPTool_ListWidgetClasses>());
	Tools.Add(MakeShared<ClaireonWidgetBPTool_ListMVVMViewModels>());
	Tools.Add(MakeShared<ClaireonWidgetBPTool_AddMVVMViewModel>());
	Tools.Add(MakeShared<ClaireonWidgetBPTool_RemoveMVVMViewModel>());
	Tools.Add(MakeShared<ClaireonWidgetBPTool_ListMVVMBindings>());
	Tools.Add(MakeShared<ClaireonWidgetBPTool_AddMVVMBinding>());
	Tools.Add(MakeShared<ClaireonWidgetBPTool_EditMVVMBinding>());
	Tools.Add(MakeShared<ClaireonWidgetBPTool_RemoveMVVMBinding>());
	Tools.Add(MakeShared<ClaireonWidgetBPTool_ApplySpec>());

	// Widget Blueprint animation extras (stubbed pending dispatch backlog)
	Tools.Add(MakeShared<ClaireonWidgetBPTool_CreateAnimation>());
	Tools.Add(MakeShared<ClaireonWidgetBPTool_DeleteAnimation>());
	Tools.Add(MakeShared<ClaireonWidgetBPTool_RenameAnimation>());
	Tools.Add(MakeShared<ClaireonWidgetBPTool_DuplicateAnimation>());
	Tools.Add(MakeShared<ClaireonWidgetBPTool_GetAnimationDetails>());
	Tools.Add(MakeShared<ClaireonWidgetBPTool_AddAnimationBinding>());
	Tools.Add(MakeShared<ClaireonWidgetBPTool_AddAnimationTrack>());
	Tools.Add(MakeShared<ClaireonWidgetBPTool_AddAnimationKeyframe>());
	Tools.Add(MakeShared<ClaireonWidgetBPTool_RemoveAnimationKeyframe>());
	Tools.Add(MakeShared<ClaireonWidgetBPTool_SetAnimationProperty>());

	// Behavior Tree + EQS MCP tools
	Tools.Add(MakeShared<ClaireonTool_BehaviorTreeInspect>());
	Tools.Add(MakeShared<ClaireonTool_BehaviorTreeInspectBlackboard>());

	// Behavior Tree editing (decomposed -- one tool per operation)
	Tools.Add(MakeShared<ClaireonBehaviorTreeTool_Open>());
	Tools.Add(MakeShared<ClaireonBehaviorTreeTool_Close>());
	Tools.Add(MakeShared<ClaireonBehaviorTreeTool_Status>());
	Tools.Add(MakeShared<ClaireonBehaviorTreeTool_AddNode>());
	Tools.Add(MakeShared<ClaireonBehaviorTreeTool_RemoveNode>());
	Tools.Add(MakeShared<ClaireonBehaviorTreeTool_MoveNode>());
	Tools.Add(MakeShared<ClaireonBehaviorTreeTool_SetNodeProperty>());
	Tools.Add(MakeShared<ClaireonBehaviorTreeTool_AddDecorator>());
	Tools.Add(MakeShared<ClaireonBehaviorTreeTool_RemoveDecorator>());
	Tools.Add(MakeShared<ClaireonBehaviorTreeTool_AddService>());
	Tools.Add(MakeShared<ClaireonBehaviorTreeTool_RemoveService>());
	Tools.Add(MakeShared<ClaireonBehaviorTreeTool_SetSubtreeAsset>());
	Tools.Add(MakeShared<ClaireonBehaviorTreeTool_UpdateAsset>());
	Tools.Add(MakeShared<ClaireonBehaviorTreeTool_Save>());
	Tools.Add(MakeShared<ClaireonBehaviorTreeTool_ListNodeTypes>());
	Tools.Add(MakeShared<ClaireonBehaviorTreeTool_ApplySpec>());

	// Blackboard editing (decomposed -- one tool per operation)
	Tools.Add(MakeShared<ClaireonBlackboardTool_Open>());
	Tools.Add(MakeShared<ClaireonBlackboardTool_Close>());
	Tools.Add(MakeShared<ClaireonBlackboardTool_Status>());
	Tools.Add(MakeShared<ClaireonBlackboardTool_AddKey>());
	Tools.Add(MakeShared<ClaireonBlackboardTool_RemoveKey>());
	Tools.Add(MakeShared<ClaireonBlackboardTool_RenameKey>());
	Tools.Add(MakeShared<ClaireonBlackboardTool_SetKeyType>());
	Tools.Add(MakeShared<ClaireonBlackboardTool_SetParent>());
	Tools.Add(MakeShared<ClaireonBlackboardTool_Save>());
	Tools.Add(MakeShared<ClaireonBlackboardTool_ApplySpec>());

	Tools.Add(MakeShared<ClaireonTool_EQSInspect>());

	// EQS editing (decomposed -- one tool per operation)
	Tools.Add(MakeShared<ClaireonEQSTool_Open>());
	Tools.Add(MakeShared<ClaireonEQSTool_CreateNew>());
	Tools.Add(MakeShared<ClaireonEQSTool_Close>());
	Tools.Add(MakeShared<ClaireonEQSTool_Status>());
	Tools.Add(MakeShared<ClaireonEQSTool_Save>());
	Tools.Add(MakeShared<ClaireonEQSTool_ApplySpec>());
	Tools.Add(MakeShared<ClaireonEQSTool_AddOption>());
	Tools.Add(MakeShared<ClaireonEQSTool_RemoveOption>());
	Tools.Add(MakeShared<ClaireonEQSTool_SetGenerator>());
	Tools.Add(MakeShared<ClaireonEQSTool_AddTest>());
	Tools.Add(MakeShared<ClaireonEQSTool_RemoveTest>());
	Tools.Add(MakeShared<ClaireonEQSTool_ReorderTests>());
	Tools.Add(MakeShared<ClaireonEQSTool_SetNodeProperty>());

	// Niagara MCP tools
	Tools.Add(MakeShared<ClaireonTool_NiagaraInspect>());
	Tools.Add(MakeShared<ClaireonNiagaraTool_Open>());
	Tools.Add(MakeShared<ClaireonNiagaraTool_Close>());
	Tools.Add(MakeShared<ClaireonNiagaraTool_Status>());
	Tools.Add(MakeShared<ClaireonNiagaraTool_FocusEmitter>());
	Tools.Add(MakeShared<ClaireonNiagaraTool_Create>());
	Tools.Add(MakeShared<ClaireonNiagaraTool_AddEmitter>());
	Tools.Add(MakeShared<ClaireonNiagaraTool_RemoveEmitter>());
	Tools.Add(MakeShared<ClaireonNiagaraTool_RenameEmitter>());
	Tools.Add(MakeShared<ClaireonNiagaraTool_SetEmitterEnabled>());
	Tools.Add(MakeShared<ClaireonNiagaraTool_AddRenderer>());
	Tools.Add(MakeShared<ClaireonNiagaraTool_RemoveRenderer>());
	Tools.Add(MakeShared<ClaireonNiagaraTool_SetRendererProperty>());
	Tools.Add(MakeShared<ClaireonNiagaraTool_SetEmitterProperty>());
	Tools.Add(MakeShared<ClaireonNiagaraTool_ListModules>());
	Tools.Add(MakeShared<ClaireonNiagaraTool_AddModule>());
	Tools.Add(MakeShared<ClaireonNiagaraTool_RemoveModule>());
	Tools.Add(MakeShared<ClaireonNiagaraTool_GetModuleInputs>());
	Tools.Add(MakeShared<ClaireonNiagaraTool_SetModuleInput>());
	Tools.Add(MakeShared<ClaireonNiagaraTool_SetSystemProperty>());
	Tools.Add(MakeShared<ClaireonNiagaraTool_AddParameter>());
	Tools.Add(MakeShared<ClaireonNiagaraTool_RemoveParameter>());
	Tools.Add(MakeShared<ClaireonNiagaraTool_SetParameterValue>());
	Tools.Add(MakeShared<ClaireonNiagaraTool_Compile>());
	Tools.Add(MakeShared<ClaireonNiagaraTool_Save>());
	Tools.Add(MakeShared<ClaireonNiagaraTool_ApplySpec>());

	// Level Sequence MCP tools
	Tools.Add(MakeShared<ClaireonTool_SequenceInspect>());
	Tools.Add(MakeShared<ClaireonTool_SequenceListTrackTypes>());
	Tools.Add(MakeShared<ClaireonTool_SequenceActorPlace>());

	// Level Sequence editing (decomposed -- one tool per operation)
	Tools.Add(MakeShared<ClaireonLevelSequenceTool_Open>());
	Tools.Add(MakeShared<ClaireonLevelSequenceTool_ApplySpec>());
	Tools.Add(MakeShared<ClaireonLevelSequenceTool_Close>());
	Tools.Add(MakeShared<ClaireonLevelSequenceTool_GetState>());
	Tools.Add(MakeShared<ClaireonLevelSequenceTool_Save>());
	Tools.Add(MakeShared<ClaireonLevelSequenceTool_FocusBinding>());
	Tools.Add(MakeShared<ClaireonLevelSequenceTool_FocusTrack>());
	Tools.Add(MakeShared<ClaireonLevelSequenceTool_AddPossessable>());
	Tools.Add(MakeShared<ClaireonLevelSequenceTool_RemovePossessable>());
	Tools.Add(MakeShared<ClaireonLevelSequenceTool_RebindActor>());
	Tools.Add(MakeShared<ClaireonLevelSequenceTool_AddSpawnable>());
	Tools.Add(MakeShared<ClaireonLevelSequenceTool_AddTrack>());
	Tools.Add(MakeShared<ClaireonLevelSequenceTool_RemoveTrack>());
	Tools.Add(MakeShared<ClaireonLevelSequenceTool_SetTrackProperty>());
	Tools.Add(MakeShared<ClaireonLevelSequenceTool_AddSection>());
	Tools.Add(MakeShared<ClaireonLevelSequenceTool_RemoveSection>());
	Tools.Add(MakeShared<ClaireonLevelSequenceTool_AddKeyframe>());
	Tools.Add(MakeShared<ClaireonLevelSequenceTool_RemoveKeyframe>());
	Tools.Add(MakeShared<ClaireonLevelSequenceTool_SetPlaybackRange>());
	Tools.Add(MakeShared<ClaireonLevelSequenceTool_AddEventKey>());
	Tools.Add(MakeShared<ClaireonLevelSequenceTool_CreateEventEndpoint>());

	// Material MCP tools
	Tools.Add(MakeShared<ClaireonTool_MaterialInspect>());
	Tools.Add(MakeShared<ClaireonTool_MaterialInstanceInspect>());
	Tools.Add(MakeShared<ClaireonTool_MaterialApply>());

	// Material editing (decomposed -- one tool per operation)
	Tools.Add(MakeShared<ClaireonMaterialTool_Open>());
	Tools.Add(MakeShared<ClaireonMaterialTool_Create>());
	Tools.Add(MakeShared<ClaireonMaterialTool_ApplySpec>());
	Tools.Add(MakeShared<ClaireonMaterialTool_Close>());
	Tools.Add(MakeShared<ClaireonMaterialTool_Status>());
	Tools.Add(MakeShared<ClaireonMaterialTool_Save>());
	Tools.Add(MakeShared<ClaireonMaterialTool_Compile>());
	Tools.Add(MakeShared<ClaireonMaterialTool_AddExpression>());
	Tools.Add(MakeShared<ClaireonMaterialTool_RemoveExpression>());
	Tools.Add(MakeShared<ClaireonMaterialTool_ConnectExpressions>());
	Tools.Add(MakeShared<ClaireonMaterialTool_DisconnectExpressionInput>());
	Tools.Add(MakeShared<ClaireonMaterialTool_ConnectToMaterialOutput>());
	Tools.Add(MakeShared<ClaireonMaterialTool_SetExpressionProperty>());
	Tools.Add(MakeShared<ClaireonMaterialTool_SetParameterDefault>());
	Tools.Add(MakeShared<ClaireonMaterialTool_SetShadingModel>());
	Tools.Add(MakeShared<ClaireonMaterialTool_SetBlendMode>());

	// Material instance editing (decomposed -- one tool per operation)
	Tools.Add(MakeShared<ClaireonMaterialInstanceTool_Open>());
	Tools.Add(MakeShared<ClaireonMaterialInstanceTool_Create>());
	Tools.Add(MakeShared<ClaireonMaterialInstanceTool_ApplySpec>());
	Tools.Add(MakeShared<ClaireonMaterialInstanceTool_Close>());
	Tools.Add(MakeShared<ClaireonMaterialInstanceTool_Status>());
	Tools.Add(MakeShared<ClaireonMaterialInstanceTool_Save>());
	Tools.Add(MakeShared<ClaireonMaterialInstanceTool_SetParent>());
	Tools.Add(MakeShared<ClaireonMaterialInstanceTool_SetScalarParameter>());
	Tools.Add(MakeShared<ClaireonMaterialInstanceTool_SetVectorParameter>());
	Tools.Add(MakeShared<ClaireonMaterialInstanceTool_SetTextureParameter>());
	Tools.Add(MakeShared<ClaireonMaterialInstanceTool_SetStaticSwitchParameter>());
	Tools.Add(MakeShared<ClaireonMaterialInstanceTool_SetStaticComponentMaskParameter>());
	Tools.Add(MakeShared<ClaireonMaterialInstanceTool_ClearParameterOverride>());

	// Audio MCP tools
	Tools.Add(MakeShared<FClaireonTool_AudioInspect>());
	Tools.Add(MakeShared<FClaireonTool_AudioApply>());

	// Audio decomposed tools (42; replaces the bundled claireon.audio_edit umbrella)
	// SoundCue (14)
	Tools.Add(MakeShared<FClaireonSoundCueTool_Open>());
	Tools.Add(MakeShared<FClaireonSoundCueTool_Close>());
	Tools.Add(MakeShared<FClaireonSoundCueTool_Status>());
	Tools.Add(MakeShared<FClaireonSoundCueTool_Save>());
	Tools.Add(MakeShared<FClaireonSoundCueTool_AddNode>());
	Tools.Add(MakeShared<FClaireonSoundCueTool_RemoveNode>());
	Tools.Add(MakeShared<FClaireonSoundCueTool_ConnectNodes>());
	Tools.Add(MakeShared<FClaireonSoundCueTool_DisconnectNodes>());
	Tools.Add(MakeShared<FClaireonSoundCueTool_SetNodeProperty>());
	Tools.Add(MakeShared<FClaireonSoundCueTool_SetNodePosition>());
	Tools.Add(MakeShared<FClaireonSoundCueTool_SetFocusedNode>());
	Tools.Add(MakeShared<FClaireonSoundCueTool_Create>());
	Tools.Add(MakeShared<FClaireonSoundCueTool_ListNodeTypes>());
	Tools.Add(MakeShared<FClaireonSoundCueTool_ApplySpec>());
	// MetaSound (11)
	Tools.Add(MakeShared<FClaireonMetaSoundTool_Open>());
	Tools.Add(MakeShared<FClaireonMetaSoundTool_Close>());
	Tools.Add(MakeShared<FClaireonMetaSoundTool_Status>());
	Tools.Add(MakeShared<FClaireonMetaSoundTool_Save>());
	Tools.Add(MakeShared<FClaireonMetaSoundTool_AddInput>());
	Tools.Add(MakeShared<FClaireonMetaSoundTool_AddOutput>());
	Tools.Add(MakeShared<FClaireonMetaSoundTool_AddNode>());
	Tools.Add(MakeShared<FClaireonMetaSoundTool_SetDefault>());
	Tools.Add(MakeShared<FClaireonMetaSoundTool_ConnectPins>());
	Tools.Add(MakeShared<FClaireonMetaSoundTool_Create>());
	Tools.Add(MakeShared<FClaireonMetaSoundTool_ApplySpec>());
	// Camera Asset (13)
	Tools.Add(MakeShared<FClaireonCameraAssetTool_Create>());
	Tools.Add(MakeShared<FClaireonCameraAssetTool_Duplicate>());
	Tools.Add(MakeShared<FClaireonCameraAssetTool_Save>());
	Tools.Add(MakeShared<FClaireonCameraAssetTool_Compile>());
	Tools.Add(MakeShared<FClaireonCameraAssetTool_ListRigs>());
	Tools.Add(MakeShared<FClaireonCameraAssetTool_AddRig>());
	Tools.Add(MakeShared<FClaireonCameraAssetTool_ListNodes>());
	Tools.Add(MakeShared<FClaireonCameraAssetTool_ListNodeClasses>());
	Tools.Add(MakeShared<FClaireonCameraAssetTool_AddNode>());
	Tools.Add(MakeShared<FClaireonCameraAssetTool_RemoveNode>());
	Tools.Add(MakeShared<FClaireonCameraAssetTool_MoveNode>());
	Tools.Add(MakeShared<FClaireonCameraAssetTool_GetNodeProperty>());
	Tools.Add(MakeShared<FClaireonCameraAssetTool_SetNodeProperty>());
	// SoundClass (5)
	Tools.Add(MakeShared<FClaireonSoundClassTool_SetProperty>());
	Tools.Add(MakeShared<FClaireonSoundClassTool_AddChild>());
	Tools.Add(MakeShared<FClaireonSoundClassTool_RemoveChild>());
	Tools.Add(MakeShared<FClaireonSoundClassTool_Create>());
	Tools.Add(MakeShared<FClaireonSoundClassTool_ApplySpec>());
	// SoundMix (6)
	Tools.Add(MakeShared<FClaireonSoundMixTool_AddClassAdjuster>());
	Tools.Add(MakeShared<FClaireonSoundMixTool_RemoveClassAdjuster>());
	Tools.Add(MakeShared<FClaireonSoundMixTool_SetClassAdjusterProperty>());
	Tools.Add(MakeShared<FClaireonSoundMixTool_SetEnvelope>());
	Tools.Add(MakeShared<FClaireonSoundMixTool_Create>());
	Tools.Add(MakeShared<FClaireonSoundMixTool_ApplySpec>());
	// Attenuation (3)
	Tools.Add(MakeShared<FClaireonAttenuationTool_SetProperty>());
	Tools.Add(MakeShared<FClaireonAttenuationTool_Create>());
	Tools.Add(MakeShared<FClaireonAttenuationTool_ApplySpec>());
	// Concurrency (3)
	Tools.Add(MakeShared<FClaireonConcurrencyTool_SetProperty>());
	Tools.Add(MakeShared<FClaireonConcurrencyTool_Create>());
	Tools.Add(MakeShared<FClaireonConcurrencyTool_ApplySpec>());

	// PCG Graph MCP tools
	Tools.Add(MakeShared<ClaireonTool_PCGGraphInspect>());

	// PCG Graph editing (decomposed -- one tool per operation)
	Tools.Add(MakeShared<ClaireonPCGGraphTool_Open>());
	Tools.Add(MakeShared<ClaireonPCGGraphTool_Close>());
	Tools.Add(MakeShared<ClaireonPCGGraphTool_GetState>());
	Tools.Add(MakeShared<ClaireonPCGGraphTool_AddNode>());
	Tools.Add(MakeShared<ClaireonPCGGraphTool_RemoveNode>());
	Tools.Add(MakeShared<ClaireonPCGGraphTool_Connect>());
	Tools.Add(MakeShared<ClaireonPCGGraphTool_Disconnect>());
	Tools.Add(MakeShared<ClaireonPCGGraphTool_DisconnectAll>());
	Tools.Add(MakeShared<ClaireonPCGGraphTool_SetNodeProperty>());
	Tools.Add(MakeShared<ClaireonPCGGraphTool_GetNodeProperties>());
	Tools.Add(MakeShared<ClaireonPCGGraphTool_ListNodeTypes>());
	Tools.Add(MakeShared<ClaireonPCGGraphTool_Focus>());
	Tools.Add(MakeShared<ClaireonPCGGraphTool_CursorBack>());
	Tools.Add(MakeShared<ClaireonPCGGraphTool_Save>());
	Tools.Add(MakeShared<ClaireonPCGGraphTool_ApplySpec>());

	// Enhanced Input MCP tools
	Tools.Add(MakeShared<ClaireonTool_InputInspect>());

	// Enhanced Input editing (decomposed -- one tool per operation)
	Tools.Add(MakeShared<ClaireonInputTool_Open>());
	Tools.Add(MakeShared<ClaireonInputTool_Create>());
	Tools.Add(MakeShared<ClaireonInputTool_Close>());
	Tools.Add(MakeShared<ClaireonInputTool_Status>());
	Tools.Add(MakeShared<ClaireonInputTool_Save>());
	Tools.Add(MakeShared<ClaireonInputTool_SetValueType>());
	Tools.Add(MakeShared<ClaireonInputTool_SetActionProperty>());
	Tools.Add(MakeShared<ClaireonInputTool_AddActionTrigger>());
	Tools.Add(MakeShared<ClaireonInputTool_RemoveActionTrigger>());
	Tools.Add(MakeShared<ClaireonInputTool_SetActionTriggerProperty>());
	Tools.Add(MakeShared<ClaireonInputTool_AddActionModifier>());
	Tools.Add(MakeShared<ClaireonInputTool_RemoveActionModifier>());
	Tools.Add(MakeShared<ClaireonInputTool_SetActionModifierProperty>());
	Tools.Add(MakeShared<ClaireonInputTool_AddMapping>());
	Tools.Add(MakeShared<ClaireonInputTool_RemoveMapping>());
	Tools.Add(MakeShared<ClaireonInputTool_SetMappingKey>());
	Tools.Add(MakeShared<ClaireonInputTool_SetMappingAction>());
	Tools.Add(MakeShared<ClaireonInputTool_AddMappingTrigger>());
	Tools.Add(MakeShared<ClaireonInputTool_RemoveMappingTrigger>());
	Tools.Add(MakeShared<ClaireonInputTool_AddMappingModifier>());
	Tools.Add(MakeShared<ClaireonInputTool_RemoveMappingModifier>());

	// Landscape and foliage MCP tools
	Tools.Add(MakeShared<ClaireonTool_LandscapeInspect>());
	Tools.Add(MakeShared<ClaireonTool_LandscapeImport>());

	// Landscape editing (decomposed -- one tool per operation)
	Tools.Add(MakeShared<ClaireonLandscapeTool_Open>());
	Tools.Add(MakeShared<ClaireonLandscapeTool_Create>());
	Tools.Add(MakeShared<ClaireonLandscapeTool_Close>());
	Tools.Add(MakeShared<ClaireonLandscapeTool_Status>());
	Tools.Add(MakeShared<ClaireonLandscapeTool_Sculpt>());
	Tools.Add(MakeShared<ClaireonLandscapeTool_PaintLayer>());
	Tools.Add(MakeShared<ClaireonLandscapeTool_PunchHole>());
	Tools.Add(MakeShared<ClaireonLandscapeTool_SetMaterial>());
	Tools.Add(MakeShared<ClaireonLandscapeTool_AddLayer>());
	Tools.Add(MakeShared<ClaireonLandscapeTool_Save>());

	// Landscape spline editing (decomposed -- one tool per operation)
	Tools.Add(MakeShared<ClaireonLandscapeSplineTool_Open>());
	Tools.Add(MakeShared<ClaireonLandscapeSplineTool_Close>());
	Tools.Add(MakeShared<ClaireonLandscapeSplineTool_Status>());
	Tools.Add(MakeShared<ClaireonLandscapeSplineTool_AddControlPoint>());
	Tools.Add(MakeShared<ClaireonLandscapeSplineTool_RemoveControlPoint>());
	Tools.Add(MakeShared<ClaireonLandscapeSplineTool_SetControlPoint>());
	Tools.Add(MakeShared<ClaireonLandscapeSplineTool_AddSegment>());
	Tools.Add(MakeShared<ClaireonLandscapeSplineTool_RemoveSegment>());
	Tools.Add(MakeShared<ClaireonLandscapeSplineTool_SetSegmentProperty>());
	Tools.Add(MakeShared<ClaireonLandscapeSplineTool_ApplyToLandscape>());
	Tools.Add(MakeShared<ClaireonLandscapeSplineTool_Save>());

	// Foliage editing (decomposed -- one tool per operation)
	Tools.Add(MakeShared<ClaireonFoliageTool_Open>());
	Tools.Add(MakeShared<ClaireonFoliageTool_Close>());
	Tools.Add(MakeShared<ClaireonFoliageTool_Status>());
	Tools.Add(MakeShared<ClaireonFoliageTool_AddFoliageType>());
	Tools.Add(MakeShared<ClaireonFoliageTool_RemoveFoliageType>());
	Tools.Add(MakeShared<ClaireonFoliageTool_Paint>());
	Tools.Add(MakeShared<ClaireonFoliageTool_Erase>());
	Tools.Add(MakeShared<ClaireonFoliageTool_SetDensity>());
	Tools.Add(MakeShared<ClaireonFoliageTool_Scatter>());
	Tools.Add(MakeShared<ClaireonFoliageTool_Save>());

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
	Tools.Add(MakeShared<ClaireonTool_ApplySpecHelp>());

	// Transaction management (decomposed -- one tool per operation)
	Tools.Add(MakeShared<ClaireonTool_TransactionUndo>());
	Tools.Add(MakeShared<ClaireonTool_TransactionRedo>());
	Tools.Add(MakeShared<ClaireonTool_TransactionHistory>());
	Tools.Add(MakeShared<ClaireonTool_TransactionBeginGroup>());
	Tools.Add(MakeShared<ClaireonTool_TransactionEndGroup>());
	Tools.Add(MakeShared<ClaireonTool_TransactionRollbackGroup>());

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
	// Decomposed chooser edit tools (#0000)
	Tools.Add(MakeShared<ClaireonTool_ChooserSetResultType>());
	Tools.Add(MakeShared<ClaireonTool_ChooserSetOutputClass>());
	Tools.Add(MakeShared<ClaireonTool_ChooserAddContextParameter>());
	Tools.Add(MakeShared<ClaireonTool_ChooserRemoveContextParameter>());
	Tools.Add(MakeShared<ClaireonTool_ChooserSetContextParameterDirection>());
	Tools.Add(MakeShared<ClaireonTool_ChooserSetFallbackResult>());
	Tools.Add(MakeShared<ClaireonTool_ChooserAddRow>());
	Tools.Add(MakeShared<ClaireonTool_ChooserRemoveRow>());
	Tools.Add(MakeShared<ClaireonTool_ChooserSetRowResult>());
	Tools.Add(MakeShared<ClaireonTool_ChooserSetColumnValue>());
	Tools.Add(MakeShared<ClaireonTool_ChooserAddColumn>());
	Tools.Add(MakeShared<ClaireonTool_ChooserRemoveColumn>());

	// Proxy Table / Proxy Asset MCP tools
	Tools.Add(MakeShared<ClaireonTool_ProxyTableInspect>());
	Tools.Add(MakeShared<ClaireonTool_ProxyAssetInspect>());

	// Enum introspection (UEnum / UUserDefinedEnum)
	Tools.Add(MakeShared<ClaireonTool_EnumInspect>());

	// Chooser tree-walk verbs (composable primitives, complement chooser_inspect)
	Tools.Add(MakeShared<ClaireonTool_ChooserWalk>());
	Tools.Add(MakeShared<ClaireonTool_ChooserRowDescend>());
	Tools.Add(MakeShared<ClaireonTool_ChooserFindRows>());
	Tools.Add(MakeShared<ClaireonTool_ChooserTraverse>());
	Tools.Add(MakeShared<ClaireonTool_ProxyTableCreate>());
	Tools.Add(MakeShared<ClaireonTool_ProxyTableDuplicate>());
	Tools.Add(MakeShared<ClaireonTool_ProxyAssetCreate>());
	Tools.Add(MakeShared<ClaireonTool_ProxyAssetDuplicate>());
	// Decomposed proxyasset edit tools (#0000)
	Tools.Add(MakeShared<ClaireonTool_ProxyAssetSetType>());
	Tools.Add(MakeShared<ClaireonTool_ProxyAssetSetResultType>());
	Tools.Add(MakeShared<ClaireonTool_ProxyAssetAddContextParameter>());
	Tools.Add(MakeShared<ClaireonTool_ProxyAssetRemoveContextParameter>());
	Tools.Add(MakeShared<ClaireonTool_ProxyAssetSetContextParameterDirection>());
	// Decomposed proxytable edit tools (#0000)
	Tools.Add(MakeShared<ClaireonTool_ProxyTableAddInherit>());
	Tools.Add(MakeShared<ClaireonTool_ProxyTableRemoveInherit>());
	Tools.Add(MakeShared<ClaireonTool_ProxyTableAddEntry>());
	Tools.Add(MakeShared<ClaireonTool_ProxyTableRemoveEntry>());
	Tools.Add(MakeShared<ClaireonTool_ProxyTableSetEntryValue>());

	// Animation Graph inspection MCP tools
	Tools.Add(MakeShared<ClaireonTool_AnimGraphInspect>());
	Tools.Add(MakeShared<ClaireonTool_AnimGraphGetGraph>());
	Tools.Add(MakeShared<ClaireonTool_AnimGraphGetNode>());
	Tools.Add(MakeShared<ClaireonTool_AnimGraphGetStateMachine>());
	Tools.Add(MakeShared<ClaireonTool_AnimGraphGetTransition>());
	Tools.Add(MakeShared<ClaireonTool_AnimGraphAnalyze>());

	// Animation Graph editing MCP tools — Lifecycle (stateless)
	Tools.Add(MakeShared<ClaireonAnimGraphTool_Create>());
	Tools.Add(MakeShared<ClaireonAnimGraphTool_CreateChild>());
	Tools.Add(MakeShared<ClaireonAnimGraphTool_Duplicate>());

	// Animation Graph editing MCP tools — Session
	Tools.Add(MakeShared<ClaireonAnimGraphTool_Open>());
	Tools.Add(MakeShared<ClaireonAnimGraphTool_Close>());
	Tools.Add(MakeShared<ClaireonAnimGraphTool_Save>());
	Tools.Add(MakeShared<ClaireonAnimGraphTool_Compile>());
	Tools.Add(MakeShared<ClaireonAnimGraphTool_SwitchGraph>());
	Tools.Add(MakeShared<ClaireonAnimGraphTool_GetState>());

	// Animation Graph editing MCP tools — Node operations
	Tools.Add(MakeShared<ClaireonAnimGraphTool_AddNode>());
	Tools.Add(MakeShared<ClaireonAnimGraphTool_RemoveNode>());
	Tools.Add(MakeShared<ClaireonAnimGraphTool_MoveNode>());
	Tools.Add(MakeShared<ClaireonAnimGraphTool_SetNodeProperty>());
	Tools.Add(MakeShared<ClaireonAnimGraphTool_ConnectPins>());
	Tools.Add(MakeShared<ClaireonAnimGraphTool_DisconnectPin>());

	// Animation Graph editing MCP tools — Pin & Binding operations
	Tools.Add(MakeShared<ClaireonAnimGraphTool_ExposePin>());
	Tools.Add(MakeShared<ClaireonAnimGraphTool_HidePin>());
	Tools.Add(MakeShared<ClaireonAnimGraphTool_SetBinding>());
	Tools.Add(MakeShared<ClaireonAnimGraphTool_RemoveBinding>());
	Tools.Add(MakeShared<ClaireonAnimGraphTool_BindFunction>());

	// Animation Graph editing MCP tools — State Machine operations
	Tools.Add(MakeShared<ClaireonAnimGraphTool_AddState>());
	Tools.Add(MakeShared<ClaireonAnimGraphTool_RemoveState>());
	Tools.Add(MakeShared<ClaireonAnimGraphTool_RenameState>());
	Tools.Add(MakeShared<ClaireonAnimGraphTool_SetEntryState>());
	Tools.Add(MakeShared<ClaireonAnimGraphTool_AddTransition>());
	Tools.Add(MakeShared<ClaireonAnimGraphTool_RemoveTransition>());
	Tools.Add(MakeShared<ClaireonAnimGraphTool_SetTransitionProperties>());

	// Animation Graph editing MCP tools — Batch graph construction
	Tools.Add(MakeShared<ClaireonAnimGraphTool_ApplyDelta>());

	// Animation Graph editing MCP tools — Variable, Function, Interface operations
	Tools.Add(MakeShared<ClaireonAnimGraphTool_AddVariable>());
	Tools.Add(MakeShared<ClaireonAnimGraphTool_RemoveVariable>());
	Tools.Add(MakeShared<ClaireonAnimGraphTool_SetVariableProperties>());
	Tools.Add(MakeShared<ClaireonAnimGraphTool_AddFunction>());
	Tools.Add(MakeShared<ClaireonAnimGraphTool_AddFunctionOverride>());
	Tools.Add(MakeShared<ClaireonAnimGraphTool_RemoveFunction>());
	Tools.Add(MakeShared<ClaireonAnimGraphTool_AddInterface>());
	Tools.Add(MakeShared<ClaireonAnimGraphTool_RemoveInterface>());

	// Blueprint-to-C++ translation tools
	Tools.Add(MakeShared<ClaireonTool_BlueprintTranslateScaffold>());
	Tools.Add(MakeShared<ClaireonTool_BlueprintTranslateImplement>());
	Tools.Add(MakeShared<ClaireonTool_BlueprintTranslateStatus>());

	return Tools;
}

// ---------------------------------------------------------------------------
// FClaireonModule
// ---------------------------------------------------------------------------

FClaireonModule::FClaireonModule() = default;
FClaireonModule::~FClaireonModule() = default;

void FClaireonModule::StartupModule()
{
	if (!GIsEditor || IsRunningCommandlet())
	{
		return;
	}

	SClaireonDiagnosticsWidget::RegisterTabSpawner();

	// Settings registration is handled automatically by UDeveloperSettings via
	// GetCategoryName() -> "Plugins" and meta=(DisplayName="Claireon").
	// Manual RegisterSettings was removed -- it caused duplicate entries.

	FClaireonRichTextStyle::Initialize();
	FClaireonToolbarStyle::Initialize();

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
	ClaireonTransactionGroupState::ResetGroupState();

	FClaireonPIEManager::Get().UnbindEditorDelegates();
	FClaireonRichTextStyle::Shutdown();
	FClaireonToolbarStyle::Shutdown();

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

	// Decide whether to front this editor with the always-on MCP proxy. When
	// enabled, the editor generates a fresh per-session bearer token, hands it
	// to the server, then asks the proxy client to spawn (or attach) and
	// register. The proxy becomes the sole ingress for MCP traffic; direct
	// connections are rejected with 401.
	//
	// The proxy is OFF by default and must be explicitly opted into. The
	// previous always-on default caused a child python.exe to inherit handles
	// from the launching shell and hold a lock on the worktree, which blocked
	// `git pull` / dispatch sync until the proxy was killed.
	//
	// Decision precedence (resolved at StartServer time):
	//   1. `-EnableMCPProxy` command-line flag -> force enabled (overrides settings).
	//   2. UClaireonSettings::bEnableProxy (default false).
	const bool bProxyEnabledByCLI = FParse::Param(FCommandLine::Get(), TEXT("EnableMCPProxy"));
	bool bEnableProxy = false;
	if (const UClaireonSettings* Settings = UClaireonSettings::Get())
	{
		bEnableProxy = Settings->bEnableProxy;
	}
	if (bProxyEnabledByCLI)
	{
		bEnableProxy = true;
	}

	FString SessionToken;
	if (bEnableProxy)
	{
		// Two guids concatenated give us 64 hex chars sans the braces, well
		// above the 32-char minimum required by /editor/register. Never
		// persisted to disk; lives only in this process and inside the proxy's
		// singleton_session dict.
		SessionToken = FGuid::NewGuid().ToString(EGuidFormats::Digits)
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
		Server->SetSessionToken(SessionToken);
	}

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

	// Stage 010 (direct-connect SHA port + auto-promote): the editor's MCP
	// listener binds the per-worktree SHA-256-derived port -- the same port
	// .mcp.json points Claude at -- not the legacy Settings::ServerPort.
	//
	// Decision tree:
	//   1. Compute SHA port from this worktree's canonical realpath.
	//   2. If bEnableProxy was opted in, kick the proxy spawn first so the
	//      auto-promote branch lands cleanly in the EADDRINUSE path below.
	//   3. TryStart on the SHA port:
	//        - Success: DirectConnect mode; we are the SHA-port owner.
	//        - Failure + proxy reachable on /admin/health: auto-promote
	//          (bind ephemeral, register with the proxy).
	//        - Failure + no Claireon proxy: surface and abort (some other
	//          process holds the port; the operator must clean up).
	//
	// -MCPServerPort= command-line override is preserved as a developer
	// escape hatch. Settings::ServerPort is no longer read (Stage 012
	// deletes the property; Stage 010 stops reading it here).
	const FString WorktreeRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	uint16 PreferredPort = Claireon::DeriveDefaultMcpPort(WorktreeRoot);

	FString CmdPortStr;
	if (FParse::Value(FCommandLine::Get(), TEXT("-MCPServerPort="), CmdPortStr))
	{
		const uint32 Override = static_cast<uint32>(FCString::Atoi(*CmdPortStr));
		if (Override > 0 && Override <= 65535)
		{
			PreferredPort = static_cast<uint16>(Override);
			bPortOverriddenByCommandLine = true;
			UE_LOG(LogClaireon, Display,
				TEXT("[MCP] -MCPServerPort= override active; binding %u instead of SHA port"),
				static_cast<uint32>(PreferredPort));
		}
	}

	// Build-id: use the engine's changelist + compile-config string as a
	// lightweight opaque identifier. The proxy treats this as opaque.
	auto MakeBuildId = []() -> FString
	{
		return FString::Printf(TEXT("%s-%s"),
			FApp::GetBuildVersion(),
			FApp::GetBuildConfiguration() == EBuildConfiguration::Debug ? TEXT("Debug")
				: FApp::GetBuildConfiguration() == EBuildConfiguration::DebugGame ? TEXT("DebugGame")
				: FApp::GetBuildConfiguration() == EBuildConfiguration::Development ? TEXT("Development")
				: FApp::GetBuildConfiguration() == EBuildConfiguration::Shipping ? TEXT("Shipping")
				: TEXT("Test"));
	};

	// Spawning hint: when bEnableProxy is opted in, bring the proxy up first
	// so the SHA port is held by the singleton when we try to bind below.
	// EnsureProxyRunning is idempotent (attaches if already up).
	if (bEnableProxy)
	{
		ProxyClient = MakeUnique<FClaireonProxyClient>();
		ProxyClient->EnsureProxyRunning();
	}

	if (Server->TryStart(PreferredPort))
	{
		// DirectConnect mode -- we own the SHA port. The proxy (if any) is
		// not in the path; .mcp.json still points Claude at this port.
		CurrentMcpMode = EClaireonMcpMode::DirectConnect;
		UE_LOG(LogClaireon, Display,
			TEXT("[MCP] Bound port %u for worktree %s (DirectConnect mode)"),
			static_cast<uint32>(PreferredPort), *WorktreeRoot);
		// If the operator opted in to bEnableProxy we still register so the
		// proxy can route to us later if it spawns; today we are the SHA-
		// port owner, but the editor-side state machine remains valid.
		if (bEnableProxy && ProxyClient.IsValid())
		{
			ProxyClient->BeginRetryRegister(
				static_cast<int32>(Server->GetPort()), SessionToken, MakeBuildId());
		}
		return;
	}

	// EADDRINUSE on the SHA port. Probe 43017 to decide between auto-
	// promote and abort.
	UE_LOG(LogClaireon, Display,
		TEXT("[MCP] Port %u busy; probing 43017 for an existing Claireon proxy."),
		static_cast<uint32>(PreferredPort));

	if (!ProxyClient.IsValid())
	{
		ProxyClient = MakeUnique<FClaireonProxyClient>();
	}

	if (!ProxyClient->PingProxyHealth())
	{
		UE_LOG(LogClaireon, Error,
			TEXT("[MCP] Port %u is held by an unknown process and no Claireon ")
			TEXT("proxy is running on 43017. Free the port (or run ")
			TEXT("Scripts/Utilities/Invoke-MultiWorktreeProxyMigration.ps1) ")
			TEXT("and relaunch the editor."),
			static_cast<uint32>(PreferredPort));
		Server.Reset();
		ProxyClient.Reset();
		return;
	}

	// Auto-promote: a Claireon proxy already holds the SHA port. Bind an
	// ephemeral local listener and register through the proxy.
	const uint16 EphemeralPort = Server->StartEphemeral();
	if (EphemeralPort == 0)
	{
		UE_LOG(LogClaireon, Error,
			TEXT("[MCP] Failed to bind ephemeral listener for proxy mode."));
		Server.Reset();
		ProxyClient.Reset();
		return;
	}

	// The proxy needs a session token even in auto-promote, since /editor/
	// register requires a 32-char token. Generate one if we did not already
	// (i.e. when bEnableProxy was false but auto-promote fired).
	if (SessionToken.IsEmpty())
	{
		SessionToken = FGuid::NewGuid().ToString(EGuidFormats::Digits)
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
		Server->SetSessionToken(SessionToken);
	}

	CurrentMcpMode = EClaireonMcpMode::ProxyAttached;
	ProxyClient->BeginRetryRegister(
		static_cast<int32>(Server->GetPort()), SessionToken, MakeBuildId());
	UE_LOG(LogClaireon, Display,
		TEXT("[MCP] Joined proxy session despite bEnableProxy=%s because a ")
		TEXT("Claireon proxy was already running (pid=%u). Editor MCP listener bound on %u."),
		bEnableProxy ? TEXT("true") : TEXT("false"),
		ProxyClient->GetCachedProxyPid(),
		static_cast<uint32>(EphemeralPort));
}

void FClaireonModule::StopServer()
{
	// Unregister from the proxy first so it stops sending MCP traffic at
	// our port before we tear down the listener. The proxy process itself
	// keeps running -- next editor launch attaches to the same proxy.
	if (ProxyClient.IsValid())
	{
		ProxyClient->Unregister();
		ProxyClient.Reset();
	}

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
	// Register toolbar button in the same section as Heavy PIE, FS Preferences, etc.
	static const FName ToolbarSection(TEXT("LevelEditor.LevelEditorToolBar.User"));
	static const FName AIChatSection(TEXT("LevelEditor.LevelEditorToolBar.User.AIChat"));

	UToolMenu* Toolbar = UToolMenus::Get()->ExtendMenu(ToolbarSection);
	if (!Toolbar)
	{
		return;
	}

	FToolMenuSection& Section = Toolbar->AddSection(AIChatSection);
	Section.AddSeparator(NAME_None);

	// Status dot color selector: red (stopped), blinking cyan/blue (processing), green (idle running).
	// A request arriving bumps Server->GetLastRequestTime(); we stay in the blink window for at
	// least MinDurationSeconds after, so instant requests are still visualized, and a stream of
	// instant requests keeps the window extended -- the time-based phase toggle continues to
	// alternate A/B through the stream.
	auto StatusDotColorLambda = []() -> FSlateColor
	{
		FClaireonServer* Server = FClaireonModule::Get().GetServer();
		if (!Server || !Server->IsRunning())
		{
			return FSlateColor(FClaireonToolbarStyle::GetStoppedColor());
		}

		const double Now = FPlatformTime::Seconds();
		const double LastRequest = Server->GetLastRequestTime();
		const double MinDuration = FClaireonToolbarStyle::GetProcessingMinDurationSeconds();

		if (LastRequest > 0.0 && (Now - LastRequest) < MinDuration)
		{
			const double HalfPeriod = FClaireonToolbarStyle::GetProcessingBlinkHalfPeriodSeconds();
			const int32 Phase = static_cast<int32>(Now / HalfPeriod) & 1;
			return FSlateColor(Phase == 0
				? FClaireonToolbarStyle::GetProcessingColorA()
				: FClaireonToolbarStyle::GetProcessingColorB());
		}

		return FSlateColor(FClaireonToolbarStyle::GetRunningColor());
	};

	TSharedRef<SButton> AIChatButton = SNew(SButton)
		.ButtonStyle(FAppStyle::Get(), "SimpleButton")
		.OnClicked_Lambda([]() -> FReply
	{
		FGlobalTabmanager::Get()->TryInvokeTab(SClaireonDiagnosticsWidget::TabId);
		return FReply::Handled();
	}).ToolTip(SNew(SToolTip)[SNew(SVerticalBox) + SVerticalBox::Slot().AutoHeight().Padding(2.0f)[SNew(STextBlock).Text_Lambda([]() -> FText
	{
		if (FClaireonModule::Get().IsServerRunning())
		{
			return INVTEXT("MCP Server: Running");
		}
		return INVTEXT("MCP Server: Stopped");
	}).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))]
				+ SVerticalBox::Slot().AutoHeight().Padding(2.0f)[SNew(STextBlock).Text_Lambda([]() -> FText
	{
		FClaireonServer* Server = FClaireonModule::Get().GetServer();
		if (Server && Server->IsRunning())
		{
			return FText::FromString(FString::Printf(TEXT("Port: %u"), Server->GetPort()));
		}
		return FText::GetEmpty();
	})] + SVerticalBox::Slot().AutoHeight().Padding(2.0f)[SNew(STextBlock).Text_Lambda([]() -> FText
	{
		FClaireonServer* Server = FClaireonModule::Get().GetServer();
		if (Server && Server->IsRunning())
		{
			FTimespan Uptime = FDateTime::Now() - Server->GetStartTime();
			return FText::FromString(FString::Printf(TEXT("Uptime: %s"),
				*Uptime.ToString(TEXT("%h:%m:%s"))));
		}
		return FText::GetEmpty();
	})] + SVerticalBox::Slot().AutoHeight().Padding(2.0f)[SNew(STextBlock).Text_Lambda([]() -> FText
	{
		FClaireonServer* Server = FClaireonModule::Get().GetServer();
		if (Server && Server->IsRunning())
		{
			return FText::FromString(FString::Printf(TEXT("Requests: %d"),
				Server->GetTotalRequestCount()));
		}
		return FText::GetEmpty();
	})] + SVerticalBox::Slot().AutoHeight().Padding(2.0f)[SNew(STextBlock).Text_Lambda([]() -> FText
	{
		FClaireonServer* Server = FClaireonModule::Get().GetServer();
		if (Server && Server->IsRunning())
		{
			return FText::FromString(FString::Printf(TEXT("Errors: %d"),
				Server->GetErrorCount()));
		}
		return FText::GetEmpty();
	})]]).ContentPadding(FMargin(4.0f))[SNew(SBox).WidthOverride(16.0f).HeightOverride(16.0f)[SNew(SOverlay) + SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)[SNew(SImage).Image(FAppStyle::GetBrush(TEXT("Icons.Comment"))).DesiredSizeOverride(FVector2D(16.0f, 16.0f))] + SOverlay::Slot().HAlign(HAlign_Right).VAlign(VAlign_Bottom)[SNew(SImage).Image(FClaireonToolbarStyle::Get().GetBrush(TEXT("ClaireonToolbar.StatusDot"))).ColorAndOpacity_Lambda(StatusDotColorLambda).DesiredSizeOverride(FVector2D(8.0f, 8.0f))]]];

	// Drive continuous repaint so the ColorAndOpacity_Lambda re-evaluates each frame of the
	// blink window even when the user isn't hovering the button. Cost is one no-op invalidation
	// per tick; the lambda itself is the only work per frame.
	AIChatButton->RegisterActiveTimer(
		0.05f, // ~20 Hz, well above the 0.15s half-period blink rate
		FWidgetActiveTimerDelegate::CreateLambda(
			[](double /*InCurrentTime*/, float /*InDeltaTime*/) -> EActiveTimerReturnType
		{
			return EActiveTimerReturnType::Continue;
		}));

	FToolMenuEntry AIChatEntry = FToolMenuEntry::InitWidget(
		TEXT("AIChat"),
		AIChatButton,
		LOCTEXT("AIChatLabel", "AI Chat"));
	AIChatEntry.StyleNameOverride = "CalloutToolbar";
	Section.AddEntry(AIChatEntry);

	FToolMenuEntry LaunchClaudeCodeEntry = FToolMenuEntry::InitToolBarButton(
		TEXT("LaunchClaudeCode"),
		FUIAction(FExecuteAction::CreateLambda([]()
	{
		ClaireonLaunch::LaunchClaudeCodeFromProjectDir();
	})),
		LOCTEXT("LaunchClaudeCodeLabel", "Claude Code"),
		LOCTEXT("LaunchClaudeCodeTooltip",
			"Open a terminal at the project root and start Claude Code, pre-configured to talk to this editor's MCP server. "
			"Inference is billed against your Claude.ai subscription, not the Anthropic API."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Console")),
		EUserInterfaceActionType::Button);
	LaunchClaudeCodeEntry.StyleNameOverride = "CalloutToolbar";
	Section.AddEntry(LaunchClaudeCodeEntry);

	// Window > AI Chat menu entry for discoverability
	{
		UToolMenu* WindowMenu = UToolMenus::Get()->ExtendMenu(
			"LevelEditor.MainMenu.Window.General.Miscellaneous");
		if (WindowMenu)
		{
			FToolMenuSection& WindowSection = WindowMenu->FindOrAddSection("WindowLayout");
			WindowSection.AddMenuEntry(
				TEXT("AIChat"),
				LOCTEXT("WindowAIChatLabel", "AI Chat"),
				LOCTEXT("WindowAIChatTooltip", "Open the AI Chat assistant (Claude REPL)"),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Comment")),
				FUIAction(FExecuteAction::CreateLambda([]()
			{
				FGlobalTabmanager::Get()->TryInvokeTab(SClaireonDiagnosticsWidget::TabId);
			})));

			WindowSection.AddMenuEntry(
				TEXT("LaunchClaudeCode"),
				LOCTEXT("WindowLaunchClaudeCodeLabel", "Launch Claude Code"),
				LOCTEXT("WindowLaunchClaudeCodeTooltip",
					"Open a terminal at the project root and start Claude Code against this editor's MCP server."),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Console")),
				FUIAction(FExecuteAction::CreateLambda([]()
			{
				ClaireonLaunch::LaunchClaudeCodeFromProjectDir();
			})));
		}
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FClaireonModule, Claireon)
