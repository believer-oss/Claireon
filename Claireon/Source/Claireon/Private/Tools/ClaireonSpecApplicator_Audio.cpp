// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSpecApplicator_Audio.h"

#include "Tools/ClaireonAudioHelpers.h"
#include "Tools/ClaireonPropertyUtils.h"
#include "ClaireonPathResolver.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#include "Sound/SoundCue.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundMix.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundConcurrency.h"

#if __has_include("MetasoundSource.h")
#include "MetasoundSource.h"
#endif
#if __has_include("Metasound.h")
#include "Metasound.h" // UMetaSoundPatch
#endif

namespace
{
	UClass* SpecApplicatorAudio_GetUClassForKind(EClaireonAudioAssetKind Kind)
	{
		switch (Kind)
		{
		case EClaireonAudioAssetKind::SoundCue:        return USoundCue::StaticClass();
#if __has_include("MetasoundSource.h")
		case EClaireonAudioAssetKind::MetaSoundSource: return UMetaSoundSource::StaticClass();
#endif
#if __has_include("Metasound.h")
		case EClaireonAudioAssetKind::MetaSoundPatch:  return UMetaSoundPatch::StaticClass();
#endif
		case EClaireonAudioAssetKind::SoundClass:      return USoundClass::StaticClass();
		case EClaireonAudioAssetKind::SoundMix:        return USoundMix::StaticClass();
		case EClaireonAudioAssetKind::Attenuation:     return USoundAttenuation::StaticClass();
		case EClaireonAudioAssetKind::Concurrency:     return USoundConcurrency::StaticClass();
		default: return nullptr;
		}
	}

	FString SpecApplicatorAudio_JsonValueToString(const TSharedPtr<FJsonValue>& V)
	{
		if (!V.IsValid()) return FString();
		FString S;
		if (V->TryGetString(S)) return S;
		double N;
		bool B;
		if (V->TryGetNumber(N))
		{
			if (FMath::IsFinite(N) && FMath::Floor(N) == N && FMath::Abs(N) < 1e15)
			{
				return FString::Printf(TEXT("%lld"), (int64)N);
			}
			return FString::Printf(TEXT("%g"), N);
		}
		if (V->TryGetBool(B)) return B ? TEXT("true") : TEXT("false");
		return FString();
	}

	/** Table of allowed *_ref tokens and the target UPROPERTY dot-path on the entry's UObject
	 *  (relative to the asset). Table mirrors APPLY_SPEC.md "*_ref -> UPROPERTY map". */
	struct FRefSpec
	{
		FString RefToken;
		FString TargetPath;             // Dot-path on the asset UObject.
		EClaireonAudioAssetKind ExpectedKind;   // The kind the target id must be.
		bool bIsConcurrencySet = false; // ConcurrencySet is a TSet<TObjectPtr<USoundConcurrency>>.
	};

	static TArray<FRefSpec> SpecApplicatorAudio_GetAllowedRefsFor(EClaireonAudioAssetKind Kind)
	{
		TArray<FRefSpec> Out;
		switch (Kind)
		{
		case EClaireonAudioAssetKind::SoundCue:
			Out.Add({ TEXT("attenuation_ref"),  TEXT("AttenuationSettings"), EClaireonAudioAssetKind::Attenuation,  false });
			Out.Add({ TEXT("concurrency_ref"),  TEXT("ConcurrencySet"),      EClaireonAudioAssetKind::Concurrency,  true  });
			Out.Add({ TEXT("sound_class_ref"),  TEXT("SoundClassObject"),    EClaireonAudioAssetKind::SoundClass,   false });
			break;
		case EClaireonAudioAssetKind::MetaSoundSource:
			Out.Add({ TEXT("attenuation_ref"),  TEXT("AttenuationSettings"), EClaireonAudioAssetKind::Attenuation,  false });
			Out.Add({ TEXT("concurrency_ref"),  TEXT("ConcurrencySet"),      EClaireonAudioAssetKind::Concurrency,  true  });
			Out.Add({ TEXT("sound_class_ref"),  TEXT("SoundClassObject"),    EClaireonAudioAssetKind::SoundClass,   false });
			break;
		case EClaireonAudioAssetKind::SoundClass:
			Out.Add({ TEXT("parent_ref"),       TEXT("ParentClass"),         EClaireonAudioAssetKind::SoundClass,   false });
			break;
		default: break;
		}
		return Out;
	}
}

