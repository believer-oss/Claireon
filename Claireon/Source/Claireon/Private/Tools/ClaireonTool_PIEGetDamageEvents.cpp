// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_PIEGetDamageEvents.h"
#include "ClaireonLog.h"
#include "ClaireonPIEManager.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"

FString ClaireonTool_PIEGetDamageEvents::GetName() const
{
	return TEXT("editor.pie.getDamageEvents");
}

FString ClaireonTool_PIEGetDamageEvents::GetDescription() const
{
	return TEXT("Get recorded damage events from a registered damage listener. "
		"Optionally clears the event buffer after retrieval.");
}

TSharedPtr<FJsonObject> ClaireonTool_PIEGetDamageEvents::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// listenerId - required
	TSharedPtr<FJsonObject> ListenerIdProp = MakeShared<FJsonObject>();
	ListenerIdProp->SetStringField(TEXT("type"), TEXT("string"));
	ListenerIdProp->SetStringField(TEXT("description"),
		TEXT("The listener ID returned by editor.pie.registerDamageListener"));
	Properties->SetObjectField(TEXT("listenerId"), ListenerIdProp);

	// clear - optional, default false
	TSharedPtr<FJsonObject> ClearProp = MakeShared<FJsonObject>();
	ClearProp->SetStringField(TEXT("type"), TEXT("boolean"));
	ClearProp->SetStringField(TEXT("description"),
		TEXT("If true, clears the event buffer after retrieval (default: false)"));
	ClearProp->SetBoolField(TEXT("default"), false);
	Properties->SetObjectField(TEXT("clear"), ClearProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	// Required fields
	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("listenerId")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_PIEGetDamageEvents::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	UE_LOG(LogClaireon, Display, TEXT("[MCP] editor.pie.getDamageEvents"));

	if (!GEditor)
	{
		return MakeErrorResult(TEXT("Editor is not available"));
	}

	if (!GEditor->IsPlaySessionInProgress())
	{
		return MakeErrorResult(TEXT("PIE is not running. Start a PIE session first with editor.pie.start"));
	}

	// Parse required parameter
	if (!Arguments.IsValid() || !Arguments->HasField(TEXT("listenerId")))
	{
		return MakeErrorResult(TEXT("Missing required parameter: listenerId"));
	}

	const FString ListenerId = Arguments->GetStringField(TEXT("listenerId"));

	bool bClear = false;
	if (Arguments->HasField(TEXT("clear")))
	{
		bClear = Arguments->GetBoolField(TEXT("clear"));
	}

	// Get events from the PIE manager
	FClaireonPIEManager& PIEManager = FClaireonPIEManager::Get();
	TArray<FMCPDamageEvent> Events = PIEManager.GetDamageEvents(ListenerId, bClear);

	// Calculate summary
	float TotalDamage = 0.0f;
	for (const FMCPDamageEvent& Event : Events)
	{
		TotalDamage += Event.Damage;
	}

	// Build output
	FString Output;
	Output += FString::Printf(TEXT("listenerId: %s\n"), *ListenerId);
	Output += FString::Printf(TEXT("eventCount: %d\n"), Events.Num());
	Output += FString::Printf(TEXT("totalDamage: %.2f\n"), TotalDamage);
	Output += FString::Printf(TEXT("cleared: %s\n"), bClear ? TEXT("true") : TEXT("false"));

	if (Events.Num() > 0)
	{
		Output += TEXT("events:\n");
		for (int32 i = 0; i < Events.Num(); ++i)
		{
			const FMCPDamageEvent& Event = Events[i];
			Output += FString::Printf(TEXT("  [%d] timestamp: %.3f, damage: %.2f"),
				i, Event.Timestamp, Event.Damage);

			if (!Event.InstigatorId.IsEmpty())
			{
				Output += FString::Printf(TEXT(", instigator: %s"), *Event.InstigatorId);
			}

			if (!Event.InstigatorClass.IsEmpty())
			{
				Output += FString::Printf(TEXT(" (%s)"), *Event.InstigatorClass);
			}

			if (Event.Tags.Num() > 0)
			{
				Output += TEXT(", tags: [");
				for (int32 j = 0; j < Event.Tags.Num(); ++j)
				{
					if (j > 0)
					{
						Output += TEXT(", ");
					}
					Output += Event.Tags[j];
				}
				Output += TEXT("]");
			}

			Output += TEXT("\n");
		}
	}
	else
	{
		Output += TEXT("events: (none)\n");
	}

	return MakeSuccessResult(nullptr, Output);
}
