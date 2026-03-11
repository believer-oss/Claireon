// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/IClaireonTool.h"
#include "Tools/ClaireonTool_NiagaraInspect.h"
#include "Tools/ClaireonTool_NiagaraEdit.h"
#include "Tools/ClaireonNiagaraHelpers.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitterHandle.h"
#include "ClaireonSessionManager.h"

// ---------------------------------------------------------------------------
// Test asset paths
// ---------------------------------------------------------------------------
static const TCHAR* TestNiagaraSystemPath = TEXT("/Game/Art_Lib/VOL/NS_LocalVolumeFog");

// ============================================================================
// ClaireonNiagaraHelpers
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, Niagara, LoadValidSystem, UNTEST_TIMEOUTMS(10000))
{
	FString Error;
	UNiagaraSystem* System = ClaireonNiagaraHelpers::LoadNiagaraSystemAsset(TestNiagaraSystemPath, Error);
	UNTEST_ASSERT_TRUE(System != nullptr);
	UNTEST_EXPECT_TRUE(Error.IsEmpty());
	UNTEST_EXPECT_TRUE(System->GetName().Contains(TEXT("NS_LocalVolumeFog")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, Niagara, LoadInvalidPath, UNTEST_TIMEOUTMS(5000))
{
	FString Error;
	UNiagaraSystem* System = ClaireonNiagaraHelpers::LoadNiagaraSystemAsset(TEXT("/Game/DoesNotExist/NS_Fake"), Error);
	UNTEST_ASSERT_TRUE(System == nullptr);
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("Failed to load")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, Niagara, LoadEmptyPath, UNTEST_TIMEOUTMS(5000))
{
	FString Error;
	UNiagaraSystem* System = ClaireonNiagaraHelpers::LoadNiagaraSystemAsset(TEXT(""), Error);
	UNTEST_ASSERT_TRUE(System == nullptr);
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("empty")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, Niagara, FormatSystemStructure, UNTEST_TIMEOUTMS(10000))
{
	FString Error;
	UNiagaraSystem* System = ClaireonNiagaraHelpers::LoadNiagaraSystemAsset(TestNiagaraSystemPath, Error);
	UNTEST_ASSERT_TRUE(System != nullptr);

	FString Output = ClaireonNiagaraHelpers::FormatNiagaraSystemStructure(System, true);
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("=== Niagara System:")));
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("Path:")));
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("Emitters:")));
	// Should contain at least one emitter section if the system has emitters
	if (System->GetEmitterHandles().Num() > 0)
	{
		UNTEST_EXPECT_TRUE(Output.Contains(TEXT("--- Emitter")));
	}
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, Niagara, ResolveRendererClassShorthand, UNTEST_TIMEOUTMS(5000))
{
	FString Error;

	UClass* SpriteClass = ClaireonNiagaraHelpers::ResolveRendererClass(TEXT("Sprite"), Error);
	UNTEST_ASSERT_TRUE(SpriteClass != nullptr);
	UNTEST_EXPECT_TRUE(SpriteClass->GetName().Contains(TEXT("Sprite")));

	UClass* MeshClass = ClaireonNiagaraHelpers::ResolveRendererClass(TEXT("Mesh"), Error);
	UNTEST_ASSERT_TRUE(MeshClass != nullptr);
	UNTEST_EXPECT_TRUE(MeshClass->GetName().Contains(TEXT("Mesh")));

	UClass* RibbonClass = ClaireonNiagaraHelpers::ResolveRendererClass(TEXT("Ribbon"), Error);
	UNTEST_ASSERT_TRUE(RibbonClass != nullptr);
	UNTEST_EXPECT_TRUE(RibbonClass->GetName().Contains(TEXT("Ribbon")));

	UClass* LightClass = ClaireonNiagaraHelpers::ResolveRendererClass(TEXT("Light"), Error);
	UNTEST_ASSERT_TRUE(LightClass != nullptr);
	UNTEST_EXPECT_TRUE(LightClass->GetName().Contains(TEXT("Light")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, Niagara, ResolveRendererClassInvalid, UNTEST_TIMEOUTMS(5000))
{
	FString Error;
	UClass* InvalidClass = ClaireonNiagaraHelpers::ResolveRendererClass(TEXT("NonexistentRenderer"), Error);
	UNTEST_ASSERT_TRUE(InvalidClass == nullptr);
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("Could not resolve")));
	co_return;
}

