// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// WI-11: montage segment mutations must keep SequenceLength and the notify
// caches fresh. UAnimMontage::PostEditChangeProperty never recalculates the
// stored length, so before the fix every segment mutator left GetPlayLength()
// stale: notify adds past the old length were rejected and manual notify
// retimes left the Notifies array unsorted.
//
// These tests build throwaway montages from in-memory /Game/__MCPTests
// AnimSequences, drive the anim_* montage tools through their JSON Execute
// entry points (session registered directly with the session manager plus the
// anim tool-data map), and assert on UAnimMontage::GetPlayLength() and notify
// ordering afterward. All test assets are deleted in teardown; nothing is
// saved to disk.

#if WITH_UNTESTED

#include "Untest.h"

#include "Tools/ClaireonAnimEditToolBase.h"
#include "Tools/ClaireonAnimTools_Montage.h"
#include "Tools/ClaireonAnimHelpers.h"
#include "Tools/IClaireonTool.h"
#include "ClaireonSessionManager.h"

#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/Skeleton.h"
#include "ReferenceSkeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Misc/FrameRate.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "UObject/Package.h"

#include "ClaireonTestAssetDeletion.h"
namespace ClaireonMontageLengthTestsInternal
{
	// All helpers carry the MontageLenTest discriminator prefix: anonymous
	// namespaces are not isolation under unity batching (linux-build-server-v2).

	// Creates a transient skeleton with a single root bone. A bone-less
	// skeleton makes the async anim-compression path assert on an invalid
	// FBoneContainer (BonePose.h:151) in commandlet runs, which kills the
	// whole test process.
	USkeleton* MontageLenTestCreateSkeleton()
	{
		USkeleton* Skeleton = NewObject<USkeleton>(GetTransientPackage(), NAME_None, RF_Transient);
		if (IsValid(Skeleton))
		{
			FReferenceSkeletonModifier Modifier(Skeleton);
			Modifier.Add(
				FMeshBoneInfo(FName(TEXT("root")), TEXT("root"), INDEX_NONE),
				FTransform::Identity);
		}
		return Skeleton;
	}

	// Creates an in-memory /Game AnimSequence with an exact play length in
	// seconds (30 fps model). Mirrors UAnimSequenceFactory::FactoryCreateNew.
	UAnimSequence* MontageLenTestCreateSequence(USkeleton* Skeleton, const FString& AssetPath, float Seconds)
	{
		UPackage* Package = CreatePackage(*AssetPath);
		if (!IsValid(Package))
		{
			return nullptr;
		}

		const FString AssetName = FPackageName::GetShortName(AssetPath);
		UAnimSequence* Seq = NewObject<UAnimSequence>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
		if (!IsValid(Seq))
		{
			return nullptr;
		}
		Seq->SetSkeleton(Skeleton);

		IAnimationDataController& Ctrl = Seq->GetController();
		Ctrl.OpenBracket(NSLOCTEXT("ClaireonMontageLenTest", "InitSeq", "Init test sequence"), /*bShouldTransact=*/false);
		Ctrl.InitializeModel();
		Ctrl.SetFrameRate(FFrameRate(30, 1), /*bShouldTransact=*/false);
		Ctrl.SetNumberOfFrames(FFrameNumber(FMath::RoundToInt32(Seconds * 30.0f)), /*bShouldTransact=*/false);
		Ctrl.NotifyPopulated();
		Ctrl.CloseBracket(/*bShouldTransact=*/false);

		FAssetRegistryModule::AssetCreated(Seq);
		return Seq;
	}

	// Creates an in-memory /Game montage whose default slot holds one segment
	// per source sequence, appended end to end. Seeds the stored length the
	// same way UAnimMontageFactory does.
	UAnimMontage* MontageLenTestCreateMontage(USkeleton* Skeleton, const FString& AssetPath, const TArray<UAnimSequence*>& SourceSequences)
	{
		UPackage* Package = CreatePackage(*AssetPath);
		if (!IsValid(Package))
		{
			return nullptr;
		}

		const FString AssetName = FPackageName::GetShortName(AssetPath);
		UAnimMontage* Montage = NewObject<UAnimMontage>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
		if (!IsValid(Montage))
		{
			return nullptr;
		}
		Montage->SetSkeleton(Skeleton);

		// UAnimMontage's constructor adds the default slot track.
		float Cursor = 0.0f;
		for (UAnimSequence* Seq : SourceSequences)
		{
			FAnimSegment NewSegment;
			NewSegment.SetAnimReference(Seq, /*bInitialize=*/true);
			NewSegment.StartPos = Cursor;
			Cursor += NewSegment.GetLength();
			Montage->SlotAnimTracks[0].AnimTrack.AnimSegments.Add(NewSegment);
		}
		Montage->SetCompositeLength(Montage->CalculateSequenceLength());

		FAssetRegistryModule::AssetCreated(Montage);
		return Montage;
	}

