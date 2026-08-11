// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

class UWorld;

class ClaireonTool_ConsoleExecute : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual EClaireonToolSessionMode GetSessionMode() const override { return EClaireonToolSessionMode::Bypass; }
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

	/** The two output channels a console command can write to. They are distinct:
	 *  a handler writes to the FOutputDevice it is handed (ReturnChannel) and/or
	 *  through UE_LOG (LogOutput). Reporting only the first is why `stat dumpframe`
	 *  and `obj list` used to come back empty. */
	struct FConsoleDispatchOutput
	{
		FString ReturnChannel;
		FString LogOutput;
	};

	/** Runs Command against World via GEngine::Exec. When bCaptureLog is true the
	 *  dispatch is bracketed by an FClaireonLogCapture at the Log floor, so
	 *  UE_LOG output emitted by the handler is collected as well. Everything the
	 *  engine logs during the call is captured, not just the command's own output.
	 *  Exposed so tests can drive a world of their own choosing. */
	static FConsoleDispatchOutput DispatchInWorld(UWorld* World, const FString& Command, bool bCaptureLog);
};
