// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonNameResolver.h"
#include "UObject/UObjectIterator.h"
#include "UObject/CoreRedirects.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "Features/IModularFeatures.h"
#include "IClaireonToolProvider.h"

namespace ClaireonNameResolver
{

	// -----------------------------------------------------------------
	// Engine-generic module list for /Script/<Module>.<Name> path resolution.
	// Project-specific modules are supplied by registered IClaireonToolProvider
	// implementations (GetKnownModules); see GetResolverModules().
	// -----------------------------------------------------------------
	static const TArray<FString> EngineKnownModules = {
		TEXT("Engine"),
		TEXT("CoreUObject"),
		TEXT("UMG"),
		TEXT("SlateCore"),
		TEXT("Slate"),
		TEXT("AIModule"),
		TEXT("NavigationSystem"),
		TEXT("GameplayAbilities"),
		TEXT("GameplayTags"),
		TEXT("GameplayTasks"),
		TEXT("EnhancedInput"),
		TEXT("Niagara"),
		TEXT("PCG"),
		TEXT("StateTreeModule"),
	};

	// -----------------------------------------------------------------
	// Engine-generic class prefix map for ResolveClassName.
	//
	// Keyed by the UE internal name of a base class (i.e., what
	// UClass::GetName() returns -- no C++ "U"/"A"/"F" prefix). Each entry
	// lists the bare prefix conventions that derived classes commonly use.
	// At resolve time, for a given RequiredBaseClass we look up its name
	// here and try prefix + Input as well as "U" + prefix + Input.
	//
	// Core ships engine-generic entries only (e.g. "BTTask_", "AnimNotify_").
	// Project-specific prefixes are supplied by registered IClaireonToolProvider
	// implementations (GetClassPrefixMap); see GetResolverPrefixMap().
	// -----------------------------------------------------------------
	static const TMap<FString, TArray<FString>>& GetEngineBaseClassPrefixMap()
	{
		static const TMap<FString, TArray<FString>> Map = {
			{ TEXT("AnimNotify"),       { TEXT("AnimNotify_") } },
			{ TEXT("AnimNotifyState"),  { TEXT("AnimNotifyState_") } },
			{ TEXT("BTNode"),           { TEXT("BTComposite_"), TEXT("BTTask_"), TEXT("BTDecorator_"), TEXT("BTService_") } },
			{ TEXT("BTCompositeNode"),  { TEXT("BTComposite_") } },
			{ TEXT("BTTaskNode"),       { TEXT("BTTask_") } },
			{ TEXT("BTDecorator"),      { TEXT("BTDecorator_") } },
			{ TEXT("BTService"),        { TEXT("BTService_") } },
		};
		return Map;
	}

	// Aggregate the module list: engine defaults + modules contributed by registered
	// providers. (No FModuleManager auto-discovery -- the Step-8 case-insensitive class
	// iterator already resolves any loaded class, so the list is only a fast-path aid.)
	static TArray<FString> GetResolverModules()
	{
		TArray<FString> Modules = EngineKnownModules;
		TArray<IClaireonToolProvider*> Providers =
			IModularFeatures::Get().GetModularFeatureImplementations<IClaireonToolProvider>(
				IClaireonToolProvider::FeatureName);
		for (IClaireonToolProvider* Provider : Providers)
		{
			if (!Provider) { continue; }
			TArray<FString> ProviderModules;
			Provider->GetKnownModules(ProviderModules);
			for (const FString& M : ProviderModules) { Modules.AddUnique(M); }
		}
		return Modules;
	}

	// Aggregate the prefix map: engine defaults overlaid with provider entries
	// (appended per key so engine + project prefixes coexist).
	static TMap<FString, TArray<FString>> GetResolverPrefixMap()
	{
		TMap<FString, TArray<FString>> Map = GetEngineBaseClassPrefixMap();
		TArray<IClaireonToolProvider*> Providers =
			IModularFeatures::Get().GetModularFeatureImplementations<IClaireonToolProvider>(
				IClaireonToolProvider::FeatureName);
		for (IClaireonToolProvider* Provider : Providers)
		{
			if (!Provider) { continue; }
			TMap<FString, TArray<FString>> ProviderMap;
			Provider->GetClassPrefixMap(ProviderMap);
			for (const TPair<FString, TArray<FString>>& Pair : ProviderMap)
			{
				TArray<FString>& Dest = Map.FindOrAdd(Pair.Key);
				for (const FString& Prefix : Pair.Value) { Dest.AddUnique(Prefix); }
			}
		}
		return Map;
	}

	// -----------------------------------------------------------------
	// Event alias map for ResolveFunctionName
	// -----------------------------------------------------------------
	static const TMap<FString, FString>& GetEventAliasMap()
	{
		static const TMap<FString, FString> Map = {
			{ TEXT("beginplay"), TEXT("ReceiveBeginPlay") },
			{ TEXT("tick"), TEXT("ReceiveTick") },
			{ TEXT("endplay"), TEXT("ReceiveEndPlay") },
			{ TEXT("destroyed"), TEXT("ReceiveDestroyed") },
			{ TEXT("actorbeginoverlap"), TEXT("ReceiveActorBeginOverlap") },
			{ TEXT("actorendoverlap"), TEXT("ReceiveActorEndOverlap") },
			{ TEXT("hit"), TEXT("ReceiveHit") },
			{ TEXT("anydamage"), TEXT("ReceiveAnyDamage") },
			{ TEXT("pointdamage"), TEXT("ReceivePointDamage") },
			{ TEXT("radialdamage"), TEXT("ReceiveRadialDamage") },
			{ TEXT("begincursorover"), TEXT("ReceiveBeginCursorOver") },
			{ TEXT("endcursorover"), TEXT("ReceiveEndCursorOver") },
			{ TEXT("clicked"), TEXT("ReceiveActorOnClicked") },
			{ TEXT("released"), TEXT("ReceiveActorOnReleased") },
		};
		return Map;
	}

	// -----------------------------------------------------------------
	// Helper: build a comma-separated candidate list string
	// -----------------------------------------------------------------
	static FString JoinCandidates(const TArray<FString>& Candidates)
	{
		return FString::JoinBy(Candidates, TEXT(", "), [](const FString& S)
		{
			return S;
		});
	}