	// Opens an anim session for the montage and registers the shared tool data
	// entry the montage tools read via RequireSession. Returns the session id
	// (empty on failure).
	FString MontageLenTestOpenSession(UAnimMontage* Montage, const FString& LockPath)
	{
		ClaireonAnimEditToolBase::EnsureDelegateRegistered();

		const FMCPOpenSessionResult Open = FClaireonSessionManager::Get().OpenSession(
			LockPath, ClaireonAnimEditToolBase::AnimSessionToolName);
		if (Open.Result != EOpenSessionResult::Success || Open.SessionId.IsEmpty())
		{
			return FString();
		}

		FAnimEditToolData DataEntry;
		DataEntry.Animation = Montage;
		DataEntry.AssetType = TEXT("AnimMontage");
		ClaireonAnimEditToolBase::ToolData.Add(Open.SessionId, DataEntry);
		return Open.SessionId;
	}

	void MontageLenTestCloseSession(const FString& SessionId)
	{
		if (!SessionId.IsEmpty())
		{
			FClaireonSessionManager::Get().CloseSession(SessionId);
			ClaireonAnimEditToolBase::ToolData.Remove(SessionId);
		}
	}

	// True only when Object's package actually has a .uasset on disk.
	//
	// Everything this suite builds is created with NewObject into a package from
	// CreatePackage() and is never saved; the montage tools it drives
	// (ClaireonAnimTools_Montage) have no save call, and the tests never open an
	// anim session through a saving tool. So these fixtures live in memory only.
	// Deleting an in-memory fixture buys nothing, and every
	// ObjectTools::ForceDeleteObjects call runs a whole-object-graph referencer
	// scan, which is the trigger for the nondeterministic Niagara-serialization
	// crash documented in
	// Docs/llm/todo/claireon-untest-harness-reliability.md item 1.
	//
	// The check is kept rather than dropping the delete outright because
	// /Game/__MCPTests is deliberately NOT gitignored: a stale .uasset left by an
	// older build or a crashed run must still be cleaned so `git status
	// --porcelain -- Content/` stays empty.
	bool MontageLenTestHasFileOnDisk(const UObject* Object)
	{
		const UPackage* Package = IsValid(Object) ? Object->GetPackage() : nullptr;
		if (!IsValid(Package))
		{
			return false;
		}
		FString FileName;
		if (!FPackageName::TryConvertLongPackageNameToFilename(
				Package->GetName(), FileName, FPackageName::GetAssetPackageExtension()))
		{
			return false;
		}
		return FPaths::FileExists(FileName);
	}

	void MontageLenTestCleanup(const TArray<UObject*>& Objects)
	{
		TArray<UObject*> ToDelete;
		for (UObject* Obj : Objects)
		{
			if (IsValid(Obj) && MontageLenTestHasFileOnDisk(Obj))
			{
				ToDelete.Add(Obj);
			}
		}
		if (ToDelete.Num() > 0)
		{
			ClaireonTestAssetDeletion::DeleteObjectsForTest(ToDelete);
		}
	}

	TSharedPtr<FJsonObject> MontageLenTestMakeArgs(const FString& SessionId)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		return Args;
	}
} // namespace ClaireonMontageLengthTestsInternal

using namespace ClaireonMontageLengthTestsInternal;

