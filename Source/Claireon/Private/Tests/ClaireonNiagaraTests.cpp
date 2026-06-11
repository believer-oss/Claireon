// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/IClaireonTool.h"
#include "Tools/ClaireonTool_NiagaraInspect.h"
#include "Tools/ClaireonNiagaraTool_Open.h"
#include "Tools/ClaireonNiagaraTool_Close.h"
#include "Tools/ClaireonNiagaraTool_Status.h"
#include "Tools/ClaireonNiagaraTool_AddEmitter.h"
#include "Tools/ClaireonNiagaraTool_AddModule.h"
#include "Tools/ClaireonNiagaraTool_RemoveModule.h"
#include "Tools/ClaireonNiagaraTool_GetModuleInputs.h"
#include "Tools/ClaireonNiagaraTool_SetModuleInput.h"
#include "Tools/ClaireonNiagaraTool_ListModules.h"
#include "Tools/ClaireonNiagaraTool_SetSystemProperty.h"
#include "Tools/ClaireonNiagaraTool_AddParameter.h"
#include "Tools/ClaireonNiagaraTool_RemoveParameter.h"
#include "Tools/ClaireonNiagaraTool_Compile.h"
#include "Tools/ClaireonNiagaraHelpers.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitterHandle.h"
#include "ClaireonSessionManager.h"

// ---------------------------------------------------------------------------
// Test asset paths
// ---------------------------------------------------------------------------
static const TCHAR* SourceNiagaraSystemPath = TEXT("/Game/Art_Lib/VOL/NS_LocalVolumeFog");

// Every test in this suite runs against a transient duplicate under
// /Game/__MCPTests (untracked content area) so that no test -- nor any tool a
// test invokes that saves packages -- can ever dirty or write the real content
// asset. Created lazily once per process; a stale on-disk copy left by a
// previous run is deleted first so each run starts pristine.
static FString GetTestNiagaraSystemPath()
{
	static FString DupPath = []() -> FString
	{
		const FString Dest = TEXT("/Game/__MCPTests/NS_LocalVolumeFog");
		if (UEditorAssetLibrary::DoesAssetExist(Dest))
		{
			UEditorAssetLibrary::DeleteAsset(Dest);
		}
		return UEditorAssetLibrary::DuplicateAsset(SourceNiagaraSystemPath, Dest)
			? Dest : FString();
	}();
	return DupPath;
}

// ---------------------------------------------------------------------------
// Helpers: open / close a session via the decomposed Open/Close tools.
// ---------------------------------------------------------------------------
namespace ClaireonNiagaraTestsHelpers
{

bool OpenTestSession(FString& OutSessionId)
{
	ClaireonNiagaraTool_Open OpenTool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), GetTestNiagaraSystemPath());

	auto Result = OpenTool.Execute(Args);
	if (Result.bIsError || !Result.Data.IsValid())
	{
		return false;
	}
	if (!Result.Data->TryGetStringField(TEXT("session_id"), OutSessionId))
	{
		return false;
	}
	return !OutSessionId.IsEmpty();
}

void CloseTestSession(const FString& SessionId)
{
	ClaireonNiagaraTool_Close CloseTool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);
	CloseTool.Execute(Args);
}

}  // namespace ClaireonNiagaraTestsHelpers

// ============================================================================
// ClaireonNiagaraHelpers
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, Niagara, LoadValidSystem, UNTEST_TIMEOUTMS(10000))
{
	FString Error;
	UNiagaraSystem* System = ClaireonNiagaraHelpers::LoadNiagaraSystemAsset(GetTestNiagaraSystemPath(), Error);
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
	UNiagaraSystem* System = ClaireonNiagaraHelpers::LoadNiagaraSystemAsset(GetTestNiagaraSystemPath(), Error);
	UNTEST_ASSERT_TRUE(System != nullptr);

	FString Output = ClaireonNiagaraHelpers::FormatNiagaraSystemStructure(System, true);
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("=== Niagara System:")));
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("Path:")));
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("Emitters:")));
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
// niagara_inspect
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
	Args->SetStringField(TEXT("asset_path"), GetTestNiagaraSystemPath());
	Args->SetStringField(TEXT("detail_level"), TEXT("full"));
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);

	// The tool returns a one-line summary ("<asset>: N emitters, M modules") plus structured
	// JSON in Data. Assert against that contract (not the legacy formatted-text blob).
	FString Output = Result.GetContentAsString();
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("NS_LocalVolumeFog")));
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("emitters")));
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("modules")));

	// Full detail must populate the structured emitters + parameters fields.
	UNTEST_ASSERT_PTR(Result.Data.Get());
	UNTEST_EXPECT_TRUE(Result.Data->HasField(TEXT("emitters")));
	UNTEST_EXPECT_TRUE(Result.Data->HasField(TEXT("parameters")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, Niagara, InspectSummary, UNTEST_TIMEOUTMS(10000))
{
	ClaireonTool_NiagaraInspect Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), GetTestNiagaraSystemPath());
	Args->SetStringField(TEXT("detail_level"), TEXT("summary"));
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);

	// Summary mode returns the same one-line summary string ("<asset>: N emitters, M modules").
	FString Output = Result.GetContentAsString();
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("NS_LocalVolumeFog")));
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("emitters")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, Niagara, InspectEmitterIndexOutOfRange, UNTEST_TIMEOUTMS(10000))
{
	ClaireonTool_NiagaraInspect Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), GetTestNiagaraSystemPath());
	Args->SetNumberField(TEXT("emitter_index"), 999);
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("out of range")));
	co_return;
}

