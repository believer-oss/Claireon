// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_FlythroughStop.h"
#include "Tools/ClaireonFlythroughManager.h"

FString ClaireonTool_FlythroughStop::GetCategory() const { return TEXT("pie"); }
FString ClaireonTool_FlythroughStop::GetOperation() const { return TEXT("flythrough_stop"); }

FString ClaireonTool_FlythroughStop::GetDescription() const
{
    return TEXT("Cancel an in-progress flythrough and release the debug camera. Stateless / non-session: stops the editor-wide flythrough without any per-asset session.");
}

TSharedPtr<FJsonObject> ClaireonTool_FlythroughStop::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));
	// Parameterless, but the empty properties object is not decoration: the
	// argument gate stays permissive for a schema with no "properties" at all,
	// so omitting it opts this tool out of undeclared-argument rejection.
	Schema->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_FlythroughStop::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FClaireonFlythroughManager* Manager = FClaireonFlythroughManager::GetActive();
	if (!Manager)
	{
		return MakeSuccessResult(nullptr, TEXT("state: Idle\nmessage: No flythrough is active"));
	}

	Manager->Stop();
	FClaireonFlythroughManager::DestroyActive();

	return MakeSuccessResult(nullptr, TEXT("status: stopped\nmessage: Flythrough cancelled and debug camera released."));
}
