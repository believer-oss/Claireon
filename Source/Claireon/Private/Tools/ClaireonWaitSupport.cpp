// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWaitSupport.h"

#include "Dom/JsonObject.h"
#include "HAL/PlatformTime.h"
#include "Misc/Guid.h"

// ---------------------------------------------------------------------------
// FClaireonPIEWaitRegistry
// ---------------------------------------------------------------------------

FClaireonPIEWaitRegistry::FClaireonPIEWaitRegistry(TFunction<double()> InClock, bool bInUseCoreTicker)
	: Clock(MoveTemp(InClock))
	, bUseCoreTicker(bInUseCoreTicker)
{
}

FClaireonPIEWaitRegistry::~FClaireonPIEWaitRegistry()
{
	if (CoreTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(CoreTickerHandle);
		CoreTickerHandle.Reset();
	}
}

FClaireonPIEWaitRegistry& FClaireonPIEWaitRegistry::Get()
{
	// Function-local static: constructed on first use, never destroyed before
	// module shutdown ordering matters. Real clock + core-ticker pumping.
	static FClaireonPIEWaitRegistry Singleton(TFunction<double()>(), /*bInUseCoreTicker*/ true);
	return Singleton;
}

double FClaireonPIEWaitRegistry::ReadClockSeconds() const
{
	return Clock ? Clock() : FPlatformTime::Seconds();
}

FString FClaireonPIEWaitRegistry::StartWait(
	const FString& ConditionName,
	TFunction<bool()> Condition,
	double TimeoutSeconds,
	TFunction<FString()> TimeoutDiagnosticsProvider)
{
	const double StartSeconds = ReadClockSeconds();
	PruneExpiredTerminalRecords(StartSeconds);

	FWaitRecord Record;
	Record.ConditionName = ConditionName;
	Record.Condition = MoveTemp(Condition);
	Record.TimeoutDiagnosticsProvider = MoveTemp(TimeoutDiagnosticsProvider);
	Record.StartSeconds = StartSeconds;
	Record.TimeoutSeconds = TimeoutSeconds;

	const FString WaitId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
	Waits.Add(WaitId, MoveTemp(Record));

	if (bUseCoreTicker)
	{
		EnsureCoreTicker();
	}

	return WaitId;
}

void FClaireonPIEWaitRegistry::AdvanceRecord(FWaitRecord& Record, double InNowSeconds)
{
	if (Record.State != EWaitState::Pending)
	{
		return;
	}

	const double Elapsed = InNowSeconds - Record.StartSeconds;

	// Condition first: met-at-deadline counts as met, not timed out.
	// A null/unset condition can never become true and only times out.
	const bool bMet = Record.Condition ? Record.Condition() : false;
	if (bMet)
	{
		Record.State = EWaitState::Met;
		Record.TerminalElapsedSeconds = Elapsed;
		Record.TerminalAtSeconds = InNowSeconds;
		return;
	}

	if (Elapsed >= Record.TimeoutSeconds)
	{
		Record.State = EWaitState::TimedOut;
		Record.TerminalElapsedSeconds = Elapsed;
		Record.TerminalAtSeconds = InNowSeconds;
		if (Record.TimeoutDiagnosticsProvider)
		{
			// Invoked exactly once, at detection time, so it reports live state.
			Record.TimeoutDiagnostics = Record.TimeoutDiagnosticsProvider();
		}
	}
}

void FClaireonPIEWaitRegistry::TickWaits()
{
	const double TickNowSeconds = ReadClockSeconds();
	PruneExpiredTerminalRecords(TickNowSeconds);
	for (TPair<FString, FWaitRecord>& Pair : Waits)
	{
		AdvanceRecord(Pair.Value, TickNowSeconds);
	}
}

void FClaireonPIEWaitRegistry::PruneExpiredTerminalRecords(double InNowSeconds)
{
	for (auto It = Waits.CreateIterator(); It; ++It)
	{
		const FWaitRecord& Record = It.Value();
		if (Record.State != EWaitState::Pending &&
			InNowSeconds - Record.TerminalAtSeconds > ClaireonWaitSupport::TerminalWaitRetentionSeconds)
		{
			It.RemoveCurrent();
		}
	}
}

FClaireonPIEWaitRegistry::FWaitStatus FClaireonPIEWaitRegistry::Poll(const FString& WaitId)
{
	FWaitStatus Status;

	FWaitRecord* Record = Waits.Find(WaitId);
	if (!Record)
	{
		Status.bFound = false;
		return Status;
	}

	const double PollNowSeconds = ReadClockSeconds();

	// Advance before reporting so a poll observes the deadline (or a
	// just-became-true condition) even if no ticker fired in between.
	AdvanceRecord(*Record, PollNowSeconds);

	Status.bFound = true;
	Status.State = Record->State;
	Status.ConditionName = Record->ConditionName;
	Status.TimeoutSeconds = Record->TimeoutSeconds;
	if (Record->State == EWaitState::Pending)
	{
		Status.ElapsedSeconds = PollNowSeconds - Record->StartSeconds;
	}
	else
	{
		Status.ElapsedSeconds = Record->TerminalElapsedSeconds;
		Status.TimeoutDiagnostics = Record->TimeoutDiagnostics;
		// Terminal state is consumed on read.
		Waits.Remove(WaitId);
	}

	return Status;
}

int32 FClaireonPIEWaitRegistry::NumPendingWaits() const
{
	int32 Count = 0;
	for (const TPair<FString, FWaitRecord>& Pair : Waits)
	{
		if (Pair.Value.State == EWaitState::Pending)
		{
			++Count;
		}
	}
	return Count;
}

