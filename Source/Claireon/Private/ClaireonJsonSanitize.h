// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/**
 * Non-finite JSON number guard.
 *
 * UE's JSON writer prints doubles with %.17g, which emits the bare tokens
 * `inf` / `-inf` / `nan` for non-finite values. Those are not legal JSON, so a
 * single poisoned field makes an entire tool result unparseable by every
 * client. The trace family is the reproducing case: the engine seeds every
 * open frame with EndTime = +inf, so any capture stopped mid-frame yields a
 * non-finite duration.
 *
 * This guard runs at the boundaries where all tool results converge, so no
 * individual tool has to remember to check.
 */
namespace ClaireonJsonSanitize
{
/** Maximum structural depth walked before the sanitizer gives up. Guards
 *  against adversarial or cyclic-looking input rather than trusting shape. */
inline constexpr int32 MaxRecursionDepth = 64;

/**
 * Replaces every non-finite JSON number with null, in place. Recurses
 * through nested objects and arrays.
 *
 * @param  InOut                  Object to sanitize. Null is a safe no-op.
 * @param  OutPoisonedFieldPaths  Appended with the dotted path of every
 *                                replaced field, e.g. "frames[3].duration_ms".
 * @return true if anything was replaced.
 */
bool SanitizeNonFiniteNumbers(const TSharedPtr<FJsonObject>& InOut, TArray<FString>& OutPoisonedFieldPaths);

/** Warning text for one replaced field. Shared so both result boundaries
 *  word the disclosure identically and a caller can match on one string. */
FString DescribeReplacement(const FString& FieldPath);
} // namespace ClaireonJsonSanitize