// ============================================================================
// niagara_edit decomposed tools - Session Lifecycle
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, NiagaraEdit, OpenCloseSession, UNTEST_TIMEOUTMS(15000))
{
	ClaireonNiagaraTool_Open OpenTool;
	ClaireonNiagaraTool_Close CloseTool;

	TSharedPtr<FJsonObject> OpenArgs = MakeShared<FJsonObject>();
	OpenArgs->SetStringField(TEXT("asset_path"), GetTestNiagaraSystemPath());

	auto OpenResult = OpenTool.Execute(OpenArgs);
	UNTEST_ASSERT_FALSE(OpenResult.bIsError);
	UNTEST_ASSERT_PTR(OpenResult.Data.Get());

	FString SessionId;
	UNTEST_ASSERT_TRUE(OpenResult.Data->TryGetStringField(TEXT("session_id"), SessionId));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	UNTEST_EXPECT_TRUE(FClaireonSessionManager::Get().IsAssetLocked(GetTestNiagaraSystemPath()));

	TSharedPtr<FJsonObject> CloseArgs = MakeShared<FJsonObject>();
	CloseArgs->SetStringField(TEXT("session_id"), SessionId);

	auto CloseResult = CloseTool.Execute(CloseArgs);
	UNTEST_ASSERT_FALSE(CloseResult.bIsError);
	UNTEST_EXPECT_TRUE(CloseResult.GetContentAsString().Contains(TEXT("closed")));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, NiagaraEdit, OpenMissingAssetPath, UNTEST_TIMEOUTMS(5000))
{
	ClaireonNiagaraTool_Open Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("asset_path")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, NiagaraEdit, StatusMissingSessionId, UNTEST_TIMEOUTMS(5000))
{
	ClaireonNiagaraTool_Status Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("session_id")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, NiagaraEdit, StatusInvalidSession, UNTEST_TIMEOUTMS(5000))
{
	ClaireonNiagaraTool_Status Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), TEXT("nonexistent-session-id"));
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("not found")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, NiagaraEdit, SessionExclusivity, UNTEST_TIMEOUTMS(15000))
{
	ClaireonNiagaraTool_Open OpenTool;

	TSharedPtr<FJsonObject> OpenArgs1 = MakeShared<FJsonObject>();
	OpenArgs1->SetStringField(TEXT("asset_path"), GetTestNiagaraSystemPath());

	auto OpenResult1 = OpenTool.Execute(OpenArgs1);
	UNTEST_ASSERT_FALSE(OpenResult1.bIsError);

	FString SessionId1;
	UNTEST_ASSERT_TRUE(OpenResult1.Data->TryGetStringField(TEXT("session_id"), SessionId1));
	UNTEST_ASSERT_FALSE(SessionId1.IsEmpty());

	// Re-opening the same asset with the SAME tool does not create a second concurrent
	// session -- the session manager reuses the existing one (see FClaireonSessionManager::
	// OpenSession: same ToolName -> ReusedExistingSession). The exclusivity guarantee is that
	// only one session exists per asset; a different tool would be blocked instead.
	TSharedPtr<FJsonObject> OpenArgs2 = MakeShared<FJsonObject>();
	OpenArgs2->SetStringField(TEXT("asset_path"), GetTestNiagaraSystemPath());

	auto OpenResult2 = OpenTool.Execute(OpenArgs2);
	UNTEST_ASSERT_FALSE(OpenResult2.bIsError);

	FString SessionId2;
	UNTEST_ASSERT_TRUE(OpenResult2.Data->TryGetStringField(TEXT("session_id"), SessionId2));
	UNTEST_EXPECT_TRUE(SessionId2 == SessionId1);

	ClaireonNiagaraTestsHelpers::CloseTestSession(SessionId1);

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, NiagaraEdit, StatusSuppressOutput, UNTEST_TIMEOUTMS(15000))
{
	FString SessionId;
	UNTEST_ASSERT_TRUE(ClaireonNiagaraTestsHelpers::OpenTestSession(SessionId));

	ClaireonNiagaraTool_Status StatusTool;
	TSharedPtr<FJsonObject> StatusArgs = MakeShared<FJsonObject>();
	StatusArgs->SetStringField(TEXT("session_id"), SessionId);
	StatusArgs->SetBoolField(TEXT("suppress_output"), true);

	auto StatusResult = StatusTool.Execute(StatusArgs);
	UNTEST_ASSERT_FALSE(StatusResult.bIsError);
	FString StatusOutput = StatusResult.GetContentAsString();
	UNTEST_EXPECT_TRUE(StatusOutput.StartsWith(TEXT("ok")));
	UNTEST_EXPECT_FALSE(StatusOutput.Contains(TEXT("=== Niagara System:")));

	ClaireonNiagaraTestsHelpers::CloseTestSession(SessionId);
	co_return;
}

