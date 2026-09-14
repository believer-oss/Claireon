// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

class FAssetEditorToolkit;

/** Missing precondition for a usable interactive asset editor. */
enum class EClaireonEditorAvailability : uint8
{
	Available,
	NoEditorEngine,
	NoSlateApplication,
	NoAssetEditorSubsystem,
};

/** What an open request actually did to the asset's editor window. */
enum class EClaireonEditorWindowState : uint8
{
	NotOpened,
	Opened,
	AlreadyOpen,
};

/** Window-open outcome, with a reason when no window opens. */
struct FClaireonEditorOpenOutcome
{
	EClaireonEditorWindowState State = EClaireonEditorWindowState::NotOpened;
	EClaireonEditorAvailability Availability = EClaireonEditorAvailability::Available;

	/** Empty unless State == NotOpened. Names the missing precondition, or the failure. */
	FString Reason;

	bool WasOpenedOrAlreadyOpen() const { return State != EClaireonEditorWindowState::NotOpened; }
};

/** Asset-editor window access for tool sessions. */
namespace ClaireonAssetEditorWindow
{
	/** Which precondition for a usable interactive asset editor is missing, if any. */
	CLAIREON_API EClaireonEditorAvailability CheckAvailability();

	CLAIREON_API const TCHAR* ToWireString(EClaireonEditorAvailability Availability);
	CLAIREON_API const TCHAR* ToWireString(EClaireonEditorWindowState State);

	/** One sentence naming the missing precondition. Empty when Available. */
	CLAIREON_API FString DescribeMissingPrecondition(EClaireonEditorAvailability Availability);

	/**
	 * Open an editor only if none exists and the availability checks pass.
	 * Existing windows remain unowned by this call.
	 */
	CLAIREON_API FClaireonEditorOpenOutcome OpenForSession(UObject* Asset);

	/**
	 * The toolkit currently registered for Asset, or null.
	 *
	 * Returns the FIRST of possibly several: an asset can be open more than once, so a
	 * caller that must not be ambiguous records the instance it got and resolves through
	 * that recorded identity afterwards rather than calling this again.
	 */
	CLAIREON_API TSharedPtr<FAssetEditorToolkit> FindToolkitForAsset(UObject* Asset);

	/** True when Instance is still registered with the asset-editor subsystem for Asset. */
	CLAIREON_API bool IsInstanceRegistered(UObject* Asset, const FAssetEditorToolkit* Instance);
}

/**
 * Open an asset editor and optionally close the specific instance opened by this scope.
 * Pre-existing windows and instances already closed by the user are left alone.
 */
class CLAIREON_API FClaireonScopedAssetEditor
{
public:
	/**
	 * @param InAsset           The asset whose editor to open.
	 * @param bInCloseOnDestroy Close on destruction -- and then ONLY the instance this
	 *                          object opened, and only while it is still registered.
	 */
	explicit FClaireonScopedAssetEditor(UObject* InAsset, bool bInCloseOnDestroy = true);
	virtual ~FClaireonScopedAssetEditor();

	FClaireonScopedAssetEditor(const FClaireonScopedAssetEditor&) = delete;
	FClaireonScopedAssetEditor& operator=(const FClaireonScopedAssetEditor&) = delete;
	FClaireonScopedAssetEditor(FClaireonScopedAssetEditor&&) = delete;
	FClaireonScopedAssetEditor& operator=(FClaireonScopedAssetEditor&&) = delete;

	/** What the construction did to the window, and why when it did nothing. */
	const FClaireonEditorOpenOutcome& GetOpenOutcome() const { return Outcome; }

	/** True when the window was already open before this object was constructed. */
	bool WasAlreadyOpen() const { return Outcome.State == EClaireonEditorWindowState::AlreadyOpen; }

	/** The toolkit this object opened or found. */
	TSharedPtr<FAssetEditorToolkit> GetToolkit() const { return Toolkit; }
	bool IsEditorOpen() const { return Toolkit.IsValid(); }

	UObject* GetAsset() const { return Asset.Get(); }

private:
	TWeakObjectPtr<UObject> Asset;
	TSharedPtr<FAssetEditorToolkit> Toolkit;
	FClaireonEditorOpenOutcome Outcome;
	bool bCloseOnDestroy = false;
};