	// =================================================================
	// ResolveClassName
	// =================================================================
	UClass* ResolveClassName(
		const FString& Input,
		UClass* RequiredBaseClass,
		FNameResolveResult& OutResult)
	{
		OutResult = FNameResolveResult();

		if (Input.IsEmpty())
		{
			OutResult.Error = TEXT("Class name is empty");
			return nullptr;
		}

		// Lambda to check base class constraint
		auto PassesBaseCheck = [RequiredBaseClass](UClass* C) -> bool
		{
			return !IsValid(RequiredBaseClass) || C->IsChildOf(RequiredBaseClass);
		};

		// Lambda to try a single name via FindFirstObject
		auto TryFind = [&](const FString& Name) -> UClass*
		{
			UClass* Found = FindFirstObject<UClass>(*Name, EFindFirstObjectOptions::NativeFirst);
			if (IsValid(Found) && PassesBaseCheck(Found))
			{
				return Found;
			}
			return nullptr;
		};

		// Step 0: Rooted object-path fast path ('/Script/Module.Class' or
		// '/Game/Path/Asset.Asset_C'). FindFirstObject searches by short name,
		// so pathed inputs must resolve via StaticFindObject. A path that
		// resolves to a class of the wrong base is a definitive answer -- the
		// short-name fuzzy steps below cannot succeed for a rooted path, so we
		// return a precise wrong-base error instead of a generic not-found.
		if (Input.StartsWith(TEXT("/")))
		{
			UClass* Found = FindObject<UClass>(nullptr, *Input);
			if (!IsValid(Found))
			{
				// FindObject only sees classes already in memory; a rooted /Game
				// path naming an unloaded Blueprint's generated class must LOAD it
				// (a fresh editor session has none of them resident). Harmless for
				// /Script/ paths (native classes are always registered).
				Found = LoadObject<UClass>(nullptr, *Input);
			}
			if (!IsValid(Found) && !Input.EndsWith(TEXT("_C")))
			{
				// Accept an asset path spelled without the generated-class suffix
				// (/Game/Dir/BP_Foo.BP_Foo -> class BP_Foo_C).
				Found = LoadObject<UClass>(nullptr, *(Input + TEXT("_C")));
			}
			if (IsValid(Found))
			{
				if (PassesBaseCheck(Found))
				{
					OutResult.bSuccess = true;
					OutResult.ResolvedName = Found->GetName();
					return Found;
				}
				OutResult.bSuccess = false;
				OutResult.Error = FString::Printf(
					TEXT("Class '%s' (from path '%s') is not a child of '%s'"),
					*Found->GetName(), *Input,
					IsValid(RequiredBaseClass) ? *RequiredBaseClass->GetName() : TEXT("<none>"));
				return nullptr;
			}
		}

		// Step 1: Exact match (also tries stripping A/U prefix since UE internal
		// names omit the C++ class prefix, e.g., "AActor" -> "Actor")
		if (UClass* Found = TryFind(Input); IsValid(Found))
		{
			OutResult.bSuccess = true;
			OutResult.ResolvedName = Found->GetName();
			return Found;
		}
		if (Input.Len() > 1)
		{
			TCHAR First = FChar::ToUpper(Input[0]);
			if (First == TEXT('A') || First == TEXT('U'))
			{
				if (UClass* Found = TryFind(Input.Mid(1)); IsValid(Found))
				{
					OutResult.bSuccess = true;
					OutResult.ResolvedName = Found->GetName();
					return Found;
				}
			}
		}

		// Fuzzy match candidates
		TArray<TPair<UClass*, FString>> FuzzyCandidates;

		auto AddCandidate = [&](UClass* C, const FString& Note)
		{
			// Avoid duplicates
			for (const auto& Pair : FuzzyCandidates)
			{
				if (Pair.Key == C)
				{
					return;
				}
			}
			FuzzyCandidates.Add({ C, Note });
		};

		// Step 2: Strip U prefix
		if (Input.Len() > 1 && Input[0] == TEXT('U') && FChar::IsUpper(Input[1]))
		{
			if (UClass* Found = TryFind(Input.Mid(1)); IsValid(Found))
			{
				AddCandidate(Found, FString::Printf(TEXT("Resolved '%s' to '%s' (stripped U prefix)"), *Input, *Found->GetName()));
			}
		}

		// Step 3: Add U prefix
		if (UClass* Found = TryFind(TEXT("U") + Input); IsValid(Found))
		{
			AddCandidate(Found, FString::Printf(TEXT("Resolved '%s' to '%s' (added U prefix)"), *Input, *Found->GetName()));
		}

		// Step 4: Add A prefix
		if (UClass* Found = TryFind(TEXT("A") + Input); IsValid(Found))
		{
			AddCandidate(Found, FString::Printf(TEXT("Resolved '%s' to '%s' (added A prefix)"), *Input, *Found->GetName()));
		}

		// Step 5: Add Component suffix
		if (UClass* Found = TryFind(Input + TEXT("Component")); IsValid(Found))
		{
			AddCandidate(Found, FString::Printf(TEXT("Resolved '%s' to '%s' (added Component suffix)"), *Input, *Found->GetName()));
		}

		// Step 6: U prefix + Component suffix
		if (UClass* Found = TryFind(TEXT("U") + Input + TEXT("Component")); IsValid(Found))
		{
			AddCandidate(Found, FString::Printf(TEXT("Resolved '%s' to '%s' (added U prefix + Component suffix)"), *Input, *Found->GetName()));
		}

		// Early exit if we already have exactly one
		if (FuzzyCandidates.Num() == 1)
		{
			OutResult.bSuccess = true;
			OutResult.ResolvedName = FuzzyCandidates[0].Key->GetName();
			OutResult.ResolutionNote = FuzzyCandidates[0].Value;
			return FuzzyCandidates[0].Key;
		}

		// Step 6.5: Domain-specific prefixes (when RequiredBaseClass is provided)
		// Looks up the base class in the prefix map (e.g., "BTTaskNode" -> {"BTTask_"})
		// and tries each prefix combined with the input, both as-is and with a
		// leading "U" object prefix. This is the canonical home for per-domain
		// prefix-walking (anim notify and BT helpers route through here).
		if (FuzzyCandidates.Num() == 0 && IsValid(RequiredBaseClass))
		{
			const TMap<FString, TArray<FString>> PrefixMap = GetResolverPrefixMap();
			if (const TArray<FString>* Prefixes = PrefixMap.Find(RequiredBaseClass->GetName()))
			{
				for (const FString& Prefix : *Prefixes)
				{
					if (UClass* Found = TryFind(Prefix + Input); IsValid(Found))
					{
						AddCandidate(Found, FString::Printf(TEXT("Resolved '%s' to '%s' (added '%s' prefix)"), *Input, *Found->GetName(), *Prefix));
					}
					if (UClass* Found = TryFind(TEXT("U") + Prefix + Input); IsValid(Found))
					{
						AddCandidate(Found, FString::Printf(TEXT("Resolved '%s' to '%s' (added 'U%s' prefix)"), *Input, *Found->GetName(), *Prefix));
					}
				}

				if (FuzzyCandidates.Num() == 1)
				{
					OutResult.bSuccess = true;
					OutResult.ResolvedName = FuzzyCandidates[0].Key->GetName();
					OutResult.ResolutionNote = FuzzyCandidates[0].Value;
					return FuzzyCandidates[0].Key;
				}
			}
		}

		// Step 7: Try /Script/ module paths
		if (FuzzyCandidates.Num() == 0)
		{
			const TArray<FString> ResolverModules = GetResolverModules();
			for (const FString& Module : ResolverModules)
			{
				FString ScriptPath = FString::Printf(TEXT("/Script/%s.%s"), *Module, *Input);
				if (UClass* Found = TryFind(ScriptPath); IsValid(Found))
				{
					AddCandidate(Found, FString::Printf(TEXT("Resolved '%s' to '%s' (found in /Script/%s)"), *Input, *Found->GetName(), *Module));
				}
			}

			if (FuzzyCandidates.Num() == 1)
			{
				OutResult.bSuccess = true;
				OutResult.ResolvedName = FuzzyCandidates[0].Key->GetName();
				OutResult.ResolutionNote = FuzzyCandidates[0].Value;
				return FuzzyCandidates[0].Key;
			}
		}

		// Step 8: Case-insensitive iteration (most expensive, last resort)
		if (FuzzyCandidates.Num() == 0)
		{
			for (TObjectIterator<UClass> It; It; ++It)
			{
				UClass* C = *It;
				if (C->GetName().Equals(Input, ESearchCase::IgnoreCase) && PassesBaseCheck(C))
				{
					AddCandidate(C, FString::Printf(TEXT("Resolved '%s' to '%s' (case-insensitive match)"), *Input, *C->GetName()));
				}
			}

			if (FuzzyCandidates.Num() == 1)
			{
				OutResult.bSuccess = true;
				OutResult.ResolvedName = FuzzyCandidates[0].Key->GetName();
				OutResult.ResolutionNote = FuzzyCandidates[0].Value;
				return FuzzyCandidates[0].Key;
			}
		}

		// Resolution
		if (FuzzyCandidates.Num() > 1)
		{
			OutResult.bSuccess = false;
			for (const auto& Pair : FuzzyCandidates)
			{
				OutResult.Candidates.Add(Pair.Key->GetName());
			}
			OutResult.Error = FString::Printf(
				TEXT("Ambiguous class name '%s' matched %d classes: %s"),
				*Input, FuzzyCandidates.Num(), *JoinCandidates(OutResult.Candidates));
			return nullptr;
		}

		OutResult.bSuccess = false;
		OutResult.Error = FString::Printf(TEXT("Class not found: '%s'"), *Input);
		return nullptr;
	}

