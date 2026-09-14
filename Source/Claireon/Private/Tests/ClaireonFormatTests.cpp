// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Headless format tests cover capability, session, graph, argument, and settings contracts.
// Live BA handler and settlement behavior require the editor suite.

#if WITH_UNTESTED

#include "Untest.h"

#include "ClaireonSessionManager.h"
#include "Tools/ClaireonBlueprintGraphTool_Create.h"
#include "Tools/ClaireonBlueprintGraphTool_Format.h"
#include "Tools/ClaireonBlueprintGraphTool_Open.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "UObject/SoftObjectPath.h"

#if WITH_BLUEPRINT_ASSIST
#include "BlueprintAssistSettings.h"
#endif

#include "ClaireonTestAssetDeletion.h"

namespace ClaireonFormatTestsInternal
{
	// Prefix helpers to avoid unity-build collisions.

	static void FMT_Cleanup(const FString& AssetPath)
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

	static FString FMT_CreateAndOpen(const TCHAR* AssetPath)
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

	static IClaireonTool::FToolResult FMT_Format(const FString& SessionId, const TCHAR* GraphName)
	{
		ClaireonBlueprintGraphTool_Format Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		if (!SessionId.IsEmpty())
		{
			Args->SetStringField(TEXT("session_id"), SessionId);
		}
		if (GraphName)
		{
			Args->SetStringField(TEXT("graph_name"), GraphName);
		}
		return Tool.Execute(Args);
	}
}

// Headless formatting must report an unavailable capability.
UNTEST_UNIT_OPTS(Claireon, Format, Format_HeadlessGivesAnExplicitCapabilityError,
	UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonFormatTestsInternal;
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_FMT_Headless");
	const FString SessionId = FMT_CreateAndOpen(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	IClaireonTool::FToolResult R = FMT_Format(SessionId, nullptr);

	UNTEST_EXPECT_TRUE(R.bIsError);
	// Allow either missing Slate or missing BA, depending on the build.
	const bool bNamesCapability =
		R.ErrorMessage.Contains(TEXT("requires an interactive editor"))
		|| R.ErrorMessage.Contains(TEXT("requires the BlueprintAssist plugin"));
	UNTEST_EXPECT_TRUE(bNamesCapability);
	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("headless"))
		|| R.ErrorMessage.Contains(TEXT("commandlet"))
		|| R.ErrorMessage.Contains(TEXT("no fallback")));

	FMT_Cleanup(AssetPath);
	co_return;
}

// Unknown graphs must fail before capability checks.
UNTEST_UNIT_OPTS(Claireon, Format, Format_UnknownGraphNameIsNamed, UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonFormatTestsInternal;
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_FMT_UnknownGraph");
	const FString SessionId = FMT_CreateAndOpen(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	IClaireonTool::FToolResult R = FMT_Format(SessionId, TEXT("NoSuchGraph"));

	UNTEST_EXPECT_TRUE(R.bIsError);
	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("NoSuchGraph")));
	UNTEST_EXPECT_FALSE(R.ErrorMessage.Contains(TEXT("requires an interactive editor")));

	FMT_Cleanup(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, Format, Format_UnknownSessionIsRejected, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonFormatTestsInternal;
	IClaireonTool::FToolResult R = FMT_Format(TEXT("not-a-real-session"), nullptr);
	UNTEST_EXPECT_TRUE(R.bIsError);
	UNTEST_EXPECT_FALSE(R.ErrorMessage.Contains(TEXT("requires an interactive editor")));
	co_return;
}

