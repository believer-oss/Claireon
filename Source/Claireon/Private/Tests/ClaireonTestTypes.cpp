// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
// Out-of-line bodies for test-only fixtures declared in ClaireonTestTypes.h.

#include "ClaireonTestTypes.h"

UClaireonTestAsyncAction* UClaireonTestAsyncAction::ClaireonTestAsyncDelay(UObject* /*WorldContextObject*/, float /*Duration*/)
{
	return NewObject<UClaireonTestAsyncAction>();
}
