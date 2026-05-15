// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/FClaireonSpecApplicatorBase.h"
#include "Tools/IClaireonTool.h"

class ULevelSequence;

/**
 * apply_spec applicator for Level Sequence assets (F6 -- stage 015 impls).
 *
 * Spec schema:
 * {
 *   "asset_path": "/Game/...",
 *   "playback_range": { "start_frame": int, "end_frame": int },   // optional
 *   "marked_frames": [{ "frame": int, "label": str }],            // optional
 *   "bindings": [
 *     {
 *       "label": str,
 *       "kind": "possessable" | "spawnable" | "root",
 *       "object_class": "/Script/...",                            // possessable / spawnable
 *       "tracks": [
 *         {
 *           "type": "transform" | "visibility" | "event" | "float" | ...,
 *           "sections": [
 *             {
 *               "start_frame": int, "end_frame": int, "row_index": int,
 *               "keyframes": [
 *                 { "frame": int, "value": <json>, "endpoint": str }
 *               ]
 *             }
 *           ]
 *         }
 *       ]
 *     }
 *   ]
 * }
 *
 * Identity rules:
 *   Bindings          -- by Label (first occurrence wins).
 *   Tracks            -- by (binding_label, Type) -- first occurrence wins.
 *   Sections          -- by (RowIndex, StartFrame).
 *   Keyframes         -- by Frame.
 *   PlaybackRange     -- direct replace.
 *
 * Apply is wrapped in the base class's FScopedTransaction. Each sub-operation
 * delegates to the same static handler functions F2's dispatcher calls so that
 * per-op semantics round-trip. Idempotency is the load-bearing invariant:
 * applying the same spec twice produces zero new operations.
 */
class CLAIREON_API FClaireonSpecApplicator_LevelSequence : public FClaireonSpecApplicatorBase
{
protected:
	virtual bool ValidateToolSpec(const TSharedPtr<FJsonObject>& Spec, TArray<FString>& OutErrors) override;
	virtual bool OpenOrCreateAsset(const FString& AssetPath, FString& OutSessionId, FString& OutError) override;
	virtual bool ApplyPass1_CreateEntities(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec) override;
	virtual bool ApplyPass2_WireRelationships(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec) override;
	virtual bool CompileAsset(const FString& SessionId, FString& OutError) override;
	virtual bool SaveAsset(const FString& SessionId, FString& OutError) override;
	virtual void CloseSession(const FString& SessionId) override;
	virtual FString GetToolName() const override { return TEXT("LevelSequence"); }

private:
	/** Local reference to the Level Sequence being edited. */
	TWeakObjectPtr<ULevelSequence> Sequence;

	/** Session id used for Session-Manager lifecycle. */
	FString ActiveSessionId;

	/** Whether this applicator opened ActiveSessionId (vs reusing a caller-owned one). */
	bool bOwnsSession = false;
};
