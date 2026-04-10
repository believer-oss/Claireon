// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonLogCapture.h"
#include "ClaireonSettings.h"

FClaireonLogCapture::FClaireonLogCapture(ELogVerbosity::Type InMinVerbosity)
	: MinVerbosity(InMinVerbosity)
{
	GLog->AddOutputDevice(this);
}

FClaireonLogCapture::~FClaireonLogCapture()
{
	GLog->RemoveOutputDevice(this);
}

void FClaireonLogCapture::Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category)
{
	// Lower numeric value = more severe. Warning=3, Error=2, Fatal=1.
	// Filter out messages less severe than our threshold.
	if (Verbosity > MinVerbosity)
	{
		return;
	}

	FScopeLock Lock(&CaptureCS);

	// Check excluded categories (only safe on game thread since UClaireonSettings::Get() uses GetDefault)
	if (IsInGameThread())
	{
		const UClaireonSettings* Settings = UClaireonSettings::Get();
		if (Settings && Settings->ExcludedEngineLogCategories.Contains(Category))
		{
			return;
		}
	}

	// Enforce caps
	if (CapturedMessages.Num() >= MaxCapturedMessages || TotalTextBytes >= MaxCapturedTextBytes)
	{
		bCapExceeded = true;
		return;
	}

	FCapturedMessage Message;
	Message.Text = V;
	Message.Verbosity = Verbosity;
	Message.Category = Category;

	TotalTextBytes += Message.Text.Len() * sizeof(TCHAR);
	CapturedMessages.Add(MoveTemp(Message));
}

FString FClaireonLogCapture::GetCapturedOutput() const
{
	FScopeLock Lock(&CaptureCS);

	FString Output;
	for (const FCapturedMessage& Message : CapturedMessages)
	{
		const TCHAR* VerbosityLabel = (Message.Verbosity <= ELogVerbosity::Error) ? TEXT("Error") : TEXT("Warning");
		Output += FString::Printf(TEXT("[%s] %s: %s\n"), VerbosityLabel, *Message.Category.ToString(), *Message.Text);
	}

	if (bCapExceeded)
	{
		Output += TEXT("[...truncated: message cap exceeded]\n");
	}

	return Output;
}

bool FClaireonLogCapture::HasErrors() const
{
	FScopeLock Lock(&CaptureCS);

	for (const FCapturedMessage& Message : CapturedMessages)
	{
		if (Message.Verbosity <= ELogVerbosity::Error)
		{
			return true;
		}
	}
	return false;
}

bool FClaireonLogCapture::HasOutput() const
{
	FScopeLock Lock(&CaptureCS);
	return CapturedMessages.Num() > 0;
}