bool FClaireonSpecApplicator_Audio::Apply(const TSharedPtr<FJsonObject>& Spec, FString& OutSummary, FString& OutError)
{
	IdToAsset.Empty();
	AssetsCreatedThisCall.Empty();

	if (!Spec.IsValid())
	{
		OutError = TEXT("Spec JSON is null");
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* EntriesArr = nullptr;
	if (!Spec->TryGetArrayField(TEXT("entries"), EntriesArr))
	{
		OutError = TEXT("Missing 'entries' array");
		return false;
	}

	// Parse + validate entry shape up front so we fail before touching the world.
	TArray<TSharedPtr<FJsonObject>> Entries;
	TSet<FString> SeenIds;
	for (const TSharedPtr<FJsonValue>& EV : *EntriesArr)
	{
		TSharedPtr<FJsonObject> E;
		if (!EV.IsValid() || !(E = EV->AsObject()).IsValid())
		{
			OutError = TEXT("Entries must be JSON objects");
			return false;
		}
		FString Id, Path, KindStr;
		if (!E->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty())
		{
			OutError = TEXT("Entry missing 'id'");
			return false;
		}
		if (SeenIds.Contains(Id))
		{
			OutError = FString::Printf(TEXT("Duplicate id '%s' in spec"), *Id);
			return false;
		}
		SeenIds.Add(Id);
		if (!E->TryGetStringField(TEXT("asset_path"), Path) || Path.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Entry '%s' missing 'asset_path'"), *Id);
			return false;
		}
		if (!E->TryGetStringField(TEXT("kind"), KindStr) || KindStr.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Entry '%s' missing 'kind'"), *Id);
			return false;
		}
		const EClaireonAudioAssetKind Kind = AudioAssetKindFromString(FStringView(KindStr));
		if (Kind == EClaireonAudioAssetKind::Unknown)
		{
			OutError = FString::Printf(TEXT("Entry '%s' has unknown kind '%s'"), *Id, *KindStr);
			return false;
		}
		Entries.Add(E);
	}

	// Outer transaction covers Pass 1 + Pass 2 so the whole spec can be undone as a unit.
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Apply Audio Spec")));

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	IAssetTools& AssetTools = AssetToolsModule.Get();

	// Pass 1: materialize.
	for (const TSharedPtr<FJsonObject>& E : Entries)
	{
		const FString Id = E->GetStringField(TEXT("id"));
		const FString Path = E->GetStringField(TEXT("asset_path"));
		const FString KindStr = E->GetStringField(TEXT("kind"));
		const EClaireonAudioAssetKind Kind = AudioAssetKindFromString(FStringView(KindStr));
		const bool bHasDefine = E->HasField(TEXT("define"));

		const FString Canon = ClaireonPathResolver::Resolve(Path).bSuccess
			? ClaireonPathResolver::Resolve(Path).ResolvedPath.Path
			: Path;

		UObject* Existing = LoadObject<UObject>(nullptr, *Canon);
		if (!Existing)
		{
			if (!bHasDefine)
			{
				OutError = FString::Printf(TEXT("Link-only entry '%s' at path '%s' does not exist."), *Id, *Canon);
				Transaction.Cancel();
				return false;
			}
			// Create.
			UClass* Cls = SpecApplicatorAudio_GetUClassForKind(Kind);
			if (!Cls)
			{
				OutError = FString::Printf(TEXT("No UClass for kind '%s'"), *KindStr);
				Transaction.Cancel();
				return false;
			}
			const FString PackagePath = FPackageName::GetLongPackagePath(Canon);
			const FString ObjectName  = FPackageName::GetShortName(Canon);
			UObject* New = AssetTools.CreateAsset(ObjectName, PackagePath, Cls, /*Factory=*/nullptr);
			if (!New)
			{
				OutError = FString::Printf(TEXT("CreateAsset failed for id='%s' path='%s'"), *Id, *Canon);
				Transaction.Cancel();
				return false;
			}
			AssetsCreatedThisCall.Add(New);
			IdToAsset.Add(Id, New);
		}
		else
		{
			// Verify class matches kind.
			UClass* Expected = SpecApplicatorAudio_GetUClassForKind(Kind);
			if (Expected && !Existing->IsA(Expected))
			{
				OutError = FString::Printf(TEXT("Existing asset at '%s' is a %s, expected %s for id='%s'"),
					*Canon, *Existing->GetClass()->GetName(), *Expected->GetName(), *Id);
				Transaction.Cancel();
				// No assets created yet in this branch, so nothing to roll back.
				return false;
			}
			IdToAsset.Add(Id, Existing);
		}
	}

	// Pass 2: wire.
	for (const TSharedPtr<FJsonObject>& E : Entries)
	{
		if (!E->HasField(TEXT("define"))) continue;
		const FString Id = E->GetStringField(TEXT("id"));
		const FString KindStr = E->GetStringField(TEXT("kind"));
		const EClaireonAudioAssetKind Kind = AudioAssetKindFromString(FStringView(KindStr));
		TSharedPtr<FJsonObject> Define = E->GetObjectField(TEXT("define"));
		UObject* Asset = IdToAsset[Id].Get();
		if (!Asset) continue;

		Asset->Modify();

		const TArray<FRefSpec> AllowedRefs = SpecApplicatorAudio_GetAllowedRefsFor(Kind);
		auto FindRefSpec = [&AllowedRefs](const FString& Token) -> const FRefSpec*
		{
			for (const FRefSpec& R : AllowedRefs) if (R.RefToken == Token) return &R;
			return nullptr;
		};

		// Walk define fields.
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Define->Values)
		{
			const FString& Key = Pair.Key;
			const TSharedPtr<FJsonValue>& Val = Pair.Value;

			if (Key.EndsWith(TEXT("_ref")))
			{
				const FRefSpec* RS = FindRefSpec(Key);
				if (!RS)
				{
					OutError = FString::Printf(TEXT("Entry '%s' uses unknown *_ref token '%s' for kind '%s'"),
						*Id, *Key, *KindStr);
					Transaction.Cancel();
					for (const TWeakObjectPtr<UObject>& Weak : AssetsCreatedThisCall)
					{
						if (UObject* Obj = Weak.Get()) ObjectTools::DeleteSingleObject(Obj, /*bPerformReferenceCheck=*/false);
					}
					return false;
				}
				FString TargetId;
				if (!Val.IsValid() || !Val->TryGetString(TargetId))
				{
					OutError = FString::Printf(TEXT("Entry '%s': '%s' must be a string id"), *Id, *Key);
					Transaction.Cancel();
					for (const TWeakObjectPtr<UObject>& Weak : AssetsCreatedThisCall)
					{
						if (UObject* Obj = Weak.Get()) ObjectTools::DeleteSingleObject(Obj, /*bPerformReferenceCheck=*/false);
					}
					return false;
				}
				TWeakObjectPtr<UObject>* TargetWeak = IdToAsset.Find(TargetId);
				UObject* Target = TargetWeak ? TargetWeak->Get() : nullptr;
				if (!Target)
				{
					OutError = FString::Printf(TEXT("Entry '%s': unresolved ref '%s' -> '%s'"), *Id, *Key, *TargetId);
					Transaction.Cancel();
					for (const TWeakObjectPtr<UObject>& Weak : AssetsCreatedThisCall)
					{
						if (UObject* Obj = Weak.Get()) ObjectTools::DeleteSingleObject(Obj, /*bPerformReferenceCheck=*/false);
					}
					return false;
				}
				// Kind-match check.
				UClass* ExpectedCls = SpecApplicatorAudio_GetUClassForKind(RS->ExpectedKind);
				if (ExpectedCls && !Target->IsA(ExpectedCls))
				{
					OutError = FString::Printf(TEXT("Kind mismatch on '%s' in entry '%s': target '%s' is a %s, expected %s"),
						*Key, *Id, *TargetId, *Target->GetClass()->GetName(), *ExpectedCls->GetName());
					Transaction.Cancel();
					for (const TWeakObjectPtr<UObject>& Weak : AssetsCreatedThisCall)
					{
						if (UObject* Obj = Weak.Get()) ObjectTools::DeleteSingleObject(Obj, /*bPerformReferenceCheck=*/false);
					}
					return false;
				}

				if (RS->bIsConcurrencySet)
				{
					// Append to TSet<TObjectPtr<USoundConcurrency>> directly.
					if (USoundBase* Base = Cast<USoundBase>(Asset))
					{
						if (USoundConcurrency* C = Cast<USoundConcurrency>(Target))
						{
							Base->ConcurrencySet.Add(C);
						}
					}
				}
				else
				{
					FString Err;
					// Use reflection to set the object property on the asset.
					FProperty* Prop = Asset->GetClass()->FindPropertyByName(*RS->TargetPath);
					if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
					{
						void* ValuePtr = ObjProp->ContainerPtrToValuePtr<void>(Asset);
						Asset->PreEditChange(ObjProp);
						ObjProp->SetObjectPropertyValue(ValuePtr, Target);
						FPropertyChangedEvent Event(ObjProp);
						Asset->PostEditChangeProperty(Event);
					}
					else
					{
						OutError = FString::Printf(TEXT("Entry '%s': property '%s' is not an object property"),
							*Id, *RS->TargetPath);
						Transaction.Cancel();
						for (const TWeakObjectPtr<UObject>& Weak : AssetsCreatedThisCall)
						{
							if (UObject* Obj = Weak.Get()) ObjectTools::DeleteSingleObject(Obj, /*bPerformReferenceCheck=*/false);
						}
						return false;
					}
				}
				continue;
			}

			// Plain field: route SoundClass through Properties struct (D6).
			FString Err;
			FString PropPath = Key;
			if (Kind == EClaireonAudioAssetKind::SoundClass)
			{
				PropPath = FString::Printf(TEXT("Properties.%s"), *Key);
			}
			else if (Kind == EClaireonAudioAssetKind::Attenuation)
			{
				PropPath = FString::Printf(TEXT("Attenuation.%s"), *Key);
			}
			else if (Kind == EClaireonAudioAssetKind::Concurrency)
			{
				PropPath = FString::Printf(TEXT("Concurrency.%s"), *Key);
			}
			const FString ValueStr = SpecApplicatorAudio_JsonValueToString(Val);
			if (!ClaireonPropertyUtils::WritePropertyByPath(Asset, PropPath, ValueStr, Err))
			{
				OutError = FString::Printf(TEXT("Entry '%s': WritePropertyByPath failed for '%s': %s"),
					*Id, *PropPath, *Err);
				Transaction.Cancel();
				for (const TWeakObjectPtr<UObject>& Weak : AssetsCreatedThisCall)
				{
					if (UObject* Obj = Weak.Get()) ObjectTools::DeleteSingleObject(Obj, /*bPerformReferenceCheck=*/false);
				}
				return false;
			}
		}
	}

	OutSummary = FString::Printf(TEXT("apply_spec ok: %d entries (%d created)"),
		Entries.Num(), AssetsCreatedThisCall.Num());
	return true;
}

