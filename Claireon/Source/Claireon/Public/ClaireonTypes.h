// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * Represents a JSON-RPC 2.0 request parsed from the HTTP body.
 */
struct FMCPJsonRpcRequest
{
	/** JSON-RPC version string (always "2.0") */
	FString JsonRpc;

	/** Request ID. Null for notifications. */
	TSharedPtr<FJsonValue> Id;

	/** Method name (e.g. "initialize", "tools/call") */
	FString Method;

	/** Optional params object */
	TSharedPtr<FJsonObject> Params;

	/** Whether this is a notification (no id field) */
	bool IsNotification() const { return !Id.IsValid() || Id->IsNull(); }

	/** Parse from a JSON object. Returns false if the object is not a valid JSON-RPC request. */
	static bool FromJsonObject(const TSharedPtr<FJsonObject>& JsonObject, FMCPJsonRpcRequest& OutRequest);
};

/**
 * Helper for building JSON-RPC 2.0 response objects.
 */
struct FMCPJsonRpcResponse
{
	/** Create a success response */
	static TSharedPtr<FJsonObject> MakeResult(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonValue>& Result);

	/** Create a success response with a JSON object result */
	static TSharedPtr<FJsonObject> MakeResult(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Result);

	/** Create an error response */
	static TSharedPtr<FJsonObject> MakeError(const TSharedPtr<FJsonValue>& Id, int32 Code, const FString& Message);

	// Standard JSON-RPC error codes
	static constexpr int32 ParseError = -32700;
	static constexpr int32 InvalidRequest = -32600;
	static constexpr int32 MethodNotFound = -32601;
	static constexpr int32 InvalidParams = -32602;
	static constexpr int32 InternalError = -32603;
};

/**
 * Request context that flows through the MCP handling pipeline.
 * Structured for future session support without rewrite.
 */
struct FMCPRequestContext
{
	/** The parsed JSON-RPC request */
	FMCPJsonRpcRequest Request;

	/** Optional session ID (empty for stateless v1) */
	FString SessionId;
};

/**
 * A single diagnostics log entry for the MCP server.
 * Stored in a ring buffer and displayed in the diagnostics widget.
 */
struct FMCPDiagnosticsEntry
{
	/** When the request was received */
	FDateTime Timestamp;

	/** JSON-RPC method (e.g. "tools/call", "initialize") */
	FString Method;

	/** Tool name, if this was a tools/call request */
	FString ToolName;

	/** Time taken to handle the request */
	double DurationMs = 0.0;

	/** Whether the response was an error */
	bool bIsError = false;

	/** Truncated request body (first 2048 chars) */
	FString RequestBody;

	/** Truncated response body (first 2048 chars) */
	FString ResponseBody;
};

/** Delegate broadcast when a new diagnostics entry is added */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnMCPDiagnosticsEntryAdded, const FMCPDiagnosticsEntry&);
