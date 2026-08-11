// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/IClaireonTool.h"
#include "Tools/ClaireonAudioHelpers.h"
#include "Tools/ClaireonTool_AudioInspect.h"
#include "Tools/ClaireonSpecApplicator_Audio.h"

// Decomposed audio tool headers (D6=A: tests call these directly, not the bundled umbrella).
#include "Tools/ClaireonSoundCueTool_Open.h"
#include "Tools/ClaireonSoundCueTool_Close.h"
#include "Tools/ClaireonSoundCueTool_Status.h"
#include "Tools/ClaireonSoundCueTool_AddNode.h"
#include "Tools/ClaireonSoundCueTool_RemoveNode.h"
#include "Tools/ClaireonSoundCueTool_SetNodePosition.h"
#include "Tools/ClaireonSoundCueTool_ListNodeTypes.h"
#include "Tools/ClaireonMetaSoundTool_Open.h"
#include "Tools/ClaireonSoundClassTool_SetProperty.h"
#include "Tools/ClaireonAttenuationTool_SetProperty.h"
#include "Tools/ClaireonConcurrencyTool_SetProperty.h"

#include "ClaireonStructReflection.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "EditorAssetLibrary.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Misc/ScopeExit.h"

#include "Sound/SoundCue.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundMix.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundConcurrency.h"
#include "Sound/SoundNode.h"

#include "ClaireonTestAssetDeletion.h"
// -----------------------------------------------------------------------------
// Fixture paths (OVERVIEW.md M2 table)
//
// Source* paths are READ-ONLY shipping content. Every write path in this file
// works on a duplicate under /Game/__MCPTests instead, because the audio
// set_property tools (soundclass / attenuation / concurrency) call
// ClaireonAssetUtils::SaveAsset and therefore rewrite the .uasset on disk.
// Pointing them at shipping content left Content/Audio/Classes/FEL_SC_SFX.uasset
// and Content/Audio/Concurrency/FEL_SCON_Default.uasset dirty in git after every
// full-suite run -- and because these tests PASS, failure triage never saw it.
// The only detector is `git status --porcelain -- Content/` being non-empty
// after a run; keep it empty.
//
// The SoundCue session tests use a duplicate too: soundcue_open calls
// USoundCue::CreateGraph() when the cue has no editor graph yet, and add_node /
// remove_node / set_node_position mutate AllNodes and the EdGraph in place.
// -----------------------------------------------------------------------------
static const TCHAR* SourcePath_SoundCue       = TEXT("/Game/Audio/SC_TBLIB_COLLAB_Subdued_Alert");
static const TCHAR* SourcePath_SoundClass     = TEXT("/Game/Audio/Classes/FEL_SC_SFX");
// NOTE: /Game/Audio/AttenuationPresets/ATT_3D_Characters is an ObjectRedirector
// (it was renamed to _Med_cone), so LoadAudioAsset rejects it with
// "class=ObjectRedirector" and DuplicateAsset faithfully copies the redirector
// stub. Point at the real SoundAttenuation. Every helper below that treats a
// missing fixture as a warn-and-pass hid this for as long as the redirector
// existed -- see the hard assert in AttenuationFixtureIsRealAsset.
static const TCHAR* SourcePath_Attenuation    = TEXT("/Game/Audio/AttenuationPresets/ATT_3D_Characters_Med_cone");
static const TCHAR* SourcePath_Concurrency    = TEXT("/Game/Audio/Concurrency/FEL_SCON_Default");

// Writable copies. Names are stable so a crashed run leaves at most one stale copy.
static const TCHAR* TestPath_SoundCue         = TEXT("/Game/__MCPTests/SC_AudioTest_Cue");
static const TCHAR* TestPath_SoundClass       = TEXT("/Game/__MCPTests/SC_AudioTest_SoundClass");
static const TCHAR* TestPath_Attenuation      = TEXT("/Game/__MCPTests/ATT_AudioTest_Attenuation");
static const TCHAR* TestPath_Concurrency      = TEXT("/Game/__MCPTests/SCON_AudioTest_Concurrency");

namespace ClaireonAudioTestsFixtures
{
	// Duplicate SourcePath -> DestPath, deleting any pre-existing DestPath first.
	//
	// The leading delete is required, not defensive: /Game/__MCPTests is NOT gitignored
	// and PERSISTS between runs, so a run that dies mid-test would otherwise hand a
	// mutated fixture to the next run.
	static bool EnsureFreshDuplicate(const TCHAR* SourcePath, const TCHAR* DestPath)
	{
		if (UEditorAssetLibrary::DoesAssetExist(DestPath))
		{
			ClaireonTestAssetDeletion::DeleteAssetForTest(DestPath);
		}
		return UEditorAssetLibrary::DuplicateAsset(SourcePath, DestPath) != nullptr;
	}

	static void DeleteDuplicate(const TCHAR* Path)
	{
		if (UEditorAssetLibrary::DoesAssetExist(Path))
		{
			ClaireonTestAssetDeletion::DeleteAssetForTest(Path);
		}
	}
}

