// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

/**
 * Shared process-wide transaction-group state for the decomposed
 * claireon.transaction_begin_group / end_group / rollback_group tools.
 *
 * FClaireonServer is single-instance and single-session, so a file-scope
 * namespace singleton is sufficient. FClaireonServer::HandleInitialized() and
 * module shutdown both call ResetGroupState() to close any leaked open groups.
 */
namespace ClaireonTransactionGroupState
{
	extern CLAIREON_API bool bGroupActive;
	extern CLAIREON_API FString ActiveGroupLabel;
	CLAIREON_API void ResetGroupState();
}