// Formatting must preserve BA settings, including on refusal.
UNTEST_UNIT_OPTS(Claireon, Format, Format_LeavesBlueprintAssistSettingsUntouched,
	UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonFormatTestsInternal;

#if !WITH_BLUEPRINT_ASSIST
	UNTEST_EXPECT_TRUE(true);
	co_return;
#else
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_FMT_Settings");
	const FString SessionId = FMT_CreateAndOpen(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	const UBASettings* Settings = GetDefault<UBASettings>();
	UNTEST_ASSERT_TRUE(Settings != nullptr);

	const auto StyleBefore = Settings->FormatAllStyle;
	const bool bDetectBefore = Settings->bDetectNewNodesAndCacheNodeSizes;

	(void)FMT_Format(SessionId, nullptr);

	const UBASettings* After = GetDefault<UBASettings>();
	UNTEST_ASSERT_TRUE(After != nullptr);
	UNTEST_EXPECT_TRUE(After->FormatAllStyle == StyleBefore);
	UNTEST_EXPECT_TRUE(After->bDetectNewNodesAndCacheNodeSizes == bDetectBefore);

	FMT_Cleanup(AssetPath);
	co_return;
#endif
}

// Require distinct errors for distinct causes.
UNTEST_UNIT_OPTS(Claireon, Format, Format_RefusalsAreDistinguishable, UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonFormatTestsInternal;
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_FMT_Distinct");
	const FString SessionId = FMT_CreateAndOpen(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	const FString Capability = FMT_Format(SessionId, nullptr).ErrorMessage;
	const FString UnknownGraph = FMT_Format(SessionId, TEXT("NoSuchGraph")).ErrorMessage;
	const FString UnknownSession = FMT_Format(TEXT("not-a-real-session"), nullptr).ErrorMessage;

	UNTEST_EXPECT_FALSE(Capability.IsEmpty());
	UNTEST_EXPECT_FALSE(UnknownGraph.IsEmpty());
	UNTEST_EXPECT_FALSE(UnknownSession.IsEmpty());
	UNTEST_EXPECT_STRNE(*Capability, *UnknownGraph);
	UNTEST_EXPECT_STRNE(*Capability, *UnknownSession);
	UNTEST_EXPECT_STRNE(*UnknownGraph, *UnknownSession);

	FMT_Cleanup(AssetPath);
	co_return;
}

// Validate island_guids before capability checks; invalid or empty lists must not widen the request to all islands.

namespace ClaireonFormatTestsInternal
{
	static IClaireonTool::FToolResult FMT_FormatWithIslands(const FString& SessionId,
	                                                        TSharedPtr<FJsonValue> IslandGuids)
	{
		ClaireonBlueprintGraphTool_Format Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		if (IslandGuids.IsValid())
		{
			Args->SetField(TEXT("island_guids"), IslandGuids);
		}
		return Tool.Execute(Args);
	}

	static FString FMT_RefusalReason(const IClaireonTool::FToolResult& Result)
	{
		FString Reason;
		if (Result.Data.IsValid())
		{
			Result.Data->TryGetStringField(TEXT("refusal_reason"), Reason);
		}
		return Reason;
	}
}

// Reject a bare string instead of an array.
UNTEST_UNIT_OPTS(Claireon, Format, Format_IslandGuidsWrongTypeIsRefused, UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonFormatTestsInternal;
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_FMT_IslandsWrongType");
	const FString SessionId = FMT_CreateAndOpen(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	const IClaireonTool::FToolResult R =
		FMT_FormatWithIslands(SessionId, MakeShared<FJsonValueString>(TEXT("A1B2C3D4")));

	UNTEST_ASSERT_TRUE(R.bIsError);
	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("island_guids")));
	const FString Reason = FMT_RefusalReason(R);
	UNTEST_EXPECT_STREQ(Reason, TEXT("bad_argument"));

	// Verify argument validation, not the environment gate, produced the refusal.
	UNTEST_EXPECT_FALSE(R.ErrorMessage.Contains(TEXT("requires an interactive editor")));

	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("every")));

	FMT_Cleanup(AssetPath);
	co_return;
}

// Reject non-string array elements without coercion.
UNTEST_UNIT_OPTS(Claireon, Format, Format_IslandGuidsNonStringElementIsRefused,
	UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonFormatTestsInternal;
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_FMT_IslandsBadElement");
	const FString SessionId = FMT_CreateAndOpen(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	TArray<TSharedPtr<FJsonValue>> Elements;
	Elements.Add(MakeShared<FJsonValueNumber>(3));

	const IClaireonTool::FToolResult R =
		FMT_FormatWithIslands(SessionId, MakeShared<FJsonValueArray>(Elements));

	UNTEST_ASSERT_TRUE(R.bIsError);
	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("GUID strings")));
	const FString Reason = FMT_RefusalReason(R);
	UNTEST_EXPECT_STREQ(Reason, TEXT("bad_argument"));
	UNTEST_EXPECT_FALSE(R.ErrorMessage.Contains(TEXT("requires an interactive editor")));

	FMT_Cleanup(AssetPath);
	co_return;
}

// An explicit empty list must not mean all islands.
UNTEST_UNIT_OPTS(Claireon, Format, Format_IslandGuidsEmptyArrayIsRefused, UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonFormatTestsInternal;
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_FMT_IslandsEmpty");
	const FString SessionId = FMT_CreateAndOpen(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	const TArray<TSharedPtr<FJsonValue>> Empty;
	const IClaireonTool::FToolResult R =
		FMT_FormatWithIslands(SessionId, MakeShared<FJsonValueArray>(Empty));

	UNTEST_ASSERT_TRUE(R.bIsError);
	const FString Reason = FMT_RefusalReason(R);
	UNTEST_EXPECT_STREQ(Reason, TEXT("bad_argument"));
	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("Omit the field")));
	UNTEST_EXPECT_FALSE(R.ErrorMessage.Contains(TEXT("requires an interactive editor")));

	FMT_Cleanup(AssetPath);
	co_return;
}

// Omitting island_guids still requests every island and reaches the capability gate.
UNTEST_UNIT_OPTS(Claireon, Format, Format_OmittedIslandGuidsStillMeansAll, UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonFormatTestsInternal;
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_FMT_IslandsOmitted");
	const FString SessionId = FMT_CreateAndOpen(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	const IClaireonTool::FToolResult R = FMT_FormatWithIslands(SessionId, nullptr);

	UNTEST_ASSERT_TRUE(R.bIsError);
	const FString Reason = FMT_RefusalReason(R);
	UNTEST_EXPECT_STRNE(*Reason, TEXT("bad_argument"));
	const bool bNamesCapability =
		R.ErrorMessage.Contains(TEXT("requires an interactive editor"))
		|| R.ErrorMessage.Contains(TEXT("requires the BlueprintAssist plugin"));
	UNTEST_EXPECT_TRUE(bNamesCapability);

	FMT_Cleanup(AssetPath);
	co_return;
}

#endif // WITH_UNTESTED
