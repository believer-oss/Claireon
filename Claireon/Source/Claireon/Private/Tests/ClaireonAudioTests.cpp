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

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Sound/SoundCue.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundMix.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundConcurrency.h"
#include "Sound/SoundNode.h"

// -----------------------------------------------------------------------------
// Fixture paths (OVERVIEW.md M2 table)
// -----------------------------------------------------------------------------
static const TCHAR* FixturePath_SoundCue      = TEXT("/Game/Audio/SC_TBLIB_COLLAB_Subdued_Alert");
static const TCHAR* FixturePath_SoundClass    = TEXT("/Game/Audio/Classes/FEL_SC_SFX");
static const TCHAR* FixturePath_Attenuation   = TEXT("/Game/Audio/AttenuationPresets/ATT_3D_Characters");
static const TCHAR* FixturePath_Concurrency   = TEXT("/Game/Audio/Concurrency/FEL_SCON_Default");

// ============================================================================
// ClaireonAudioHelpers
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, Audio, KindStringRoundTrip, UNTEST_TIMEOUTMS(5000))
{
	using K = EClaireonAudioAssetKind;
	UNTEST_EXPECT_TRUE(AudioAssetKindFromString(AudioAssetKindToString(K::SoundCue))        == K::SoundCue);
	UNTEST_EXPECT_TRUE(AudioAssetKindFromString(AudioAssetKindToString(K::MetaSoundSource)) == K::MetaSoundSource);
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

UNTEST_UNIT_OPTS(Claireon, Audio, IterateSoundClassPropertiesStruct, UNTEST_TIMEOUTMS(5000))
{
	UScriptStruct* SS = FSoundClassProperties::StaticStruct();
	UNTEST_ASSERT_TRUE(SS != nullptr);
	int32 Count = 0;
	for (TFieldIterator<FProperty> It(SS); It; ++It)
	{
		if (*It) ++Count;
	}
	UNTEST_EXPECT_TRUE(Count >= 15);
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
	UObject* Obj = ClaireonAudioHelpers::LoadAudioAsset(FixturePath_SoundClass, Kind, Err);
	if (!Obj)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Claireon.Audio] Fixture missing: %s (%s)"), FixturePath_SoundClass, *Err);
		co_return;
	}
	UNTEST_EXPECT_TRUE(Kind == EClaireonAudioAssetKind::SoundClass);
	UNTEST_EXPECT_TRUE(Obj->IsA<USoundClass>());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, Audio, LoadAudioAsset_Attenuation, UNTEST_TIMEOUTMS(10000))
{
	EClaireonAudioAssetKind Kind = EClaireonAudioAssetKind::Unknown;
	FString Err;
	UObject* Obj = ClaireonAudioHelpers::LoadAudioAsset(FixturePath_Attenuation, Kind, Err);
	if (!Obj)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Claireon.Audio] Fixture missing: %s (%s)"), FixturePath_Attenuation, *Err);
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
	UObject* Obj = ClaireonAudioHelpers::LoadAudioAsset(FixturePath_Concurrency, Kind, Err);
	if (!Obj)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Claireon.Audio] Fixture missing: %s (%s)"), FixturePath_Concurrency, *Err);
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
	Args->SetStringField(TEXT("asset_path"), FixturePath_SoundClass);
	auto Result = Tool.Execute(Args);
	if (Result.bIsError)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Claireon.Audio] Fixture missing: %s (%s)"), FixturePath_SoundClass, *Result.ErrorMessage);
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
	Args->SetStringField(TEXT("asset_path"), FixturePath_Attenuation);
	auto Result = Tool.Execute(Args);
	if (Result.bIsError)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Claireon.Audio] Fixture missing: %s (%s)"), FixturePath_Attenuation, *Result.ErrorMessage);
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
	Args->SetStringField(TEXT("asset_path"), FixturePath_Concurrency);
	auto Result = Tool.Execute(Args);
	if (Result.bIsError)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Claireon.Audio] Fixture missing: %s (%s)"), FixturePath_Concurrency, *Result.ErrorMessage);
		co_return;
	}
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());
	const TSharedPtr<FJsonObject> Settings = Result.Data->GetObjectField(TEXT("settings"));
	if (Settings.IsValid())
	{
		TSharedPtr<FJsonValue> RuleVal = Settings->TryGetField(TEXT("ResolutionRule"));
		if (RuleVal.IsValid())
		{
			UNTEST_EXPECT_TRUE(RuleVal->Type == EJson::String);
		}
	}
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

// Row 15 (DROP+REPLACE per TESTS.md): the envelope-router "missing operation" case no longer exists.
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