// ============================================================================
// ClaireonAudioHelpers
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, Audio, KindStringRoundTrip, UNTEST_TIMEOUTMS(5000))
{
	using K = EClaireonAudioAssetKind;
	UNTEST_EXPECT_TRUE(AudioAssetKindFromString(AudioAssetKindToString(K::SoundCue))        == K::SoundCue);
	UNTEST_EXPECT_TRUE(AudioAssetKindFromString(AudioAssetKindToString(K::MetaSoundSource)) == K::MetaSoundSource);
	// MetaSoundPatch must also round-trip; was a missing branch in EClaireonAudioAssetKind.
	UNTEST_EXPECT_TRUE(AudioAssetKindFromString(AudioAssetKindToString(K::MetaSoundPatch))  == K::MetaSoundPatch);
	UNTEST_EXPECT_TRUE(AudioAssetKindToString(K::MetaSoundPatch).Equals(TEXT("metasound_patch")));
	UNTEST_EXPECT_TRUE(AudioAssetKindFromString(AudioAssetKindToString(K::SoundClass))      == K::SoundClass);
	UNTEST_EXPECT_TRUE(AudioAssetKindFromString(AudioAssetKindToString(K::SoundMix))        == K::SoundMix);
	UNTEST_EXPECT_TRUE(AudioAssetKindFromString(AudioAssetKindToString(K::Attenuation))     == K::Attenuation);
	UNTEST_EXPECT_TRUE(AudioAssetKindFromString(AudioAssetKindToString(K::Concurrency))     == K::Concurrency);
	UNTEST_EXPECT_TRUE(AudioAssetKindFromString(TEXT("nonsense_kind")) == K::Unknown);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, Audio, SoundNodeRegistryHasCommonClasses, UNTEST_TIMEOUTMS(5000))
{
	const TMap<FName, UClass*>& Registry = ClaireonAudioHelpers::GetSoundNodeClassRegistry();
	UNTEST_EXPECT_TRUE(Registry.Num() >= 8);
	UNTEST_EXPECT_TRUE(Registry.Contains(FName(TEXT("wave_player"))));
	UNTEST_EXPECT_TRUE(Registry.Contains(FName(TEXT("random"))));
	UNTEST_EXPECT_TRUE(Registry.Contains(FName(TEXT("mixer"))));
	UNTEST_EXPECT_TRUE(Registry.Contains(FName(TEXT("modulator"))));
	UNTEST_EXPECT_TRUE(Registry.Contains(FName(TEXT("looping"))));
	UNTEST_EXPECT_TRUE(Registry.Contains(FName(TEXT("delay"))));
	UNTEST_EXPECT_TRUE(Registry.Contains(FName(TEXT("concatenator"))));
	UNTEST_EXPECT_TRUE(Registry.Contains(FName(TEXT("switch"))));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, Audio, ResolveSoundNodeClassInvalid, UNTEST_TIMEOUTMS(5000))
{
	UClass* Cls = ClaireonAudioHelpers::ResolveSoundNodeClass(FName(TEXT("nonexistent_node")));
	UNTEST_EXPECT_TRUE(Cls == nullptr);
	co_return;
}

// Root cause of the rewrite: this test used to walk TFieldIterator itself, guarded
// by `if (*It)` (a TFieldIterator never yields null while it converts to true, so
// that branch can never be false), and asserted Count >= 15 on a UE engine struct.
// It called no Claireon code at all, so no Claireon regression could fail it. It
// now goes through the reflection entry point the audio property tools depend on
// (ClaireonStructReflection), and pins the one field the soundclass_set_property
// test writes. What is "lost" is nothing: the raw engine-struct field count is now
// asserted via field_count on the Claireon-produced schema.
UNTEST_UNIT_OPTS(Claireon, Audio, IterateSoundClassPropertiesStruct, UNTEST_TIMEOUTMS(10000))
{
	UScriptStruct* SS = FSoundClassProperties::StaticStruct();
	UNTEST_ASSERT_PTR(SS);

	TSharedPtr<FJsonObject> Schema = ClaireonStructReflection::SerializeStructSchema(SS, /*bIncludeDefaults=*/false, /*bIncludeMetadata=*/false);
	UNTEST_ASSERT_TRUE(Schema.IsValid());

	FString StructName;
	UNTEST_EXPECT_TRUE(Schema->TryGetStringField(TEXT("name"), StructName));
	UNTEST_EXPECT_STREQ(StructName, TEXT("SoundClassProperties"));

	double FieldCount = 0.0;
	UNTEST_ASSERT_TRUE(Schema->TryGetNumberField(TEXT("field_count"), FieldCount));
	UNTEST_EXPECT_GE((int32)FieldCount, 15);

	const TArray<TSharedPtr<FJsonValue>>* Fields = nullptr;
	UNTEST_ASSERT_TRUE(Schema->TryGetArrayField(TEXT("fields"), Fields));
	UNTEST_EXPECT_TRUE(Fields->Num() == (int32)FieldCount);

	// Volume is the field Audio.EditOpenCloseSoundClass writes; if Claireon stops
	// surfacing it (or reclassifies it), that write path is unreachable.
	bool bFoundVolume = false;
	FString VolumeKind;
	for (const TSharedPtr<FJsonValue>& FieldVal : *Fields)
	{
		if (!FieldVal.IsValid() || FieldVal->Type != EJson::Object) continue;
		const TSharedPtr<FJsonObject> FieldObj = FieldVal->AsObject();
		if (!FieldObj.IsValid()) continue;
		FString FieldName;
		if (FieldObj->TryGetStringField(TEXT("name"), FieldName) && FieldName == TEXT("Volume"))
		{
			bFoundVolume = true;
			FieldObj->TryGetStringField(TEXT("kind"), VolumeKind);
			break;
		}
	}
	UNTEST_EXPECT_TRUE(bFoundVolume);
	UNTEST_EXPECT_TRUE(VolumeKind == TEXT("Float") || VolumeKind == TEXT("Double"));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, Audio, LoadAudioAssetBadPath, UNTEST_TIMEOUTMS(5000))
{
	EClaireonAudioAssetKind Kind = EClaireonAudioAssetKind::Unknown;
	FString Err;
	UObject* Obj = ClaireonAudioHelpers::LoadAudioAsset(TEXT("/Game/DoesNotExist/_____fake"), Kind, Err);
	UNTEST_EXPECT_TRUE(Obj == nullptr);
	UNTEST_EXPECT_TRUE(!Err.IsEmpty());
	UNTEST_EXPECT_TRUE(Kind == EClaireonAudioAssetKind::Unknown);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, Audio, LoadAudioAsset_SoundClass, UNTEST_TIMEOUTMS(10000))
{
	EClaireonAudioAssetKind Kind = EClaireonAudioAssetKind::Unknown;
	FString Err;
	UObject* Obj = ClaireonAudioHelpers::LoadAudioAsset(SourcePath_SoundClass, Kind, Err);
	if (!IsValid(Obj))
	{
		UE_LOG(LogTemp, Warning, TEXT("[Claireon.Audio] Fixture missing: %s (%s)"), SourcePath_SoundClass, *Err);
		co_return;
	}
	UNTEST_EXPECT_TRUE(Kind == EClaireonAudioAssetKind::SoundClass);
	UNTEST_EXPECT_TRUE(Obj->IsA<USoundClass>());
	co_return;
}

