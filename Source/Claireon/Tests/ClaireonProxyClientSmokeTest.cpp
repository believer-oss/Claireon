// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
//
// Smoke tests for FClaireonProxyClient.
//
// Coverage tracks ALWAYS_ON_MCP_PROXY_TESTS.md "C++ unit/smoke test":
//   1. Spawn + register + unregister round-trip (gated on vendored Python).
//   2. Skip behavior when Python is missing (always runnable).
//   3. Lock-file attach path (gated on vendored Python).
//
// Category: Game target, "Claireon.ProxyClient.Smoke.*".
// Filter:   EAutomationTestFlags::EditorContext | CommandletContext | EngineFilter.
//           CommandletContext is required for Scripts/Testing/Invoke-UntestTests.ps1
//           to see these at all: it runs -run=UntestRunTests, and a commandlet is
//           NOT EditorContext (Core/Private/Misc/AutomationTest.cpp computes
//           bRunningEditor = GIsEditor && !IsRunningCommandlet()).
//
// The suite is intentionally minimal: it exercises the public surface of
// FClaireonProxyClient without relying on the full editor MCP server
// startup, so it can be run in any automation pass that loads the
// Claireon module. Tests that need a real Python subprocess check for
// the vendored interpreter up-front and emit AddInfo(...) + return true
// if it is unavailable; no test is expected to fail on a machine that
// simply lacks the vendored runtime.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformFileManager.h"
#include "Interfaces/IPluginManager.h"
#include "HttpManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"

#include "ClaireonProxyClient.h"
#include "ClaireonProxyConstants.h"

namespace ClaireonProxyClientSmokeTest
{
	/** Expected location of the vendored Python shipped with the custom engine. */
	static FString ExpectedVendoredPythonPath()
	{
		return FPaths::ConvertRelativePathToFull(
			FPaths::EngineDir() / TEXT("Binaries/ThirdParty/Python3/Win64/python.exe"));
	}

	/** Expected location of the Python proxy script shipped with the plugin. */
	static FString ExpectedProxyScriptPath()
	{
		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Claireon"));
		if (!Plugin.IsValid())
		{
			return FString();
		}
		return FPaths::ConvertRelativePathToFull(
			Plugin->GetContentDir() / TEXT("Python/claireon_proxy.py"));
	}

	/** True iff all preconditions for spawning a real proxy are satisfied. */
	static bool CanRunLiveProxyTests()
	{
		const FString PythonExe = ExpectedVendoredPythonPath();
		const FString ScriptPath = ExpectedProxyScriptPath();
		return FPaths::FileExists(PythonExe) && FPaths::FileExists(ScriptPath);
	}

	/**
	 * Poll the proxy's registration port until it accepts a TCP connection,
	 * or the deadline elapses. Returns true if the proxy answered at least
	 * one GET /health probe before the deadline.
	 */
	static bool WaitForProxyListening(double DeadlineSeconds)
	{
		const FString Url = FString::Printf(
			TEXT("http://%s:%d%s"),
			ClaireonProxy::LoopbackHost,
			ClaireonProxy::PROXY_REG_PORT,
			ClaireonProxy::HealthEndpoint);

		const double Deadline = FPlatformTime::Seconds() + DeadlineSeconds;
		while (FPlatformTime::Seconds() < Deadline)
		{
			TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request =
				FHttpModule::Get().CreateRequest();
			Request->SetURL(Url);
			Request->SetVerb(TEXT("GET"));
			Request->SetTimeout(1.0f);

			if (Request->ProcessRequest())
			{
				const double RequestDeadline = FPlatformTime::Seconds() + 1.5;
				while (Request->GetStatus() == EHttpRequestStatus::Processing
					&& FPlatformTime::Seconds() < RequestDeadline)
				{
					FHttpModule::Get().GetHttpManager().Tick(0.0f);
					FPlatformProcess::Sleep(0.01f);
				}
				if (Request->GetStatus() == EHttpRequestStatus::Succeeded)
				{
					FHttpResponsePtr Response = Request->GetResponse();
					if (Response.IsValid() && Response->GetResponseCode() > 0)
					{
						return true;
					}
				}
			}
			FPlatformProcess::Sleep(0.1f);
		}
		return false;
	}

	/** Best-effort kill of any proxy we may have spawned so teardown leaves the host clean. */
	static void KillLingeringProxy(uint32 Pid)
	{
		if (Pid == 0)
		{
			return;
		}
		FProcHandle Handle = FPlatformProcess::OpenProcess(Pid);
		if (Handle.IsValid())
		{
			FPlatformProcess::TerminateProc(Handle, /*KillTree=*/ false);
			FPlatformProcess::CloseProc(Handle);
		}
	}

	/** Generate a 64-char random token meeting the proxy's >= 32-char constraint. */
	static FString MakeTestToken()
	{
		return FGuid::NewGuid().ToString(EGuidFormats::Digits)
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	}
} // namespace ClaireonProxyClientSmokeTest

