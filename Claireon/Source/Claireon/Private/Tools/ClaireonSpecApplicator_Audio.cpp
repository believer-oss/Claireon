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
	if (Kind != EClaireonAudioAssetKind::MetaSoundSource)
	{
		OutError = FString::Printf(TEXT("ApplyMetaSoundSpec: kind '%s' is not MetaSoundSource"), *KindStr);
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

	// Full inputs/outputs/nodes/edges materialization via per-cohort apply_spec is deferred to the
	// dedicated session-based MetaSound tools. Per-cohort apply_spec here creates/locates the asset.
	OutSummary = FString::Printf(TEXT("metasound_apply_spec ok (asset only): %s%s"),
		*Asset->GetPathName(), bCreated ? TEXT(" (created)") : TEXT(""));
	return true;
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