// Guard: the shipping fixtures every Attenuation/Concurrency test depends on must
// resolve to REAL audio assets, not ObjectRedirector stubs. This is the one test in
// the family that hard-fails, because all the others treat an unloadable fixture as
// warn-and-co_return -- which Untest scores as a PASS. ATT_3D_Characters was renamed
// to ATT_3D_Characters_Med_cone and left a redirector behind; every attenuation test
// then "passed" while covering nothing, and EditAttenuationProperty duplicated the
// redirector and failed downstream on a null load. Fail here instead.
UNTEST_UNIT_OPTS(Claireon, Audio, FixturesAreRealAssetsNotRedirectors, UNTEST_TIMEOUTMS(10000))
{
	EClaireonAudioAssetKind AttKind = EClaireonAudioAssetKind::Unknown;
	FString AttErr;
	UObject* AttObj = ClaireonAudioHelpers::LoadAudioAsset(SourcePath_Attenuation, AttKind, AttErr);
	if (!IsValid(AttObj))
	{
		UE_LOG(LogTemp, Error, TEXT("[Claireon.Audio] Attenuation fixture unusable: %s (%s)"),
			SourcePath_Attenuation, *AttErr);
	}
	UNTEST_EXPECT_PTR(AttObj);
	UNTEST_EXPECT_TRUE(AttKind == EClaireonAudioAssetKind::Attenuation);

	EClaireonAudioAssetKind ConKind = EClaireonAudioAssetKind::Unknown;
	FString ConErr;
	UObject* ConObj = ClaireonAudioHelpers::LoadAudioAsset(SourcePath_Concurrency, ConKind, ConErr);
	if (!IsValid(ConObj))
	{
		UE_LOG(LogTemp, Error, TEXT("[Claireon.Audio] Concurrency fixture unusable: %s (%s)"),
			SourcePath_Concurrency, *ConErr);
	}
	UNTEST_EXPECT_PTR(ConObj);
	UNTEST_EXPECT_TRUE(ConKind == EClaireonAudioAssetKind::Concurrency);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, Audio, LoadAudioAsset_Attenuation, UNTEST_TIMEOUTMS(10000))
{
	EClaireonAudioAssetKind Kind = EClaireonAudioAssetKind::Unknown;
	FString Err;
	UObject* Obj = ClaireonAudioHelpers::LoadAudioAsset(SourcePath_Attenuation, Kind, Err);
	if (!IsValid(Obj))
	{
		UE_LOG(LogTemp, Warning, TEXT("[Claireon.Audio] Fixture missing: %s (%s)"), SourcePath_Attenuation, *Err);
		co_return;
	}
	UNTEST_EXPECT_TRUE(Kind == EClaireonAudioAssetKind::Attenuation);
	UNTEST_EXPECT_TRUE(Obj->IsA<USoundAttenuation>());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, Audio, LoadAudioAsset_Concurrency, UNTEST_TIMEOUTMS(10000))
{
	EClaireonAudioAssetKind Kind = EClaireonAudioAssetKind::Unknown;
	FString Err;
	UObject* Obj = ClaireonAudioHelpers::LoadAudioAsset(SourcePath_Concurrency, Kind, Err);
	if (!IsValid(Obj))
	{
		UE_LOG(LogTemp, Warning, TEXT("[Claireon.Audio] Fixture missing: %s (%s)"), SourcePath_Concurrency, *Err);
		co_return;
	}
	UNTEST_EXPECT_TRUE(Kind == EClaireonAudioAssetKind::Concurrency);
	UNTEST_EXPECT_TRUE(Obj->IsA<USoundConcurrency>());
	co_return;
}

