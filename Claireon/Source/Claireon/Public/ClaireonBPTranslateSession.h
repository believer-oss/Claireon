// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

struct FClaireonBPTranslateNodeStatus
{
	// pending, implemented, or skipped
	FString Status;

	// Node class type (e.g., K2Node_CallFunction)
	FString Type;

	// Node display name
	FString Name;
};

struct FClaireonBPTranslateBlueprintState
{
	FString GeneratedHeader;
	FString GeneratedSource;
	FString HeaderHash;
	FString SourceHash;

	TMap<FString, FClaireonBPTranslateNodeStatus> Nodes;

	int32 TotalNodes = 0;
	int32 ImplementedNodes = 0;
	int32 SkippedNodes = 0;
};

struct FClaireonBPTranslateCrossRef
{
	FString FromBP;
	FString FromNode;
	FString ToBP;
	FString ToClass;
};

class FClaireonBPTranslateSession
{
public:
	FString SessionId;
	FString Created;
	FString TargetModule;
	FString TargetDirectory;
	FString IniSection;
	TMap<FString, FClaireonBPTranslateBlueprintState> Blueprints;
	TArray<FClaireonBPTranslateCrossRef> CrossReferences;

	// in_progress or complete
	FString Status;

	// Persist session state to a JSON file
	bool SaveToFile(const FString& FilePath) const;

	// Load session state from a JSON file
	static FClaireonBPTranslateSession LoadFromFile(const FString& FilePath);

	// Update the status of a specific node within a blueprint
	void UpdateNodeStatus(const FString& BlueprintPath, const FString& NodeGuid, const FString& NewStatus);

	// Get completion statistics as a JSON object
	TSharedPtr<FJsonObject> GetCompletionStats() const;

	// Compute SHA-256 hash of a file's contents, returned as hex string
	static FString ComputeFileHash(const FString& FilePath);

	// Resolve a session by ID, optionally from a file path.
	// Returns nullptr and sets OutError if not found.
	static TSharedPtr<FClaireonBPTranslateSession> ResolveSession(
		const FString& SessionId, const FString& SessionFile, FString& OutError);

	// Cache a session in memory
	static void CacheSession(TSharedPtr<FClaireonBPTranslateSession> Session);

	// The file path where this session was last saved/loaded
	FString SessionFilePath;

private:
	static TMap<FString, TSharedPtr<FClaireonBPTranslateSession>> SessionCache;
};