	// =================================================================
	// ResolveClassNameMultiBase
	// =================================================================
	UClass* ResolveClassNameMultiBase(
		const FString& Input,
		const TArray<UClass*>& AcceptableBaseClasses,
		FNameResolveResult& OutResult)
	{
		OutResult = FNameResolveResult();

		if (Input.IsEmpty())
		{
			OutResult.Error = TEXT("Class name is empty");
			return nullptr;
		}

		bool bHasAnyBase = false;
		for (UClass* Base : AcceptableBaseClasses)
		{
			if (IsValid(Base))
			{
				bHasAnyBase = true;
				break;
			}
		}
		if (!bHasAnyBase)
		{
			OutResult.Error = TEXT("ResolveClassNameMultiBase: no acceptable base classes were provided");
			return nullptr;
		}

		// Run the full single-base pipeline once per base and merge the
		// outcomes. This is the cross-base retry the single-base gate lacks:
		// a class discarded under one base (IsChildOf failure) is retried under
		// the next base before resolution gives up.
		TArray<UClass*> ResolvedClasses;
		TArray<FNameResolveResult> ResolvedResults;
		TArray<FString> AttemptedScopes;
		FString FirstPerBaseAmbiguityError;
		TArray<FString> FirstPerBaseAmbiguityCandidates;

		for (UClass* Base : AcceptableBaseClasses)
		{
			if (!IsValid(Base))
			{
				continue;
			}
			AttemptedScopes.Add(Base->GetName());

			FNameResolveResult BaseResult;
			if (UClass* Found = ResolveClassName(Input, Base, BaseResult); IsValid(Found))
			{
				if (!ResolvedClasses.Contains(Found))
				{
					ResolvedClasses.Add(Found);
					ResolvedResults.Add(BaseResult);
				}
			}
			else if (BaseResult.Candidates.Num() > 1 && FirstPerBaseAmbiguityError.IsEmpty())
			{
				// Ambiguity within one base scope: remember it (error AND
				// candidates, so callers can inspect or tiebreak), but keep
				// trying the other scopes -- a unique match under a later base
				// is a better answer than an early ambiguity.
				FirstPerBaseAmbiguityError = BaseResult.Error;
				FirstPerBaseAmbiguityCandidates = BaseResult.Candidates;
			}
		}

		if (ResolvedClasses.Num() == 1)
		{
			OutResult = ResolvedResults[0];
			return ResolvedClasses[0];
		}

		if (ResolvedClasses.Num() > 1)
		{
			OutResult.bSuccess = false;
			for (UClass* ResolvedClass : ResolvedClasses)
			{
				OutResult.Candidates.Add(ResolvedClass->GetName());
			}
			OutResult.Error = FString::Printf(
				TEXT("Ambiguous class name '%s' resolved to %d different classes across base scopes (%s): %s"),
				*Input, ResolvedClasses.Num(),
				*FString::Join(AttemptedScopes, TEXT(", ")),
				*JoinCandidates(OutResult.Candidates));
			return nullptr;
		}

		OutResult.bSuccess = false;
		if (!FirstPerBaseAmbiguityError.IsEmpty())
		{
			// Nothing resolved uniquely, but at least one scope had a
			// within-base ambiguity: surface that error and its candidate
			// list -- it is a more precise cause than a generic not-found.
			OutResult.Error = FirstPerBaseAmbiguityError;
			OutResult.Candidates = FirstPerBaseAmbiguityCandidates;
		}
		else
		{
			// NOTE: this exact error text is pinned by
			// Tests/ClaireonNotifyClassResolveTests.cpp
			// (MultiBase_GarbageNameNamesScopes). Keep them in sync.
			OutResult.Error = FString::Printf(
				TEXT("Class not found: '%s' (attempted scopes: %s)"),
				*Input, *FString::Join(AttemptedScopes, TEXT(", ")));
		}
		return nullptr;
	}