// ============================================================================
// editor.niagara.inspect
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, Niagara, InspectMissingAssetPath, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_NiagaraInspect Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("asset_path")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, Niagara, InspectBadAssetPath, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_NiagaraInspect Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TEXT("/Game/DoesNotExist/NS_Fake"));
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("Failed to load")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, Niagara, InspectFullDetail, UNTEST_TIMEOUTMS(10000))
{
	ClaireonTool_NiagaraInspect Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TestNiagaraSystemPath);
	Args->SetStringField(TEXT("detail_level"), TEXT("full"));
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);

	FString Output = Result.GetContentAsString();
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("=== Niagara System:")));
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("NS_LocalVolumeFog")));
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("Emitters:")));
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("=== User Parameters ===")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, Niagara, InspectSummary, UNTEST_TIMEOUTMS(10000))
{
	ClaireonTool_NiagaraInspect Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TestNiagaraSystemPath);
	Args->SetStringField(TEXT("detail_level"), TEXT("summary"));
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);

	FString Output = Result.GetContentAsString();
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("=== Niagara System:")));
	// Summary should still contain emitter structure
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("Emitters:")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, Niagara, InspectEmitterIndexOutOfRange, UNTEST_TIMEOUTMS(10000))
{
	ClaireonTool_NiagaraInspect Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TestNiagaraSystemPath);
	Args->SetNumberField(TEXT("emitter_index"), 999);
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("out of range")));
	co_return;
}

