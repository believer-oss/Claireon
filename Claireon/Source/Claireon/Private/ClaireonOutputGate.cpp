// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonOutputGate.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

FString FClaireonOutputGate::RouteResult(const IClaireonTool::FToolResult& Result)
{
	// TODO: Stage 009+ — Size-based routing with index threshold
	// For now, return direct JSON serialization
	if (Result.Data.IsValid())
	{
		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(Result.Data.ToSharedRef(), Writer);
		return OutputString;
	}
	return TEXT("{}");
}

FString FClaireonOutputGate::RouteLogs(const FString& Logs)
{
	// TODO: Stage 009+ — Size-based routing with index threshold
	return Logs;
}