// ============================================================================
// audio_inspect (kept; inspect tool was not part of the decomposition)
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, Audio, InspectMissingAssetPath, UNTEST_TIMEOUTMS(5000))
{
	FClaireonTool_AudioInspect Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, Audio, InspectBadAssetPath, UNTEST_TIMEOUTMS(5000))
{
	FClaireonTool_AudioInspect Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TEXT("/Game/DoesNotExist/_____fake"));
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, Audio, InspectSoundClass, UNTEST_TIMEOUTMS(10000))
{
	FClaireonTool_AudioInspect Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), SourcePath_SoundClass);
	auto Result = Tool.Execute(Args);
	if (Result.bIsError)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Claireon.Audio] Fixture missing: %s (%s)"), SourcePath_SoundClass, *Result.ErrorMessage);
		co_return;
	}
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());
	FString KindOut;
	UNTEST_EXPECT_TRUE(Result.Data->TryGetStringField(TEXT("kind"), KindOut));
	UNTEST_EXPECT_TRUE(KindOut == TEXT("sound_class"));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, Audio, InspectAttenuation, UNTEST_TIMEOUTMS(10000))
{
	FClaireonTool_AudioInspect Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), SourcePath_Attenuation);
	auto Result = Tool.Execute(Args);
	if (Result.bIsError)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Claireon.Audio] Fixture missing: %s (%s)"), SourcePath_Attenuation, *Result.ErrorMessage);
		co_return;
	}
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());
	FString KindOut;
	UNTEST_EXPECT_TRUE(Result.Data->TryGetStringField(TEXT("kind"), KindOut));
	UNTEST_EXPECT_TRUE(KindOut == TEXT("attenuation"));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, Audio, InspectConcurrency_EnumIsString, UNTEST_TIMEOUTMS(10000))
{
	FClaireonTool_AudioInspect Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), SourcePath_Concurrency);
	auto Result = Tool.Execute(Args);
	if (Result.bIsError)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Claireon.Audio] Fixture missing: %s (%s)"), SourcePath_Concurrency, *Result.ErrorMessage);
		co_return;
	}
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());
	// Root cause: this used to be `if (Settings.IsValid()) if (RuleVal.IsValid())`,
	// so it caught an enum-as-number regression but silently passed if the settings
	// object or the ResolutionRule field disappeared entirely -- the more likely
	// failure. Assert each level before descending.
	const TSharedPtr<FJsonObject>* SettingsPtr = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetObjectField(TEXT("settings"), SettingsPtr));
	UNTEST_ASSERT_TRUE(SettingsPtr && (*SettingsPtr).IsValid());

	const TSharedPtr<FJsonValue> RuleVal = (*SettingsPtr)->TryGetField(TEXT("ResolutionRule"));
	UNTEST_ASSERT_TRUE(RuleVal.IsValid());
	UNTEST_ASSERT_TRUE(RuleVal->Type == EJson::String);
	// A string is only useful if it is the enumerator name, not a stringified int.
	const FString RuleStr = RuleVal->AsString();
	UNTEST_EXPECT_FALSE(RuleStr.IsEmpty());
	UNTEST_EXPECT_TRUE(RuleStr.Contains(TEXT("Stop")) || RuleStr.Contains(TEXT("Prevent")));
	co_return;
}

// ============================================================================
// Decomposed audio tools - session lifecycle, SoundCue graph ops, reflection ops
// ============================================================================

namespace ClaireonAudioTestsImpl
{
	/** Extract the session_id from the Data payload returned by an open op. */
	static FString ExtractSessionId(const IClaireonTool::FToolResult& R)
	{
		if (R.bIsError || !R.Data.IsValid()) return FString();
		FString Id;
		R.Data->TryGetStringField(TEXT("session_id"), Id);
		return Id;
	}

	static IClaireonTool::FToolResult OpenSoundCue(const FString& Path)
	{
		FClaireonSoundCueTool_Open Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), Path);
		return Tool.Execute(Args);
	}

	static IClaireonTool::FToolResult CloseSoundCue(const FString& Id)
	{
		FClaireonSoundCueTool_Close Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), Id);
		return Tool.Execute(Args);
	}
}

// Row 14: invalid session id -> error envelope from cohort _status.
UNTEST_UNIT_OPTS(Claireon, Audio, EditInvalidSession, UNTEST_TIMEOUTMS(5000))
{
	FClaireonSoundCueTool_Status Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), TEXT("nonexistent-session-id"));
	auto Res = Tool.Execute(Args);
	UNTEST_EXPECT_TRUE(Res.bIsError);
	co_return;
}

// Row 15: the envelope-router "missing operation" case no longer exists.
// Replacement: soundcue_open with empty args -> error envelope (asset_path missing).
UNTEST_UNIT_OPTS(Claireon, Audio, EditMissingOperation, UNTEST_TIMEOUTMS(5000))
{
	FClaireonSoundCueTool_Open Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	auto Res = Tool.Execute(Args);
	UNTEST_EXPECT_TRUE(Res.bIsError);
	co_return;
}

// Row 16: list_node_types -> soundcue_list_node_types.
UNTEST_UNIT_OPTS(Claireon, Audio, EditListNodeTypes, UNTEST_TIMEOUTMS(5000))
{
	FClaireonSoundCueTool_ListNodeTypes Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	auto Res = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Res.bIsError);
	UNTEST_ASSERT_TRUE(Res.Data.IsValid());
	double Count = 0;
	UNTEST_EXPECT_TRUE(Res.Data->TryGetNumberField(TEXT("count"), Count));
	UNTEST_EXPECT_TRUE(Count >= 8);
	co_return;
}

