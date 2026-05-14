// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_ApplySpec.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Dom/JsonObject.h"
#include "Tools/ClaireonSpecApplicator_WidgetBP.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_ApplySpec::GetName() const
{
    return TEXT("claireon.widgetbp_apply_spec");
}

FString ClaireonWidgetBPTool_ApplySpec::GetDescription() const
{
    return TEXT("Apply a declarative JSON specification to create/modify the Widget Blueprint atomically. Stateless / non-session: opens its own internal editing session keyed by asset_path. Transactional. The spec runs as one rollback unit; partial failures revert all spec operations together.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_ApplySpec::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("asset_path"), TEXT("Widget Blueprint asset path to create or modify."), true);
    Builder.AddObject(TEXT("spec"), TEXT("Declarative JSON specification describing widgets, properties, slots, bindings, and viewmodels."), true);
    Builder.AddString(TEXT("session_id"), TEXT("Optional existing session to reuse instead of opening a new one."));
    return Builder.Build();
}

FToolResult ClaireonWidgetBPTool_ApplySpec::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    // apply_spec is session-less; it manages its own session lifecycle.
    TSharedPtr<FJsonObject> Params = Arguments.IsValid() ? Arguments : MakeShared<FJsonObject>();
    if (Params->HasField(TEXT("params")))
    {
        const TSharedPtr<FJsonObject>* NestedObj = nullptr;
        if (Params->TryGetObjectField(TEXT("params"), NestedObj) && NestedObj && NestedObj->IsValid())
        {
            Params = *NestedObj;
        }
    }
	// Extract asset_path -- required
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("apply_spec requires 'asset_path' parameter"));
	}

	// Extract spec -- required JSON object
	const TSharedPtr<FJsonObject>* SpecPtr = nullptr;
	if (!Params->TryGetObjectField(TEXT("spec"), SpecPtr) || !SpecPtr || !SpecPtr->IsValid())
	{
		return MakeErrorResult(TEXT("apply_spec requires 'spec' parameter (JSON object)"));
	}

	// Optional: reuse an existing session
	FString SessionId;
	Params->TryGetStringField(TEXT("session_id"), SessionId);

	FClaireonSpecApplicator_WidgetBP Applicator;
	return Applicator.ApplySpec(*SpecPtr, AssetPath, SessionId);
}

