// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Stateless tool: runs the details-panel change cycle on an actor component so
 * engine state derived from its properties is re-established.
 *
 * The repair for raw property writes. A write that bypasses
 * PreEditChange / PostEditChangeProperty -- Python's set_editor_property, a
 * direct pointer poke, a tool that only wrote memory -- leaves every piece of
 * derived state that hangs off the notification stale. The reported case was
 * navigation: UActorComponent::SetCanEverAffectNavigation is not a UFUNCTION,
 * so Python can only write bCanEverAffectNavigation raw, which skips
 * HandleCanEverAffectNavigationChange and desyncs the navigation octree from
 * the component's own flag.
 *
 * This tool does what the details panel does:
 *   Component->PreEditChange(Prop)      // takes an FComponentReregisterContext
 *   Component->PostEditChangeProperty() // ConsolidatedPostEditChange: re-registers,
 *                                       // and runs the per-property handlers
 *
 * Naming changed_property matters. UActorComponent::ConsolidatedPostEditChange
 * dispatches its special cases on the changed property's name, so
 * changed_property="bCanEverAffectNavigation" is what makes the nav path run.
 * Omitting it still performs a full unregister/re-register, which covers the
 * render/physics-state class of staleness but not the name-dispatched handlers.
 *
 * Composes with uobject_set_property rather than duplicating it: that tool
 * writes and notifies the owning component itself, so this one is for repairing
 * after a write that came from somewhere else.
 *
 * The pre/post pair is RAII-guarded. UActorComponent::PreEditChange checkf()s
 * when a previous pre had no matching post, so an unpaired call would take down
 * the editor on the NEXT edit of that component, far from the cause.
 */
class CLAIREON_API ClaireonTool_ComponentReregister : public IClaireonTool
{
public:
	FString GetCategory() const override { return TEXT("component"); }
	FString GetOperation() const override;
	FString GetDescription() const override;
	TArray<FString> GetSearchKeywords() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	EClaireonToolSessionMode GetSessionMode() const override { return EClaireonToolSessionMode::EditorWide; }
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