// Row 17: SoundClass is stateless (no _open).
// Replacement: soundclass_set_property round-trip on a COPY of the SoundClass fixture.
//
// Two root causes fixed here:
//  1. soundclass_set_property saves the asset, so writing to the shipping fixture
//     left Content/Audio/Classes/FEL_SC_SFX.uasset modified after every run.
//  2. the test asserted only !bIsError and never read the value back, so a write
//     that silently did nothing still passed.
UNTEST_UNIT_OPTS(Claireon, Audio, EditOpenCloseSoundClass, UNTEST_TIMEOUTMS(30000))
{
	if (!ClaireonAudioTestsFixtures::EnsureFreshDuplicate(SourcePath_SoundClass, TestPath_SoundClass))
	{
		UE_LOG(LogTemp, Warning, TEXT("[Claireon.Audio] Could not duplicate fixture %s -> %s; skipping."),
			SourcePath_SoundClass, TestPath_SoundClass);
		co_return;
	}
	ON_SCOPE_EXIT { ClaireonAudioTestsFixtures::DeleteDuplicate(TestPath_SoundClass); };

	EClaireonAudioAssetKind K = EClaireonAudioAssetKind::Unknown;
	FString LoadErr;
	USoundClass* Cls = Cast<USoundClass>(ClaireonAudioHelpers::LoadAudioAsset(TestPath_SoundClass, K, LoadErr));
	UNTEST_ASSERT_PTR(Cls);

	// 0.85 must differ from whatever the fixture ships with, otherwise the read-back
	// below would pass even if the write never happened. Do NOT replace this with
	// "write back the value you just read" -- that idiom makes the test unfalsifiable.
	const float Target = 0.85f;
	const float Before = Cls->Properties.Volume;
	UNTEST_ASSERT_FALSE(FMath::IsNearlyEqual(Before, Target));

	FClaireonSoundClassTool_SetProperty Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TestPath_SoundClass);
	Args->SetStringField(TEXT("property_path"), TEXT("Volume"));
	Args->SetNumberField(TEXT("value"), Target);
	auto Res = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Res.bIsError);

	// Read the property back off the live object: a success envelope is not proof of a write.
	UNTEST_EXPECT_NEAR(Cls->Properties.Volume, Target, KINDA_SMALL_NUMBER);
	co_return;
}

// Row 18: I1/D3=B mutual exclusion - second SoundCue open on the same path returns error.
// Runs on a COPY: soundcue_open calls USoundCue::CreateGraph() when the cue has no
// editor graph yet, which mutates shipping content as a side effect of "just opening".
UNTEST_UNIT_OPTS(Claireon, Audio, EditDoubleOpenBlocks, UNTEST_TIMEOUTMS(30000))
{
	if (!ClaireonAudioTestsFixtures::EnsureFreshDuplicate(SourcePath_SoundCue, TestPath_SoundCue))
	{
		UE_LOG(LogTemp, Warning, TEXT("[Claireon.Audio] Could not duplicate fixture %s -> %s; skipping."),
			SourcePath_SoundCue, TestPath_SoundCue);
		co_return;
	}
	ON_SCOPE_EXIT { ClaireonAudioTestsFixtures::DeleteDuplicate(TestPath_SoundCue); };

	auto Open1 = ClaireonAudioTestsImpl::OpenSoundCue(TestPath_SoundCue);
	UNTEST_ASSERT_FALSE(Open1.bIsError);
	const FString Id1 = ClaireonAudioTestsImpl::ExtractSessionId(Open1);

	auto Open2 = ClaireonAudioTestsImpl::OpenSoundCue(TestPath_SoundCue);
	UNTEST_EXPECT_TRUE(Open2.bIsError);
	UNTEST_EXPECT_TRUE(Open2.ErrorMessage.Contains(TEXT("locked")));

	ClaireonAudioTestsImpl::CloseSoundCue(Id1);
	co_return;
}

