// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Smoke tests for the decomposed MCP tool inventory produced by
// DECOMPOSE_TOOLS stages 003-017. Each decomposed tool class is instantiated
// directly and asserted to expose a non-empty Name, non-empty Description,
// and a valid InputSchema. This guarantees that every decomposed tool is at
// least discoverable through the standard IClaireonTool surface, satisfying
// stage 019's "at least one smoke test per decomposed tool" requirement.

#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/IClaireonTool.h"

// Transaction (stage 003)
#include "Tools/ClaireonTool_TransactionUndo.h"
#include "Tools/ClaireonTool_TransactionRedo.h"
#include "Tools/ClaireonTool_TransactionHistory.h"
#include "Tools/ClaireonTool_TransactionBeginGroup.h"
#include "Tools/ClaireonTool_TransactionEndGroup.h"
#include "Tools/ClaireonTool_TransactionRollbackGroup.h"

// Blackboard (stage 004)
#include "Tools/ClaireonBlackboardTool_Open.h"
#include "Tools/ClaireonBlackboardTool_Close.h"
#include "Tools/ClaireonBlackboardTool_Save.h"
#include "Tools/ClaireonBlackboardTool_Status.h"
#include "Tools/ClaireonBlackboardTool_AddKey.h"
#include "Tools/ClaireonBlackboardTool_RemoveKey.h"
#include "Tools/ClaireonBlackboardTool_RenameKey.h"
#include "Tools/ClaireonBlackboardTool_SetKeyType.h"
#include "Tools/ClaireonBlackboardTool_SetParent.h"
#include "Tools/ClaireonBlackboardTool_ApplySpec.h"

// EQS (stage 005)
#include "Tools/ClaireonEQSTool_ApplySpec.h"

// PCG (stage 006)
#include "Tools/ClaireonPCGGraphTool_ApplySpec.h"

// Behavior Tree (stage 007)
#include "Tools/ClaireonBehaviorTreeTool_ApplySpec.h"

// Niagara (stage 014)
#include "Tools/ClaireonNiagaraTool_ApplySpec.h"

// StateTree (stage 015)
#include "Tools/ClaireonStateTreeTool_ApplySpec.h"

// Blueprint Graph (stage 016)
#include "Tools/ClaireonBlueprintGraphTool_ApplySpec.h"
#include "Tools/ClaireonBlueprintGraphTool_Create.h"

// Widget BP (stage 017)
#include "Tools/ClaireonWidgetBPTool_ApplySpec.h"
#include "Tools/ClaireonWidgetBPTool_Create.h"
#include "Tools/ClaireonWidgetBPTool_AddWidget.h"
#include "Tools/ClaireonWidgetBPTool_SetWidgetProperty.h"
#include "Tools/ClaireonWidgetBPTool_Compile.h"
#include "Tools/ClaireonWidgetBPTool_Save.h"
#include "Tools/ClaireonWidgetBPTool_Close.h"

// Chooser decomposition (#0000 cohort 1)
#include "Tools/ClaireonChooserTool_SetResultType.h"
#include "Tools/ClaireonChooserTool_SetOutputClass.h"
#include "Tools/ClaireonChooserTool_AddContextParameter.h"
#include "Tools/ClaireonChooserTool_RemoveContextParameter.h"
#include "Tools/ClaireonChooserTool_SetContextParameterDirection.h"
#include "Tools/ClaireonChooserTool_SetFallbackResult.h"

// ProxyAsset decomposition (#0000 cohort 2)
#include "Tools/ClaireonProxyAssetTool_SetType.h"
#include "Tools/ClaireonProxyAssetTool_SetResultType.h"
#include "Tools/ClaireonProxyAssetTool_AddContextParameter.h"
#include "Tools/ClaireonProxyAssetTool_RemoveContextParameter.h"
#include "Tools/ClaireonProxyAssetTool_SetContextParameterDirection.h"

// ProxyTable decomposition (#0000 cohort 3)
#include "Tools/ClaireonProxyTableTool_AddInherit.h"
#include "Tools/ClaireonProxyTableTool_RemoveInherit.h"

namespace DecomposedToolsSmokeHelpers
{
	template<typename TToolClass>
	bool ValidateTool(const TCHAR* ExpectedNameSubstr)
	{
		TToolClass Tool;
		const FString Name = Tool.GetName();
		const FString Desc = Tool.GetDescription();
		TSharedPtr<FJsonObject> Schema = Tool.GetInputSchema();

		if (Name.IsEmpty())
		{
			UE_LOG(LogTemp, Error, TEXT("[DecomposedToolsSmoke] Empty Name for tool"));
			return false;
		}
		if (!Name.StartsWith(TEXT("claireon.")))
		{
			UE_LOG(LogTemp, Error, TEXT("[DecomposedToolsSmoke] Tool name '%s' missing 'claireon.' prefix"), *Name);
			return false;
		}
		if (ExpectedNameSubstr && !Name.Contains(ExpectedNameSubstr))
		{
			UE_LOG(LogTemp, Error, TEXT("[DecomposedToolsSmoke] Tool name '%s' missing expected substring '%s'"),
				*Name, ExpectedNameSubstr);
			return false;
		}
		if (Desc.IsEmpty())
		{
			UE_LOG(LogTemp, Error, TEXT("[DecomposedToolsSmoke] Empty Description for tool '%s'"), *Name);
			return false;
		}
		if (!Schema.IsValid())
		{
			UE_LOG(LogTemp, Error, TEXT("[DecomposedToolsSmoke] Null InputSchema for tool '%s'"), *Name);
			return false;
		}
		FString SchemaType;
		if (!Schema->TryGetStringField(TEXT("type"), SchemaType))
		{
			UE_LOG(LogTemp, Error, TEXT("[DecomposedToolsSmoke] InputSchema for tool '%s' missing 'type'"), *Name);
			return false;
		}
		return true;
	}
}