	// =================================================================
	// ResolvePinName
	// =================================================================
	UEdGraphPin* ResolvePinName(
		UEdGraphNode* Node,
		const FString& Input,
		EEdGraphPinDirection DirectionHint,
		FNameResolveResult& OutResult)
	{
		OutResult = FNameResolveResult();

		if (!IsValid(Node))
		{
			OutResult.Error = TEXT("Node is null");
			return nullptr;
		}

		if (Input.IsEmpty())
		{
			OutResult.Error = TEXT("Pin name is empty");
			return nullptr;
		}

		// Helper to check direction constraint
		auto PassesDirection = [DirectionHint](UEdGraphPin* Pin) -> bool
		{
			return DirectionHint == EGPD_MAX || Pin->Direction == DirectionHint;
		};

		// Handle the Claireon-serialized disambiguated form
		// (`name_FriendlySuffix` or `name[N]`). Latent nodes like AwaitDelay
		// can carry multiple pins with the same PinName; the serializer emits
		// disambiguated names so callers can target each one. Engine-side
		// PinName is still the raw form, so we resolve to the matching pin
		// by walking duplicates and matching the suffix.
		auto SanitizeFriendlySuffix = [](const FString& Friendly) -> FString
		{
			FString Out;
			Out.Reserve(Friendly.Len());
			for (TCHAR Ch : Friendly)
			{
				if (FChar::IsAlnum(Ch) || Ch == TEXT('_'))
				{
					Out.AppendChar(Ch);
				}
			}
			return Out;
		};
		auto TryResolveDisambiguatedForm = [&](const FString& Spec) -> UEdGraphPin*
		{
			// Indexed form: "name[N]"
			int32 OpenBracket = INDEX_NONE;
			if (Spec.FindChar(TEXT('['), OpenBracket) && Spec.EndsWith(TEXT("]")))
			{
				const FString BaseName = Spec.Left(OpenBracket);
				const FString IndexStr = Spec.Mid(OpenBracket + 1, Spec.Len() - OpenBracket - 2);
				if (IndexStr.IsNumeric())
				{
					const int32 TargetIndex = FCString::Atoi(*IndexStr);
					int32 SeenIndex = 0;
					for (UEdGraphPin* Pin : Node->Pins)
					{
						if (!Pin || !PassesDirection(Pin))
						{
							continue;
						}
						if (Pin->GetName().Equals(BaseName, ESearchCase::CaseSensitive))
						{
							if (SeenIndex == TargetIndex)
							{
								return Pin;
							}
							++SeenIndex;
						}
					}
				}
			}
			// Suffix form: "name_FriendlySuffix"
			int32 UnderscorePos = INDEX_NONE;
			if (Spec.FindLastChar(TEXT('_'), UnderscorePos))
			{
				const FString BaseName = Spec.Left(UnderscorePos);
				const FString Suffix = Spec.Mid(UnderscorePos + 1);
				if (!BaseName.IsEmpty() && !Suffix.IsEmpty())
				{
					for (UEdGraphPin* Pin : Node->Pins)
					{
						if (!Pin || !PassesDirection(Pin))
						{
							continue;
						}
						if (!Pin->GetName().Equals(BaseName, ESearchCase::CaseSensitive))
						{
							continue;
						}
						const FString FriendlySuffix = SanitizeFriendlySuffix(Pin->PinFriendlyName.ToString());
						if (FriendlySuffix.Equals(Suffix, ESearchCase::IgnoreCase))
						{
							return Pin;
						}
					}
				}
			}
			return nullptr;
		};

		// Step 1: Exact match with direction, then without
		if (DirectionHint != EGPD_MAX)
		{
			if (UEdGraphPin* Found = Node->FindPin(*Input, DirectionHint))
			{
				// Count duplicates -- if more than one, the bare name is
				// ambiguous and we must require the disambiguated form.
				int32 DupCount = 0;
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin && Pin->Direction == DirectionHint && Pin->PinName == Found->PinName)
					{
						++DupCount;
					}
				}
				if (DupCount <= 1)
				{
					OutResult.bSuccess = true;
					OutResult.ResolvedName = Found->GetName();
					return Found;
				}
			}
			else if (UEdGraphPin* Disambiguated = TryResolveDisambiguatedForm(Input))
			{
				OutResult.bSuccess = true;
				OutResult.ResolvedName = Disambiguated->GetName();
				OutResult.ResolutionNote = FString::Printf(
					TEXT("Resolved '%s' to duplicate pin via disambiguated form"), *Input);
				return Disambiguated;
			}
		}
		if (UEdGraphPin* Found = Node->FindPin(*Input))
		{
			if (PassesDirection(Found))
			{
				int32 DupCount = 0;
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin && Pin->PinName == Found->PinName && PassesDirection(Pin))
					{
						++DupCount;
					}
				}
				if (DupCount <= 1)
				{
					OutResult.bSuccess = true;
					OutResult.ResolvedName = Found->GetName();
					return Found;
				}
			}
		}
		// If bare name was ambiguous (DupCount > 1), fall through to try the
		// disambiguated form; if THAT also fails we emit an ambiguity error
		// listing the available disambiguated names.
		if (UEdGraphPin* Disambiguated = TryResolveDisambiguatedForm(Input))
		{
			OutResult.bSuccess = true;
			OutResult.ResolvedName = Disambiguated->GetName();
			OutResult.ResolutionNote = FString::Printf(
				TEXT("Resolved '%s' to duplicate pin via disambiguated form"), *Input);
			return Disambiguated;
		}
		// Detect bare-name ambiguity and report it as a structured error.
		{
			TArray<FString> AmbiguousOptions;
			TMap<FName, int32> PinNameCounts;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin && PassesDirection(Pin))
				{
					++PinNameCounts.FindOrAdd(Pin->PinName);
				}
			}
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || !PassesDirection(Pin))
				{
					continue;
				}
				if (!Pin->GetName().Equals(Input, ESearchCase::IgnoreCase))
				{
					continue;
				}
				if (PinNameCounts.FindRef(Pin->PinName) <= 1)
				{
					continue;
				}
				const FString FriendlySuffix = SanitizeFriendlySuffix(Pin->PinFriendlyName.ToString());
				if (!FriendlySuffix.IsEmpty() && !FriendlySuffix.Equals(Pin->GetName(), ESearchCase::IgnoreCase))
				{
					AmbiguousOptions.Add(FString::Printf(TEXT("%s_%s"), *Pin->GetName(), *FriendlySuffix));
				}
				else
				{
					AmbiguousOptions.Add(FString::Printf(TEXT("%s[%d]"), *Pin->GetName(), AmbiguousOptions.Num()));
				}
			}
			if (AmbiguousOptions.Num() > 1)
			{
				OutResult.Error = FString::Printf(
					TEXT("Pin name '%s' is ambiguous on this node (%d pins share the name). Specify the disambiguated form: %s"),
					*Input, AmbiguousOptions.Num(),
					*FString::Join(AmbiguousOptions, TEXT(", ")));
				return nullptr;
			}
		}

		// Collect fuzzy candidates
		TArray<TPair<UEdGraphPin*, FString>> FuzzyCandidates;

		auto AddCandidate = [&](UEdGraphPin* P, const FString& Note)
		{
			for (const auto& Pair : FuzzyCandidates)
			{
				if (Pair.Key == P)
				{
					return;
				}
			}
			FuzzyCandidates.Add({ P, Note });
		};

		// Step 2: Case-insensitive
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->GetName().Equals(Input, ESearchCase::IgnoreCase) && PassesDirection(Pin))
			{
				AddCandidate(Pin, FString::Printf(TEXT("Resolved '%s' to '%s' (case-insensitive match)"), *Input, *Pin->GetName()));
			}
		}

		if (FuzzyCandidates.Num() == 1)
		{
			OutResult.bSuccess = true;
			OutResult.ResolvedName = FuzzyCandidates[0].Key->GetName();
			OutResult.ResolutionNote = FuzzyCandidates[0].Value;
			return FuzzyCandidates[0].Key;
		}

		// Step 3: Common aliases (only if no case-insensitive matches found)
		if (FuzzyCandidates.Num() == 0)
		{
			FString LowerInput = Input.ToLower();

			UEdGraphPin* AliasResult = nullptr;
			FString AliasNote;

			if (LowerInput == TEXT("exec") || LowerInput == TEXT("execute") || LowerInput == TEXT("in"))
			{
				// Find first input exec pin
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec && Pin->Direction == EGPD_Input)
					{
						AliasResult = Pin;
						AliasNote = FString::Printf(TEXT("Resolved '%s' to '%s' (exec input alias)"), *Input, *Pin->GetName());
						break;
					}
				}
			}
			else if (LowerInput == TEXT("then") || LowerInput == TEXT("out") || LowerInput == TEXT("output"))
			{
				// Find first output exec pin
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec && Pin->Direction == EGPD_Output)
					{
						AliasResult = Pin;
						AliasNote = FString::Printf(TEXT("Resolved '%s' to '%s' (exec output alias)"), *Input, *Pin->GetName());
						break;
					}
				}
			}
			else if (LowerInput == TEXT("return") || LowerInput == TEXT("return_value") || LowerInput == TEXT("returnvalue"))
			{
				AliasResult = Node->FindPin(TEXT("ReturnValue"));
				if (AliasResult)
				{
					AliasNote = FString::Printf(TEXT("Resolved '%s' to 'ReturnValue' (return value alias)"), *Input);
				}
			}
			else if (LowerInput == TEXT("self") || LowerInput == TEXT("target"))
			{
				AliasResult = Node->FindPin(TEXT("self"));
				if (AliasResult)
				{
					AliasNote = FString::Printf(TEXT("Resolved '%s' to 'self' (self/target alias)"), *Input);
				}
			}
			else if (LowerInput == TEXT("result"))
			{
				// Find first non-exec output pin
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
					{
						AliasResult = Pin;
						AliasNote = FString::Printf(TEXT("Resolved '%s' to '%s' (result alias)"), *Input, *Pin->GetName());
						break;
					}
				}
			}

			if (AliasResult && PassesDirection(AliasResult))
			{
				OutResult.bSuccess = true;
				OutResult.ResolvedName = AliasResult->GetName();
				OutResult.ResolutionNote = AliasNote;
				return AliasResult;
			}
		}

		// Step 4: Substring match
		if (FuzzyCandidates.Num() == 0)
		{
			TArray<TPair<UEdGraphPin*, FString>> SubstringMatches;
			FString LowerInput = Input.ToLower();
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin->GetName().ToLower().Contains(LowerInput) && PassesDirection(Pin))
				{
					SubstringMatches.Add({ Pin, FString::Printf(TEXT("Resolved '%s' to '%s' (substring match)"), *Input, *Pin->GetName()) });
				}
			}

			if (SubstringMatches.Num() == 1)
			{
				OutResult.bSuccess = true;
				OutResult.ResolvedName = SubstringMatches[0].Key->GetName();
				OutResult.ResolutionNote = SubstringMatches[0].Value;
				return SubstringMatches[0].Key;
			}

			// If multiple substring matches, add them to candidates for error reporting but continue to step 5
			if (SubstringMatches.Num() > 1)
			{
				for (const auto& Pair : SubstringMatches)
				{
					AddCandidate(Pair.Key, Pair.Value);
				}
			}
		}

		// Step 5: Strip direction prefix
		if (FuzzyCandidates.Num() == 0)
		{
			FString StrippedInput;
			if (Input.StartsWith(TEXT("output_"), ESearchCase::IgnoreCase))
			{
				StrippedInput = Input.Mid(7);
			}
			else if (Input.StartsWith(TEXT("input_"), ESearchCase::IgnoreCase))
			{
				StrippedInput = Input.Mid(6);
			}

			if (!StrippedInput.IsEmpty())
			{
				// Retry exact match with stripped prefix
				if (DirectionHint != EGPD_MAX)
				{
					if (UEdGraphPin* Found = Node->FindPin(*StrippedInput, DirectionHint))
					{
						OutResult.bSuccess = true;
						OutResult.ResolvedName = Found->GetName();
						OutResult.ResolutionNote = FString::Printf(TEXT("Resolved '%s' to '%s' (stripped direction prefix)"), *Input, *Found->GetName());
						return Found;
					}
				}
				if (UEdGraphPin* Found = Node->FindPin(*StrippedInput))
				{
					if (PassesDirection(Found))
					{
						OutResult.bSuccess = true;
						OutResult.ResolvedName = Found->GetName();
						OutResult.ResolutionNote = FString::Printf(TEXT("Resolved '%s' to '%s' (stripped direction prefix)"), *Input, *Found->GetName());
						return Found;
					}
				}

				// Retry case-insensitive with stripped prefix
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin->GetName().Equals(StrippedInput, ESearchCase::IgnoreCase) && PassesDirection(Pin))
					{
						AddCandidate(Pin, FString::Printf(TEXT("Resolved '%s' to '%s' (stripped direction prefix, case-insensitive)"), *Input, *Pin->GetName()));
					}
				}

				if (FuzzyCandidates.Num() == 1)
				{
					OutResult.bSuccess = true;
					OutResult.ResolvedName = FuzzyCandidates[0].Key->GetName();
					OutResult.ResolutionNote = FuzzyCandidates[0].Value;
					return FuzzyCandidates[0].Key;
				}
			}
		}

		// Step 6: Final resolution
		if (FuzzyCandidates.Num() > 1)
		{
			OutResult.bSuccess = false;
			for (const auto& Pair : FuzzyCandidates)
			{
				OutResult.Candidates.Add(Pair.Key->GetName());
			}
			OutResult.Error = FString::Printf(
				TEXT("Ambiguous pin name '%s' matched %d pins: %s"),
				*Input, FuzzyCandidates.Num(), *JoinCandidates(OutResult.Candidates));
			return nullptr;
		}

		OutResult.bSuccess = false;
		OutResult.Error = FString::Printf(TEXT("Pin not found: '%s' on node '%s'"), *Input, *Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
		return nullptr;
	}

	// =================================================================
	// ResolveFunctionName
	// =================================================================
	UFunction* ResolveFunctionName(
		UClass* OwnerClass,
		const FString& Input,
		FNameResolveResult& OutResult)
	{
		OutResult = FNameResolveResult();

		if (!IsValid(OwnerClass))
		{
			OutResult.Error = TEXT("Owner class is null");
			return nullptr;
		}

		if (Input.IsEmpty())
		{
			OutResult.Error = TEXT("Function name is empty");
			return nullptr;
		}

		// Step 1: Exact match
		if (UFunction* Found = OwnerClass->FindFunctionByName(FName(*Input)); IsValid(Found))
		{
			OutResult.bSuccess = true;
			OutResult.ResolvedName = Found->GetName();
			return Found;
		}

		// Step 2: Event alias map
		{
			const TMap<FString, FString>& Aliases = GetEventAliasMap();
			FString LowerInput = Input.ToLower();
			if (const FString* MappedName = Aliases.Find(LowerInput))
			{
				if (UFunction* Found = OwnerClass->FindFunctionByName(FName(**MappedName)); IsValid(Found))
				{
					OutResult.bSuccess = true;
					OutResult.ResolvedName = Found->GetName();
					OutResult.ResolutionNote = FString::Printf(TEXT("Resolved '%s' to '%s' (event alias)"), *Input, **MappedName);
					return Found;
				}
			}
		}

		// Step 3: K2_ prefix addition
		{
			FString K2Name = TEXT("K2_") + Input;
			if (UFunction* Found = OwnerClass->FindFunctionByName(FName(*K2Name)); IsValid(Found))
			{
				OutResult.bSuccess = true;
				OutResult.ResolvedName = Found->GetName();
				OutResult.ResolutionNote = FString::Printf(TEXT("Resolved '%s' to '%s' (added K2_ prefix)"), *Input, *Found->GetName());
				return Found;
			}
		}

		// Step 4: K2_ prefix removal
		if (Input.StartsWith(TEXT("K2_")))
		{
			FString Stripped = Input.Mid(3);
			if (UFunction* Found = OwnerClass->FindFunctionByName(FName(*Stripped)); IsValid(Found))
			{
				OutResult.bSuccess = true;
				OutResult.ResolvedName = Found->GetName();
				OutResult.ResolutionNote = FString::Printf(TEXT("Resolved '%s' to '%s' (stripped K2_ prefix)"), *Input, *Found->GetName());
				return Found;
			}
		}

		// Step 4.5: Core redirects. Serialized member references keep pre-rename
		// names (e.g. AwaitPlayVO after a C++ rename); ini FunctionRedirects are
		// registered with FCoreRedirects but FindFunctionByName never consults
		// them. Try the redirect against the class and each of its supers (the
		// redirect names the declaring class, which may be a base).
		{
			for (UClass* RedirectClass = OwnerClass; IsValid(RedirectClass); RedirectClass = RedirectClass->GetSuperClass())
			{
				// Include the declaring package: ini FunctionRedirects register their
				// OldName fully qualified (/Script/Module.Class.Function) and a query
				// with an unspecified package does not match them.
				const FCoreRedirectObjectName OldRedirectName(FName(*Input), RedirectClass->GetFName(),
					RedirectClass->GetOutermost()->GetFName());
				const FCoreRedirectObjectName NewRedirectName =
					FCoreRedirects::GetRedirectedName(ECoreRedirectFlags::Type_Function, OldRedirectName);
				if (NewRedirectName.ObjectName != OldRedirectName.ObjectName && !NewRedirectName.ObjectName.IsNone())
				{
					if (UFunction* Found = OwnerClass->FindFunctionByName(NewRedirectName.ObjectName); IsValid(Found))
					{
						OutResult.bSuccess = true;
						OutResult.ResolvedName = Found->GetName();
						OutResult.ResolutionNote = FString::Printf(
							TEXT("Resolved '%s' to '%s' (core redirect on %s)"),
							*Input, *Found->GetName(), *RedirectClass->GetName());
						return Found;
					}
				}
			}
		}

		// Step 5: Case-insensitive; when the input carries spaces, also accept the
		// editor display name ("Empower Ability" for EmpowerAbility or an explicit
		// DisplayName meta) and the space-stripped spelling -- serialized graph
		// dumps carry the friendly name for some event overrides.
		const bool bInputHasSpaces = Input.Contains(TEXT(" "));
		const FString InputNoSpaces = bInputHasSpaces ? Input.Replace(TEXT(" "), TEXT("")) : Input;
		TArray<TPair<UFunction*, FString>> FuzzyCandidates;
		for (TFieldIterator<UFunction> It(OwnerClass); It; ++It)
		{
			UFunction* Func = *It;
			bool bMatched = Func->GetName().Equals(Input, ESearchCase::IgnoreCase);
			FString Note;
			if (bMatched)
			{
				Note = FString::Printf(TEXT("Resolved '%s' to '%s' (case-insensitive match)"), *Input, *Func->GetName());
			}
			else if (bInputHasSpaces
				&& (Func->GetName().Equals(InputNoSpaces, ESearchCase::IgnoreCase)
					|| Func->GetDisplayNameText().ToString().Equals(Input, ESearchCase::IgnoreCase)))
			{
				bMatched = true;
				Note = FString::Printf(TEXT("Resolved '%s' to '%s' (display-name match)"), *Input, *Func->GetName());
			}
			if (bMatched)
			{
				// Avoid duplicates
				bool bAlreadyAdded = false;
				for (const auto& Pair : FuzzyCandidates)
				{
					if (Pair.Key == Func)
					{
						bAlreadyAdded = true;
						break;
					}
				}
				if (!bAlreadyAdded)
				{
					FuzzyCandidates.Add({ Func, Note });
				}
			}
		}

		// Step 6: Resolution
		if (FuzzyCandidates.Num() == 1)
		{
			OutResult.bSuccess = true;
			OutResult.ResolvedName = FuzzyCandidates[0].Key->GetName();
			OutResult.ResolutionNote = FuzzyCandidates[0].Value;
			return FuzzyCandidates[0].Key;
		}

		if (FuzzyCandidates.Num() > 1)
		{
			OutResult.bSuccess = false;
			for (const auto& Pair : FuzzyCandidates)
			{
				OutResult.Candidates.Add(Pair.Key->GetName());
			}
			OutResult.Error = FString::Printf(
				TEXT("Ambiguous function name '%s' matched %d functions: %s"),
				*Input, FuzzyCandidates.Num(), *JoinCandidates(OutResult.Candidates));
			return nullptr;
		}

		OutResult.bSuccess = false;
		OutResult.Error = FString::Printf(TEXT("Function not found: '%s' on class '%s'"), *Input, *OwnerClass->GetName());
		return nullptr;
	}

	// =================================================================
	// ResolvePropertyName
	// =================================================================
	FProperty* ResolvePropertyName(
		UStruct* Struct,
		const FString& Input,
		FNameResolveResult& OutResult)
	{
		OutResult = FNameResolveResult();

		if (!IsValid(Struct))
		{
			OutResult.Error = TEXT("Struct is null");
			return nullptr;
		}

		if (Input.IsEmpty())
		{
			OutResult.Error = TEXT("Property name is empty");
			return nullptr;
		}

		// Step 1: Exact match
		if (FProperty* Found = Struct->FindPropertyByName(FName(*Input)))
		{
			OutResult.bSuccess = true;
			OutResult.ResolvedName = Found->GetName();
			return Found;
		}

		// Fuzzy candidates
		TArray<TPair<FProperty*, FString>> FuzzyCandidates;

		auto AddCandidate = [&](FProperty* P, const FString& Note)
		{
			for (const auto& Pair : FuzzyCandidates)
			{
				if (Pair.Key == P)
				{
					return;
				}
			}
			FuzzyCandidates.Add({ P, Note });
		};

		// Step 2: Case-insensitive
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			FProperty* Prop = *It;
			if (Prop->GetName().Equals(Input, ESearchCase::IgnoreCase))
			{
				AddCandidate(Prop, FString::Printf(TEXT("Resolved '%s' to '%s' (case-insensitive match)"), *Input, *Prop->GetName()));
			}
		}

		if (FuzzyCandidates.Num() == 1)
		{
			OutResult.bSuccess = true;
			OutResult.ResolvedName = FuzzyCandidates[0].Key->GetName();
			OutResult.ResolutionNote = FuzzyCandidates[0].Value;
			return FuzzyCandidates[0].Key;
		}

		// Step 3: Add "b" prefix (boolean convention)
		if (FuzzyCandidates.Num() == 0)
		{
			FString BPrefixed = TEXT("b") + Input;
			if (FProperty* Found = Struct->FindPropertyByName(FName(*BPrefixed)))
			{
				AddCandidate(Found, FString::Printf(TEXT("Resolved '%s' to '%s' (added b prefix)"), *Input, *Found->GetName()));
			}
		}

		// Step 4: Strip "b" prefix
		if (FuzzyCandidates.Num() == 0 && Input.Len() > 1 && Input[0] == TEXT('b') && FChar::IsUpper(Input[1]))
		{
			FString Stripped = Input.Mid(1);
			if (FProperty* Found = Struct->FindPropertyByName(FName(*Stripped)))
			{
				AddCandidate(Found, FString::Printf(TEXT("Resolved '%s' to '%s' (stripped b prefix)"), *Input, *Found->GetName()));
			}

			// Also try case-insensitive on stripped name
			if (FuzzyCandidates.Num() == 0)
			{
				for (TFieldIterator<FProperty> It(Struct); It; ++It)
				{
					FProperty* Prop = *It;
					if (Prop->GetName().Equals(Stripped, ESearchCase::IgnoreCase))
					{
						AddCandidate(Prop, FString::Printf(TEXT("Resolved '%s' to '%s' (stripped b prefix, case-insensitive)"), *Input, *Prop->GetName()));
					}
				}
			}
		}

		if (FuzzyCandidates.Num() == 1)
		{
			OutResult.bSuccess = true;
			OutResult.ResolvedName = FuzzyCandidates[0].Key->GetName();
			OutResult.ResolutionNote = FuzzyCandidates[0].Value;
			return FuzzyCandidates[0].Key;
		}

		// Step 5: Strip type suffixes
		if (FuzzyCandidates.Num() == 0)
		{
			static const TArray<FString> Suffixes = { TEXT("Property"), TEXT("Value"), TEXT("Ref") };
			for (const FString& Suffix : Suffixes)
			{
				if (Input.EndsWith(Suffix, ESearchCase::IgnoreCase) && Input.Len() > Suffix.Len())
				{
					FString Shortened = Input.Left(Input.Len() - Suffix.Len());

					// Try exact match on shortened
					if (FProperty* Found = Struct->FindPropertyByName(FName(*Shortened)))
					{
						AddCandidate(Found, FString::Printf(TEXT("Resolved '%s' to '%s' (stripped '%s' suffix)"), *Input, *Found->GetName(), *Suffix));
					}

					// Try case-insensitive on shortened
					if (FuzzyCandidates.Num() == 0)
					{
						for (TFieldIterator<FProperty> It(Struct); It; ++It)
						{
							FProperty* Prop = *It;
							if (Prop->GetName().Equals(Shortened, ESearchCase::IgnoreCase))
							{
								AddCandidate(Prop, FString::Printf(TEXT("Resolved '%s' to '%s' (stripped '%s' suffix, case-insensitive)"), *Input, *Prop->GetName(), *Suffix));
							}
						}
					}

					if (FuzzyCandidates.Num() > 0)
					{
						break; // Stop trying suffixes once we have candidates
					}
				}
			}
		}

		// Step 6: Resolution
		if (FuzzyCandidates.Num() == 1)
		{
			OutResult.bSuccess = true;
			OutResult.ResolvedName = FuzzyCandidates[0].Key->GetName();
			OutResult.ResolutionNote = FuzzyCandidates[0].Value;
			return FuzzyCandidates[0].Key;
		}

		if (FuzzyCandidates.Num() > 1)
		{
			OutResult.bSuccess = false;
			for (const auto& Pair : FuzzyCandidates)
			{
				OutResult.Candidates.Add(Pair.Key->GetName());
			}
			OutResult.Error = FString::Printf(
				TEXT("Ambiguous property name '%s' matched %d properties: %s"),
				*Input, FuzzyCandidates.Num(), *JoinCandidates(OutResult.Candidates));
			return nullptr;
		}

		OutResult.bSuccess = false;
		OutResult.Error = FString::Printf(TEXT("Property not found: '%s' on struct '%s'"), *Input, *Struct->GetName());
		return nullptr;
	}

	// =================================================================
	// ResolveStructName
	// =================================================================
	UScriptStruct* ResolveStructName(
		const FString& Input,
		FNameResolveResult& OutResult)
	{
		OutResult = FNameResolveResult();

		if (Input.IsEmpty())
		{
			OutResult.Error = TEXT("Struct name is empty");
			return nullptr;
		}

		// Lambda to try a single name via FindFirstObject
		auto TryFind = [](const FString& Name) -> UScriptStruct*
		{
			return FindFirstObject<UScriptStruct>(*Name, EFindFirstObjectOptions::NativeFirst);
		};

		// Step 1: Exact match (also tries stripping F prefix since UE internal
		// names omit the C++ struct prefix, e.g., "FVector" -> "Vector")
		if (UScriptStruct* Found = TryFind(Input); IsValid(Found))
		{
			OutResult.bSuccess = true;
			OutResult.ResolvedName = Found->GetName();
			return Found;
		}
		if (Input.Len() > 1 && FChar::ToUpper(Input[0]) == TEXT('F'))
		{
			if (UScriptStruct* Found = TryFind(Input.Mid(1)); IsValid(Found))
			{
				OutResult.bSuccess = true;
				OutResult.ResolvedName = Found->GetName();
				return Found;
			}
		}

		// Fuzzy candidates
		TArray<TPair<UScriptStruct*, FString>> FuzzyCandidates;

		auto AddCandidate = [&](UScriptStruct* S, const FString& Note)
		{
			for (const auto& Pair : FuzzyCandidates)
			{
				if (Pair.Key == S)
				{
					return;
				}
			}
			FuzzyCandidates.Add({ S, Note });
		};

		// Step 2: Add F prefix
		if (UScriptStruct* Found = TryFind(TEXT("F") + Input); IsValid(Found))
		{
			AddCandidate(Found, FString::Printf(TEXT("Resolved '%s' to '%s' (added F prefix)"), *Input, *Found->GetName()));
		}

		// Step 3: Strip F prefix
		if (FuzzyCandidates.Num() == 0 && Input.Len() > 1 && Input[0] == TEXT('F') && FChar::IsUpper(Input[1]))
		{
			if (UScriptStruct* Found = TryFind(Input.Mid(1)); IsValid(Found))
			{
				AddCandidate(Found, FString::Printf(TEXT("Resolved '%s' to '%s' (stripped F prefix)"), *Input, *Found->GetName()));
			}
		}

		if (FuzzyCandidates.Num() == 1)
		{
			OutResult.bSuccess = true;
			OutResult.ResolvedName = FuzzyCandidates[0].Key->GetName();
			OutResult.ResolutionNote = FuzzyCandidates[0].Value;
			return FuzzyCandidates[0].Key;
		}

		// Step 4: Try /Script/ module paths
		if (FuzzyCandidates.Num() == 0)
		{
			const TArray<FString> ResolverModules = GetResolverModules();
			for (const FString& Module : ResolverModules)
			{
				FString ScriptPath = FString::Printf(TEXT("/Script/%s.%s"), *Module, *Input);
				if (UScriptStruct* Found = TryFind(ScriptPath); IsValid(Found))
				{
					AddCandidate(Found, FString::Printf(TEXT("Resolved '%s' to '%s' (found in /Script/%s)"), *Input, *Found->GetName(), *Module));
				}
			}

			if (FuzzyCandidates.Num() == 1)
			{
				OutResult.bSuccess = true;
				OutResult.ResolvedName = FuzzyCandidates[0].Key->GetName();
				OutResult.ResolutionNote = FuzzyCandidates[0].Value;
				return FuzzyCandidates[0].Key;
			}
		}

		// Step 5: Case-insensitive iteration (most expensive, last resort)
		if (FuzzyCandidates.Num() == 0)
		{
			for (TObjectIterator<UScriptStruct> It; It; ++It)
			{
				UScriptStruct* S = *It;
				if (S->GetName().Equals(Input, ESearchCase::IgnoreCase))
				{
					AddCandidate(S, FString::Printf(TEXT("Resolved '%s' to '%s' (case-insensitive match)"), *Input, *S->GetName()));
				}
			}

			if (FuzzyCandidates.Num() == 1)
			{
				OutResult.bSuccess = true;
				OutResult.ResolvedName = FuzzyCandidates[0].Key->GetName();
				OutResult.ResolutionNote = FuzzyCandidates[0].Value;
				return FuzzyCandidates[0].Key;
			}
		}

		// Step 6: Resolution
		if (FuzzyCandidates.Num() > 1)
		{
			OutResult.bSuccess = false;
			for (const auto& Pair : FuzzyCandidates)
			{
				OutResult.Candidates.Add(Pair.Key->GetName());
			}
			OutResult.Error = FString::Printf(
				TEXT("Ambiguous struct name '%s' matched %d structs: %s"),
				*Input, FuzzyCandidates.Num(), *JoinCandidates(OutResult.Candidates));
			return nullptr;
		}

		OutResult.bSuccess = false;
		OutResult.Error = FString::Printf(TEXT("Struct not found: '%s'"), *Input);
		return nullptr;
	}

	// =================================================================
	// ResolveEnumName
	// =================================================================
	UEnum* ResolveEnumName(
		const FString& Input,
		FNameResolveResult& OutResult)
	{
		OutResult = FNameResolveResult();

		if (Input.IsEmpty())
		{
			OutResult.Error = TEXT("Enum name is empty");
			return nullptr;
		}

		// Lambda to try a single name via FindFirstObject
		auto TryFind = [](const FString& Name) -> UEnum*
		{
			return FindFirstObject<UEnum>(*Name, EFindFirstObjectOptions::NativeFirst);
		};

		// Step 1: Exact match (also tries stripping E prefix since UE internal
		// names may omit the C++ enum prefix)
		if (UEnum* Found = TryFind(Input); IsValid(Found))
		{
			OutResult.bSuccess = true;
			OutResult.ResolvedName = Found->GetName();
			return Found;
		}
		if (Input.Len() > 1 && FChar::ToUpper(Input[0]) == TEXT('E'))
		{
			if (UEnum* Found = TryFind(Input.Mid(1)); IsValid(Found))
			{
				OutResult.bSuccess = true;
				OutResult.ResolvedName = Found->GetName();
				return Found;
			}
		}

		// Fuzzy candidates
		TArray<TPair<UEnum*, FString>> FuzzyCandidates;

		auto AddCandidate = [&](UEnum* E, const FString& Note)
		{
			for (const auto& Pair : FuzzyCandidates)
			{
				if (Pair.Key == E)
				{
					return;
				}
			}
			FuzzyCandidates.Add({ E, Note });
		};

		// Step 2: Add E prefix
		if (UEnum* Found = TryFind(TEXT("E") + Input); IsValid(Found))
		{
			AddCandidate(Found, FString::Printf(TEXT("Resolved '%s' to '%s' (added E prefix)"), *Input, *Found->GetName()));
		}

		// Step 3: Strip E prefix
		if (FuzzyCandidates.Num() == 0 && Input.Len() > 1 && Input[0] == TEXT('E') && FChar::IsUpper(Input[1]))
		{
			if (UEnum* Found = TryFind(Input.Mid(1)); IsValid(Found))
			{
				AddCandidate(Found, FString::Printf(TEXT("Resolved '%s' to '%s' (stripped E prefix)"), *Input, *Found->GetName()));
			}
		}

		if (FuzzyCandidates.Num() == 1)
		{
			OutResult.bSuccess = true;
			OutResult.ResolvedName = FuzzyCandidates[0].Key->GetName();
			OutResult.ResolutionNote = FuzzyCandidates[0].Value;
			return FuzzyCandidates[0].Key;
		}

		// Step 4: Try /Script/ module paths
		if (FuzzyCandidates.Num() == 0)
		{
			const TArray<FString> ResolverModules = GetResolverModules();
			for (const FString& Module : ResolverModules)
			{
				FString ScriptPath = FString::Printf(TEXT("/Script/%s.%s"), *Module, *Input);
				if (UEnum* Found = TryFind(ScriptPath); IsValid(Found))
				{
					AddCandidate(Found, FString::Printf(TEXT("Resolved '%s' to '%s' (found in /Script/%s)"), *Input, *Found->GetName(), *Module));
				}
			}

			if (FuzzyCandidates.Num() == 1)
			{
				OutResult.bSuccess = true;
				OutResult.ResolvedName = FuzzyCandidates[0].Key->GetName();
				OutResult.ResolutionNote = FuzzyCandidates[0].Value;
				return FuzzyCandidates[0].Key;
			}
		}

		// Step 5: Case-insensitive iteration (most expensive, last resort)
		if (FuzzyCandidates.Num() == 0)
		{
			for (TObjectIterator<UEnum> It; It; ++It)
			{
				UEnum* E = *It;
				if (E->GetName().Equals(Input, ESearchCase::IgnoreCase))
				{
					AddCandidate(E, FString::Printf(TEXT("Resolved '%s' to '%s' (case-insensitive match)"), *Input, *E->GetName()));
				}
			}

			if (FuzzyCandidates.Num() == 1)
			{
				OutResult.bSuccess = true;
				OutResult.ResolvedName = FuzzyCandidates[0].Key->GetName();
				OutResult.ResolutionNote = FuzzyCandidates[0].Value;
				return FuzzyCandidates[0].Key;
			}
		}

		// Step 6: Resolution
		if (FuzzyCandidates.Num() > 1)
		{
			OutResult.bSuccess = false;
			for (const auto& Pair : FuzzyCandidates)
			{
				OutResult.Candidates.Add(Pair.Key->GetName());
			}
			OutResult.Error = FString::Printf(
				TEXT("Ambiguous enum name '%s' matched %d enums: %s"),
				*Input, FuzzyCandidates.Num(), *JoinCandidates(OutResult.Candidates));
			return nullptr;
		}

		OutResult.bSuccess = false;
		OutResult.Error = FString::Printf(TEXT("Enum not found: '%s'"), *Input);
		return nullptr;
	}

} // namespace ClaireonNameResolver