// Row 19: open + add_node + remove_node + close on a COPY of the SoundCue fixture
// (add_node/remove_node mutate AllNodes and the EdGraph of the loaded cue in place).
UNTEST_UNIT_OPTS(Claireon, Audio, EditSoundCue_AddRemoveNode, UNTEST_TIMEOUTMS(30000))
{
	if (!ClaireonAudioTestsFixtures::EnsureFreshDuplicate(SourcePath_SoundCue, TestPath_SoundCue))
	{
		UE_LOG(LogTemp, Warning, TEXT("[Claireon.Audio] Could not duplicate fixture %s -> %s; skipping."),
			SourcePath_SoundCue, TestPath_SoundCue);
		co_return;
	}
	ON_SCOPE_EXIT { ClaireonAudioTestsFixtures::DeleteDuplicate(TestPath_SoundCue); };

	auto Open = ClaireonAudioTestsImpl::OpenSoundCue(TestPath_SoundCue);
	UNTEST_ASSERT_FALSE(Open.bIsError);
	const FString Id = ClaireonAudioTestsImpl::ExtractSessionId(Open);

	// Baseline the two collections BEFORE the add. The old assertion was
	// Nodes.Num() >= AllNodes.Num(), which stays true when add_node appends to
	// AllNodes and forgets the EdGraph -- i.e. it could not fail for the exact
	// desync it was named after.
	FString CuePath;
	UNTEST_ASSERT_TRUE(Open.Data.IsValid() && Open.Data->TryGetStringField(TEXT("asset_path"), CuePath));
	USoundCue* Cue = LoadObject<USoundCue>(nullptr, *CuePath);
	UNTEST_ASSERT_PTR(Cue);
	const int32 BeforeAllNodes = Cue->AllNodes.Num();
#if WITH_EDITORONLY_DATA
	// soundcue_open guarantees a graph exists.
	UNTEST_ASSERT_PTR(Cue->SoundCueGraph.Get());
	const int32 BeforeGraphNodes = Cue->SoundCueGraph->Nodes.Num();
#endif

	FClaireonSoundCueTool_AddNode AddTool;
	TSharedPtr<FJsonObject> AddArgs = MakeShared<FJsonObject>();
	AddArgs->SetStringField(TEXT("session_id"), Id);
	AddArgs->SetStringField(TEXT("node_class"), TEXT("mixer"));
	auto Add = AddTool.Execute(AddArgs);
	UNTEST_ASSERT_FALSE(Add.bIsError);

	int32 AddedIdx = -1;
	if (Add.Data.IsValid())
	{
		double N = 0;
		if (Add.Data->TryGetNumberField(TEXT("node_index"), N)) AddedIdx = (int32)N;
	}
	UNTEST_ASSERT_TRUE(AddedIdx >= 0);

	// invariant: AllNodes and EdGraph stay in sync after the decomposed call.
	// Exactly one sound node and exactly one graph node were added, and the new
	// sound node's graph node is actually reachable from the graph.
	UNTEST_EXPECT_EQ(Cue->AllNodes.Num(), BeforeAllNodes + 1);
#if WITH_EDITORONLY_DATA
	UNTEST_EXPECT_EQ(Cue->SoundCueGraph->Nodes.Num(), BeforeGraphNodes + 1);
	UNTEST_ASSERT_TRUE(Cue->AllNodes.IsValidIndex(AddedIdx));
	USoundNode* AddedNode = Cue->AllNodes[AddedIdx];
	UNTEST_ASSERT_PTR(AddedNode);
	UNTEST_EXPECT_PTR(AddedNode->GraphNode.Get());
	UNTEST_EXPECT_TRUE(Cue->SoundCueGraph->Nodes.Contains(AddedNode->GraphNode));
#endif

	FClaireonSoundCueTool_RemoveNode RmTool;
	TSharedPtr<FJsonObject> RmArgs = MakeShared<FJsonObject>();
	RmArgs->SetStringField(TEXT("session_id"), Id);
	RmArgs->SetNumberField(TEXT("node_index"), AddedIdx);
	auto Rm = RmTool.Execute(RmArgs);
	UNTEST_EXPECT_FALSE(Rm.bIsError);

	ClaireonAudioTestsImpl::CloseSoundCue(Id);
	co_return;
}

// Row 20: open + add_node + set_node_position on a COPY of the SoundCue fixture
// (the node add and the position write both mutate the loaded cue in place).
UNTEST_UNIT_OPTS(Claireon, Audio, EditSoundCue_SetNodePosition, UNTEST_TIMEOUTMS(30000))
{
	if (!ClaireonAudioTestsFixtures::EnsureFreshDuplicate(SourcePath_SoundCue, TestPath_SoundCue))
	{
		UE_LOG(LogTemp, Warning, TEXT("[Claireon.Audio] Could not duplicate fixture %s -> %s; skipping."),
			SourcePath_SoundCue, TestPath_SoundCue);
		co_return;
	}
	ON_SCOPE_EXIT { ClaireonAudioTestsFixtures::DeleteDuplicate(TestPath_SoundCue); };

	auto Open = ClaireonAudioTestsImpl::OpenSoundCue(TestPath_SoundCue);
	UNTEST_ASSERT_FALSE(Open.bIsError);
	const FString Id = ClaireonAudioTestsImpl::ExtractSessionId(Open);

	FClaireonSoundCueTool_AddNode AddTool;
	TSharedPtr<FJsonObject> AddArgs = MakeShared<FJsonObject>();
	AddArgs->SetStringField(TEXT("session_id"), Id);
	AddArgs->SetStringField(TEXT("node_class"), TEXT("delay"));
	auto Add = AddTool.Execute(AddArgs);
	UNTEST_ASSERT_FALSE(Add.bIsError);

	int32 Idx = 0;
	if (Add.Data.IsValid())
	{
		double N = 0;
		Add.Data->TryGetNumberField(TEXT("node_index"), N);
		Idx = (int32)N;
	}

	FClaireonSoundCueTool_SetNodePosition PosTool;
	TSharedPtr<FJsonObject> PosArgs = MakeShared<FJsonObject>();
	PosArgs->SetStringField(TEXT("session_id"), Id);
	PosArgs->SetNumberField(TEXT("node_index"), Idx);
	PosArgs->SetNumberField(TEXT("pos_x"), 1234);
	PosArgs->SetNumberField(TEXT("pos_y"), -567);
	auto Set = PosTool.Execute(PosArgs);
	UNTEST_EXPECT_FALSE(Set.bIsError);

	FClaireonSoundCueTool_RemoveNode RmTool;
	TSharedPtr<FJsonObject> RmArgs = MakeShared<FJsonObject>();
	RmArgs->SetStringField(TEXT("session_id"), Id);
	RmArgs->SetNumberField(TEXT("node_index"), Idx);
	RmTool.Execute(RmArgs);
	ClaireonAudioTestsImpl::CloseSoundCue(Id);
	co_return;
}