// ============================================================================
// list_modules (no session required)
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, NiagaraEdit, ListModules_ReturnsResults, UNTEST_TIMEOUTMS(15000))
{
	ClaireonNiagaraTool_ListModules Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	auto Result = Tool.Execute(Args);
	FString Output = Result.GetContentAsString();
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("=== Available Modules")));
	UNTEST_EXPECT_FALSE(Output.Contains(TEXT("(no modules found")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, NiagaraEdit, ListModules_QuerySpawn, UNTEST_TIMEOUTMS(15000))
{
	ClaireonNiagaraTool_ListModules Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("query"), TEXT("spawn"));
	auto Result = Tool.Execute(Args);
	FString Output = Result.GetContentAsString();
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("Spawn")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, NiagaraEdit, ListModules_InvalidStack, UNTEST_TIMEOUTMS(10000))
{
	ClaireonNiagaraTool_ListModules Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("stack"), TEXT("InvalidStack"));
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	FString Output = Result.GetContentAsString();
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("resolve")) || Output.Contains(TEXT("Unknown")) || Output.Contains(TEXT("Invalid")));
	co_return;
}

// ============================================================================
// Module operations (session required)
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, NiagaraEdit, AddModule_ByShortName, UNTEST_TIMEOUTMS(30000))
{
	FString SessionId;
	UNTEST_ASSERT_TRUE(ClaireonNiagaraTestsHelpers::OpenTestSession(SessionId));

	{
		ClaireonNiagaraTool_AddEmitter Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetStringField(TEXT("emitter_name"), TEXT("TestEmitter_AddModule"));
		auto Result = Tool.Execute(Args);
		FString Output = Result.GetContentAsString();
		UNTEST_EXPECT_TRUE(Output.Contains(TEXT("add_emitter")) || Output.Contains(TEXT("TestEmitter_AddModule")));
	}

	FString Error;
	UNiagaraSystem* System = ClaireonNiagaraHelpers::LoadNiagaraSystemAsset(GetTestNiagaraSystemPath(), Error);
	int32 EmitterIdx = System ? System->GetEmitterHandles().Num() - 1 : 0;

	{
		ClaireonNiagaraTool_AddModule Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetNumberField(TEXT("emitter_index"), EmitterIdx);
		Args->SetStringField(TEXT("stack"), TEXT("ParticleSpawn"));
		Args->SetStringField(TEXT("module"), TEXT("Spawn Rate"));

		auto Result = Tool.Execute(Args);
		FString Output = Result.GetContentAsString();
		UNTEST_EXPECT_TRUE(Output.Contains(TEXT("add_module")) || Output.Contains(TEXT("Spawn")));
	}

	ClaireonNiagaraTestsHelpers::CloseTestSession(SessionId);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, NiagaraEdit, GetModuleInputs_Success, UNTEST_TIMEOUTMS(30000))
{
	FString SessionId;
	UNTEST_ASSERT_TRUE(ClaireonNiagaraTestsHelpers::OpenTestSession(SessionId));

	{
		ClaireonNiagaraTool_AddEmitter Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetStringField(TEXT("emitter_name"), TEXT("TestEmitter_Inputs"));
		Tool.Execute(Args);
	}

	FString Error;
	UNiagaraSystem* System = ClaireonNiagaraHelpers::LoadNiagaraSystemAsset(GetTestNiagaraSystemPath(), Error);
	int32 EmitterIdx = System ? System->GetEmitterHandles().Num() - 1 : 0;

	{
		ClaireonNiagaraTool_AddModule Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetNumberField(TEXT("emitter_index"), EmitterIdx);
		Args->SetStringField(TEXT("stack"), TEXT("ParticleSpawn"));
		Args->SetStringField(TEXT("module"), TEXT("Spawn Rate"));
		Tool.Execute(Args);
	}

	{
		ClaireonNiagaraTool_GetModuleInputs Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetNumberField(TEXT("emitter_index"), EmitterIdx);
		Args->SetStringField(TEXT("stack"), TEXT("ParticleSpawn"));
		Args->SetNumberField(TEXT("module_index"), 0);

		auto Result = Tool.Execute(Args);
		FString Output = Result.GetContentAsString();
		UNTEST_EXPECT_TRUE(Output.Contains(TEXT("=== Module Inputs")));
	}

	ClaireonNiagaraTestsHelpers::CloseTestSession(SessionId);
	co_return;
}

