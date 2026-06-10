// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * B33: static-analysis lint for UCancellableAsyncAction subclasses.
 *
 * UE's K2Node_BaseAsyncTask walks only the first BlueprintAssignable delegate
 * signature when laying out output pins, so a UCancellableAsyncAction subclass
 * that declares multiple BlueprintAssignable delegates with heterogeneous
 * signatures silently drops pins for the non-matching delegates. This tool is
 * an offline reflection-based linter; it does NOT emit a runtime warning, it
 * just reports the conflicting delegate names so a developer can resolve them
 * before the silent pin loss hits a caller.
 *
 * Input: asset path of a Blueprint, or a native class path
 * (/Script/Module.ClassName). The tool resolves to a UClass; if the resolved
 * class is not a UCancellableAsyncAction subclass it returns a "not
 * applicable" result with no warning.
 *
 * Output: a list of delegate signature groups; if more than one distinct
 * BlueprintAssignable delegate signature exists, the response carries a
 * warning naming each delegate's signature function.
 *
 * Read-only. Immediate-mode (no session required).
 */
class CLAIREON_API ClaireonTool_UClassCheckAsyncActionDelegateSignatures : public IClaireonTool
{
public:
	FString GetCategory() const override { return TEXT("uclass"); }
	FString GetOperation() const override;
	FString GetDescription() const override;
	TArray<FString> GetSearchKeywords() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
