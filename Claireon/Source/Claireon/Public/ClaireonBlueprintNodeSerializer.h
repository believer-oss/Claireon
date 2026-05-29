// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UEdGraphNode;

/**
 * Shared serializer for Blueprint graph nodes. Used by both the session-based
 * bp_inspect_node op and the stateless
 * claireon.blueprint_inspect_node tool.
 *
 * Payload shape is defined in the inspect-node payload spec. The serializer
 * handles pin-type structuring, per-class fields (function_reference,
 * variable_reference, macro_reference, custom_event_name), and truncation
 * of linked_to (32 entries) and default_value (1024 bytes).
 */
namespace ClaireonBlueprintNodeSerializer
{
	/** Serialize a Blueprint node to a JSON object per the inspect-node spec. */
	TSharedPtr<FJsonObject> SerializeNodeToJson(
		const UEdGraphNode* Node,
		bool bIncludeConnections,
		bool bIncludePinDefaults);

	/** Serialize to pretty-printed JSON string. */
	FString SerializeNodeToString(
		const UEdGraphNode* Node,
		bool bIncludeConnections,
		bool bIncludePinDefaults);
}
