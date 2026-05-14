// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonNameResolver.h"
#include "UObject/UObjectIterator.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"

namespace ClaireonNameResolver
{

	// -----------------------------------------------------------------
	// Static module list for /Script/<Module>.<Name> path resolution.
	// Downstream projects can extend this list at runtime through the
	// resolver's registration API rather than editing this file.
	// -----------------------------------------------------------------
	static const TArray<FString> KnownModules = {
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
	// Domain-specific class prefix map for ResolveClassName
	//
	// Keyed by the UE internal name of a base class (i.e., what
	// UClass::GetName() returns -- no C++ "U"/"A"/"F" prefix). Each entry
	// lists the bare prefix conventions that derived classes commonly use.
	// At resolve time, for a given RequiredBaseClass we look up its name
	// here and try prefix + Input as well as "U" + prefix + Input.
	//
	// Add new entries when a domain has a stable naming convention that
	// AI clients are likely to omit (e.g., "BTTask_", "AnimNotify_").
	// -----------------------------------------------------------------
	static const TMap<FString, TArray<FString>>& GetBaseClassPrefixMap()
	{
		static const TMap<FString, TArray<FString>> Map = {
			// Animation notifies. The default engine prefixes are listed;
			// downstream projects often add their own short-code prefixes
			// (e.g. "MyProjAN_") and can extend this map at runtime.
			{ TEXT("AnimNotify"), { TEXT("AnimNotify_") } },
			{ TEXT("AnimNotifyState"), { TEXT("AnimNotifyState_") } },

			// Behavior tree nodes. UBTNode is the common parent so it lists
			// every category; the specific bases narrow to their own prefix.
			{ TEXT("BTNode"), { TEXT("BTComposite_"), TEXT("BTTask_"), TEXT("BTDecorator_"), TEXT("BTService_") } },
			{ TEXT("BTCompositeNode"), { TEXT("BTComposite_") } },
			{ TEXT("BTTaskNode"), { TEXT("BTTask_") } },
			{ TEXT("BTDecorator"), { TEXT("BTDecorator_") } },
			{ TEXT("BTService"), { TEXT("BTService_") } },
		};
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
			return !RequiredBaseClass || C->IsChildOf(RequiredBaseClass);
		};

		// Lambda to try a single name via FindFirstObject
		auto TryFind = [&](const FString& Name) -> UClass*
		{
			UClass* Found = FindFirstObject<UClass>(*Name, EFindFirstObjectOptions::NativeFirst);
			if (Found && PassesBaseCheck(Found))
			{
				return Found;
			}
			return nullptr;
		};

		// Step 1: Exact match (also tries stripping A/U prefix since UE internal
		// names omit the C++ class prefix, e.g., "AActor" -> "Actor")
		if (UClass* Found = TryFind(Input))
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
				if (UClass* Found = TryFind(Input.Mid(1)))
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
			if (UClass* Found = TryFind(Input.Mid(1)))
			{
				AddCandidate(Found, FString::Printf(TEXT("Resolved '%s' to '%s' (stripped U prefix)"), *Input, *Found->GetName()));
			}
		}

		// Step 3: Add U prefix
		if (UClass* Found = TryFind(TEXT("U") + Input))
		{
			AddCandidate(Found, FString::Printf(TEXT("Resolved '%s' to '%s' (added U prefix)"), *Input, *Found->GetName()));
		}

		// Step 4: Add A prefix
		if (UClass* Found = TryFind(TEXT("A") + Input))
		{
			AddCandidate(Found, FString::Printf(TEXT("Resolved '%s' to '%s' (added A prefix)"), *Input, *Found->GetName()));
		}

		// Step 5: Add Component suffix
		if (UClass* Found = TryFind(Input + TEXT("Component")))
		{
			AddCandidate(Found, FString::Printf(TEXT("Resolved '%s' to '%s' (added Component suffix)"), *Input, *Found->GetName()));
		}

