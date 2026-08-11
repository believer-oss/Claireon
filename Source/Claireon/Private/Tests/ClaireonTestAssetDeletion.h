// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "ObjectTools.h"
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
		return ValidObjects.Num() > 0 ? ObjectTools::DeleteObjectsUnchecked(ValidObjects) : 0;
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
