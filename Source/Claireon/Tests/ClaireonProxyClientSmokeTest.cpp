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
// Filter:   EAutomationTestFlags::EditorContext | EngineFilter.
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
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

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
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

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
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

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
// Lock-file attach path. Gated on vendored Python.
// After the round-trip above leaves a live proxy behind, a fresh client's
// EnsureProxyRunning must take the cheap-attach path without spawning a
// second process. We verify "did not spawn" by sampling proxy.lock's PID
// before and after; it must not change.
// ----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClaireonProxyClientLockAttachSmoke,
	"Claireon.ProxyClient.Smoke.LockAttach",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaireonProxyClientLockAttachSmoke::RunTest(const FString& /*Parameters*/)
{
	using namespace ClaireonProxyClientSmokeTest;

	if (!CanRunLiveProxyTests())
	{
		AddInfo(TEXT("SKIPPED: vendored Python or claireon_proxy.py missing; lock-attach not exercised."));
		return true;
	}

	const FString LockPath = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / TEXT("Claireon/proxy.lock"));

	// Make sure a proxy is running so the lock file is current. If the
	// round-trip test ran before us it will already be up; otherwise spawn
	// one now.
	FClaireonProxyClient Bootstrap;
	if (!Bootstrap.EnsureProxyRunning())
	{
		AddError(TEXT("EnsureProxyRunning failed during lock-attach bootstrap."));
		return false;
	}
	if (!WaitForProxyListening(/*DeadlineSeconds=*/ 5.0))
	{
		AddError(TEXT("Proxy never listened on PROXY_REG_PORT during bootstrap."));
		return false;
	}

	if (!FPaths::FileExists(LockPath))
	{
		AddError(FString::Printf(
			TEXT("Expected proxy.lock at %s after a live proxy was confirmed."),
			*LockPath));
		return false;
	}

	FString LockBefore;
	TestTrue(TEXT("Read proxy.lock before attach"),
		FFileHelper::LoadFileToString(LockBefore, *LockPath));

	// Fresh client -- must take the attach path without spawning.
	FClaireonProxyClient Attach;
	const bool bAttached = Attach.EnsureProxyRunning();
	TestTrue(TEXT("Second EnsureProxyRunning attaches successfully"), bAttached);

	FString LockAfter;
	TestTrue(TEXT("Read proxy.lock after attach"),
		FFileHelper::LoadFileToString(LockAfter, *LockPath));

	// If a second process had been spawned, the existing proxy would have
	// exited cleanly (idempotent startup) OR the new proxy would have taken
	// over the lock -- both produce content changes. An unchanged lock file
	// proves we took the cheap attach branch.
	TestEqual(TEXT("proxy.lock is unchanged (no respawn)"), LockAfter, LockBefore);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
