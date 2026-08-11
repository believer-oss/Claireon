// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Contract tests for the runtime GAS tools (gas_runtime_inspect / gas_apply_effect /
// gas_remove_effect / gas_grant_ability / gas_activate_ability / gas_set_tags /
// gas_set_attribute). Runtime GAS behavior needs a live PIE session and cannot
// be exercised headless, so these lock the pure, environment-independent
// contract: exact tool names, ReadOnly session mode, the shared
// actorId/net_mode/pie_instance schema surface, and the missing-actorId error
// path. Manual PIE smoke coverage is documented in CLAIREON-GAS-TOOLS-SPEC.md
// section 4 (mirrors how pie_test_ability is covered).

#if WITH_UNTESTED

#include "Untest.h"

#include "Tools/IClaireonTool.h"
#include "Tools/ClaireonTool_GasInspect.h"
#include "Tools/ClaireonTool_GasApplyEffect.h"
#include "Tools/ClaireonTool_GasRemoveEffect.h"
#include "Tools/ClaireonTool_GasGrantAbility.h"
#include "Tools/ClaireonTool_GasActivateAbility.h"
#include "Tools/ClaireonTool_GasSetTags.h"
#include "Tools/ClaireonTool_GasSetAttribute.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace ClaireonGasToolsContractHelpers
{
	struct FNamedTool
	{
		TSharedPtr<IClaireonTool> Tool;
		FString ExpectedName;
	};

	// One instance of every gas_* tool paired with its expected wire name.
	static TArray<FNamedTool> MakeGasTools()
	{
		TArray<FNamedTool> Tools;
		Tools.Add({ MakeShared<ClaireonTool_GasInspect>(),         TEXT("gas_runtime_inspect") });
		Tools.Add({ MakeShared<ClaireonTool_GasApplyEffect>(),     TEXT("gas_apply_effect") });
		Tools.Add({ MakeShared<ClaireonTool_GasRemoveEffect>(),    TEXT("gas_remove_effect") });
		Tools.Add({ MakeShared<ClaireonTool_GasGrantAbility>(),    TEXT("gas_grant_ability") });
		Tools.Add({ MakeShared<ClaireonTool_GasActivateAbility>(), TEXT("gas_activate_ability") });
		Tools.Add({ MakeShared<ClaireonTool_GasSetTags>(),         TEXT("gas_set_tags") });
		Tools.Add({ MakeShared<ClaireonTool_GasSetAttribute>(),    TEXT("gas_set_attribute") });
		return Tools;
	}

	static bool SchemaHasProperty(const TSharedPtr<FJsonObject>& Schema, const TCHAR* PropertyName)
	{
		if (!Schema.IsValid()) { return false; }
		const TSharedPtr<FJsonObject>* Properties = nullptr;
		if (!Schema->TryGetObjectField(TEXT("properties"), Properties) || Properties == nullptr)
		{
			return false;
		}
		return (*Properties)->HasField(PropertyName);
	}

	static bool SchemaRequires(const TSharedPtr<FJsonObject>& Schema, const FString& Field)
	{
		if (!Schema.IsValid()) { return false; }
		const TArray<TSharedPtr<FJsonValue>>* Required = nullptr;
		if (!Schema->TryGetArrayField(TEXT("required"), Required) || Required == nullptr)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Required)
		{
			if (Value.IsValid() && Value->AsString() == Field)
			{
				return true;
			}
		}
		return false;
	}
}

using namespace ClaireonGasToolsContractHelpers;

// Budget: the bare UNTEST_UNIT default is 0.50ms (FUntestUnitFixture::DefaultTimeoutMs),
// which is not a deliberate perf assertion. Too tight now that the tool registry is
// populated process-wide and this test does real work -- do not restore the default.
UNTEST_UNIT_OPTS(Claireon, GasToolsContract, ToolNamesAreExact, UNTEST_TIMEOUTMS(10000))
{
	for (const FNamedTool& Entry : MakeGasTools())
	{
		UNTEST_ASSERT_TRUE(Entry.Tool.IsValid());
		UNTEST_EXPECT_STREQ(*Entry.Tool->GetName(), *Entry.ExpectedName);
	}
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, GasToolsContract, AllToolsAreReadOnlySession, UNTEST_TIMEOUTMS(10000))
{
	// These mutate PIE runtime, not on-disk assets -- no asset-session handling.
	for (const FNamedTool& Entry : MakeGasTools())
	{
		UNTEST_EXPECT_TRUE(Entry.Tool->GetSessionMode() == EClaireonToolSessionMode::ReadOnly);
		UNTEST_EXPECT_FALSE(Entry.Tool->RequiresNoPIE());
	}
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, GasToolsContract, SchemasExposeActorIdAndPieParams, UNTEST_TIMEOUTMS(10000))
{
	for (const FNamedTool& Entry : MakeGasTools())
	{
		const TSharedPtr<FJsonObject> Schema = Entry.Tool->GetInputSchema();
		UNTEST_ASSERT_TRUE(Schema.IsValid());
		// Shared surface from ClaireonGasToolCommon::AddCommonSchemaParams.
		UNTEST_EXPECT_TRUE(SchemaHasProperty(Schema, TEXT("actorId")));
		UNTEST_EXPECT_TRUE(SchemaHasProperty(Schema, TEXT("pie_instance")));
		UNTEST_EXPECT_TRUE(SchemaHasProperty(Schema, TEXT("net_mode")));
		// Every gas_* tool marks actorId required.
		UNTEST_EXPECT_TRUE(SchemaRequires(Schema, TEXT("actorId")));
	}
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, GasToolsContract, MissingActorIdIsAnError, UNTEST_TIMEOUTMS(10000))
{
	// Environment-independent: actorId is validated before the PIE guard, so an
	// empty arg object errors on actorId regardless of whether PIE is running.
	for (const FNamedTool& Entry : MakeGasTools())
	{
		const TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		const IClaireonTool::FToolResult Result = Entry.Tool->Execute(Args);
		UNTEST_EXPECT_TRUE(Result.bIsError);
		UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("actorId")));
	}
	co_return;
}

#endif // WITH_UNTESTED