// Row 21: stateless attenuation property write, on a COPY.
// attenuation_set_property calls SaveAsset, so this used to rewrite the shipping
// ATT_3D_Characters.uasset on every run.
UNTEST_UNIT_OPTS(Claireon, Audio, EditAttenuationProperty, UNTEST_TIMEOUTMS(30000))
{
	if (!ClaireonAudioTestsFixtures::EnsureFreshDuplicate(SourcePath_Attenuation, TestPath_Attenuation))
	{
		UE_LOG(LogTemp, Warning, TEXT("[Claireon.Audio] Could not duplicate fixture %s -> %s; skipping."),
			SourcePath_Attenuation, TestPath_Attenuation);
		co_return;
	}
	ON_SCOPE_EXIT { ClaireonAudioTestsFixtures::DeleteDuplicate(TestPath_Attenuation); };

	EClaireonAudioAssetKind K = EClaireonAudioAssetKind::Unknown;
	FString LoadErr;
	UObject* Att = ClaireonAudioHelpers::LoadAudioAsset(TestPath_Attenuation, K, LoadErr);
	UNTEST_ASSERT_PTR(Att);

	FClaireonAttenuationTool_SetProperty Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TestPath_Attenuation);
	Args->SetStringField(TEXT("property_path"), TEXT("bAttenuate"));
	Args->SetBoolField(TEXT("value"), true);
	auto Res = Tool.Execute(Args);
	UNTEST_EXPECT_FALSE(Res.bIsError);
	co_return;
}

// Row 22: stateless concurrency property write with enum-by-name coercion, on a COPY.
// concurrency_set_property calls SaveAsset, so this used to leave
// Content/Audio/Concurrency/FEL_SCON_Default.uasset dirty in git after every run.
UNTEST_UNIT_OPTS(Claireon, Audio, EditConcurrencyEnumByName, UNTEST_TIMEOUTMS(30000))
{
	if (!ClaireonAudioTestsFixtures::EnsureFreshDuplicate(SourcePath_Concurrency, TestPath_Concurrency))
	{
		UE_LOG(LogTemp, Warning, TEXT("[Claireon.Audio] Could not duplicate fixture %s -> %s; skipping."),
			SourcePath_Concurrency, TestPath_Concurrency);
		co_return;
	}
	ON_SCOPE_EXIT { ClaireonAudioTestsFixtures::DeleteDuplicate(TestPath_Concurrency); };

	EClaireonAudioAssetKind K = EClaireonAudioAssetKind::Unknown;
	FString LoadErr;
	UObject* Con = ClaireonAudioHelpers::LoadAudioAsset(TestPath_Concurrency, K, LoadErr);
	UNTEST_ASSERT_PTR(Con);

	FClaireonConcurrencyTool_SetProperty Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TestPath_Concurrency);
	Args->SetStringField(TEXT("property_path"), TEXT("ResolutionRule"));
	Args->SetStringField(TEXT("value"), TEXT("StopOldest"));
	auto Res = Tool.Execute(Args);
	UNTEST_EXPECT_FALSE(Res.bIsError);
	co_return;
}

