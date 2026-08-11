// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Misc/OutputDevice.h"

/**
 * RAII guard: attaches to GLog on construction, detaches on destruction.
 * Captures every message emitted during its lifetime whose verbosity is at
 * least as severe as InMinVerbosity (default Warning). Pass
 * ELogVerbosity::Log to also capture Display and Log lines -- most console
 * commands (`stat dumpframe`, `obj list`) report at those levels, so a
 * Warning floor captures nothing from them.
 *
 * Threading contract:
 * - The critical section (CaptureCS) guards ONLY the mutable message buffer
 *   (CapturedMessages, TotalTextBytes, bCapExceeded).
 * - MinVerbosity is read-only after construction and is intentionally not
 *   under the lock.
 */
class FClaireonLogCapture : public FOutputDevice
{
public:
	explicit FClaireonLogCapture(ELogVerbosity::Type InMinVerbosity = ELogVerbosity::Warning);
	~FClaireonLogCapture();

	// Non-copyable, non-movable (attached to GLog)
	FClaireonLogCapture(const FClaireonLogCapture&) = delete;
	FClaireonLogCapture& operator=(const FClaireonLogCapture&) = delete;
	FClaireonLogCapture(FClaireonLogCapture&&) = delete;
	FClaireonLogCapture& operator=(FClaireonLogCapture&&) = delete;

	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override;

	/** Registers as an UNBUFFERED device: GLog calls Serialize synchronously on the
	 *  emitting thread instead of routing through the async primary-log-thread queue.
	 *  Serialize is CaptureCS-guarded, so concurrent calls are safe, and synchronous
	 *  delivery makes GetCapturedOutput() complete for callers that read immediately
	 *  after the scope of interest (tool execution, tests). As a buffered device the
	 *  read raced asynchronous delivery and could miss lines still in the queue. */
	virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

	/** Returns captured messages formatted as "[Verbosity] Category: text", one per line
	 *  (e.g. "[Error] LogClaireon: ...", "[Log] LogTemp: ..."). */
	FString GetCapturedOutput() const;

	/** Returns true if any Error-level messages were captured. */
	bool HasErrors() const;

	/** Returns true if any messages were captured at all. */
	bool HasOutput() const;

	/** Maximum number of messages to capture before truncating. */
	static constexpr int32 MaxCapturedMessages = 999;

	/** Maximum total text size in bytes before truncating. */
	static constexpr int32 MaxCapturedTextBytes = 4 * 1024 * 1024;

private:
	struct FCapturedMessage
	{
		FString Text;
		ELogVerbosity::Type Verbosity;
		FName Category;
	};

	TArray<FCapturedMessage> CapturedMessages;
	// Read-only after construction; not guarded by CaptureCS.
	ELogVerbosity::Type MinVerbosity;
	int32 TotalTextBytes = 0;
	bool bCapExceeded = false;
	// Guards ONLY the mutable message buffer (CapturedMessages, TotalTextBytes, bCapExceeded).
	mutable FCriticalSection CaptureCS;
};
