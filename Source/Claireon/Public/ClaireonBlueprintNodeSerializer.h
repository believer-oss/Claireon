// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UEdGraphNode;

/**
 * Shared serializer for Blueprint graph nodes. Used by both the session-based
 * bp_inspect_node op and the stateless
 * blueprint_inspect_node tool.
 *
 * Payload shape is defined in FRACTURE/03_inspect_node.md. The serializer
 * handles pin-type structuring, per-class fields (function_reference,
 * variable_reference, macro_reference, custom_event_name), and truncation
 * of linked_to (32 entries) and default_value (1024 bytes).
 */
namespace ClaireonBlueprintNodeSerializer
{
	/** Serialize a Blueprint node to a JSON object per the fractured 03 spec. */
	TSharedPtr<FJsonObject> SerializeNodeToJson(
		const UEdGraphNode* Node,
		bool bIncludeConnections,
		bool bIncludePinDefaults);

	/** Serialize to pretty-printed JSON string. */
	FString SerializeNodeToString(
		const UEdGraphNode* Node,
		bool bIncludeConnections,
		bool bIncludePinDefaults);

	/**
	 * Append the node's identity member references to an existing JSON object.
	 * Covers everything needed to replay a node through bp_add_node:
	 * function_reference (CallFunction/Event), variable_reference (Get/Set,
	 * incl. member_parent/member_scope), macro_reference, target_type (casts),
	 * proxy fields (async-task nodes), delegate_reference (MC delegate nodes),
	 * struct_type, enum_type, timeline_name, component/delegate for bound
	 * events, and bound graph name for composites. Used by bp_get_graph.
	 */
	void AppendMemberReferenceFields(
		const UEdGraphNode* Node,
		const TSharedPtr<FJsonObject>& OutJson);
}
