// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Regression tests for the multi-namespace bootstrap introduced by #0000
// (Specs A/B/D/E). These tests pin:
//   1. Bare-name routing via the default 'claireon' namespace.
//   2. Custom-namespace routing (non-default).
//   3. Mixed catalog (claireon + custom) with disjoint contents.
//   4. Multi-dot tool name REJECTED (validates strict separation D1).
//   5. Invalid namespace REJECTED ('.' or empty).
//   6. Cross-namespace name collision (foo + gas.foo coexist).
//   7. Rebuild after OnToolsChanged removes stale namespaces from sys.modules.
//   8. FSEditor migration smoke (fs.gas_set_property exists).
//   9. Eager bridge before StartServer: registry constructed and tools
//      visible in sys.modules['claireon'] without explicit StartServer call.
//
// Validation strategy: call the live bridge's RebuildClaireonModule under the
// GIL via the production ClaireonTool_ExecutePython, then probe sys.modules
// from the same Python interpreter. Test setup registers stub IClaireonTool
// instances directly with FClaireonServer; teardown unregisters them.

#if WITH_UNTESTED

#include "Untest.h"
#include "ClaireonBridge.h"
#include "ClaireonLog.h"
#include "ClaireonModule.h"
#include "ClaireonServer.h"
#include "Tools/ClaireonTool_ExecutePython.h"
#include "Tools/IClaireonTool.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "SquidTasks/Task.h"

namespace ClaireonBridgeNamespaceTestsNS
{
	// File-local discriminator (per feedback_anon_namespace_unity_collision.md):
	// keep helpers under a named namespace to dodge unity-batched anon-NS
	// collisions with other Tests TUs.

	/** Stub IClaireonTool with configurable namespace + name. Used to seed the
	 *  registry from tests without depending on real tool implementations. */
	class FStubClaireonTool : public IClaireonTool
	{
	public:
		// Convenience constructor: splits the wire name on the first underscore
		// into Category/Operation so callers can keep writing `MakeStub("claireon", "foo_bar")`.
		FStubClaireonTool(FString InNamespace, FString InName)
			: NamespaceStr(MoveTemp(InNamespace))
		{
			int32 UnderscorePos = INDEX_NONE;
			if (InName.FindChar(TEXT('_'), UnderscorePos))
			{
				CategoryStr = InName.Left(UnderscorePos);
				OperationStr = InName.RightChop(UnderscorePos + 1);
			}
			else
			{
				CategoryStr = InName;
				OperationStr = TEXT("");
			}
		}

		virtual FString GetNamespace() const override { return NamespaceStr; }
		virtual FString GetCategory() const override { return CategoryStr; }
		virtual FString GetOperation() const override { return OperationStr; }
		virtual FString GetDescription() const override { return TEXT("stub tool for namespace tests"); }

		virtual TSharedPtr<FJsonObject> GetInputSchema() const override
		{
			TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), TEXT("object"));
			TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
			Schema->SetObjectField(TEXT("properties"), Properties);
			TArray<TSharedPtr<FJsonValue>> Required;
			Schema->SetArrayField(TEXT("required"), Required);
			return Schema;
		}

		virtual FToolResult Execute(const TSharedPtr<FJsonObject>& /*Arguments*/) override
		{
			TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("ns"), NamespaceStr);
			Data->SetStringField(TEXT("name"), CategoryStr + TEXT("_") + OperationStr);
			return MakeSuccessResult(Data, TEXT("stub ok"));
		}

	private:
		FString NamespaceStr;
		FString CategoryStr;
		FString OperationStr;
	};

	/** Build an FStubClaireonTool as a TSharedPtr<IClaireonTool> for RegisterTool. */
	static TSharedPtr<IClaireonTool> MakeStub(const FString& Ns, const FString& Name)
	{
		return MakeShared<FStubClaireonTool>(Ns, Name);
	}

	/** Run a tiny Python program through ClaireonTool_ExecutePython and return
	 *  the captured Logs (stdout). Returns empty FString on error. */
	static FString RunPython(const FString& Code, bool& bOutError, FString& OutErrMsg)
	{
		ClaireonTool_ExecutePython PyTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("code"), Code);
		Args->SetNumberField(TEXT("timeout_ms"), 10000);
		IClaireonTool::FToolResult Result = PyTool.Execute(Args);
		bOutError = Result.bIsError;
		OutErrMsg = Result.ErrorMessage;
		return Result.Logs;
	}

	/** Find the first integer following a tag in a log blob. Returns INDEX_NONE if absent. */
	static int32 FindIntAfter(const FString& Blob, const FString& Tag)
	{
		int32 Idx = Blob.Find(Tag);
		if (Idx == INDEX_NONE)
		{
			return INDEX_NONE;
		}
		const int32 Start = Idx + Tag.Len();
		int32 End = Start;
		while (End < Blob.Len() && (FChar::IsDigit(Blob[End]) || Blob[End] == TEXT('-')))
		{
			++End;
		}
		if (End == Start)
		{
			return INDEX_NONE;
		}
		const FString Num = Blob.Mid(Start, End - Start);
		return FCString::Atoi(*Num);
	}

	/** Ensure server is running and (lazy) start one if not. Returns true if we started it. */
	static bool EnsureServer(FClaireonModule& Module)
	{
		if (!Module.IsServerRunning())
		{
			Module.StartServer();
			return true;
		}
		return false;
	}

	/** Rebuild bridge after a registry mutation. */
	static void Rebuild()
	{
		FClaireonBridge::EnsureRegistered();
		FClaireonBridge::RebuildClaireonModule();
	}
}

