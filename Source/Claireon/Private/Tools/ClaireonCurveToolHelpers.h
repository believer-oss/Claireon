// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Curves/RealCurve.h"
#include "Curves/RichCurve.h"
#include "Tools/IClaireonTool.h"

class UCurveBase;

/**
 * Shared plumbing for the curve_* tool family: loading a curve asset by path and
 * resolving the curve_index / curve_name sub-curve selector.
 *
 * Lives in one place because all three mutating tools need identical selector
 * semantics and identical error text -- triplicating it is how the "valid names"
 * lists drift apart.
 */
namespace ClaireonCurveToolHelpers
{
	/** Map the interp_mode param to its engine enum. Unknown/empty -> cubic. */
	ERichCurveInterpMode ParseInterpMode(const FString& ModeString);

	/** Canonical name for an interp mode, for echoing back in responses. */
	const TCHAR* InterpModeToString(ERichCurveInterpMode Mode);

	/**
	 * Load and type-check a curve asset by (possibly non-canonical) path.
	 * Returns null with OutError set on any failure.
	 */
	UCurveBase* LoadCurveAsset(const FString& AssetPath, FString& OutError);

	/**
	 * Resolve the sub-curve selector against a loaded asset.
	 *
	 * @param Curve             The loaded curve asset.
	 * @param Arguments         The tool's arguments; curve_index / curve_name are read from here.
	 * @param bAllowSelectAll   When true, absent selectors mean "every sub-curve"
	 *                          (curve_clear_keys). When false, absent means index 0.
	 * @param OutSelected       Receives the selected entries, in asset order.
	 * @param OutError          Set on failure, naming the valid indices or names for
	 *                          this asset's actual type.
	 * @return True on success.
	 */
	bool ResolveSubCurves(
		UCurveBase* Curve,
		const TSharedPtr<FJsonObject>& Arguments,
		bool bAllowSelectAll,
		TArray<FRichCurveEditInfo>& OutSelected,
		FString& OutError);
}