		// Step 6: U prefix + Component suffix
		if (UClass* Found = TryFind(TEXT("U") + Input + TEXT("Component")))
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
		if (FuzzyCandidates.Num() == 0 && RequiredBaseClass)
		{
			const TMap<FString, TArray<FString>>& PrefixMap = GetBaseClassPrefixMap();
			if (const TArray<FString>* Prefixes = PrefixMap.Find(RequiredBaseClass->GetName()))
			{
				for (const FString& Prefix : *Prefixes)
				{
					if (UClass* Found = TryFind(Prefix + Input))
					{
						AddCandidate(Found, FString::Printf(TEXT("Resolved '%s' to '%s' (added '%s' prefix)"), *Input, *Found->GetName(), *Prefix));
					}
					if (UClass* Found = TryFind(TEXT("U") + Prefix + Input))
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
			for (const FString& Module : KnownModules)
			{
				FString ScriptPath = FString::Printf(TEXT("/Script/%s.%s"), *Module, *Input);
				if (UClass* Found = TryFind(ScriptPath))
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
	// ResolvePinName
	// =================================================================
	UEdGraphPin* ResolvePinName(
		UEdGraphNode* Node,
		const FString& Input,
		EEdGraphPinDirection DirectionHint,
		FNameResolveResult& OutResult)
	{
		OutResult = FNameResolveResult();

		if (!Node)
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

		// Step 1: Exact match with direction, then without
		if (DirectionHint != EGPD_MAX)
		{
			if (UEdGraphPin* Found = Node->FindPin(*Input, DirectionHint))
			{
				OutResult.bSuccess = true;
				OutResult.ResolvedName = Found->GetName();
				return Found;
			}
		}
		if (UEdGraphPin* Found = Node->FindPin(*Input))
		{
			if (PassesDirection(Found))
			{
				OutResult.bSuccess = true;
				OutResult.ResolvedName = Found->GetName();
				return Found;
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

		if (!OwnerClass)
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
		if (UFunction* Found = OwnerClass->FindFunctionByName(FName(*Input)))
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
				if (UFunction* Found = OwnerClass->FindFunctionByName(FName(**MappedName)))
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
			if (UFunction* Found = OwnerClass->FindFunctionByName(FName(*K2Name)))
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
			if (UFunction* Found = OwnerClass->FindFunctionByName(FName(*Stripped)))
			{
				OutResult.bSuccess = true;
				OutResult.ResolvedName = Found->GetName();
				OutResult.ResolutionNote = FString::Printf(TEXT("Resolved '%s' to '%s' (stripped K2_ prefix)"), *Input, *Found->GetName());
				return Found;
			}
		}

		// Step 5: Case-insensitive
		TArray<TPair<UFunction*, FString>> FuzzyCandidates;
		for (TFieldIterator<UFunction> It(OwnerClass); It; ++It)
		{
			UFunction* Func = *It;
			if (Func->GetName().Equals(Input, ESearchCase::IgnoreCase))
			{
				FString Note = FString::Printf(TEXT("Resolved '%s' to '%s' (case-insensitive match)"), *Input, *Func->GetName());
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

		if (!Struct)
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
		if (UScriptStruct* Found = TryFind(Input))
		{
			OutResult.bSuccess = true;
			OutResult.ResolvedName = Found->GetName();
			return Found;
		}
		if (Input.Len() > 1 && FChar::ToUpper(Input[0]) == TEXT('F'))
		{
			if (UScriptStruct* Found = TryFind(Input.Mid(1)))
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
		if (UScriptStruct* Found = TryFind(TEXT("F") + Input))
		{
			AddCandidate(Found, FString::Printf(TEXT("Resolved '%s' to '%s' (added F prefix)"), *Input, *Found->GetName()));
		}

		// Step 3: Strip F prefix
		if (FuzzyCandidates.Num() == 0 && Input.Len() > 1 && Input[0] == TEXT('F') && FChar::IsUpper(Input[1]))
		{
			if (UScriptStruct* Found = TryFind(Input.Mid(1)))
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
			for (const FString& Module : KnownModules)
			{
				FString ScriptPath = FString::Printf(TEXT("/Script/%s.%s"), *Module, *Input);
				if (UScriptStruct* Found = TryFind(ScriptPath))
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
		if (UEnum* Found = TryFind(Input))
		{
			OutResult.bSuccess = true;
			OutResult.ResolvedName = Found->GetName();
			return Found;
		}
		if (Input.Len() > 1 && FChar::ToUpper(Input[0]) == TEXT('E'))
		{
			if (UEnum* Found = TryFind(Input.Mid(1)))
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
		if (UEnum* Found = TryFind(TEXT("E") + Input))
		{
			AddCandidate(Found, FString::Printf(TEXT("Resolved '%s' to '%s' (added E prefix)"), *Input, *Found->GetName()));
		}

		// Step 3: Strip E prefix
		if (FuzzyCandidates.Num() == 0 && Input.Len() > 1 && Input[0] == TEXT('E') && FChar::IsUpper(Input[1]))
		{
			if (UEnum* Found = TryFind(Input.Mid(1)))
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
			for (const FString& Module : KnownModules)
			{
				FString ScriptPath = FString::Printf(TEXT("/Script/%s.%s"), *Module, *Input);
				if (UEnum* Found = TryFind(ScriptPath))
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