// ----------------------------------------------------------------------------
// Static helpers exposed via FClaireonProxyClient public API.
// These run anywhere -- no vendored Python, no network, no spawn.
// ----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClaireonProxyClientStaticsSmoke,
	"Claireon.ProxyClient.Smoke.Statics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FClaireonProxyClientStaticsSmoke::RunTest(const FString& /*Parameters*/)
{
	using namespace ClaireonProxyClientSmokeTest;

	const FString BaseUrl = FClaireonProxyClient::GetRegistrationBaseUrl();
	const FString Expected = FString::Printf(
		TEXT("http://%s:%d"),
		ClaireonProxy::LoopbackHost,
		ClaireonProxy::PROXY_REG_PORT);
	TestEqual(TEXT("Registration base URL"), BaseUrl, Expected);

	// Plugin must resolve or the whole module is misconfigured.
	TestTrue(TEXT("Claireon plugin is discoverable"),
		IPluginManager::Get().FindPlugin(TEXT("Claireon")).IsValid());

	// Proxy script must ship alongside the plugin.
	const FString ScriptPath = ExpectedProxyScriptPath();
	TestFalse(TEXT("Expected claireon_proxy.py path resolvable"), ScriptPath.IsEmpty());
	TestTrue(TEXT("claireon_proxy.py exists on disk"), FPaths::FileExists(ScriptPath));

	return true;
}

// ----------------------------------------------------------------------------
// Skip behavior when vendored Python is missing.
// We cannot physically remove the vendored Python at test time, so this test
// exercises the reverse contract: if the proxy script is present and spawn
// fails, SpawnDetachedProxy logs and returns false. Run only when we do NOT
// have the vendored interpreter.
// ----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClaireonProxyClientSkipWhenPythonMissing,
	"Claireon.ProxyClient.Smoke.SkipWhenPythonMissing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FClaireonProxyClientSkipWhenPythonMissing::RunTest(const FString& /*Parameters*/)
{
	using namespace ClaireonProxyClientSmokeTest;

	const FString PythonExe = ExpectedVendoredPythonPath();
	if (FPaths::FileExists(PythonExe))
	{
		AddInfo(FString::Printf(
			TEXT("Vendored Python present at %s; skip-on-missing branch is not exercised here. ")
			TEXT("Covered implicitly by the module load failing loud when rebuild is required."),
			*PythonExe));
		return true;
	}

	FClaireonProxyClient Client;
	const bool bRunning = Client.EnsureProxyRunning();
	TestFalse(TEXT("EnsureProxyRunning must return false when vendored Python is absent"), bRunning);
	TestFalse(TEXT("IsRegistered must be false after a failed EnsureProxyRunning"), Client.IsRegistered());
	return true;
}

// ----------------------------------------------------------------------------
// Spawn + register + unregister round-trip against the real proxy.
// Gated on vendored Python availability. Cleans up any spawned process on exit.
// ----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClaireonProxyClientRoundTripSmoke,
	"Claireon.ProxyClient.Smoke.RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FClaireonProxyClientRoundTripSmoke::RunTest(const FString& /*Parameters*/)
{
	using namespace ClaireonProxyClientSmokeTest;

	if (!CanRunLiveProxyTests())
	{
		AddInfo(TEXT("SKIPPED: vendored Python or claireon_proxy.py missing; round-trip not exercised."));
		return true;
	}

	// Spawn + wait. EnsureProxyRunning tolerates an already-live proxy (attach)
	// or spawns fresh; either is fine for the round-trip.
	FClaireonProxyClient Client;
	const bool bRunning = Client.EnsureProxyRunning();
	if (!bRunning)
	{
		AddError(TEXT("EnsureProxyRunning returned false; cannot exercise round-trip."));
		return false;
	}

	// The proxy may have been mid-binding; give it a generous budget before
	// Register() so the first attempt does not race the listener coming up.
	if (!WaitForProxyListening(/*DeadlineSeconds=*/ 5.0))
	{
		AddError(TEXT("Proxy never answered on PROXY_REG_PORT within 5s."));
		return false;
	}

	// Register with a fake editor_mcp_port. The proxy will accept the
	// registration without verifying the port is live -- it only opens the
	// forward connection lazily on tools/call.
	const FString Token = MakeTestToken();
	const int32 FakeEditorPort = 65000; // deliberately arbitrary; see above.
	const FString BuildId = TEXT("smoke-test-build");

	const bool bRegistered = Client.RegisterAndReturnAccepted(FakeEditorPort, Token, BuildId);
	if (!bRegistered)
	{
		AddError(TEXT("RegisterAndReturnAccepted() failed on a fresh proxy; see log for reason."));
		return false;
	}

	TestTrue(TEXT("IsRegistered after RegisterAndReturnAccepted"), Client.IsRegistered());
	// Identity is (pid, start_time_ns); StartTimeNs must be populated by the
	// time RegisterAndReturnAccepted returns true. Zero is a valid value when
	// GetProcessTimes/proc fails, but in normal CI/dev runs we expect a
	// non-zero tick.
	TestNotEqual(TEXT("StartTimeNs is populated after RegisterAndReturnAccepted"),
		Client.GetStartTimeNs(), static_cast<int64>(0));

	// Newest-wins: a second Register on the SAME worktree displaces the
	// first session. The second client's RegisterAndReturnAccepted succeeds
	// (no 409 singleton_session), and the first client's next heartbeat
	// would surface evicted_by -- but we verify the easy-to-observe half
	// here (acceptance) and leave the heartbeat-side observation to the
	// retry-register spec which already covers the eviction state machine.
	FClaireonProxyClient SecondClient;
	TestTrue(TEXT("Second client reaches the existing proxy"),
		SecondClient.EnsureProxyRunning());
	const bool bSecondRegistered = SecondClient.RegisterAndReturnAccepted(
		FakeEditorPort, MakeTestToken(), TEXT("smoke-test-second"));
	TestTrue(TEXT("Second concurrent register accepted (newest-wins)"), bSecondRegistered);
	TestTrue(TEXT("Second client IsRegistered is true"), SecondClient.IsRegistered());

	// Unregister; verify IsRegistered flips back.
	SecondClient.Unregister();
	TestFalse(TEXT("IsRegistered false after Unregister"), SecondClient.IsRegistered());

	// Leave the proxy running -- subsequent editor launches (or tests) will
	// attach to it. This matches the production lifecycle contract.
	return true;
}

