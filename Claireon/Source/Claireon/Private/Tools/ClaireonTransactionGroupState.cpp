// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTransactionGroupState.h"
#include "ClaireonLog.h"
#include "Editor.h"

namespace ClaireonTransactionGroupState
{
	bool bGroupActive = false;
	FString ActiveGroupLabel;

	void ResetGroupState()
	{
		if (bGroupActive)
		{
			if (GEditor)
			{
				GEditor->EndTransaction();
			}
			UE_LOG(LogClaireon, Warning, TEXT("[MCP] Closing leaked transaction group: '%s'"), *ActiveGroupLabel);
		}
		bGroupActive = false;
		ActiveGroupLabel.Empty();
	}
}