// Discovery regression guard: a freshly-added module has no overridden inputs, so the
// old surfaced-pin walk reported "(no inputs)". The metadata-based discovery must now list
// the module's regular inputs with a "(not overridden)" status even though none are set.
UNTEST_UNIT_OPTS(Claireon, NiagaraEdit, GetModuleInputs_FindsUnoverriddenRegularInputs, UNTEST_TIMEOUTMS(30000))
{
	FString SessionId;
	UNTEST_ASSERT_TRUE(ClaireonNiagaraTestsHelpers::OpenTestSession(SessionId));

	{
		ClaireonNiagaraTool_AddEmitter Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetStringField(TEXT("emitter_name"), TEXT("TestEmitter_Discovery"));
		Tool.Execute(Args);
	}

	FString Error;
	UNiagaraSystem* System = ClaireonNiagaraHelpers::LoadNiagaraSystemAsset(GetTestNiagaraSystemPath(), Error);
	int32 EmitterIdx = System ? System->GetEmitterHandles().Num() - 1 : 0;

	{
		ClaireonNiagaraTool_AddModule Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetNumberField(TEXT("emitter_index"), EmitterIdx);
		Args->SetStringField(TEXT("stack"), TEXT("ParticleSpawn"));
		Args->SetStringField(TEXT("module"), TEXT("Spawn Rate"));
		Tool.Execute(Args);
	}

	{
		// module_index 0 of ParticleSpawn is InitializeParticle, which exposes many regular
		// inputs (Lifetime, Color, Mass, ...), none overridden on a fresh emitter.
		ClaireonNiagaraTool_GetModuleInputs Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetNumberField(TEXT("emitter_index"), EmitterIdx);
		Args->SetStringField(TEXT("stack"), TEXT("ParticleSpawn"));
		Args->SetNumberField(TEXT("module_index"), 0);

		auto Result = Tool.Execute(Args);
		UNTEST_ASSERT_FALSE(Result.bIsError);
		FString Output = Result.GetContentAsString();
		UNTEST_EXPECT_TRUE(Output.Contains(TEXT("=== Module Inputs")));
		// Core proof: regular (un-overridden) inputs are now discovered and listed. Before the
		// fix this reported "(no inputs)" because the module had no surfaced override pins.
		UNTEST_EXPECT_FALSE(Output.Contains(TEXT("(no inputs)")));
		UNTEST_EXPECT_TRUE(Output.Contains(TEXT("(not overridden)")));
		UNTEST_EXPECT_TRUE(Output.Contains(TEXT("Module.Lifetime")));
	}

	ClaireonNiagaraTestsHelpers::CloseTestSession(SessionId);
	co_return;
}