// -----------------------------------------------------------------------------
// Per-cohort applicators (decomposed apply_spec). See AUDIO_DECOMPOSE_APPLY_SPEC.md.
// Each applies a single-asset spec of the form:
//   { "kind": "<Cohort>", "asset_path": "/Game/...", "properties": { ... } }
// Cross-references via *_ref tokens (when supported by cohort) are resolved as
// path strings to existing assets - per-cohort apply_spec does NOT create the
// referenced assets (use the entries-based legacy Apply for create-then-wire).
// -----------------------------------------------------------------------------

namespace
{
	bool SpecApplicatorAudio_LoadOrCreateForCohort(const FString& AssetPath, EClaireonAudioAssetKind Cohort,
		bool bAllowCreate, UObject*& OutAsset, bool& bOutCreated, FString& OutError)
	{
		bOutCreated = false;
		const ClaireonPathResolver::FResolveResult Resolved = ClaireonPathResolver::Resolve(AssetPath);
		const FString Canon = Resolved.bSuccess ? Resolved.ResolvedPath.Path : AssetPath;

		UClass* Cls = SpecApplicatorAudio_GetUClassForKind(Cohort);
		if (!Cls)
		{
			OutError = FString::Printf(TEXT("No UClass for kind"));
			return false;
		}

		UObject* Existing = LoadObject<UObject>(nullptr, *Canon);
		if (Existing)
		{
			if (!Existing->IsA(Cls))
			{
				OutError = FString::Printf(TEXT("Existing asset at '%s' is a %s, expected %s"),
					*Canon, *Existing->GetClass()->GetName(), *Cls->GetName());
				return false;
			}
			OutAsset = Existing;
			return true;
		}

		if (!bAllowCreate)
		{
			OutError = FString::Printf(TEXT("Asset not found: %s"), *Canon);
			return false;
		}

		FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
		IAssetTools& AssetTools = AssetToolsModule.Get();
		const FString PackagePath = FPackageName::GetLongPackagePath(Canon);
		const FString ObjectName = FPackageName::GetShortName(Canon);
		UObject* New = AssetTools.CreateAsset(ObjectName, PackagePath, Cls, /*Factory=*/nullptr);
		if (!New)
		{
			OutError = FString::Printf(TEXT("CreateAsset failed for '%s'"), *Canon);
			return false;
		}
		OutAsset = New;
		bOutCreated = true;
		return true;
	}
}

