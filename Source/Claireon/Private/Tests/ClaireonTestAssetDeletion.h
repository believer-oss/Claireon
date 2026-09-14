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

// For test-owned fixtures only. DeleteObjectsUnchecked skips DeleteSingleObject's
// reference check and still removes files, but downstream cleanup can reach the
// referencer finder and crash. Product deletion must retain reference checks.
namespace ClaireonTestAssetDeletion
{
	/**
	 * Commandlets do not broadcast OnEndFrame. Flush deferred element removals so
	 * selection lists can purge destroyed handles before package unloading. Call only
	 * ProcessDeferredElementsToDestroy; OnEndFrame also manages the engine GC guard.
	 */
	inline void FlushDeferredElementRemovals()
	{
		if (UTypedElementRegistry* Registry = UTypedElementRegistry::GetInstance())
		{
			Registry->ProcessDeferredElementsToDestroy();
		}
	}

	/**
	 * Replace object selection when it holds a destroyed element with an unset type ID.
	 * Read handle IDs without resolving interfaces: resolution asserts on these handles.
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
		// Flush before sanitizing: deferred destruction resets type IDs and exposes stale
		// handles. Repeat after deletion for elements destroyed during cleanup.
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
