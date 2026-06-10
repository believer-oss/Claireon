// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Misc/OutputDevice.h"

/**
 * RAII guard: attaches to GLog on construction, detaches on destruction.
 * Captures Error and Warning messages emitted during its lifetime.
 *
 * Threading contract:
 * - The critical section (CaptureCS) guards ONLY the mutable message buffer
 *   (CapturedMessages, TotalTextBytes, bCapExceeded).
 * - ExcludedCategoriesSnapshot is const and lock-free post-construction;
 *   safety relies on the release fence provided by GLog->AddOutputDevice
 *   at the end of the constructor.
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

	/** Returns captured messages formatted as "[Error] Category: text" or "[Warning] Category: text", one per line. */
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

	// Snapshot of the user denylist at construction time. Captured here so
	// that off-thread log emissions (AnimBP compile, async loading, etc.) can
	// still be filtered without touching the UObject CDO from a worker thread.
	// const + lock-free post-construction; safety via GLog->AddOutputDevice release fence.
	const TSet<FName> ExcludedCategoriesSnapshot;
};
