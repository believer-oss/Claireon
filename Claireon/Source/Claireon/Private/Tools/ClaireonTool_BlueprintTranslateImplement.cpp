// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_BlueprintTranslateImplement.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase.h" // kBPCategory
#include "Tools/ClaireonBlueprintTranslateTool_Inspect.h"
#include "Tools/ClaireonBlueprintTranslateTool_Implement.h"
#include "Tools/ClaireonBlueprintTranslateTool_ForceImplement.h"
#include "Tools/ClaireonBlueprintTranslateTool_Skip.h"
#include "Tools/ClaireonBlueprintTranslateTool_MarkComplete.h"
#include "ClaireonLog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FString ClaireonTool_BlueprintTranslateImplement::GetCategory() const { return kBPCategory; }
FString ClaireonTool_BlueprintTranslateImplement::GetOperation() const { return TEXT("translate_implement"); }

FString ClaireonTool_BlueprintTranslateImplement::GetDescription() const
{
	return TEXT("DEPRECATED: dispatches on 'action'. Use the per-action tools instead: "
	            "bp_translate_inspect, bp_translate_implement_node, bp_translate_force_implement_node, "
	            "bp_translate_skip_node, bp_translate_mark_complete.");
}

TSharedPtr<FJsonObject> ClaireonTool_BlueprintTranslateImplement::GetInputSchema() const
{
	// Preserved schema for backwards-compatible callers; new callers should target the per-action tools directly.
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> SessionProp = MakeShared<FJsonObject>();
	SessionProp->SetStringField(TEXT("type"), TEXT("string"));
	SessionProp->SetStringField(TEXT("description"), TEXT("Session ID returned by the scaffold tool."));
	Properties->SetObjectField(TEXT("session_id"), SessionProp);

	TSharedPtr<FJsonObject> FileProp = MakeShared<FJsonObject>();
	FileProp->SetStringField(TEXT("type"), TEXT("string"));
	FileProp->SetStringField(TEXT("description"), TEXT("Direct path to the session JSON file."));
	Properties->SetObjectField(TEXT("session_file"), FileProp);

	TSharedPtr<FJsonObject> BPProp = MakeShared<FJsonObject>();
	BPProp->SetStringField(TEXT("type"), TEXT("string"));
	BPProp->SetStringField(TEXT("description"), TEXT("Blueprint asset path within the session."));
	Properties->SetObjectField(TEXT("blueprint"), BPProp);

	TSharedPtr<FJsonObject> GuidProp = MakeShared<FJsonObject>();
	GuidProp->SetStringField(TEXT("type"), TEXT("string"));
	GuidProp->SetStringField(TEXT("description"), TEXT("GUID of the node to operate on."));
	Properties->SetObjectField(TEXT("node_guid"), GuidProp);

	TSharedPtr<FJsonObject> ActionProp = MakeShared<FJsonObject>();
	ActionProp->SetStringField(TEXT("type"), TEXT("string"));
	ActionProp->SetStringField(TEXT("description"),
		TEXT("Action: inspect, implement, force_implement, skip, or mark_complete."));
	TArray<TSharedPtr<FJsonValue>> ActionEnum;
	ActionEnum.Add(MakeShared<FJsonValueString>(TEXT("inspect")));
	ActionEnum.Add(MakeShared<FJsonValueString>(TEXT("implement")));
	ActionEnum.Add(MakeShared<FJsonValueString>(TEXT("force_implement")));
	ActionEnum.Add(MakeShared<FJsonValueString>(TEXT("skip")));
	ActionEnum.Add(MakeShared<FJsonValueString>(TEXT("mark_complete")));
	ActionProp->SetArrayField(TEXT("enum"), ActionEnum);
	Properties->SetObjectField(TEXT("action"), ActionProp);

	TSharedPtr<FJsonObject> CodeProp = MakeShared<FJsonObject>();
	CodeProp->SetStringField(TEXT("type"), TEXT("string"));
	CodeProp->SetStringField(TEXT("description"),
		TEXT("Replacement C++ code for the node region. Required for implement/force_implement."));
	Properties->SetObjectField(TEXT("code"), CodeProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("session_id")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("action")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_BlueprintTranslateImplement::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString Action;
	if (!Arguments.IsValid() || !Arguments->TryGetStringField(TEXT("action"), Action) || Action.IsEmpty())
	{
		return MakeErrorResult(TEXT("'action' is required. Valid values: inspect, implement, force_implement, skip, mark_complete."));
	}

	if (Action == TEXT("inspect"))
	{
		UE_LOG(LogClaireon, Warning, TEXT("[bp_translate_implement] DEPRECATED: forward to 'bp_translate_inspect'."));
		ClaireonBlueprintTranslateTool_Inspect Tool;
		return Tool.Execute(Arguments);
	}
	if (Action == TEXT("implement"))
	{
		UE_LOG(LogClaireon, Warning, TEXT("[bp_translate_implement] DEPRECATED: forward to 'bp_translate_implement_node'."));
		ClaireonBlueprintTranslateTool_Implement Tool;
		return Tool.Execute(Arguments);
	}
	if (Action == TEXT("force_implement"))
	{
		UE_LOG(LogClaireon, Warning, TEXT("[bp_translate_implement] DEPRECATED: forward to 'bp_translate_force_implement_node'."));
		ClaireonBlueprintTranslateTool_ForceImplement Tool;
		return Tool.Execute(Arguments);
	}
	if (Action == TEXT("skip"))
	{
		UE_LOG(LogClaireon, Warning, TEXT("[bp_translate_implement] DEPRECATED: forward to 'bp_translate_skip_node'."));
		ClaireonBlueprintTranslateTool_Skip Tool;
		return Tool.Execute(Arguments);
	}
	if (Action == TEXT("mark_complete"))
	{
		UE_LOG(LogClaireon, Warning, TEXT("[bp_translate_implement] DEPRECATED: forward to 'bp_translate_mark_complete'."));
		ClaireonBlueprintTranslateTool_MarkComplete Tool;
		return Tool.Execute(Arguments);
	}

	return MakeErrorResult(FString::Printf(
		TEXT("Invalid action: %s. Valid values: inspect, implement, force_implement, skip, mark_complete."), *Action));
}