// Override round-trip: set a discovered regular input and confirm it reads back as
// overridden. Before the discovery fix the set failed with "Input not found".
UNTEST_UNIT_OPTS(Claireon, NiagaraEdit, SetModuleInput_RegularInputRoundTrip, UNTEST_TIMEOUTMS(30000))
{
	FString SessionId;
	UNTEST_ASSERT_TRUE(ClaireonNiagaraTestsHelpers::OpenTestSession(SessionId));

	{
		ClaireonNiagaraTool_AddEmitter Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetStringField(TEXT("emitter_name"), TEXT("TestEmitter_RoundTrip"));
		Tool.Execute(Args);
	}

	FString Error;
	UNiagaraSystem* System = ClaireonNiagaraHelpers::LoadNiagaraSystemAsset(GetTestNiagaraSystemPath(), Error);
	int32 EmitterIdx = System ? System->GetEmitterHandles().Num() - 1 : 0;

	{
		// Set a discovered, previously un-overridden regular input by its bare name (no
		// "Module." prefix) on InitializeParticle (ParticleSpawn module 0). Before the fix this
		// failed with "Input not found" because discovery never surfaced it.
		ClaireonNiagaraTool_SetModuleInput Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetNumberField(TEXT("emitter_index"), EmitterIdx);
		Args->SetStringField(TEXT("stack"), TEXT("ParticleSpawn"));
		Args->SetNumberField(TEXT("module_index"), 0);
		Args->SetStringField(TEXT("input_name"), TEXT("Lifetime"));
		Args->SetStringField(TEXT("value"), TEXT("2.5"));

		auto Result = Tool.Execute(Args);
		UNTEST_ASSERT_FALSE(Result.bIsError);
	}

	{
		ClaireonNiagaraTool_GetModuleInputs Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetNumberField(TEXT("emitter_index"), EmitterIdx);
		Args->SetStringField(TEXT("stack"), TEXT("ParticleSpawn"));
		Args->SetNumberField(TEXT("module_index"), 0);

		auto Result = Tool.Execute(Args);
		UNTEST_ASSERT_FALSE(Result.bIsError);
		FString Output = Result.GetContentAsString();
		UNTEST_EXPECT_TRUE(Output.Contains(TEXT("(overridden)")));
	}

	ClaireonNiagaraTestsHelpers::CloseTestSession(SessionId);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, NiagaraEdit, RemoveModule_Success, UNTEST_TIMEOUTMS(30000))
{
	FString SessionId;
	UNTEST_ASSERT_TRUE(ClaireonNiagaraTestsHelpers::OpenTestSession(SessionId));

	{
		ClaireonNiagaraTool_AddEmitter Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetStringField(TEXT("emitter_name"), TEXT("TestEmitter_Remove"));
		Tool.Execute(Args);
	}

	FString Error;
	UNiagaraSystem* System = ClaireonNiagaraHelpers::LoadNiagaraSystemAsset(GetTestNiagaraSystemPath(), Error);
	int32 EmitterIdx = System ? System->GetEmitterHandles().Num() - 1 : 0;

	{
		ClaireonNiagaraTool_AddModule Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetNumberField(TEXT("emitter_index"), EmitterIdx);
		Args->SetStringField(TEXT("stack"), TEXT("ParticleSpawn"));
		Args->SetStringField(TEXT("module"), TEXT("Spawn Rate"));
		Tool.Execute(Args);
	}

	{
		ClaireonNiagaraTool_RemoveModule Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetNumberField(TEXT("emitter_index"), EmitterIdx);
		Args->SetStringField(TEXT("stack"), TEXT("ParticleSpawn"));
		Args->SetNumberField(TEXT("module_index"), 0);

		auto Result = Tool.Execute(Args);
		FString Output = Result.GetContentAsString();
		UNTEST_EXPECT_TRUE(Output.Contains(TEXT("remove_module")));
	}

	ClaireonNiagaraTestsHelpers::CloseTestSession(SessionId);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, NiagaraEdit, AddModule_InvalidStack, UNTEST_TIMEOUTMS(15000))
{
	FString SessionId;
	UNTEST_ASSERT_TRUE(ClaireonNiagaraTestsHelpers::OpenTestSession(SessionId));

	ClaireonNiagaraTool_AddModule Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);
	Args->SetNumberField(TEXT("emitter_index"), 0);
	Args->SetStringField(TEXT("stack"), TEXT("Bogus"));
	Args->SetStringField(TEXT("module"), TEXT("Spawn Rate"));

	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	FString Output = Result.GetContentAsString();
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("resolve")) || Output.Contains(TEXT("Unknown")) || Output.Contains(TEXT("Invalid")));

	ClaireonNiagaraTestsHelpers::CloseTestSession(SessionId);
	co_return;
}

