// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Editor.h"
#include "Elements/Framework/TypedElementRegistry.h"
#include "Elements/Framework/TypedElementSelectionSet.h"
#include "ObjectTools.h"
#include "Selection.h"
#include "UObject/UObjectGlobals.h"

// Deletion of fixtures a test created, WITHOUT the editor's referencer scan.
//
// ObjectTools::ForceDeleteObjects -- and UEditorAssetLibrary::DeleteAsset, which routes
// to it via UEditorAssetSubsystem -- run RecursiveRetrieveReferencers and
// ForceReplaceReferences. Both walk EVERY live UObject with a reference-finding archive
// and call UObject::Serialize on each one. In an editor that has any real content
// resident, that is not safe: serializing a UNiagaraEmitter faults inside its nested
// struct arrays, and serializing a UK2Node pin faults on its FText. The fault kills the
// whole commandlet, so one test's cleanup destroys every test scheduled after it -- which
// is why suite runs kept ending as "N passed, 900+ DID NOT RUN".
//
// This is not fixable by configuration. The crash reproduces on every archive that path
// can pick: FReferencerFinderArchive (default), and FFindReferencersArchive both via
// ForceReplaceReferences and under Editor.UseLegacyGetReferencersForDeletion=1.
//
// The scan protects nothing here. A test fixture is created moments earlier by the test
// itself, so it cannot have acquired referencers the test does not know about.
// DeleteObjectsUnchecked skips the scan (bPerformReferenceCheck=false) and still deletes
// the package from disk through CleanupAfterSuccessfulDelete, so cleanup semantics are
// unchanged.
//
// Product code must keep using ForceDeleteObjects: there the reference check is the
// point, because the user is deleting assets that other assets may legitimately
// reference.
namespace ClaireonTestAssetDeletion
{
	/**
	 * Flush the typed-element frame-end work the commandlet never runs (P2-21).
	 *
	 * UTypedElementRegistry processes deferred element destroys and notifies
	 * element lists of pending removals from FCoreDelegates::OnEndFrame -- which
	 * CommandletHelpers::TickEngine NEVER broadcasts (it ticks GEngine, Slate and
	 * the core ticker; only the render-thread OnEndFrameRT fires, and only when
	 * rendering). So in a commandlet, an element destroyed after entering any
	 * element list (e.g. an object selected somewhere, then GC'd) stays in that
	 * list as a zombie handle with an unset type id, forever. The first
	 * ClearSelection to walk it -- ObjectTools::CleanupAfterSuccessfulDelete ->
	 * UPackageTools::UnloadPackages clears the GB object selection -- trips
	 * `checkf(RegisteredElementType)` (TypedElementRegistry.h:612) and kills the
	 * whole run. Reproduced deterministically: any prefix of the suite that
	 * selects an object element + Claireon.PIEActorIdScope.
	 * StaleIdReturnsNullAfterActorDestroyed's Destroy+CollectGarbage +
	 * any later asset-deleting cleanup.
	 *
	 * ProcessDeferredElementsToDestroy is the piece of OnEndFrame that matters
	 * here (and the only piece this engine exports publicly): its
	 * OnProcessingDeferredElementsToDestroy broadcast is what element lists
	 * purge their dead handles on. Deliberately NOT OnEndFrame() itself: that
	 * also manages the within-frame GC-guard flag, which is not ours to flip.
	 */
	inline void FlushDeferredElementRemovals()
	{
		if (UTypedElementRegistry* Registry = UTypedElementRegistry::GetInstance())
		{
			Registry->ProcessDeferredElementsToDestroy();
		}
	}

	/**
	 * Replace the GB object selection's element set when it holds a ZOMBIE
	 * handle (P2-21): an entry whose element was destroyed while selected --
	 * live internal data, element id reset to Unset (type id 0). Nothing in
	 * the engine removes such an entry from a selection list (deselect-before-
	 * destroy is an editor-maintained invariant that a commandlet test run
	 * does not uphold, and destruction timing rides on GC, which is why the
	 * suite crash was intermittent). Every path that RESOLVES the list --
	 * ClearSelection, DeselectAll, GetSelectedObject -- trips
	 * `checkf(RegisteredElementType)` (TypedElementRegistry.h:612) on it, so
	 * the sanitize must not resolve: detection reads only the handle ids, and
	 * the cure swaps the whole selection set via the public
	 * USelection::SetElementSelectionSet (releasing handles never resolves
	 * interfaces). A commandlet test run has no selection worth preserving.
	 */
	inline void SanitizeObjectSelectionForTest()
	{
		if (!GEditor)
		{
			return;
		}
		USelection* Selection = GEditor->GetSelectedObjects();
		if (!Selection)
		{
			return;
		}
		const UTypedElementSelectionSet* SelectionSet = Selection->GetElementSelectionSet();
		if (!SelectionSet)
		{
			return;
		}

		bool bHasZombie = false;
		FTypedElementListConstRef ElementList = SelectionSet->GetElementList();
		for (int32 i = 0; i < ElementList->Num(); ++i)
		{
			const FTypedElementHandle Handle = ElementList->GetElementHandleAt(i);
			if (Handle && Handle.GetId().GetTypeId() == 0)
			{
				bHasZombie = true;
				break;
			}
		}

		if (bHasZombie)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[P2-21] The GB object selection held a destroyed-element (zombie) handle; "
				     "replacing the selection set before asset deletion so UnloadPackages' "
				     "ClearSelection does not assert. Something selected an object and destroyed "
				     "it (usually via GC) without deselecting."));
			Selection->SetElementSelectionSet(
				NewObject<UTypedElementSelectionSet>(Selection, NAME_None, RF_Transactional));
		}
	}

	/** Delete already-loaded fixture objects. Returns how many were deleted. */
	inline int32 DeleteObjectsForTest(const TArray<UObject*>& Objects)
	{
		TArray<UObject*> ValidObjects;
		ValidObjects.Reserve(Objects.Num());
		for (UObject* Object : Objects)
		{
			if (IsValid(Object))
			{
				ValidObjects.Add(Object);
			}
		}
		if (ValidObjects.Num() == 0)
		{
			return 0;
		}
		// Zombie-proof the object selection BEFORE the delete: its package
		// unload (CleanupAfterSuccessfulDelete -> UPackageTools::UnloadPackages)
		// runs ClearSelection, which resolves every held handle through the
		// registry and trips checkf on a zombie.
		//
		// ORDER MATTERS: flush FIRST, then sanitize. A deferred-destroyed
		// element keeps its id until the flush processes it, so it is
		// invisible to the zombie scan; flushing after the scan would MINT the
		// zombie right before the delete walks the list. Same pair after the
		// delete, for the elements the delete itself destroys.
		FlushDeferredElementRemovals();
		SanitizeObjectSelectionForTest();
		const int32 NumDeleted = ObjectTools::DeleteObjectsUnchecked(ValidObjects);
		FlushDeferredElementRemovals();
		SanitizeObjectSelectionForTest();
		return NumDeleted;
	}

	/**
	 * Delete a fixture by asset path. False if nothing was there.
	 *
	 * Loaded quietly: callers use this as a delete-if-exists, and a missing package must
	 * not emit the load warning that Untest would score as a failure.
	 */
	inline bool DeleteAssetForTest(const FString& AssetPath)
	{
		UObject* Asset = StaticLoadObject(
			UObject::StaticClass(), nullptr, *AssetPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!IsValid(Asset))
		{
			return false;
		}
		return DeleteObjectsForTest({ Asset }) > 0;
	}
}
