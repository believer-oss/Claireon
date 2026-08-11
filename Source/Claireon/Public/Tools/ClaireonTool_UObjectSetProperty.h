// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Stateless tool: reflection-based property writer for any loaded UObject.
 *
 * The write counterpart to uobject_inspect. Reaches every reflected property
 * the inspector can read -- including fields declared without a Blueprint
 * accessor specifier, fields declared protected or private in C++, transient
 * fields, nested struct members, and TArray elements -- on assets, CDOs, and
 * live PIE instances.
 *
 * Object-path resolution is literally shared with uobject_inspect
 * (ClaireonPathResolver::ResolveObjectFromPath), so any path that inspects
 * also writes. Property-path resolution is shared with bp_set_cdo_property
 * and level_set_actor_property (ClaireonPropertyUtils), which is what gives
 * this tool dot paths, [N] indexing, and Blueprint-CDO SCS component
 * redirection for free.
 *
 * Guard: reaching past the details panel is opt-in. A property that is not
 * CPF_Edit, or that is CPF_EditConst, is refused unless the caller passes
 * allow_non_editable=true. The refusal carries a hint whose args are the
 * original call with the flag set, so the opt-in is one re-issue away but is
 * never implicit. Writing a private field that the class invariants assume
 * only its owner touches is a real way to corrupt engine state, and the
 * caller should have to say so.
 *
 * Coherence: every write runs inside an FScopedTransaction with Modify() on
 * the target (and on the owning Blueprint when the target is an SCS template
 * or a Blueprint CDO), and goes through ClaireonPropertyUtils::WritePropertyByPath,
 * which brackets the import with PreEditChange / PostEditChangeProperty. Skipping
 * the notify is what desyncs derived engine state -- the nav-octree class of bug
 * that motivated the companion re-register tool.
 *
 * When the path traverses into a sub-object -- "MyComp.bCanEverAffectNavigation"
 * -- the change cycle is additionally run on THAT object. WritePropertyByPath
 * notifies only the root, which for a component path means the actor hears about
 * it and the component does not, so UActorComponent::ConsolidatedPostEditChange
 * never runs and the octree never resyncs. That is the exact reported failure.
 * Repairing a write that came from somewhere else is component_reregister's job;
 * this tool does not leave its own writes needing repair.
 *
 * Every successful write emits one audit line to LogClaireon naming the
 * object, path, old value, new value, and whether the non-editable opt-in
 * was used.
 */
class CLAIREON_API ClaireonTool_UObjectSetProperty : public IClaireonTool
{
public:
	FString GetCategory() const override { return TEXT("uobject"); }
	FString GetOperation() const override;
	FString GetDescription() const override;
	TArray<FString> GetSearchKeywords() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	EClaireonToolSessionMode GetSessionMode() const override { return EClaireonToolSessionMode::EditorWide; }
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
