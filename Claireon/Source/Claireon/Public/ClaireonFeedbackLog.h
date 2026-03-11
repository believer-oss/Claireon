// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"

/**
 * Records operator feedback for MCP tool and feature improvement.
 * Persists entries to disk so the log survives editor restarts.
 *
 * Storage layout:
 *   {ProjectSavedDir}/MCP/Feedback/
 *     index.json
 *     entries/
 *       {EntryId}.json
 */
class FClaireonFeedbackLog
{
public:
	/** Get the singleton instance */
	static FClaireonFeedbackLog& Get();

	/**
	 * Record a feedback entry.
	 * @return Entry ID on success, empty string if persistence fails.
	 */
	FString RecordFeedback(
		const FString& Text,
		const TArray<FString>& RelatedMCPTools,
		const TArray<FString>& RelatedFeatures,
		bool bIsBug,
		bool bIsFeedback,
		bool bIsSuggestion,
		const FString& OperatorName,
		const FString& ClientInfo);

	/** Get the feedback log directory path */
	FString GetFeedbackDir() const;

private:
	FClaireonFeedbackLog();

	/** Generate a unique entry ID based on timestamp */
	FString GenerateEntryId() const;

	/** Write the index.json file */
	void WriteIndex() const;

	/** Load existing index from disk */
	void LoadIndex();

	/** Rotate old entries if over max count */
	void RotateEntries();

	/** In-memory entry list */
	struct FFeedbackEntry
	{
		FString Id;
		FDateTime Timestamp;
		FString TextPreview;                // First N chars of text
		TArray<FString> RelatedMCPTools;
		TArray<FString> RelatedFeatures;
		bool bIsBug = false;
		bool bIsFeedback = true;
		bool bIsSuggestion = false;
		FString OperatorName;
		FString ClientInfo;
	};

	TArray<FFeedbackEntry> Entries;
	bool bIndexLoaded = false;

	static constexpr int32 MaxEntries = 200;
	static constexpr int32 PreviewLength = 200;

	/** Thread safety */
	mutable FCriticalSection CriticalSection;
};