namespace
{
	// Generic single-cohort apply_spec impl shared by ApplyAttenuationSpec / ApplyConcurrencySpec /
	// (other reflection-only cohorts to follow). The bundled apply_spec keeps the entries-based
	// shape; per-cohort apply_spec uses a single-asset spec form. PropertyPrefix is the dot-prefix
	// that gets prepended to flat keys (e.g. "Attenuation." or "Concurrency.").
	bool SpecApplicatorAudio_ApplyReflectionCohortSpec(
		const TSharedPtr<FJsonObject>& Spec,
		EClaireonAudioAssetKind ExpectedKind,
		const FString& KindLabel,
		const FString& PropertyPrefix,
		const FString& TransactionLabel,
		const FString& SummaryLabel,
		TMap<FString, TWeakObjectPtr<UObject>>& IdToAsset,
		TArray<TWeakObjectPtr<UObject>>& AssetsCreatedThisCall,
		FString& OutSummary,
		FString& OutError)
	{
		IdToAsset.Empty();
		AssetsCreatedThisCall.Empty();

		if (!Spec.IsValid())
		{
			OutError = TEXT("Spec JSON is null");
			return false;
		}

		FString KindStr;
		if (!Spec->TryGetStringField(TEXT("kind"), KindStr) || KindStr.IsEmpty())
		{
			OutError = TEXT("Spec missing 'kind'");
			return false;
		}
		const EClaireonAudioAssetKind Kind = AudioAssetKindFromString(FStringView(KindStr));
		if (Kind != ExpectedKind)
		{
			OutError = FString::Printf(TEXT("%s: kind '%s' does not match expected"),
				*SummaryLabel, *KindStr);
			return false;
		}

		FString AssetPath;
		if (!Spec->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		{
			OutError = TEXT("Spec missing 'asset_path'");
			return false;
		}

		FScopedTransaction Transaction(FText::FromString(TransactionLabel));

		UObject* Asset = nullptr;
		bool bCreated = false;
		if (!SpecApplicatorAudio_LoadOrCreateForCohort(AssetPath, Kind, /*bAllowCreate=*/true, Asset, bCreated, OutError))
		{
			Transaction.Cancel();
			return false;
		}
		if (bCreated)
		{
			AssetsCreatedThisCall.Add(Asset);
		}

		Asset->Modify();

		const TSharedPtr<FJsonObject>* PropsObj = nullptr;
		if (Spec->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj && PropsObj->IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*PropsObj)->Values)
			{
				const FString& Key = Pair.Key;
				const FString ValueStr = SpecApplicatorAudio_JsonValueToString(Pair.Value);
				const FString PropPath = Key.StartsWith(PropertyPrefix)
					? Key
					: PropertyPrefix + Key;
				FString Err;
				if (!ClaireonPropertyUtils::WritePropertyByPath(Asset, PropPath, ValueStr, Err))
				{
					OutError = FString::Printf(TEXT("WritePropertyByPath failed for '%s': %s"), *PropPath, *Err);
					Transaction.Cancel();
					for (const TWeakObjectPtr<UObject>& Weak : AssetsCreatedThisCall)
					{
						if (UObject* Obj = Weak.Get()) ObjectTools::DeleteSingleObject(Obj, /*bPerformReferenceCheck=*/false);
					}
					return false;
				}
			}
		}

		Asset->MarkPackageDirty();
		OutSummary = FString::Printf(TEXT("%s ok: %s%s"),
			*SummaryLabel, *Asset->GetPathName(), bCreated ? TEXT(" (created)") : TEXT(""));
		return true;
	}
}

bool FClaireonSpecApplicator_Audio::ApplyAttenuationSpec(const TSharedPtr<FJsonObject>& Spec, FString& OutSummary, FString& OutError)
{
	return SpecApplicatorAudio_ApplyReflectionCohortSpec(Spec, EClaireonAudioAssetKind::Attenuation, TEXT("Attenuation"),
		TEXT("Attenuation."), TEXT("[Claireon] Apply Attenuation Spec"),
		TEXT("attenuation_apply_spec"), IdToAsset, AssetsCreatedThisCall, OutSummary, OutError);
}

bool FClaireonSpecApplicator_Audio::ApplyConcurrencySpec(const TSharedPtr<FJsonObject>& Spec, FString& OutSummary, FString& OutError)
{
	return SpecApplicatorAudio_ApplyReflectionCohortSpec(Spec, EClaireonAudioAssetKind::Concurrency, TEXT("Concurrency"),
		TEXT("Concurrency."), TEXT("[Claireon] Apply Concurrency Spec"),
		TEXT("concurrency_apply_spec"), IdToAsset, AssetsCreatedThisCall, OutSummary, OutError);
}

bool FClaireonSpecApplicator_Audio::ApplySoundClassSpec(const TSharedPtr<FJsonObject>& Spec, FString& OutSummary, FString& OutError)
{
	if (!SpecApplicatorAudio_ApplyReflectionCohortSpec(Spec, EClaireonAudioAssetKind::SoundClass, TEXT("SoundClass"),
		TEXT("Properties."), TEXT("[Claireon] Apply SoundClass Spec"),
		TEXT("soundclass_apply_spec"), IdToAsset, AssetsCreatedThisCall, OutSummary, OutError))
	{
		return false;
	}

	// Optional: child_classes [paths...] -> AddUnique into ChildClasses.
	const TArray<TSharedPtr<FJsonValue>>* ChildArr = nullptr;
	if (Spec->TryGetArrayField(TEXT("child_classes"), ChildArr) && ChildArr)
	{
		FString AssetPath;
		Spec->TryGetStringField(TEXT("asset_path"), AssetPath);
		const ClaireonPathResolver::FResolveResult Resolved = ClaireonPathResolver::Resolve(AssetPath);
		const FString Canon = Resolved.bSuccess ? Resolved.ResolvedPath.Path : AssetPath;
		USoundClass* Parent = LoadObject<USoundClass>(nullptr, *Canon);
		if (Parent)
		{
			Parent->Modify();
			for (const TSharedPtr<FJsonValue>& V : *ChildArr)
			{
				FString ChildPath;
				if (!V.IsValid() || !V->TryGetString(ChildPath) || ChildPath.IsEmpty()) continue;
				const ClaireonPathResolver::FResolveResult ChildResolved = ClaireonPathResolver::Resolve(ChildPath);
				if (USoundClass* Child = ChildResolved.bSuccess
					? LoadObject<USoundClass>(nullptr, *ChildResolved.ResolvedPath.Path)
					: nullptr)
				{
					Parent->ChildClasses.AddUnique(Child);
				}
			}
			Parent->MarkPackageDirty();
		}
	}
	return true;
}