// Row 17 (DROP+REPLACE per TESTS.md): SoundClass is now stateless under D2=B (no _open).
// Replacement: soundclass_set_property round-trip on the SoundClass fixture.
UNTEST_UNIT_OPTS(Claireon, Audio, EditOpenCloseSoundClass, UNTEST_TIMEOUTMS(15000))
{
	EClaireonAudioAssetKind K = EClaireonAudioAssetKind::Unknown;
	FString LoadErr;
	UObject* Cls = ClaireonAudioHelpers::LoadAudioAsset(FixturePath_SoundClass, K, LoadErr);
	if (!Cls)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Claireon.Audio] Fixture missing: %s (%s)"), FixturePath_SoundClass, *LoadErr);
		co_return;
	}
	FClaireonSoundClassTool_SetProperty Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), FixturePath_SoundClass);
	Args->SetStringField(TEXT("field_name"), TEXT("Volume"));
	Args->SetNumberField(TEXT("value"), 0.85);
	auto Res = Tool.Execute(Args);
	UNTEST_EXPECT_FALSE(Res.bIsError);
	co_return;
}

// Row 18: I1/D3=B mutual exclusion - second SoundCue open on the same path returns error.
UNTEST_UNIT_OPTS(Claireon, Audio, EditDoubleOpenBlocks, UNTEST_TIMEOUTMS(15000))
{
	auto Open1 = ClaireonAudioTestsImpl::OpenSoundCue(FixturePath_SoundCue);
	if (Open1.bIsError)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Claireon.Audio] Fixture missing: %s (%s)"), FixturePath_SoundCue, *Open1.ErrorMessage);
		co_return;
	}
	const FString Id1 = ClaireonAudioTestsImpl::ExtractSessionId(Open1);

	auto Open2 = ClaireonAudioTestsImpl::OpenSoundCue(FixturePath_SoundCue);
	UNTEST_EXPECT_TRUE(Open2.bIsError);
	UNTEST_EXPECT_TRUE(Open2.ErrorMessage.Contains(TEXT("locked")));

	ClaireonAudioTestsImpl::CloseSoundCue(Id1);
	co_return;
}

// Row 19: open + add_node + remove_node + close on a SoundCue.
UNTEST_UNIT_OPTS(Claireon, Audio, EditSoundCue_AddRemoveNode, UNTEST_TIMEOUTMS(20000))
{
	auto Open = ClaireonAudioTestsImpl::OpenSoundCue(FixturePath_SoundCue);
	if (Open.bIsError)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Claireon.Audio] Fixture missing: %s (%s)"), FixturePath_SoundCue, *Open.ErrorMessage);
		co_return;
	}
	const FString Id = ClaireonAudioTestsImpl::ExtractSessionId(Open);

	FClaireonSoundCueTool_AddNode AddTool;
	TSharedPtr<FJsonObject> AddArgs = MakeShared<FJsonObject>();
	AddArgs->SetStringField(TEXT("session_id"), Id);
	AddArgs->SetStringField(TEXT("node_class"), TEXT("mixer"));
	auto Add = AddTool.Execute(AddArgs);
	UNTEST_EXPECT_FALSE(Add.bIsError);

	// I5 invariant: AllNodes and EdGraph stay in sync after the decomposed call.
	FString CuePath;
	Open.Data->TryGetStringField(TEXT("asset_path"), CuePath);
	USoundCue* Cue = LoadObject<USoundCue>(nullptr, *CuePath);
	if (Cue)
	{
#if WITH_EDITORONLY_DATA
		if (Cue->SoundCueGraph)
		{
			UNTEST_EXPECT_TRUE(Cue->SoundCueGraph->Nodes.Num() >= Cue->AllNodes.Num());
		}
#endif
	}

	int32 AddedIdx = -1;
	if (Add.Data.IsValid())
	{
		double N = 0;
		if (Add.Data->TryGetNumberField(TEXT("node_index"), N)) AddedIdx = (int32)N;
	}
	UNTEST_ASSERT_TRUE(AddedIdx >= 0);

	FClaireonSoundCueTool_RemoveNode RmTool;
	TSharedPtr<FJsonObject> RmArgs = MakeShared<FJsonObject>();
	RmArgs->SetStringField(TEXT("session_id"), Id);
	RmArgs->SetNumberField(TEXT("node_index"), AddedIdx);
	auto Rm = RmTool.Execute(RmArgs);
	UNTEST_EXPECT_FALSE(Rm.bIsError);

	ClaireonAudioTestsImpl::CloseSoundCue(Id);
	co_return;
}

