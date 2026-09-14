// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;

namespace ClaireonAstralJsonGuard
{
	struct FParseResult
	{
		TSharedPtr<FJsonObject> Object;
		TSharedPtr<FJsonValue> RequestId;
		FString Error;
		bool bAmbiguousKeys = false;
	};

	/** HTTP and Python ingress use these before acquiring sessions or dispatching tools. */
	FParseResult DeserializeRpc(const FString& RawJson);
	FParseResult DeserializeToolArguments(const FString& ToolName, const FString& RawJson);
}