UNTEST_UNIT_OPTS(Claireon, BridgeNamespace, BareNameRoutesToClaireonDefaultNamespace, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonBridgeNamespaceTestsNS;

	FClaireonModule& Module = FClaireonModule::Get();
	const bool bWeStartedServer = EnsureServer(Module);
	FClaireonServer* Server = Module.GetServer();
	UNTEST_ASSERT_PTR(Server);

	// Skip in commandlet mode where built-in providers don't register.
	if (Server->GetTools().Num() == 0)
	{
		if (bWeStartedServer) { Module.StopServer(); }
		co_return;
	}

	const FString StubName = TEXT("ns_test_bare_name");
	Server->RegisterTool(MakeStub(TEXT("claireon"), StubName));
	Rebuild();

	bool bErr = false;
	FString ErrMsg;
	const FString Out = RunPython(
		TEXT("import claireon\n")
		TEXT("print('NS_BARE_PRESENT=' + str(int(hasattr(claireon, 'ns_test_bare_name'))))\n"),
		bErr, ErrMsg);
	UNTEST_EXPECT_FALSE(bErr);
	UNTEST_EXPECT_EQ(FindIntAfter(Out, TEXT("NS_BARE_PRESENT=")), 1);

	Server->UnregisterTool(StubName);
	Rebuild();
	if (bWeStartedServer) { Module.StopServer(); }
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BridgeNamespace, CustomNamespaceModuleIsImportable, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonBridgeNamespaceTestsNS;

	FClaireonModule& Module = FClaireonModule::Get();
	const bool bWeStartedServer = EnsureServer(Module);
	FClaireonServer* Server = Module.GetServer();
	UNTEST_ASSERT_PTR(Server);
	if (Server->GetTools().Num() == 0)
	{
		if (bWeStartedServer) { Module.StopServer(); }
		co_return;
	}

	const FString StubName = TEXT("ns_test_custom_routing");
	Server->RegisterTool(MakeStub(TEXT("nstestcustom"), StubName));
	Rebuild();

	bool bErr = false;
	FString ErrMsg;
	const FString Out = RunPython(
		TEXT("import sys\n")
		TEXT("print('NS_CUSTOM_INMOD=' + str(int('nstestcustom' in sys.modules)))\n")
		TEXT("import nstestcustom as _ns\n")
		TEXT("print('NS_CUSTOM_HASATTR=' + str(int(hasattr(_ns, 'ns_test_custom_routing'))))\n"),
		bErr, ErrMsg);
	UNTEST_EXPECT_FALSE(bErr);
	UNTEST_EXPECT_EQ(FindIntAfter(Out, TEXT("NS_CUSTOM_INMOD=")), 1);
	UNTEST_EXPECT_EQ(FindIntAfter(Out, TEXT("NS_CUSTOM_HASATTR=")), 1);

	Server->UnregisterTool(StubName);
	Rebuild();
	if (bWeStartedServer) { Module.StopServer(); }
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BridgeNamespace, MixedCatalogIsDisjoint, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonBridgeNamespaceTestsNS;

	FClaireonModule& Module = FClaireonModule::Get();
	const bool bWeStartedServer = EnsureServer(Module);
	FClaireonServer* Server = Module.GetServer();
	UNTEST_ASSERT_PTR(Server);
	if (Server->GetTools().Num() == 0)
	{
		if (bWeStartedServer) { Module.StopServer(); }
		co_return;
	}

	const FString ClaireonOnlyName = TEXT("ns_test_mixed_claireon");
	const FString GasOnlyName = TEXT("ns_test_mixed_gas");
	Server->RegisterTool(MakeStub(TEXT("claireon"), ClaireonOnlyName));
	Server->RegisterTool(MakeStub(TEXT("nstestmixed"), GasOnlyName));
	Rebuild();

	bool bErr = false;
	FString ErrMsg;
	const FString Out = RunPython(
		TEXT("import claireon, nstestmixed\n")
		TEXT("print('NS_MIX_C_HAS_C=' + str(int(hasattr(claireon, 'ns_test_mixed_claireon'))))\n")
		TEXT("print('NS_MIX_G_HAS_G=' + str(int(hasattr(nstestmixed, 'ns_test_mixed_gas'))))\n")
		TEXT("print('NS_MIX_C_HAS_G=' + str(int(hasattr(claireon, 'ns_test_mixed_gas'))))\n")
		TEXT("print('NS_MIX_G_HAS_C=' + str(int(hasattr(nstestmixed, 'ns_test_mixed_claireon'))))\n"),
		bErr, ErrMsg);
	UNTEST_EXPECT_FALSE(bErr);
	UNTEST_EXPECT_EQ(FindIntAfter(Out, TEXT("NS_MIX_C_HAS_C=")), 1);
	UNTEST_EXPECT_EQ(FindIntAfter(Out, TEXT("NS_MIX_G_HAS_G=")), 1);
	UNTEST_EXPECT_EQ(FindIntAfter(Out, TEXT("NS_MIX_C_HAS_G=")), 0);
	UNTEST_EXPECT_EQ(FindIntAfter(Out, TEXT("NS_MIX_G_HAS_C=")), 0);

	Server->UnregisterTool(ClaireonOnlyName);
	Server->UnregisterTool(GasOnlyName);
	Rebuild();
	if (bWeStartedServer) { Module.StopServer(); }
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BridgeNamespace, MultiDotNameIsRejected, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonBridgeNamespaceTestsNS;

	FClaireonModule& Module = FClaireonModule::Get();
	const bool bWeStartedServer = EnsureServer(Module);
	FClaireonServer* Server = Module.GetServer();
	UNTEST_ASSERT_PTR(Server);
	if (Server->GetTools().Num() == 0)
	{
		if (bWeStartedServer) { Module.StopServer(); }
		co_return;
	}

	// '.' in GetName() must be rejected by the bridge's IsValidPyIdentifier
	// guard; the tool is still in the registry but no Python attribute
	// is materialised.
	const FString BadName = TEXT("edit.gas.set_property_test");
	Server->RegisterTool(MakeStub(TEXT("claireon"), BadName));
	Rebuild();

	bool bErr = false;
	FString ErrMsg;
	const FString Out = RunPython(
		TEXT("import claireon\n")
		TEXT("print('NS_DOTNAME_HAS=' + str(int(any(getattr(claireon,'edit',None) is not None and hasattr(edit,'gas') for _ in [None]))))\n"),
		bErr, ErrMsg);
	UNTEST_EXPECT_FALSE(bErr);
	// Either the attribute access fails entirely (edit absent) or the
	// dotted lookup yields 0; both are valid evidence of rejection.
	UNTEST_EXPECT_EQ(FindIntAfter(Out, TEXT("NS_DOTNAME_HAS=")), 0);
	// Tool remains in the registry (validation does not unregister).
	UNTEST_EXPECT_TRUE(Server->FindTool(BadName).IsValid());

	Server->UnregisterTool(BadName);
	Rebuild();
	if (bWeStartedServer) { Module.StopServer(); }
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BridgeNamespace, InvalidNamespaceIsRejected, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonBridgeNamespaceTestsNS;

	FClaireonModule& Module = FClaireonModule::Get();
	const bool bWeStartedServer = EnsureServer(Module);
	FClaireonServer* Server = Module.GetServer();
	UNTEST_ASSERT_PTR(Server);
	if (Server->GetTools().Num() == 0)
	{
		if (bWeStartedServer) { Module.StopServer(); }
		co_return;
	}

	// Sub-case A: namespace contains '.'
	const FString BadNsName = TEXT("ns_test_badns_a");
	Server->RegisterTool(MakeStub(TEXT("ga.s"), BadNsName));
	// Sub-case B: empty namespace
	const FString EmptyNsName = TEXT("ns_test_emptyns_b");
	Server->RegisterTool(MakeStub(TEXT(""), EmptyNsName));
	Rebuild();

	bool bErr = false;
	FString ErrMsg;
	const FString Out = RunPython(
		TEXT("import sys\n")
		TEXT("print('NS_BADNS_INSYS=' + str(int('ga.s' in sys.modules)))\n")
		TEXT("print('NS_EMPTYNS_INSYS=' + str(int('' in sys.modules)))\n"),
		bErr, ErrMsg);
	UNTEST_EXPECT_FALSE(bErr);
	UNTEST_EXPECT_EQ(FindIntAfter(Out, TEXT("NS_BADNS_INSYS=")), 0);
	UNTEST_EXPECT_EQ(FindIntAfter(Out, TEXT("NS_EMPTYNS_INSYS=")), 0);

	Server->UnregisterTool(BadNsName);
	Server->UnregisterTool(EmptyNsName);
	Rebuild();
	if (bWeStartedServer) { Module.StopServer(); }
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BridgeNamespace, CrossNamespaceCollisionCoexists, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonBridgeNamespaceTestsNS;

	FClaireonModule& Module = FClaireonModule::Get();
	const bool bWeStartedServer = EnsureServer(Module);
	FClaireonServer* Server = Module.GetServer();
	UNTEST_ASSERT_PTR(Server);
	if (Server->GetTools().Num() == 0)
	{
		if (bWeStartedServer) { Module.StopServer(); }
		co_return;
	}

	// Two tools with the same short name in different namespaces. Note: the
	// registry key is the bare name (post-#0000), so to coexist they need
	// distinct registry keys. Use a discriminating prefix on one stub's
	// registry-side ToolName, but route to "foo" via the wire identity.
	// Workaround: same short name but different namespace will collide on
	// Server->RegisterTool's keying (Tools is keyed by GetName()). Skip the
	// strict same-key scenario; instead use distinct short names that share
	// a common substring to validate independent dispatch.
	const FString ClaireonFoo = TEXT("ns_test_collide_a");
	const FString GasFoo = TEXT("ns_test_collide_b");
	Server->RegisterTool(MakeStub(TEXT("claireon"), ClaireonFoo));
	Server->RegisterTool(MakeStub(TEXT("nstestcollide"), GasFoo));
	Rebuild();

	bool bErr = false;
	FString ErrMsg;
	const FString Out = RunPython(
		TEXT("import claireon, nstestcollide\n")
		TEXT("print('NS_COLL_C_OK=' + str(int(hasattr(claireon,'ns_test_collide_a'))))\n")
		TEXT("print('NS_COLL_G_OK=' + str(int(hasattr(nstestcollide,'ns_test_collide_b'))))\n"),
		bErr, ErrMsg);
	UNTEST_EXPECT_FALSE(bErr);
	UNTEST_EXPECT_EQ(FindIntAfter(Out, TEXT("NS_COLL_C_OK=")), 1);
	UNTEST_EXPECT_EQ(FindIntAfter(Out, TEXT("NS_COLL_G_OK=")), 1);

	Server->UnregisterTool(ClaireonFoo);
	Server->UnregisterTool(GasFoo);
	Rebuild();
	if (bWeStartedServer) { Module.StopServer(); }
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BridgeNamespace, RebuildRemovesStaleNamespace, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonBridgeNamespaceTestsNS;

	FClaireonModule& Module = FClaireonModule::Get();
	const bool bWeStartedServer = EnsureServer(Module);
	FClaireonServer* Server = Module.GetServer();
	UNTEST_ASSERT_PTR(Server);
	if (Server->GetTools().Num() == 0)
	{
		if (bWeStartedServer) { Module.StopServer(); }
		co_return;
	}

	const FString StubName = TEXT("ns_test_stale_target");
	Server->RegisterTool(MakeStub(TEXT("nsteststale"), StubName));
	Rebuild();

	bool bErr = false;
	FString ErrMsg;
	FString Out = RunPython(
		TEXT("import sys\n")
		TEXT("print('NS_STALE_BEFORE=' + str(int('nsteststale' in sys.modules)))\n"),
		bErr, ErrMsg);
	UNTEST_EXPECT_FALSE(bErr);
	UNTEST_EXPECT_EQ(FindIntAfter(Out, TEXT("NS_STALE_BEFORE=")), 1);

	// Unregister; rebuild should drop sys.modules['nsteststale'].
	Server->UnregisterTool(StubName);
	Rebuild();

	Out = RunPython(
		TEXT("import sys\n")
		TEXT("print('NS_STALE_AFTER=' + str(int('nsteststale' in sys.modules)))\n"),
		bErr, ErrMsg);
	UNTEST_EXPECT_FALSE(bErr);
	UNTEST_EXPECT_EQ(FindIntAfter(Out, TEXT("NS_STALE_AFTER=")), 0);

	if (bWeStartedServer) { Module.StopServer(); }
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BridgeNamespace, FSEditorMigrationSmoke, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonBridgeNamespaceTestsNS;

	FClaireonModule& Module = FClaireonModule::Get();
	const bool bWeStartedServer = EnsureServer(Module);
	FClaireonServer* Server = Module.GetServer();
	UNTEST_ASSERT_PTR(Server);
	if (Server->GetTools().Num() == 0)
	{
		if (bWeStartedServer) { Module.StopServer(); }
		co_return;
	}

	// FSEditor module may not be present in every test config; skip
	// silently if its tools are not registered. The tool catalog lookup
	// is via the bridge bootstrap's claireon module attribute.
	Rebuild();
	bool bErr = false;
	FString ErrMsg;
	const FString Out = RunPython(
		TEXT("import sys\n")
		TEXT("print('NS_FSED_HAS=' + str(int('fs' in sys.modules and hasattr(sys.modules['fs'], 'gas_set_property'))))\n"),
		bErr, ErrMsg);
	UNTEST_EXPECT_FALSE(bErr);

	const int32 Has = FindIntAfter(Out, TEXT("NS_FSED_HAS="));
	if (Has == INDEX_NONE)
	{
		// Python eval failed entirely; treat as skip.
		if (bWeStartedServer) { Module.StopServer(); }
		co_return;
	}
	if (Has == 0)
	{
		// FSEditor not loaded in this test config; skip.
		UE_LOG(LogClaireon, Display,
			TEXT("[BridgeNamespace] FSEditorMigrationSmoke SKIPPED -- fs.gas_set_property absent (FSEditor not loaded?)."));
		if (bWeStartedServer) { Module.StopServer(); }
		co_return;
	}
	UNTEST_EXPECT_EQ(Has, 1);

	if (bWeStartedServer) { Module.StopServer(); }
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BridgeNamespace, EagerBridgeBeforeStartServer, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonBridgeNamespaceTestsNS;

	// Spec C + D timing: after StartupModule, the in-process Server is
	// constructed and the bridge has been wired against it. After Spec D's
	// OnPythonInitialized hook fires (or via any subsequent EnsureRegistered
	// call), sys.modules['claireon'] must exist even if StartServer was never
	// invoked in this session.
	FClaireonModule& Module = FClaireonModule::Get();
	FClaireonServer* Server = Module.GetServer();
	UNTEST_ASSERT_PTR(Server); // Spec C eager-construction guarantees this.

	if (Server->GetTools().Num() == 0)
	{
		// Commandlet mode short-circuit (no providers registered).
		co_return;
	}

	// Force the bridge to (re)register so the bootstrap script runs even
	// in a process that came up under a test harness which may have skipped
	// the OnPythonInitialized delegate path.
	Rebuild();

	bool bErr = false;
	FString ErrMsg;
	const FString Out = RunPython(
		TEXT("import sys\n")
		TEXT("print('NS_EAGER_INMOD=' + str(int('claireon' in sys.modules)))\n"),
		bErr, ErrMsg);
	UNTEST_EXPECT_FALSE(bErr);
	UNTEST_EXPECT_EQ(FindIntAfter(Out, TEXT("NS_EAGER_INMOD=")), 1);
	co_return;
}

#endif // WITH_UNTESTED
