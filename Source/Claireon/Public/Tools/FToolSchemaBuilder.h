// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * Lightweight JSON Schema builder for tool inputSchema definitions.
 * Reduces boilerplate when declaring per-tool parameter schemas.
 */
struct CLAIREON_API FToolSchemaBuilder
{
	TSharedPtr<FJsonObject> Schema;
	TSharedPtr<FJsonObject> Properties;
	TArray<TSharedPtr<FJsonValue>> RequiredFields;

	FToolSchemaBuilder();

	void AddString(const FString& Name, const FString& Description, bool bRequired = false);
	void AddNumber(const FString& Name, const FString& Description, bool bRequired = false);
	void AddInteger(const FString& Name, const FString& Description, bool bRequired = false);
	void AddBoolean(const FString& Name, const FString& Description, bool bRequired = false);
	void AddObject(const FString& Name, const FString& Description, bool bRequired = false);
	void AddArray(const FString& Name, const FString& Description, bool bRequired = false);
	void AddEnum(const FString& Name, const FString& Description, const TArray<FString>& Values, bool bRequired = false);

	/** Finalize and return the schema. Sets the required array if any fields were marked required. */
	TSharedPtr<FJsonObject> Build();

	/** Add the common session_id + suppress_output parameters (session_id is required). */
	void AddSessionParams();
};