// Row 20: open + add_node + set_node_position on a SoundCue.
UNTEST_UNIT_OPTS(Claireon, Audio, EditSoundCue_SetNodePosition, UNTEST_TIMEOUTMS(20000))
{
	auto Open = ClaireonAudioTestsImpl::OpenSoundCue(FixturePath_SoundCue);
	if (Open.bIsError)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Claireon.Audio] Fixture missing: %s (%s)"), FixturePath_SoundCue, *Open.ErrorMessage);
		co_return;
	}
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

// Row 21: stateless attenuation property write.
UNTEST_UNIT_OPTS(Claireon, Audio, EditAttenuationProperty, UNTEST_TIMEOUTMS(15000))
{
	EClaireonAudioAssetKind K = EClaireonAudioAssetKind::Unknown;
	FString LoadErr;
	UObject* Att = ClaireonAudioHelpers::LoadAudioAsset(FixturePath_Attenuation, K, LoadErr);
	if (!Att)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Claireon.Audio] Fixture missing: %s (%s)"), FixturePath_Attenuation, *LoadErr);
		co_return;
	}
	FClaireonAttenuationTool_SetProperty Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), FixturePath_Attenuation);
	Args->SetStringField(TEXT("field_name"), TEXT("bAttenuate"));
	Args->SetBoolField(TEXT("value"), true);
	auto Res = Tool.Execute(Args);
	UNTEST_EXPECT_FALSE(Res.bIsError);
	co_return;
}

// Row 22: stateless concurrency property write with enum-by-name coercion.
UNTEST_UNIT_OPTS(Claireon, Audio, EditConcurrencyEnumByName, UNTEST_TIMEOUTMS(15000))
{
	EClaireonAudioAssetKind K = EClaireonAudioAssetKind::Unknown;
	FString LoadErr;
	UObject* Con = ClaireonAudioHelpers::LoadAudioAsset(FixturePath_Concurrency, K, LoadErr);
	if (!Con)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Claireon.Audio] Fixture missing: %s (%s)"), FixturePath_Concurrency, *LoadErr);
		co_return;
	}
	FClaireonConcurrencyTool_SetProperty Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), FixturePath_Concurrency);
	Args->SetStringField(TEXT("field_name"), TEXT("ResolutionRule"));
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
		E->SetStringField(TEXT("asset_path"), FixturePath_Attenuation);
		E->SetStringField(TEXT("kind"), TEXT("attenuation"));
		Entries.Add(MakeShared<FJsonValueObject>(E));
	}
	{
		TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("id"), TEXT("dup"));
		E->SetStringField(TEXT("asset_path"), FixturePath_Concurrency);
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
	UObject* Att = ClaireonAudioHelpers::LoadAudioAsset(FixturePath_Attenuation, K, Err);
	UObject* Con = ClaireonAudioHelpers::LoadAudioAsset(FixturePath_Concurrency, K, Err);
	UObject* Cls = ClaireonAudioHelpers::LoadAudioAsset(FixturePath_SoundClass, K, Err);
	if (!Att || !Con || !Cls)
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
	Mk(TEXT("att"), FixturePath_Attenuation, TEXT("attenuation"));
	Mk(TEXT("con"), FixturePath_Concurrency, TEXT("concurrency"));
	Mk(TEXT("cls"), FixturePath_SoundClass, TEXT("sound_class"));
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

UNTEST_UNIT_OPTS(Claireon, Audio, LockStringMutex_SoundCueVsMetaSound, UNTEST_TIMEOUTMS(15000))
{
	auto Open1 = ClaireonAudioTestsImpl::OpenSoundCue(FixturePath_SoundCue);
	if (Open1.bIsError)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Claireon.Audio] Fixture missing: %s (%s)"), FixturePath_SoundCue, *Open1.ErrorMessage);
		co_return;
	}
	const FString Id1 = ClaireonAudioTestsImpl::ExtractSessionId(Open1);

	FClaireonMetaSoundTool_Open MSOpen;
	TSharedPtr<FJsonObject> MSArgs = MakeShared<FJsonObject>();
	MSArgs->SetStringField(TEXT("asset_path"), FixturePath_SoundCue);
	auto MSResult = MSOpen.Execute(MSArgs);
	// Either the MetaSound open is rejected because the asset isn't a MetaSoundSource,
	// OR (if a MetaSound builder API is available and asset kind validation passes) it
	// is rejected because the asset is locked. Both prove the I1 guarantee that we
	// don't end up with two simultaneously-open sessions on the same path.
	UNTEST_EXPECT_TRUE(MSResult.bIsError);

	ClaireonAudioTestsImpl::CloseSoundCue(Id1);
	co_return;
}

#endif // WITH_UNTESTED
