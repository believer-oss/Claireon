// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTransactionGroupState.h"
#include "ClaireonLog.h"
#include "Editor.h"

namespace ClaireonTransactionGroupState
{
	bool bGroupActive = false;
	FString ActiveGroupLabel;
	FGuid ActiveGroupTransactionId;

	FString MakeGroupTitle(const FString& Label)
	{
		return FString::Printf(TEXT("[Claireon] %s"), *Label);
	}

	void ResetGroupState()
	{
		if (bGroupActive)
		{
			if (IsValid(GEditor))
			{
				GEditor->EndTransaction();
			}
			UE_LOG(LogClaireon, Warning, TEXT("[MCP] Closing leaked transaction group: '%s'"), *ActiveGroupLabel);
		}
		bGroupActive = false;
		ActiveGroupLabel.Empty();
		ActiveGroupTransactionId.Invalidate();
	}
}
