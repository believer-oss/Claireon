// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
//
// P2-21: the zombie-selection guard in ClaireonTestAssetDeletion.
//
// The suite crash this guards against is INTERMITTENT (it rides GC timing),
// so this test manufactures the poisoned state deterministically: select an
// object element into the GB object selection, then destroy the element out
// from under it -- exactly what happens when something selects an object and
// GC reaps it without a deselect, which nothing in the engine cleans up
// (selection sets subscribe to OnElementReplaced/OnElementUpdated only, and
// deselect-before-destroy is an editor invariant no commandlet run upholds).
// Without the sanitize, the next resolving walk over the selection
// (ClearSelection / DeselectAll / UnloadPackages during asset deletion) trips
// `checkf(RegisteredElementType)` in TypedElementRegistry.h and kills the
// commandlet. Full mechanism:
// Docs/llm/work/claireon-p2-band/000-P2-21-INVESTIGATION.md.

#if WITH_UNTESTED

#include "Untest.h"

#include "ClaireonTestAssetDeletion.h"

#include "Editor.h"
#include "Elements/Framework/EngineElementsLibrary.h"
#include "Elements/Framework/TypedElementSelectionSet.h"
#include "Selection.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace ClaireonTADTestsInternal
{
	// Count handles whose element id has been reset (type id 0) -- the zombie
	// signature. Reads ids only; never resolves, so it cannot assert.
	static int32 TAD_CountZombieHandles(const USelection* Selection)
	{
		const UTypedElementSelectionSet* SelectionSet = Selection->GetElementSelectionSet();
		if (!IsValid(SelectionSet))
		{
			return 0;
		}
		int32 Zombies = 0;
		FTypedElementListConstRef ElementList = SelectionSet->GetElementList();
		for (int32 i = 0; i < ElementList->Num(); ++i)
		{
			const FTypedElementHandle Handle = ElementList->GetElementHandleAt(i);
			if (Handle && Handle.GetId().GetTypeId() == 0)
			{
				++Zombies;
			}
		}
		return Zombies;
	}
} // namespace ClaireonTADTestsInternal

UNTEST_UNIT_OPTS(Claireon, TestAssetDeletion, SanitizeDisarmsZombieSelectionHandle, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonTADTestsInternal;

	if (!IsValid(GEditor))
	{
		UE_LOG(LogTemp, Log, TEXT("SanitizeDisarmsZombieSelectionHandle: no GEditor, skipping."));
		co_return;
	}
	USelection* Selection = GEditor->GetSelectedObjects();
	UNTEST_ASSERT_PTR(Selection);

	// Leave whatever is selected as we found it: this test swaps the set, so
	// pre-existing selection state (there should be none in a commandlet) is
	// not preserved -- assert the precondition instead of hiding it.
	UNTEST_ASSERT_EQ(Selection->Num(), 0);

	// A transient object to select. Kept alive by a strong ptr so only the
	// ELEMENT dies, not the object -- the zombie shape does not depend on the
	// object's lifetime, only on the element's.
	TStrongObjectPtr<UObject> Obj(NewObject<UPackage>(GetTransientPackage(), TEXT("ClaireonP221ZombieFixture")));
	Selection->Select(Obj.Get());
	UNTEST_ASSERT_EQ(Selection->Num(), 1);
	UNTEST_EXPECT_EQ(TAD_CountZombieHandles(Selection), 0);

	// Destroy the element out from under the selection. Destruction is
	// DEFERRED: the id survives until the registry processes the queue (in a
	// real run, OnPostGarbageCollect does this -- the GC-timing coupling that
	// made the suite crash intermittent). Flush explicitly to materialize the
	// zombie: internal data kept alive by the list's handle, id reset.
	UEngineElementsLibrary::DestroyEditorObjectElement(Obj.Get());
	ClaireonTestAssetDeletion::FlushDeferredElementRemovals();
	UNTEST_ASSERT_EQ(TAD_CountZombieHandles(Selection), 1);

	// The guard under test: detects the zombie and swaps the selection set.
	ClaireonTestAssetDeletion::SanitizeObjectSelectionForTest();
	UNTEST_EXPECT_EQ(TAD_CountZombieHandles(Selection), 0);
	UNTEST_EXPECT_EQ(Selection->Num(), 0);

	// The walk that killed whole suite runs pre-guard must now be safe.
	Selection->DeselectAll();

	// And a clean selection still round-trips through the fresh set.
	Selection->Select(Obj.Get());
	UNTEST_EXPECT_EQ(Selection->Num(), 1);
	Selection->Deselect(Obj.Get());
	UNTEST_EXPECT_EQ(Selection->Num(), 0);

	co_return;
}

#endif // WITH_UNTESTED
