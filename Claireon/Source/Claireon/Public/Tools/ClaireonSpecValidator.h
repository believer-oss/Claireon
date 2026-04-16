// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Base class for apply_spec JSON validation.
 *
 * Performs structural checks common to all tools:
 * - Required top-level fields present
 * - Entry IDs are unique within the spec
 * - ID references are internally consistent (no dangling refs)
 * - Parameter types match expectations
 *
 * Per-tool subclasses override ValidateTool() for tool-specific rules
 * (valid entry types, property schemas, reference semantics).
 */
class CLAIREON_API FClaireonSpecValidator
{
public:
	virtual ~FClaireonSpecValidator() = default;

	/**
	 * Validate a spec JSON object. Returns true if valid.
	 * @param Spec The spec JSON to validate
	 * @param OutErrors Accumulated error messages
	 * @return true if spec is valid
	 */
	bool Validate(const TSharedPtr<FJsonObject>& Spec, TArray<FString>& OutErrors);

protected:
	/** Tool-specific validation. Override in per-tool validator subclasses. */
	virtual bool ValidateTool(const TSharedPtr<FJsonObject>& Spec, TArray<FString>& OutErrors);

private:
	/** Check structural requirements common to all specs. */
	bool ValidateStructure(const TSharedPtr<FJsonObject>& Spec, TArray<FString>& OutErrors);
};