// ============================================================================
// Test 1: add_segment recalculates SequenceLength to the sum of the segment
// lengths (1s + 2s -> 3s), matching the slot track's own length.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, MontageLength, AddSegmentRecalculatesSequenceLength, UNTEST_TIMEOUTMS(60000))
{
	USkeleton* Skeleton = MontageLenTestCreateSkeleton();
	UNTEST_ASSERT_PTR(Skeleton);

	UAnimSequence* SeqA = MontageLenTestCreateSequence(Skeleton, TEXT("/Game/__MCPTests/WI11_AddSeg_SeqA"), 1.0f);
	UAnimSequence* SeqB = MontageLenTestCreateSequence(Skeleton, TEXT("/Game/__MCPTests/WI11_AddSeg_SeqB"), 2.0f);
	UNTEST_ASSERT_PTR(SeqA);
	UNTEST_ASSERT_PTR(SeqB);
	UNTEST_ASSERT_NEAR(SeqA->GetPlayLength(), 1.0f, 0.001f);
	UNTEST_ASSERT_NEAR(SeqB->GetPlayLength(), 2.0f, 0.001f);

	UAnimMontage* Montage = MontageLenTestCreateMontage(
		Skeleton, TEXT("/Game/__MCPTests/WI11_AddSeg_Montage"), { SeqA });
	UNTEST_ASSERT_PTR(Montage);
	UNTEST_ASSERT_NEAR(Montage->GetPlayLength(), 1.0f, 0.001f);

	const FString SessionId = MontageLenTestOpenSession(Montage, TEXT("/Game/__MCPTests/WI11_AddSeg_Montage"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	TSharedPtr<FJsonObject> Args = MontageLenTestMakeArgs(SessionId);
	Args->SetStringField(TEXT("anim_path"), TEXT("/Game/__MCPTests/WI11_AddSeg_SeqB"));

	ClaireonAnimTool_AddSegment Tool;
	const IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);

	UNTEST_ASSERT_EQ(Montage->SlotAnimTracks[0].AnimTrack.AnimSegments.Num(), 2);
	const float TrackLength = Montage->SlotAnimTracks[0].AnimTrack.GetLength();
	UNTEST_EXPECT_NEAR(TrackLength, 3.0f, 0.001f);

	// The core WI-11 assertion: the stored SequenceLength followed the edit.
	UNTEST_EXPECT_NEAR(Montage->GetPlayLength(), 3.0f, 0.001f);
	UNTEST_EXPECT_NEAR(Montage->GetPlayLength(), TrackLength, 0.001f);

	MontageLenTestCloseSession(SessionId);
	MontageLenTestCleanup({ Montage, SeqA, SeqB });
	co_return;
}

// ============================================================================
// Test 2: retime_segment (only segment made longer via play rate) grows the
// stored SequenceLength, and a notify placed past the OLD length -- rejected
// before the retime -- is accepted afterward.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, MontageLength, RetimeGrowsLengthAndAcceptsNotifyPastOldLength, UNTEST_TIMEOUTMS(60000))
{
	USkeleton* Skeleton = MontageLenTestCreateSkeleton();
	UNTEST_ASSERT_PTR(Skeleton);

	UAnimSequence* Seq = MontageLenTestCreateSequence(Skeleton, TEXT("/Game/__MCPTests/WI11_Retime_Seq"), 1.0f);
	UNTEST_ASSERT_PTR(Seq);

	UAnimMontage* Montage = MontageLenTestCreateMontage(
		Skeleton, TEXT("/Game/__MCPTests/WI11_Retime_Montage"), { Seq });
	UNTEST_ASSERT_PTR(Montage);
	UNTEST_ASSERT_NEAR(Montage->GetPlayLength(), 1.0f, 0.001f);

	// Baseline: 1.5s is out of range while the montage is 1s long.
	FString NotifyError;
	const int32 RejectedIndex = ClaireonAnimHelpers::AddSkeletonNotify(
		Montage, TEXT("WI11_PastEnd"), 1.5f, 0, NotifyError);
	UNTEST_ASSERT_EQ(RejectedIndex, -1);
	UNTEST_ASSERT_FALSE(NotifyError.IsEmpty());

	const FString SessionId = MontageLenTestOpenSession(Montage, TEXT("/Game/__MCPTests/WI11_Retime_Montage"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	// Halve the play rate: segment (and montage) length 1s -> 2s.
	TSharedPtr<FJsonObject> Args = MontageLenTestMakeArgs(SessionId);
	Args->SetNumberField(TEXT("segment_index"), 0);
	Args->SetNumberField(TEXT("new_play_rate"), 0.5);

	ClaireonAnimTool_RetimeSegment Tool;
	const IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);

	// The stored SequenceLength grew with the segment.
	UNTEST_EXPECT_NEAR(Montage->GetPlayLength(), 2.0f, 0.001f);

	// A notify past the OLD length now succeeds against the fresh length.
	NotifyError.Reset();
	const int32 AcceptedIndex = ClaireonAnimHelpers::AddSkeletonNotify(
		Montage, TEXT("WI11_PastEnd"), 1.5f, 0, NotifyError);
	UNTEST_EXPECT_TRUE(NotifyError.IsEmpty());
	UNTEST_ASSERT_GE(AcceptedIndex, 0);
	UNTEST_EXPECT_NEAR(Montage->Notifies[AcceptedIndex].GetTime(), 1.5f, 0.001f);

	MontageLenTestCloseSession(SessionId);
	MontageLenTestCleanup({ Montage, Seq });
	co_return;
}

// ============================================================================
// Test 3: a manual-mode retime that scales one notify past a later notify
// leaves the Notifies array sorted by time afterward (RefreshCacheData ran).
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, MontageLength, RetimeInvertedNotifyOrderIsSorted, UNTEST_TIMEOUTMS(60000))
{
	USkeleton* Skeleton = MontageLenTestCreateSkeleton();
	UNTEST_ASSERT_PTR(Skeleton);

	UAnimSequence* Seq = MontageLenTestCreateSequence(Skeleton, TEXT("/Game/__MCPTests/WI11_Sort_Seq"), 1.0f);
	UNTEST_ASSERT_PTR(Seq);

	// Two 1s segments: [0,1) and [1,2). Montage length 2s.
	UAnimMontage* Montage = MontageLenTestCreateMontage(
		Skeleton, TEXT("/Game/__MCPTests/WI11_Sort_Montage"), { Seq, Seq });
	UNTEST_ASSERT_PTR(Montage);
	UNTEST_ASSERT_NEAR(Montage->GetPlayLength(), 2.0f, 0.001f);

	// Notify NA inside segment 0 (0.9s), notify NB inside segment 1 (1.1s).
	FString NotifyError;
	UNTEST_ASSERT_GE(ClaireonAnimHelpers::AddSkeletonNotify(Montage, TEXT("WI11_NA"), 0.9f, 0, NotifyError), 0);
	UNTEST_ASSERT_GE(ClaireonAnimHelpers::AddSkeletonNotify(Montage, TEXT("WI11_NB"), 1.1f, 0, NotifyError), 0);
	UNTEST_ASSERT_EQ(Montage->Notifies.Num(), 2);

	const FString SessionId = MontageLenTestOpenSession(Montage, TEXT("/Game/__MCPTests/WI11_Sort_Montage"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	// Retime segment 0 to twice its length. Default manual notify mode scales
	// NA 0.9 -> 1.8, past NB's 1.1: array order inverts unless re-sorted.
	TSharedPtr<FJsonObject> Args = MontageLenTestMakeArgs(SessionId);
	Args->SetNumberField(TEXT("segment_index"), 0);
	Args->SetNumberField(TEXT("new_play_rate"), 0.5);

	ClaireonAnimTool_RetimeSegment Tool;
	const IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);

	// Segment 0 grew to 2s; segment 1 collapsed to start at 2s -> total 3s.
	UNTEST_EXPECT_NEAR(Montage->GetPlayLength(), 3.0f, 0.001f);

	// The Notifies array is sorted by time after the mutation.
	UNTEST_ASSERT_EQ(Montage->Notifies.Num(), 2);
	UNTEST_EXPECT_LE(Montage->Notifies[0].GetTime(), Montage->Notifies[1].GetTime());
	UNTEST_EXPECT_TRUE(Montage->Notifies[0].NotifyName == FName(TEXT("WI11_NB")));
	UNTEST_EXPECT_TRUE(Montage->Notifies[1].NotifyName == FName(TEXT("WI11_NA")));
	UNTEST_EXPECT_NEAR(Montage->Notifies[0].GetTime(), 1.1f, 0.001f);
	UNTEST_EXPECT_NEAR(Montage->Notifies[1].GetTime(), 1.8f, 0.001f);

	MontageLenTestCloseSession(SessionId);
	MontageLenTestCleanup({ Montage, Seq });
	co_return;
}

#endif // WITH_UNTESTED
