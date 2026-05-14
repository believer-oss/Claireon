// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_PIEUnregisterDamageListener.h"
#include "ClaireonLog.h"
#include "ClaireonPIEManager.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"

FString ClaireonTool_PIEUnregisterDamageListener::GetCategory() const { return TEXT("pie"); }
FString ClaireonTool_PIEUnregisterDamageListener::GetOperation() const { return TEXT("unregister_damage_listener"); }

FString ClaireonTool_PIEUnregisterDamageListener::GetDescription() const
{
	return TEXT("Unregister a damage event listener, unbinding the delegate and removing it from the registry.");
}

TSharedPtr<FJsonObject> ClaireonTool_PIEUnregisterDamageListener::GetInputSchema() const
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

	Schema->SetObjectField(TEXT("properties"), Properties);

	// Required fields
	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("listenerId")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_PIEUnregisterDamageListener::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	UE_LOG(LogClaireon, Display, TEXT("[MCP] editor.pie.unregisterDamageListener"));

	if (!GEditor)
	{
		return MakeErrorResult(TEXT("Editor is not available"));
	}

	if (!GEditor->IsPlaySessionInProgress())
	{
		return MakeErrorResult(TEXT("PIE is not running. No active listeners to unregister."));
	}

	// Parse required parameter
	if (!Arguments.IsValid() || !Arguments->HasField(TEXT("listenerId")))
	{
		return MakeErrorResult(TEXT("Missing required parameter: listenerId"));
	}

	const FString ListenerId = Arguments->GetStringField(TEXT("listenerId"));

	// Unregister via PIE manager
	FClaireonPIEManager& PIEManager = FClaireonPIEManager::Get();
	const bool bUnregistered = PIEManager.UnregisterDamageListener(ListenerId);

	// Build output
	FString Output;
	Output += FString::Printf(TEXT("listenerId: %s\n"), *ListenerId);
	Output += FString::Printf(TEXT("unregistered: %s\n"), bUnregistered ? TEXT("true") : TEXT("false"));

	if (!bUnregistered)
	{
		Output += TEXT("Note: Listener was not found. It may have already been unregistered, ");
		Output += TEXT("or the listener ID may be invalid.\n");
	}

	return MakeSuccessResult(nullptr, Output);
}