// ============================================================================
// apply_spec (FClaireonSpecApplicator_Audio - umbrella retained per R4 for one release)
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, Audio, ApplySpec_MissingEntries, UNTEST_TIMEOUTMS(5000))
{
	FClaireonSpecApplicator_Audio App;
	TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
	FString Summary, Err;
	UNTEST_EXPECT_FALSE(App.Apply(Spec, Summary, Err));
	UNTEST_EXPECT_TRUE(Err.Contains(TEXT("entries")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, Audio, ApplySpec_DuplicateId_Error, UNTEST_TIMEOUTMS(5000))
{
	FClaireonSpecApplicator_Audio App;
	TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Entries;
	{
		TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("id"), TEXT("dup"));
		E->SetStringField(TEXT("asset_path"), SourcePath_Attenuation);
		E->SetStringField(TEXT("kind"), TEXT("attenuation"));
		Entries.Add(MakeShared<FJsonValueObject>(E));
	}
	{
		TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("id"), TEXT("dup"));
		E->SetStringField(TEXT("asset_path"), SourcePath_Concurrency);
		E->SetStringField(TEXT("kind"), TEXT("concurrency"));
		Entries.Add(MakeShared<FJsonValueObject>(E));
	}
	Spec->SetArrayField(TEXT("entries"), Entries);

	FString Summary, Err;
	UNTEST_EXPECT_FALSE(App.Apply(Spec, Summary, Err));
	UNTEST_EXPECT_TRUE(Err.Contains(TEXT("Duplicate")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, Audio, ApplySpec_UnknownKind_Error, UNTEST_TIMEOUTMS(5000))
{
	FClaireonSpecApplicator_Audio App;
	TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Entries;
	TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
	E->SetStringField(TEXT("id"), TEXT("a"));
	E->SetStringField(TEXT("asset_path"), TEXT("/Game/Foo"));
	E->SetStringField(TEXT("kind"), TEXT("sound_submix"));
	Entries.Add(MakeShared<FJsonValueObject>(E));
	Spec->SetArrayField(TEXT("entries"), Entries);

	FString Summary, Err;
	UNTEST_EXPECT_FALSE(App.Apply(Spec, Summary, Err));
	UNTEST_EXPECT_TRUE(Err.Contains(TEXT("unknown kind")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, Audio, ApplySpec_MissingLinkOnly_Error, UNTEST_TIMEOUTMS(5000))
{
	FClaireonSpecApplicator_Audio App;
	TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Entries;
	TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
	E->SetStringField(TEXT("id"), TEXT("missing"));
	E->SetStringField(TEXT("asset_path"), TEXT("/Game/__MCPTests/DoesNotExist/NEVER"));
	E->SetStringField(TEXT("kind"), TEXT("attenuation"));
	Entries.Add(MakeShared<FJsonValueObject>(E));
	Spec->SetArrayField(TEXT("entries"), Entries);

	FString Summary, Err;
	UNTEST_EXPECT_FALSE(App.Apply(Spec, Summary, Err));
	UNTEST_EXPECT_TRUE(Err.Contains(TEXT("Link-only")) || Err.Contains(TEXT("does not exist")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, Audio, ApplySpec_PureLinkOnly, UNTEST_TIMEOUTMS(15000))
{
	EClaireonAudioAssetKind K = EClaireonAudioAssetKind::Unknown;
	FString Err;
	UObject* Att = ClaireonAudioHelpers::LoadAudioAsset(SourcePath_Attenuation, K, Err);
	UObject* Con = ClaireonAudioHelpers::LoadAudioAsset(SourcePath_Concurrency, K, Err);
	UObject* Cls = ClaireonAudioHelpers::LoadAudioAsset(SourcePath_SoundClass, K, Err);
	if (!IsValid(Att) || !IsValid(Con) || !IsValid(Cls))
	{
		UE_LOG(LogTemp, Warning, TEXT("[Claireon.Audio] One or more link-only fixtures missing; skipping."));
		co_return;
	}

	FClaireonSpecApplicator_Audio App;
	TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Entries;
	auto Mk = [&Entries](const TCHAR* Id, const TCHAR* Path, const TCHAR* Kind)
	{
		TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("id"), Id);
		E->SetStringField(TEXT("asset_path"), Path);
		E->SetStringField(TEXT("kind"), Kind);
		Entries.Add(MakeShared<FJsonValueObject>(E));
	};
	Mk(TEXT("att"), SourcePath_Attenuation, TEXT("attenuation"));
	Mk(TEXT("con"), SourcePath_Concurrency, TEXT("concurrency"));
	Mk(TEXT("cls"), SourcePath_SoundClass, TEXT("sound_class"));
	Spec->SetArrayField(TEXT("entries"), Entries);

	FString Summary, ErrOut;
	UNTEST_EXPECT_TRUE(App.Apply(Spec, Summary, ErrOut));
	UNTEST_EXPECT_TRUE(Summary.Contains(TEXT("3 entries")));
	UNTEST_EXPECT_TRUE(Summary.Contains(TEXT("0 created")));
	co_return;
}

// ============================================================================
// Lock-string mutual-exclusion (I1 / D3=B): SoundCue + MetaSound must contend on
// the same path under the literal "audio_edit" lock string.
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, Audio, LockStringMutex_SoundCueVsMetaSound, UNTEST_TIMEOUTMS(30000))
{
	if (!ClaireonAudioTestsFixtures::EnsureFreshDuplicate(SourcePath_SoundCue, TestPath_SoundCue))
	{
		UE_LOG(LogTemp, Warning, TEXT("[Claireon.Audio] Could not duplicate fixture %s -> %s; skipping."),
			SourcePath_SoundCue, TestPath_SoundCue);
		co_return;
	}
	ON_SCOPE_EXIT { ClaireonAudioTestsFixtures::DeleteDuplicate(TestPath_SoundCue); };

	auto Open1 = ClaireonAudioTestsImpl::OpenSoundCue(TestPath_SoundCue);
	UNTEST_ASSERT_FALSE(Open1.bIsError);
	const FString Id1 = ClaireonAudioTestsImpl::ExtractSessionId(Open1);

	FClaireonMetaSoundTool_Open MSOpen;
	TSharedPtr<FJsonObject> MSArgs = MakeShared<FJsonObject>();
	MSArgs->SetStringField(TEXT("asset_path"), TestPath_SoundCue);
	auto MSResult = MSOpen.Execute(MSArgs);

	// Root cause of the previous vacuity: handing metasound_open a USoundCue path
	// makes it fail its own kind validation (or the builder-API compile gate) long
	// before it reaches the shared "audio_edit" lock, so the old bare
	// EXPECT_TRUE(bIsError) passed for the wrong reason -- and the comment said so.
	// Pin the two messages that path can legitimately produce so this cannot start
	// passing for a third, unrelated reason (bad args, missing asset, ...).
	//
	// Coverage gap, deliberately not papered over: cross-cohort lock contention
	// (a real UMetaSoundSource open contending with a SoundCue session on the same
	// path) needs a MetaSound fixture in the test project, which does not exist yet.
	// Same-cohort contention on the "audio_edit" lock IS covered by
	// Claireon.Audio.EditDoubleOpenBlocks.
	UNTEST_ASSERT_TRUE(MSResult.bIsError);
	const bool bRejectedOnKind = MSResult.ErrorMessage.Contains(TEXT("not a MetaSoundSource or MetaSoundPatch"));
	const bool bBuilderApiAbsent = MSResult.ErrorMessage.Contains(TEXT("MetaSound builder API not available"));
	UNTEST_EXPECT_TRUE(bRejectedOnKind || bBuilderApiAbsent);
	UNTEST_EXPECT_FALSE(MSResult.ErrorMessage.Contains(TEXT("Missing required parameter")));

	ClaireonAudioTestsImpl::CloseSoundCue(Id1);
	co_return;
}

#endif // WITH_UNTESTED
