// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"

/**
 * Records Python script invocations for auditing and pattern analysis.
 * Persists entries to disk so the log survives editor restarts.
 *
 * Storage layout:
 *   {ProjectSavedDir}/MCP/PythonAuditLog/
 *     index.json
 *     scripts/
 *       {EntryId}.py
 *       {EntryId}_output.txt
 */
class FClaireonPythonAuditLog
{
public:
	/** Get the singleton instance */
	static FClaireonPythonAuditLog& Get();

	/**
	 * Record a Python script execution.
	 * Called by ClaireonTool_ExecutePython after execution completes.
	 *
	 * @param ScriptText     The user's Python code (not including prefix/suffix)
	 * @param Output         Captured log output
	 * @param bSuccess       Whether execution completed successfully
	 * @param DurationMs     Execution duration in milliseconds
	 * @param ToolCallCount  Number of claireon.* calls made during execution
	 * @param ResultSummary  First 500 chars of the serialized result
	 */
	void RecordInvocation(
		const FString& ScriptText,
		const FString& Output,
		bool bSuccess,
		double DurationMs,
		int32 ToolCallCount = 0,
		const FString& ResultSummary = TEXT(""));

	/**
	 * Retrieve recent audit log entries as a JSON string.
	 * @param Limit Maximum entries to return
	 * @param FilterSuccess If set, only return entries matching this success value
	 * @return JSON string containing the entries
	 */
	FString GetRecentEntries(int32 Limit = 50, TOptional<bool> FilterSuccess = {}) const;

	/** Get the audit log directory path */
	FString GetAuditLogDir() const;

private:
	FClaireonPythonAuditLog();

	/** Generate a unique entry ID based on timestamp */
	FString GenerateEntryId() const;

	/** Write the index.json file */
	void WriteIndex() const;

	/** Load existing index from disk */
	void LoadIndex();

	/** Rotate old entries if over max count */
	void RotateEntries();

	/** In-memory entry list */
	struct FAuditEntry
	{
		FString Id;
		FDateTime Timestamp;
		FString ScriptPath;     // Relative to audit dir
		FString OutputPath;     // Relative to audit dir
		int32 ScriptSizeBytes = 0;
		bool bSuccess = false;
		double DurationMs = 0.0;
		int32 ToolCallCount = 0;        // Number of claireon.* calls made during execution
		FString ResultSummary;           // First 500 chars of the serialized result
		FString ScriptPreview;           // First N chars of the script
	};

	TArray<FAuditEntry> Entries;
	bool bIndexLoaded = false;

	static constexpr int32 MaxEntries = 500;
	static constexpr int32 PreviewLength = 100;

	/** Thread safety */
	mutable FCriticalSection CriticalSection;
};