// ----------------------------------------------------------------------------
// Attach path. Gated on vendored Python.
// After the round-trip above leaves a live proxy behind, a fresh client's
// EnsureProxyRunning must take the cheap-attach path without spawning a second
// process. "Did not spawn" is verified by sampling the proxy's own pid, from
// /admin/health, before and after the attach.
//
// It is deliberately NOT verified through proxy.lock. That file is written per
// worktree (claireon_proxy.py::proxy_lock_path -> <worktree_root>/Saved/Claireon/
// proxy.lock) while PROXY_REG_PORT is the fixed 43017 for the whole machine, and
// EnsureProxyRunning's attach branch returns true as soon as anything answers
// GET /health on that port -- logging "Attached to existing proxy". So on a
// machine running several worktrees, a proxy owned by a sibling checkout
// satisfies both EnsureProxyRunning and WaitForProxyListening while this
// worktree holds no lock at all, and the old assertion failed for a reason that
// had nothing to do with the attach path. ClaireonProxyClient.h already
// documents that a proxy on 43017 may not own this worktree's SHA port -- which
// is exactly why EnsureWorktreeBound exists -- so the inference from "something
// is listening" to "this worktree has a lock" was never sound.
//
// The pid is the right observable: it is a property of whichever proxy owns the
// port, so it answers "was a second process spawned" identically whether the
// live proxy belongs to this worktree or another one.
// ----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClaireonProxyClientLockAttachSmoke,
	"Claireon.ProxyClient.Smoke.LockAttach",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FClaireonProxyClientLockAttachSmoke::RunTest(const FString& /*Parameters*/)
{
	using namespace ClaireonProxyClientSmokeTest;

	if (!CanRunLiveProxyTests())
	{
		AddInfo(TEXT("SKIPPED: vendored Python or claireon_proxy.py missing; lock-attach not exercised."));
		return true;
	}

	// Make sure some proxy owns PROXY_REG_PORT. If the round-trip test ran before
	// us it will already be up; otherwise spawn one now.
	FClaireonProxyClient Bootstrap;
	if (!Bootstrap.EnsureProxyRunning())
	{
		AddError(TEXT("EnsureProxyRunning failed during attach bootstrap."));
		return false;
	}
	if (!WaitForProxyListening(/*DeadlineSeconds=*/ 5.0))
	{
		AddError(TEXT("Proxy never listened on PROXY_REG_PORT during bootstrap."));
		return false;
	}

	// Identity before the attach. PingProxyHealth caches the pid the proxy reports
	// from /admin/health, and only returns true for a response carrying the
	// Claireon shape -- so a non-Claireon occupant of 43017 fails here rather than
	// being mistaken for a proxy.
	if (!Bootstrap.PingProxyHealth())
	{
		AddError(TEXT("/admin/health did not answer with a Claireon proxy response after a live proxy was confirmed."));
		return false;
	}
	const uint32 PidBefore = Bootstrap.GetCachedProxyPid();
	if (PidBefore == 0)
	{
		AddError(TEXT("/admin/health answered but reported no pid, so 'did not respawn' cannot be established."));
		return false;
	}

	// Fresh client -- must take the attach path without spawning.
	FClaireonProxyClient Attach;
	const bool bAttached = Attach.EnsureProxyRunning();
	TestTrue(TEXT("Second EnsureProxyRunning attaches successfully"), bAttached);

	if (!Attach.PingProxyHealth())
	{
		AddError(TEXT("/admin/health stopped answering after the attach, so the attach did not leave a live proxy."));
		return false;
	}
	const uint32 PidAfter = Attach.GetCachedProxyPid();

	// The invariant: a spawn would have produced a different process. Whether the
	// newcomer won the port or the incumbent kept it under idempotent startup, the
	// reported pid would move.
	if (PidAfter != PidBefore)
	{
		AddError(FString::Printf(
			TEXT("Proxy pid changed across the attach (%u -> %u), so EnsureProxyRunning spawned rather than attached."),
			PidBefore, PidAfter));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