// ============================================================================
// editor.niagara.edit — Session Lifecycle
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, Niagara, EditOpenCloseSession, UNTEST_TIMEOUTMS(15000))
{
	ClaireonTool_NiagaraEdit Tool;

	// Open session
	TSharedPtr<FJsonObject> OpenArgs = MakeShared<FJsonObject>();
	OpenArgs->SetStringField(TEXT("operation"), TEXT("open"));
	TSharedPtr<FJsonObject> OpenParams = MakeShared<FJsonObject>();
	OpenParams->SetStringField(TEXT("asset_path"), TestNiagaraSystemPath);
	OpenArgs->SetObjectField(TEXT("params"), OpenParams);

	auto OpenResult = Tool.Execute(OpenArgs);
	UNTEST_ASSERT_FALSE(OpenResult.bIsError);

	FString OpenOutput = OpenResult.GetContentAsString();
	UNTEST_EXPECT_TRUE(OpenOutput.Contains(TEXT("Session ID:")));
	UNTEST_EXPECT_TRUE(OpenOutput.Contains(TEXT("=== Niagara System:")));

	// Extract session ID
	FString SessionId;
	{
		int32 SessionStart = OpenOutput.Find(TEXT("Session ID: "));
		if (SessionStart != INDEX_NONE)
		{
			SessionStart += FString(TEXT("Session ID: ")).Len();
			int32 SessionEnd = OpenOutput.Find(TEXT("\n"), ESearchCase::IgnoreCase, ESearchDir::FromStart, SessionStart);
			if (SessionEnd != INDEX_NONE)
			{
				SessionId = OpenOutput.Mid(SessionStart, SessionEnd - SessionStart).TrimStartAndEnd();
			}
		}
	}
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	// Verify asset is locked via session manager
	UNTEST_EXPECT_TRUE(FClaireonSessionManager::Get().IsAssetLocked(TestNiagaraSystemPath));

	// Close session
	TSharedPtr<FJsonObject> CloseArgs = MakeShared<FJsonObject>();
	CloseArgs->SetStringField(TEXT("operation"), TEXT("close"));
	CloseArgs->SetStringField(TEXT("session_id"), SessionId);
	CloseArgs->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());

	auto CloseResult = Tool.Execute(CloseArgs);
	UNTEST_ASSERT_FALSE(CloseResult.bIsError);
	UNTEST_EXPECT_TRUE(CloseResult.GetContentAsString().Contains(TEXT("Session closed")));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, Niagara, EditMissingOperation, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_NiagaraEdit Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("operation")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, Niagara, EditMissingSessionId, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_NiagaraEdit Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("operation"), TEXT("status"));
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("session_id")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, Niagara, EditInvalidSession, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_NiagaraEdit Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("operation"), TEXT("status"));
	Args->SetStringField(TEXT("session_id"), TEXT("nonexistent-session-id"));
	Args->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("not found")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, Niagara, EditSuppressOutput, UNTEST_TIMEOUTMS(15000))
{
	ClaireonTool_NiagaraEdit Tool;

	// Open session
	TSharedPtr<FJsonObject> OpenArgs = MakeShared<FJsonObject>();
	OpenArgs->SetStringField(TEXT("operation"), TEXT("open"));
	TSharedPtr<FJsonObject> OpenParams = MakeShared<FJsonObject>();
	OpenParams->SetStringField(TEXT("asset_path"), TestNiagaraSystemPath);
	OpenArgs->SetObjectField(TEXT("params"), OpenParams);

	auto OpenResult = Tool.Execute(OpenArgs);
	UNTEST_ASSERT_FALSE(OpenResult.bIsError);

	// Extract session ID
	FString OpenOutput = OpenResult.GetContentAsString();
	FString SessionId;
	{
		int32 SessionStart = OpenOutput.Find(TEXT("Session ID: "));
		if (SessionStart != INDEX_NONE)
		{
			SessionStart += FString(TEXT("Session ID: ")).Len();
			int32 SessionEnd = OpenOutput.Find(TEXT("\n"), ESearchCase::IgnoreCase, ESearchDir::FromStart, SessionStart);
			if (SessionEnd != INDEX_NONE)
			{
				SessionId = OpenOutput.Mid(SessionStart, SessionEnd - SessionStart).TrimStartAndEnd();
			}
		}
	}
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	// Status with suppress_output=true should return minimal response
	TSharedPtr<FJsonObject> StatusArgs = MakeShared<FJsonObject>();
	StatusArgs->SetStringField(TEXT("operation"), TEXT("status"));
	StatusArgs->SetStringField(TEXT("session_id"), SessionId);
	StatusArgs->SetBoolField(TEXT("suppress_output"), true);
	StatusArgs->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());

	auto StatusResult = Tool.Execute(StatusArgs);
	UNTEST_ASSERT_FALSE(StatusResult.bIsError);

	FString StatusOutput = StatusResult.GetContentAsString();
	UNTEST_EXPECT_TRUE(StatusOutput.StartsWith(TEXT("ok")));
	// Suppressed output should NOT contain full system structure
	UNTEST_EXPECT_FALSE(StatusOutput.Contains(TEXT("=== Niagara System:")));

	// Close session
	TSharedPtr<FJsonObject> CloseArgs = MakeShared<FJsonObject>();
	CloseArgs->SetStringField(TEXT("operation"), TEXT("close"));
	CloseArgs->SetStringField(TEXT("session_id"), SessionId);
	CloseArgs->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
	Tool.Execute(CloseArgs);

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, Niagara, EditSessionExclusivity, UNTEST_TIMEOUTMS(15000))
{
	ClaireonTool_NiagaraEdit Tool;

	// Open first session
	TSharedPtr<FJsonObject> OpenArgs1 = MakeShared<FJsonObject>();
	OpenArgs1->SetStringField(TEXT("operation"), TEXT("open"));
	TSharedPtr<FJsonObject> OpenParams1 = MakeShared<FJsonObject>();
	OpenParams1->SetStringField(TEXT("asset_path"), TestNiagaraSystemPath);
	OpenArgs1->SetObjectField(TEXT("params"), OpenParams1);

	auto OpenResult1 = Tool.Execute(OpenArgs1);
	UNTEST_ASSERT_FALSE(OpenResult1.bIsError);

	// Extract session ID for cleanup
	FString OpenOutput1 = OpenResult1.GetContentAsString();
	FString SessionId1;
	{
		int32 SessionStart = OpenOutput1.Find(TEXT("Session ID: "));
		if (SessionStart != INDEX_NONE)
		{
			SessionStart += FString(TEXT("Session ID: ")).Len();
			int32 SessionEnd = OpenOutput1.Find(TEXT("\n"), ESearchCase::IgnoreCase, ESearchDir::FromStart, SessionStart);
			if (SessionEnd != INDEX_NONE)
			{
				SessionId1 = OpenOutput1.Mid(SessionStart, SessionEnd - SessionStart).TrimStartAndEnd();
			}
		}
	}
	UNTEST_ASSERT_FALSE(SessionId1.IsEmpty());

	// Try to open second session on same asset — should fail
	TSharedPtr<FJsonObject> OpenArgs2 = MakeShared<FJsonObject>();
	OpenArgs2->SetStringField(TEXT("operation"), TEXT("open"));
	TSharedPtr<FJsonObject> OpenParams2 = MakeShared<FJsonObject>();
	OpenParams2->SetStringField(TEXT("asset_path"), TestNiagaraSystemPath);
	OpenArgs2->SetObjectField(TEXT("params"), OpenParams2);

	auto OpenResult2 = Tool.Execute(OpenArgs2);
	UNTEST_ASSERT_TRUE(OpenResult2.bIsError);
	// Should contain a blocking error message
	UNTEST_EXPECT_TRUE(OpenResult2.GetContentAsString().Contains(TEXT("locked")));

	// Close first session
	TSharedPtr<FJsonObject> CloseArgs = MakeShared<FJsonObject>();
	CloseArgs->SetStringField(TEXT("operation"), TEXT("close"));
	CloseArgs->SetStringField(TEXT("session_id"), SessionId1);
	CloseArgs->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
	Tool.Execute(CloseArgs);

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, Niagara, EditUnknownOperation, UNTEST_TIMEOUTMS(15000))
{
	ClaireonTool_NiagaraEdit Tool;

	// Open session first
	TSharedPtr<FJsonObject> OpenArgs = MakeShared<FJsonObject>();
	OpenArgs->SetStringField(TEXT("operation"), TEXT("open"));
	TSharedPtr<FJsonObject> OpenParams = MakeShared<FJsonObject>();
	OpenParams->SetStringField(TEXT("asset_path"), TestNiagaraSystemPath);
	OpenArgs->SetObjectField(TEXT("params"), OpenParams);

	auto OpenResult = Tool.Execute(OpenArgs);
	UNTEST_ASSERT_FALSE(OpenResult.bIsError);

	FString OpenOutput = OpenResult.GetContentAsString();
	FString SessionId;
	{
		int32 SessionStart = OpenOutput.Find(TEXT("Session ID: "));
		if (SessionStart != INDEX_NONE)
		{
			SessionStart += FString(TEXT("Session ID: ")).Len();
			int32 SessionEnd = OpenOutput.Find(TEXT("\n"), ESearchCase::IgnoreCase, ESearchDir::FromStart, SessionStart);
			if (SessionEnd != INDEX_NONE)
			{
				SessionId = OpenOutput.Mid(SessionStart, SessionEnd - SessionStart).TrimStartAndEnd();
			}
		}
	}
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	// Try unknown operation
	TSharedPtr<FJsonObject> BadOpArgs = MakeShared<FJsonObject>();
	BadOpArgs->SetStringField(TEXT("operation"), TEXT("nonexistent_operation"));
	BadOpArgs->SetStringField(TEXT("session_id"), SessionId);
	BadOpArgs->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());

	auto BadOpResult = Tool.Execute(BadOpArgs);
	UNTEST_ASSERT_TRUE(BadOpResult.bIsError);
	UNTEST_EXPECT_TRUE(BadOpResult.GetContentAsString().Contains(TEXT("Unknown operation")));

	// Close session
	TSharedPtr<FJsonObject> CloseArgs = MakeShared<FJsonObject>();
	CloseArgs->SetStringField(TEXT("operation"), TEXT("close"));
	CloseArgs->SetStringField(TEXT("session_id"), SessionId);
	CloseArgs->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
	Tool.Execute(CloseArgs);

	co_return;
}


#endif // WITH_UNTESTED