int32 FClaireonPIEWaitRegistry::NumTrackedWaits() const
{
	return Waits.Num();
}

void FClaireonPIEWaitRegistry::EnsureCoreTicker()
{
	if (CoreTickerHandle.IsValid())
	{
		return;
	}

	// 'this' is only captured for the never-destroyed production singleton
	// (bUseCoreTicker is true only for Get()); test instances never register.
	CoreTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([this](float /*DeltaTime*/)
		{
			TickWaits();
			if (NumPendingWaits() == 0)
			{
				// Nothing left to advance: unregister until the next StartWait.
				CoreTickerHandle.Reset();
				return false;
			}
			return true;
		}));
}

// ---------------------------------------------------------------------------
// ClaireonWaitSupport free functions
// ---------------------------------------------------------------------------

double ClaireonWaitSupport::ResolveTestRunTimeoutSeconds(bool bNoTimeout)
{
	return bNoTimeout ? TestRunNoTimeoutCapSeconds : TestRunDefaultTimeoutSeconds;
}

ClaireonWaitSupport::FStartWaitOutcome ClaireonWaitSupport::StartOrCompleteWait(
	FClaireonPIEWaitRegistry& Registry,
	const FString& ConditionName,
	TFunction<bool()> Condition,
	double TimeoutSeconds,
	TFunction<FString()> TimeoutDiagnosticsProvider)
{
	FStartWaitOutcome Outcome;

	if (Condition && Condition())
	{
		Outcome.bImmediatelyMet = true;
		return Outcome;
	}

	Outcome.bImmediatelyMet = false;
	Outcome.WaitId = Registry.StartWait(
		ConditionName, MoveTemp(Condition), TimeoutSeconds, MoveTemp(TimeoutDiagnosticsProvider));
	return Outcome;
}

IClaireonTool::FToolResult ClaireonWaitSupport::BuildWaitPollResult(
	FClaireonPIEWaitRegistry& Registry, const FString& WaitId)
{
	const FClaireonPIEWaitRegistry::FWaitStatus Status = Registry.Poll(WaitId);

	if (!Status.bFound)
	{
		return IClaireonTool::MakeErrorResult(FString::Printf(
			TEXT("Unknown wait_id '%s'. The wait was never started, or its terminal state was already consumed by a previous poll."),
			*WaitId));
	}

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("wait_id"), WaitId);
	ResultData->SetStringField(TEXT("condition"), Status.ConditionName);
	ResultData->SetNumberField(TEXT("elapsedSeconds"), Status.ElapsedSeconds);
	ResultData->SetNumberField(TEXT("timeoutSeconds"), Status.TimeoutSeconds);

	FString Summary;
	Summary += FString::Printf(TEXT("condition: %s\n"), *Status.ConditionName);

	switch (Status.State)
	{
	case FClaireonPIEWaitRegistry::EWaitState::Pending:
	{
		ResultData->SetStringField(TEXT("status"), TEXT("waiting"));
		ResultData->SetBoolField(TEXT("conditionMet"), false);
		ResultData->SetBoolField(TEXT("timedOut"), false);
		Summary += TEXT("status: waiting\n");
		Summary += FString::Printf(TEXT("waitId: %s\n"), *WaitId);
		Summary += FString::Printf(TEXT("elapsedSeconds: %.3f\n"), Status.ElapsedSeconds);
		Summary += FString::Printf(TEXT("timeoutSeconds: %.1f\n"), Status.TimeoutSeconds);
		Summary += TEXT("Note: Condition not yet met. Poll again with the same wait_id.\n");
		break;
	}
	case FClaireonPIEWaitRegistry::EWaitState::Met:
	{
		ResultData->SetStringField(TEXT("status"), TEXT("met"));
		ResultData->SetBoolField(TEXT("conditionMet"), true);
		ResultData->SetBoolField(TEXT("timedOut"), false);
		Summary += TEXT("conditionMet: true\n");
		Summary += TEXT("timedOut: false\n");
		Summary += FString::Printf(TEXT("elapsedSeconds: %.3f\n"), Status.ElapsedSeconds);
		Summary += FString::Printf(TEXT("timeoutSeconds: %.1f\n"), Status.TimeoutSeconds);
		break;
	}
	case FClaireonPIEWaitRegistry::EWaitState::TimedOut:
	default:
	{
		ResultData->SetStringField(TEXT("status"), TEXT("timedOut"));
		ResultData->SetBoolField(TEXT("conditionMet"), false);
		ResultData->SetBoolField(TEXT("timedOut"), true);
		Summary += TEXT("conditionMet: false\n");
		Summary += TEXT("timedOut: true\n");
		Summary += FString::Printf(TEXT("elapsedSeconds: %.3f\n"), Status.ElapsedSeconds);
		Summary += FString::Printf(TEXT("timeoutSeconds: %.1f\n"), Status.TimeoutSeconds);
		Summary += FString::Printf(TEXT("Note: Condition '%s' was not met within %.1f seconds.\n"),
			*Status.ConditionName, Status.TimeoutSeconds);
		if (!Status.TimeoutDiagnostics.IsEmpty())
		{
			Summary += Status.TimeoutDiagnostics;
			ResultData->SetStringField(TEXT("diagnostics"), Status.TimeoutDiagnostics);
		}
		break;
	}
	}

	return IClaireonTool::MakeSuccessResult(ResultData, Summary);
}