bool FClaireonSpecApplicator_Audio::ApplySoundCueSpec(const TSharedPtr<FJsonObject>& Spec, FString& OutSummary, FString& OutError)
{
	IdToAsset.Empty();
	AssetsCreatedThisCall.Empty();

	if (!Spec.IsValid())
	{
		OutError = TEXT("Spec JSON is null");
		return false;
	}
	FString KindStr;
	if (!Spec->TryGetStringField(TEXT("kind"), KindStr) || KindStr.IsEmpty())
	{
		OutError = TEXT("Spec missing 'kind'");
		return false;
	}
	const EClaireonAudioAssetKind Kind = AudioAssetKindFromString(FStringView(KindStr));
	if (Kind != EClaireonAudioAssetKind::SoundCue)
	{
		OutError = FString::Printf(TEXT("ApplySoundCueSpec: kind '%s' is not SoundCue"), *KindStr);
		return false;
	}
	FString AssetPath;
	if (!Spec->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("Spec missing 'asset_path'");
		return false;
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Apply SoundCue Spec")));
	UObject* Asset = nullptr;
	bool bCreated = false;
	if (!SpecApplicatorAudio_LoadOrCreateForCohort(AssetPath, Kind, /*bAllowCreate=*/true, Asset, bCreated, OutError))
	{
		Transaction.Cancel();
		return false;
	}
	if (bCreated) AssetsCreatedThisCall.Add(Asset);
	Asset->Modify();
	Asset->MarkPackageDirty();

	// Full nodes/edges rewrite via per-cohort apply_spec is deferred.
	// The bundled entries-based Apply remains the path for full graph specs (R4: legacy umbrella retained).
	// Per-cohort apply_spec here creates/locates the asset only, leaving graph mutation to dedicated tools.
	OutSummary = FString::Printf(TEXT("soundcue_apply_spec ok (asset only): %s%s"),
		*Asset->GetPathName(), bCreated ? TEXT(" (created)") : TEXT(""));
	return true;
}

// MetaSound write-side apply_spec is conditionally compiled in if the engine ships the builder
// API headers. Without them we fall back to the asset-only stub (load/create then save).
#if __has_include("MetasoundDocumentBuilderRegistry.h") && __has_include("MetasoundBuilderSubsystem.h") && __has_include("MetasoundFrontendLiteral.h")
#define CLAIREON_HAS_METASOUND_APPLY_API 1
#else
#define CLAIREON_HAS_METASOUND_APPLY_API 0
#endif

#if CLAIREON_HAS_METASOUND_APPLY_API
#include "MetasoundDocumentBuilderRegistry.h"
#include "MetasoundBuilderSubsystem.h"
#include "MetasoundFrontendLiteral.h"

namespace
{
	// File-local discriminator (MSApplySpec_) avoids unity-batch name collisions.
	bool MSApplySpec_BuildLiteralFromJson(FName DataType, const TSharedPtr<FJsonValue>& Value, FMetasoundFrontendLiteral& Out)
	{
		const FString TypeStr = DataType.ToString();
		if (TypeStr == TEXT("Bool"))
		{
			bool B = false;
			if (Value.IsValid() && Value->TryGetBool(B)) { Out.Set(B); return true; }
			return false;
		}
		if (TypeStr == TEXT("Float") || TypeStr == TEXT("Time"))
		{
			double N = 0;
			if (Value.IsValid() && Value->TryGetNumber(N)) { Out.Set(static_cast<float>(N)); return true; }
			return false;
		}
		if (TypeStr == TEXT("Int32"))
		{
			double N = 0;
			if (Value.IsValid() && Value->TryGetNumber(N)) { Out.Set(static_cast<int32>(N)); return true; }
			return false;
		}
		if (TypeStr == TEXT("String"))
		{
			FString S;
			if (Value.IsValid() && Value->TryGetString(S)) { Out.Set(S); return true; }
			return false;
		}
		if (Value.IsValid())
		{
			bool B; double N; FString S;
			if (Value->TryGetBool(B)) { Out.Set(B); return true; }
			if (Value->TryGetNumber(N)) { Out.Set(static_cast<float>(N)); return true; }
			if (Value->TryGetString(S)) { Out.Set(S); return true; }
		}
		return false;
	}
}
#endif

bool FClaireonSpecApplicator_Audio::ApplyMetaSoundSpec(const TSharedPtr<FJsonObject>& Spec, FString& OutSummary, FString& OutError)
{
	IdToAsset.Empty();
	AssetsCreatedThisCall.Empty();

	if (!Spec.IsValid())
	{
		OutError = TEXT("Spec JSON is null");
		return false;
	}
	FString KindStr;
	if (!Spec->TryGetStringField(TEXT("kind"), KindStr) || KindStr.IsEmpty())
	{
		OutError = TEXT("Spec missing 'kind'");
		return false;
	}
	const EClaireonAudioAssetKind Kind = AudioAssetKindFromString(FStringView(KindStr));
	// Accept both MetaSoundSource and MetaSoundPatch -- they share IMetaSoundDocumentInterface.
	if (Kind != EClaireonAudioAssetKind::MetaSoundSource && Kind != EClaireonAudioAssetKind::MetaSoundPatch)
	{
		OutError = FString::Printf(TEXT("ApplyMetaSoundSpec: kind '%s' is not MetaSoundSource or MetaSoundPatch"), *KindStr);
		return false;
	}
	FString AssetPath;
	if (!Spec->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("Spec missing 'asset_path'");
		return false;
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Apply MetaSound Spec")));
	UObject* Asset = nullptr;
	bool bCreated = false;
	if (!SpecApplicatorAudio_LoadOrCreateForCohort(AssetPath, Kind, /*bAllowCreate=*/true, Asset, bCreated, OutError))
	{
		Transaction.Cancel();
		return false;
	}
	if (bCreated) AssetsCreatedThisCall.Add(Asset);
	Asset->Modify();
	Asset->MarkPackageDirty();

#if !CLAIREON_HAS_METASOUND_APPLY_API
	OutSummary = FString::Printf(TEXT("metasound_apply_spec ok (asset only -- builder API unavailable): %s%s"),
		*Asset->GetPathName(), bCreated ? TEXT(" (created)") : TEXT(""));
	return true;
#else
	using namespace Metasound::Engine;
	FDocumentBuilderRegistry& Registry = FDocumentBuilderRegistry::GetChecked();
	UMetaSoundBuilderBase& BuilderObj = Registry.FindOrBeginBuilding<UMetaSoundBuilderBase>(*Asset);

	auto RollbackAll = [&]()
	{
		Transaction.Cancel();
		for (const TWeakObjectPtr<UObject>& Weak : AssetsCreatedThisCall)
		{
			if (UObject* Obj = Weak.Get()) ObjectTools::DeleteSingleObject(Obj, /*bPerformReferenceCheck=*/false);
		}
	};

	int32 InputsAdded = 0, OutputsAdded = 0, NodesAdded = 0, InterfacesAdded = 0, EdgesAdded = 0, DefaultsSet = 0;

	// Pass 1a: interfaces
	const TArray<TSharedPtr<FJsonValue>>* IFArr = nullptr;
	if (Spec->TryGetArrayField(TEXT("interfaces"), IFArr) && IFArr)
	{
		for (const TSharedPtr<FJsonValue>& V : *IFArr)
		{
			FString IName;
			if (!V.IsValid() || !V->TryGetString(IName) || IName.IsEmpty()) continue;
			EMetaSoundBuilderResult R = EMetaSoundBuilderResult::Failed;
			BuilderObj.AddInterface(FName(*IName), R);
			if (R != EMetaSoundBuilderResult::Succeeded)
			{
				OutError = FString::Printf(TEXT("AddInterface failed for '%s' (use claireon.metasound_list_available_interfaces to discover valid names)"), *IName);
				RollbackAll();
				return false;
			}
			++InterfacesAdded;
		}
	}

	// Pass 1b: inputs
	const TArray<TSharedPtr<FJsonValue>>* InsArr = nullptr;
	if (Spec->TryGetArrayField(TEXT("inputs"), InsArr) && InsArr)
	{
		for (const TSharedPtr<FJsonValue>& V : *InsArr)
		{
			if (!V.IsValid()) continue;
			const TSharedPtr<FJsonObject>* InObj = nullptr;
			if (!V->TryGetObject(InObj) || !InObj || !InObj->IsValid()) continue;
			FString Name, Type;
			if (!(*InObj)->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty()) { OutError = TEXT("input missing 'name'"); RollbackAll(); return false; }
			if (!(*InObj)->TryGetStringField(TEXT("type"), Type) || Type.IsEmpty()) { OutError = TEXT("input missing 'type'"); RollbackAll(); return false; }
			FMetasoundFrontendLiteral Default;
			MSApplySpec_BuildLiteralFromJson(FName(*Type), (*InObj)->TryGetField(TEXT("default")), Default);
			EMetaSoundBuilderResult R = EMetaSoundBuilderResult::Failed;
			BuilderObj.AddGraphInputNode(FName(*Name), FName(*Type), Default, R, /*bIsConstructorInput=*/false);
			if (R != EMetaSoundBuilderResult::Succeeded)
			{
				OutError = FString::Printf(TEXT("AddGraphInputNode failed for '%s' (type %s)"), *Name, *Type);
				RollbackAll();
				return false;
			}
			++InputsAdded;
		}
	}

	// Pass 1c: outputs
	const TArray<TSharedPtr<FJsonValue>>* OutsArr = nullptr;
	if (Spec->TryGetArrayField(TEXT("outputs"), OutsArr) && OutsArr)
	{
		for (const TSharedPtr<FJsonValue>& V : *OutsArr)
		{
			if (!V.IsValid()) continue;
			const TSharedPtr<FJsonObject>* OObj = nullptr;
			if (!V->TryGetObject(OObj) || !OObj || !OObj->IsValid()) continue;
			FString Name, Type;
			if (!(*OObj)->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty()) { OutError = TEXT("output missing 'name'"); RollbackAll(); return false; }
			if (!(*OObj)->TryGetStringField(TEXT("type"), Type) || Type.IsEmpty()) { OutError = TEXT("output missing 'type'"); RollbackAll(); return false; }
			FMetasoundFrontendLiteral Default;
			MSApplySpec_BuildLiteralFromJson(FName(*Type), (*OObj)->TryGetField(TEXT("default")), Default);
			EMetaSoundBuilderResult R = EMetaSoundBuilderResult::Failed;
			BuilderObj.AddGraphOutputNode(FName(*Name), FName(*Type), Default, R, /*bIsConstructorOutput=*/false);
			if (R != EMetaSoundBuilderResult::Succeeded)
			{
				OutError = FString::Printf(TEXT("AddGraphOutputNode failed for '%s' (type %s)"), *Name, *Type);
				RollbackAll();
				return false;
			}
			++OutputsAdded;
		}
	}

	// Pass 1d: nodes (class_namespace + class_name)
	const TArray<TSharedPtr<FJsonValue>>* NodesArr = nullptr;
	if (Spec->TryGetArrayField(TEXT("nodes"), NodesArr) && NodesArr)
	{
		for (const TSharedPtr<FJsonValue>& V : *NodesArr)
		{
			if (!V.IsValid()) continue;
			const TSharedPtr<FJsonObject>* NObj = nullptr;
			if (!V->TryGetObject(NObj) || !NObj || !NObj->IsValid()) continue;
			FString NSp, Name, Variant;
			(*NObj)->TryGetStringField(TEXT("class_namespace"), NSp);
			if (!(*NObj)->TryGetStringField(TEXT("class_name"), Name) || Name.IsEmpty())
			{
				OutError = TEXT("node missing 'class_name'");
				RollbackAll();
				return false;
			}
			(*NObj)->TryGetStringField(TEXT("class_variant"), Variant);
			// UE 5.5 UMetaSoundBuilderBase::AddNodeByClassName signature is
			// (FMetasoundFrontendClassName, EMetaSoundBuilderResult& OutResult).
			// MajorVersion is part of FMetasoundFrontendClassName itself (set via
			// the constructor in a future overload not yet exposed); for now we
			// document the requested major_version in the error path on failure
			// but cannot select between versions at the builder API surface.
			int32 MajorVersion = 1;
			{
				int32 MV = 0;
				if ((*NObj)->TryGetNumberField(TEXT("major_version"), MV)) MajorVersion = MV;
			}
			// Brace init forces variable definition (parens would be parsed as a function
			// declaration in MSVC -- "most vexing parse" because FName(*NSp) is itself a
			// constructor that the compiler reads as a function-pointer type).
			FMetasoundFrontendClassName ClassName{FName(*NSp), FName(*Name), FName(*Variant)};
			EMetaSoundBuilderResult R = EMetaSoundBuilderResult::Failed;
			BuilderObj.AddNodeByClassName(ClassName, R);
			if (R != EMetaSoundBuilderResult::Succeeded)
			{
				OutError = FString::Printf(TEXT("AddNodeByClassName failed for %s.%s (requested v%d)"), *NSp, *Name, MajorVersion);
				RollbackAll();
				return false;
			}
			++NodesAdded;
		}
	}

	// Pass 2a: input defaults (after inputs are created)
	const TArray<TSharedPtr<FJsonValue>>* DefsArr = nullptr;
	if (Spec->TryGetArrayField(TEXT("input_defaults"), DefsArr) && DefsArr)
	{
		for (const TSharedPtr<FJsonValue>& V : *DefsArr)
		{
			if (!V.IsValid()) continue;
			const TSharedPtr<FJsonObject>* DObj = nullptr;
			if (!V->TryGetObject(DObj) || !DObj || !DObj->IsValid()) continue;
			FString Name, Type;
			if (!(*DObj)->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty()) { OutError = TEXT("input_default missing 'name'"); RollbackAll(); return false; }
			if (!(*DObj)->TryGetStringField(TEXT("type"), Type) || Type.IsEmpty()) { OutError = TEXT("input_default missing 'type'"); RollbackAll(); return false; }
			FMetasoundFrontendLiteral Lit;
			if (!MSApplySpec_BuildLiteralFromJson(FName(*Type), (*DObj)->TryGetField(TEXT("value")), Lit))
			{
				OutError = FString::Printf(TEXT("input_default '%s' value could not be converted to type %s"), *Name, *Type);
				RollbackAll();
				return false;
			}
			EMetaSoundBuilderResult R = EMetaSoundBuilderResult::Failed;
			BuilderObj.SetGraphInputDefault(FName(*Name), Lit, R);
			if (R != EMetaSoundBuilderResult::Succeeded)
			{
				OutError = FString::Printf(TEXT("SetGraphInputDefault failed for '%s'"), *Name);
				RollbackAll();
				return false;
			}
			++DefaultsSet;
		}
	}

	// Pass 2b: connect input-node -> output-node by name (matches connect_pins shape).
	const TArray<TSharedPtr<FJsonValue>>* EdgesArr = nullptr;
	if (Spec->TryGetArrayField(TEXT("connections"), EdgesArr) && EdgesArr)
	{
		for (const TSharedPtr<FJsonValue>& V : *EdgesArr)
		{
			if (!V.IsValid()) continue;
			const TSharedPtr<FJsonObject>* CObj = nullptr;
			if (!V->TryGetObject(CObj) || !CObj || !CObj->IsValid()) continue;
			FString GraphInputName, GraphOutputName;
			if (!(*CObj)->TryGetStringField(TEXT("graph_input_name"), GraphInputName) || GraphInputName.IsEmpty())
			{
				OutError = TEXT("connection missing 'graph_input_name'"); RollbackAll(); return false;
			}
			if (!(*CObj)->TryGetStringField(TEXT("graph_output_name"), GraphOutputName) || GraphOutputName.IsEmpty())
			{
				OutError = TEXT("connection missing 'graph_output_name'"); RollbackAll(); return false;
			}
			EMetaSoundBuilderResult FindR = EMetaSoundBuilderResult::Failed;
			FMetaSoundNodeHandle InputNode = BuilderObj.FindGraphInputNode(FName(*GraphInputName), FindR);
			if (FindR != EMetaSoundBuilderResult::Succeeded)
			{
				OutError = FString::Printf(TEXT("Graph input '%s' not found"), *GraphInputName);
				RollbackAll();
				return false;
			}
			FindR = EMetaSoundBuilderResult::Failed;
			FMetaSoundNodeHandle OutputNode = BuilderObj.FindGraphOutputNode(FName(*GraphOutputName), FindR);
			if (FindR != EMetaSoundBuilderResult::Succeeded)
			{
				OutError = FString::Printf(TEXT("Graph output '%s' not found"), *GraphOutputName);
				RollbackAll();
				return false;
			}
			TArray<FMetaSoundBuilderNodeOutputHandle> InOutputs = BuilderObj.FindNodeOutputs(InputNode, FindR);
			if (FindR != EMetaSoundBuilderResult::Succeeded || InOutputs.Num() == 0)
			{
				OutError = FString::Printf(TEXT("Input node '%s' has no output pin"), *GraphInputName);
				RollbackAll();
				return false;
			}
			TArray<FMetaSoundBuilderNodeInputHandle> OutInputs = BuilderObj.FindNodeInputs(OutputNode, FindR);
			if (FindR != EMetaSoundBuilderResult::Succeeded || OutInputs.Num() == 0)
			{
				OutError = FString::Printf(TEXT("Output node '%s' has no input pin"), *GraphOutputName);
				RollbackAll();
				return false;
			}
			EMetaSoundBuilderResult ConnR = EMetaSoundBuilderResult::Failed;
			BuilderObj.ConnectNodes(InOutputs[0], OutInputs[0], ConnR);
			if (ConnR != EMetaSoundBuilderResult::Succeeded)
			{
				OutError = FString::Printf(TEXT("ConnectNodes failed (input=%s, output=%s)"), *GraphInputName, *GraphOutputName);
				RollbackAll();
				return false;
			}
			++EdgesAdded;
		}
	}

	OutSummary = FString::Printf(
		TEXT("metasound_apply_spec ok: %s%s -- %d interface(s), %d input(s), %d output(s), %d node(s), %d default(s), %d edge(s)"),
		*Asset->GetPathName(), bCreated ? TEXT(" (created)") : TEXT(""),
		InterfacesAdded, InputsAdded, OutputsAdded, NodesAdded, DefaultsSet, EdgesAdded);
	return true;
#endif
}

bool FClaireonSpecApplicator_Audio::ApplySoundMixSpec(const TSharedPtr<FJsonObject>& Spec, FString& OutSummary, FString& OutError)
{
	IdToAsset.Empty();
	AssetsCreatedThisCall.Empty();

	if (!Spec.IsValid())
	{
		OutError = TEXT("Spec JSON is null");
		return false;
	}
	FString KindStr;
	if (!Spec->TryGetStringField(TEXT("kind"), KindStr) || KindStr.IsEmpty())
	{
		OutError = TEXT("Spec missing 'kind'");
		return false;
	}
	const EClaireonAudioAssetKind Kind = AudioAssetKindFromString(FStringView(KindStr));
	if (Kind != EClaireonAudioAssetKind::SoundMix)
	{
		OutError = FString::Printf(TEXT("ApplySoundMixSpec: kind '%s' is not SoundMix"), *KindStr);
		return false;
	}
	FString AssetPath;
	if (!Spec->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("Spec missing 'asset_path'");
		return false;
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Apply SoundMix Spec")));
	UObject* Asset = nullptr;
	bool bCreated = false;
	if (!SpecApplicatorAudio_LoadOrCreateForCohort(AssetPath, Kind, /*bAllowCreate=*/true, Asset, bCreated, OutError))
	{
		Transaction.Cancel();
		return false;
	}
	if (bCreated) AssetsCreatedThisCall.Add(Asset);

	USoundMix* Mix = Cast<USoundMix>(Asset);
	if (!Mix)
	{
		Transaction.Cancel();
		OutError = TEXT("Loaded asset is not a USoundMix");
		return false;
	}
	Mix->Modify();

	// Envelope
	const TSharedPtr<FJsonObject>* EnvObj = nullptr;
	if (Spec->TryGetObjectField(TEXT("envelope"), EnvObj) && EnvObj && EnvObj->IsValid())
	{
		double V;
		if ((*EnvObj)->TryGetNumberField(TEXT("initial_delay"), V)) Mix->InitialDelay = static_cast<float>(V);
		if ((*EnvObj)->TryGetNumberField(TEXT("fade_in_time"), V))  Mix->FadeInTime = static_cast<float>(V);
		if ((*EnvObj)->TryGetNumberField(TEXT("duration"), V))      Mix->Duration = static_cast<float>(V);
		if ((*EnvObj)->TryGetNumberField(TEXT("fade_out_time"), V)) Mix->FadeOutTime = static_cast<float>(V);
	}

	// Class adjusters
	const TArray<TSharedPtr<FJsonValue>>* AdjArr = nullptr;
	if (Spec->TryGetArrayField(TEXT("class_adjusters"), AdjArr) && AdjArr)
	{
		for (const TSharedPtr<FJsonValue>& AV : *AdjArr)
		{
			if (!AV.IsValid()) continue;
			const TSharedPtr<FJsonObject>* AObj = nullptr;
			if (!AV->TryGetObject(AObj) || !AObj || !AObj->IsValid()) continue;
			FString ClassPath;
			if (!(*AObj)->TryGetStringField(TEXT("sound_class"), ClassPath) || ClassPath.IsEmpty()) continue;
			const ClaireonPathResolver::FResolveResult R = ClaireonPathResolver::Resolve(ClassPath);
			USoundClass* SC = R.bSuccess ? LoadObject<USoundClass>(nullptr, *R.ResolvedPath.Path) : nullptr;
			if (!SC) continue;
			// Skip dupes (matches add_class_adjuster invariant).
			bool bDupe = false;
			for (const FSoundClassAdjuster& Existing : Mix->SoundClassEffects)
			{
				if (Existing.SoundClassObject && Existing.SoundClassObject->GetPathName() == SC->GetPathName())
				{
					bDupe = true; break;
				}
			}
			if (bDupe) continue;
			FSoundClassAdjuster A;
			A.SoundClassObject = SC;
			double V;
			if ((*AObj)->TryGetNumberField(TEXT("volume_adjuster"), V)) A.VolumeAdjuster = static_cast<float>(V);
			if ((*AObj)->TryGetNumberField(TEXT("pitch_adjuster"), V))  A.PitchAdjuster = static_cast<float>(V);
			Mix->SoundClassEffects.Add(A);
		}
	}

	Mix->MarkPackageDirty();
	OutSummary = FString::Printf(TEXT("soundmix_apply_spec ok: %s%s"),
		*Mix->GetPathName(), bCreated ? TEXT(" (created)") : TEXT(""));
	return true;
}