UNTEST_UNIT(Claireon, DecomposedToolsSmoke, TransactionToolsExposeDiscoverableSurface)
{
	using namespace DecomposedToolsSmokeHelpers;
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonTool_TransactionUndo>(TEXT("transaction_undo")));
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonTool_TransactionRedo>(TEXT("transaction_redo")));
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonTool_TransactionHistory>(TEXT("transaction_history")));
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonTool_TransactionBeginGroup>(TEXT("transaction_begin_group")));
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonTool_TransactionEndGroup>(TEXT("transaction_end_group")));
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonTool_TransactionRollbackGroup>(TEXT("transaction_rollback_group")));
	co_return;
}

UNTEST_UNIT(Claireon, DecomposedToolsSmoke, BlackboardToolsExposeDiscoverableSurface)
{
	using namespace DecomposedToolsSmokeHelpers;
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonBlackboardTool_Open>(TEXT("blackboard_open")));
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonBlackboardTool_Close>(TEXT("blackboard_close")));
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonBlackboardTool_Save>(TEXT("blackboard_save")));
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonBlackboardTool_Status>(TEXT("blackboard_status")));
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonBlackboardTool_AddKey>(TEXT("blackboard_add_key")));
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonBlackboardTool_RemoveKey>(TEXT("blackboard_remove_key")));
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonBlackboardTool_RenameKey>(TEXT("blackboard_rename_key")));
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonBlackboardTool_SetKeyType>(TEXT("blackboard_set_key_type")));
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonBlackboardTool_SetParent>(TEXT("blackboard_set_parent")));
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonBlackboardTool_ApplySpec>(TEXT("blackboard_apply_spec")));
	co_return;
}

UNTEST_UNIT(Claireon, DecomposedToolsSmoke, ApplySpecToolsExposeDiscoverableSurface)
{
	using namespace DecomposedToolsSmokeHelpers;
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonBehaviorTreeTool_ApplySpec>(TEXT("behaviortree_apply_spec")));
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonEQSTool_ApplySpec>(TEXT("eqs_apply_spec")));
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonPCGGraphTool_ApplySpec>(TEXT("pcg_apply_spec")));
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonNiagaraTool_ApplySpec>(TEXT("niagara_apply_spec")));
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonStateTreeTool_ApplySpec>(TEXT("statetree_apply_spec")));
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonBlueprintGraphTool_ApplySpec>(TEXT("blueprint_graph_apply_spec")));
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonWidgetBPTool_ApplySpec>(TEXT("widgetbp_apply_spec")));
	co_return;
}

UNTEST_UNIT(Claireon, DecomposedToolsSmoke, WidgetBPCoreLifecycleToolsExposeDiscoverableSurface)
{
	using namespace DecomposedToolsSmokeHelpers;
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonWidgetBPTool_Create>(TEXT("widgetbp_create")));
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonWidgetBPTool_AddWidget>(TEXT("widgetbp_add_widget")));
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonWidgetBPTool_SetWidgetProperty>(TEXT("widgetbp_set_widget_property")));
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonWidgetBPTool_Compile>(TEXT("widgetbp_compile")));
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonWidgetBPTool_Save>(TEXT("widgetbp_save")));
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonWidgetBPTool_Close>(TEXT("widgetbp_close")));
	co_return;
}

UNTEST_UNIT(Claireon, DecomposedToolsSmoke, BlueprintGraphCreateExposesDiscoverableSurface)
{
	using namespace DecomposedToolsSmokeHelpers;
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonBlueprintGraphTool_Create>(TEXT("blueprint_graph_create")));
	co_return;
}

UNTEST_UNIT(Claireon, DecomposedToolsSmoke, ChooserDecomposedToolsExposeDiscoverableSurface)
{
	using namespace DecomposedToolsSmokeHelpers;
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonTool_ChooserSetResultType>(TEXT("chooser_set_result_type")));
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonTool_ChooserSetOutputClass>(TEXT("chooser_set_output_class")));
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonTool_ChooserAddContextParameter>(TEXT("chooser_add_context_parameter")));
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonTool_ChooserRemoveContextParameter>(TEXT("chooser_remove_context_parameter")));
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonTool_ChooserSetContextParameterDirection>(TEXT("chooser_set_context_parameter_direction")));
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonTool_ChooserSetFallbackResult>(TEXT("chooser_set_fallback_result")));
	co_return;
}

UNTEST_UNIT(Claireon, DecomposedToolsSmoke, ProxyAssetDecomposedToolsExposeDiscoverableSurface)
{
	using namespace DecomposedToolsSmokeHelpers;
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonTool_ProxyAssetSetType>(TEXT("proxyasset_set_type")));
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonTool_ProxyAssetSetResultType>(TEXT("proxyasset_set_result_type")));
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonTool_ProxyAssetAddContextParameter>(TEXT("proxyasset_add_context_parameter")));
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonTool_ProxyAssetRemoveContextParameter>(TEXT("proxyasset_remove_context_parameter")));
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonTool_ProxyAssetSetContextParameterDirection>(TEXT("proxyasset_set_context_parameter_direction")));
	co_return;
}

UNTEST_UNIT(Claireon, DecomposedToolsSmoke, ProxyTableDecomposedToolsExposeDiscoverableSurface)
{
	using namespace DecomposedToolsSmokeHelpers;
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonTool_ProxyTableAddInherit>(TEXT("proxytable_add_inherit")));
	UNTEST_EXPECT_TRUE(ValidateTool<ClaireonTool_ProxyTableRemoveInherit>(TEXT("proxytable_remove_inherit")));
	co_return;
}

#endif // WITH_UNTESTED