// ============================================================================
// System property & parameter operations
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, NiagaraEdit, SetSystemProperty_Success, UNTEST_TIMEOUTMS(15000))
{
	FString SessionId;
	UNTEST_ASSERT_TRUE(ClaireonNiagaraTestsHelpers::OpenTestSession(SessionId));

	ClaireonNiagaraTool_SetSystemProperty Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);
	Args->SetStringField(TEXT("property_name"), TEXT("bFixedBounds"));
	Args->SetStringField(TEXT("value"), TEXT("true"));

	auto Result = Tool.Execute(Args);
	FString Output = Result.GetContentAsString();
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("set_system_property")) || Output.Contains(TEXT("bFixedBounds")));

	ClaireonNiagaraTestsHelpers::CloseTestSession(SessionId);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, NiagaraEdit, AddParameter_Float, UNTEST_TIMEOUTMS(15000))
{
	FString SessionId;
	UNTEST_ASSERT_TRUE(ClaireonNiagaraTestsHelpers::OpenTestSession(SessionId));

	ClaireonNiagaraTool_AddParameter Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);
	Args->SetStringField(TEXT("name"), TEXT("TestFloat"));
	Args->SetStringField(TEXT("type"), TEXT("Float"));

	auto Result = Tool.Execute(Args);
	FString Output = Result.GetContentAsString();
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("add_parameter")) || Output.Contains(TEXT("TestFloat")));

	ClaireonNiagaraTestsHelpers::CloseTestSession(SessionId);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, NiagaraEdit, RemoveParameter_Success, UNTEST_TIMEOUTMS(15000))
{
	FString SessionId;
	UNTEST_ASSERT_TRUE(ClaireonNiagaraTestsHelpers::OpenTestSession(SessionId));

	{
		ClaireonNiagaraTool_AddParameter AddTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetStringField(TEXT("name"), TEXT("TestParamToRemove"));
		Args->SetStringField(TEXT("type"), TEXT("Float"));
		AddTool.Execute(Args);
	}

	{
		ClaireonNiagaraTool_RemoveParameter Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetStringField(TEXT("name"), TEXT("TestParamToRemove"));

		auto Result = Tool.Execute(Args);
		FString Output = Result.GetContentAsString();
		UNTEST_EXPECT_TRUE(Output.Contains(TEXT("remove_parameter")) || Output.Contains(TEXT("TestParamToRemove")));
	}

	ClaireonNiagaraTestsHelpers::CloseTestSession(SessionId);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, NiagaraEdit, AddParameter_InvalidType, UNTEST_TIMEOUTMS(15000))
{
	FString SessionId;
	UNTEST_ASSERT_TRUE(ClaireonNiagaraTestsHelpers::OpenTestSession(SessionId));

	ClaireonNiagaraTool_AddParameter Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);
	Args->SetStringField(TEXT("name"), TEXT("BadParam"));
	Args->SetStringField(TEXT("type"), TEXT("InvalidType"));

	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	FString Output = Result.GetContentAsString();
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("Unsupported")) || Output.Contains(TEXT("Invalid")));

	ClaireonNiagaraTestsHelpers::CloseTestSession(SessionId);
	co_return;
}

// ============================================================================
// Compile
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, NiagaraEdit, Compile_Success, UNTEST_TIMEOUTMS(30000))
{
	FString SessionId;
	UNTEST_ASSERT_TRUE(ClaireonNiagaraTestsHelpers::OpenTestSession(SessionId));

	ClaireonNiagaraTool_Compile Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);

	auto Result = Tool.Execute(Args);
	FString Output = Result.GetContentAsString();
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("Success")));

	ClaireonNiagaraTestsHelpers::CloseTestSession(SessionId);
	co_return;
}

// ============================================================================
// Enhanced status
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, NiagaraEdit, Status_IncludesStacks, UNTEST_TIMEOUTMS(15000))
{
	FString SessionId;
	UNTEST_ASSERT_TRUE(ClaireonNiagaraTestsHelpers::OpenTestSession(SessionId));

	ClaireonNiagaraTool_Status Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);

	auto Result = Tool.Execute(Args);
	FString Output = Result.GetContentAsString();
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("=== Session Status ===")));
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("Stacks:")) || Output.Contains(TEXT("--- Emitter")));

	ClaireonNiagaraTestsHelpers::CloseTestSession(SessionId);
	co_return;
}


#endif // WITH_UNTESTED
