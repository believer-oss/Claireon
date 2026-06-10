// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/FClaireonSpecApplicatorBase.h"

class UWidget;
class UWidgetBlueprint;

/**
 * apply_spec applicator for Widget Blueprint assets.
 *
 * Spec schema (tree tool pattern):
 * - widgets[] with id, type, parent, children, properties, slot_properties
 *
 * Pass 1: Create all widgets in parent-first order
 * Pass 2: Add children to parents, set properties and slot properties
 */
class CLAIREON_API FClaireonSpecApplicator_WidgetBP : public FClaireonSpecApplicatorBase
{
protected:
	virtual bool ValidateToolSpec(const TSharedPtr<FJsonObject>& Spec, TArray<FString>& OutErrors) override;
	virtual bool OpenOrCreateAsset(const FString& AssetPath, FString& OutSessionId, FString& OutError) override;
	virtual bool ApplyPass1_CreateEntities(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec) override;
	virtual bool ApplyPass2_WireRelationships(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec) override;
	virtual bool CompileAsset(const FString& SessionId, FString& OutError) override;
	virtual bool SaveAsset(const FString& SessionId, FString& OutError) override;
	virtual void CloseSession(const FString& SessionId) override;
	virtual FString GetToolName() const override { return TEXT("WidgetBP"); }

private:
	/** Local reference to the Widget Blueprint being edited. */
	TWeakObjectPtr<UWidgetBlueprint> WidgetBlueprint;

	/** SpecId -> created widget. Maintained by Pass 1 so Pass 2 can wire children that aren't yet attached to the tree. */
	TMap<FString, TWeakObjectPtr<UWidget>> WidgetsBySpecId;
};
