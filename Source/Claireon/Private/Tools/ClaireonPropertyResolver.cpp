// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonPropertyResolver.h"
#include "Tools/ClaireonPropertyUtils.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/Blueprint.h"

bool ClaireonPropertyResolver::ResolvePropertyOnActor(
	AActor* Actor,
	const FString& PropertyPath,
	FResolvedProperty& OutResolved,
	FString& OutError)
{
	// Step 1: Null check + parse the first path segment
	if (!Actor)
	{
		OutError = TEXT("Null actor");
		return false;
	}

	FString FirstSegment;
	FString Remainder;
	if (!PropertyPath.Split(TEXT("."), &FirstSegment, &Remainder))
	{
		FirstSegment = PropertyPath;
		Remainder = FString();
	}

	// Step 2: Check if first segment is a component name
	TArray<UActorComponent*> AllComponents;
	Actor->GetComponents(AllComponents);

	for (UActorComponent* Component : AllComponents)
	{
		if (!Component)
		{
			continue;
		}
		if (Component->GetName() == FirstSegment)
		{
			if (Remainder.IsEmpty())
			{
				OutError = FString::Printf(TEXT("'%s' is a component name, not a property. Use 'ComponentName.PropertyName' syntax."), *FirstSegment);
				return false;
			}

			// Verify the first segment of Remainder exists as a property on the component
			FString RemainderFirst;
			FString RemainderRest;
			if (!Remainder.Split(TEXT("."), &RemainderFirst, &RemainderRest))
			{
				RemainderFirst = Remainder;
			}
			if (Component->GetClass()->FindPropertyByName(FName(*RemainderFirst)))
			{
				OutResolved.TargetObject = Component;
				OutResolved.ResolvedOn = Component->GetName();
				OutResolved.RemainingPath = Remainder;
				OutResolved.QualifiedPath = PropertyPath;
				OutResolved.Note = TEXT("Resolved via explicit component name prefix");
				return true;
			}
		}
	}

	// Step 3: Check actor root
	if (Actor->GetClass()->FindPropertyByName(FName(*FirstSegment)))
	{
		OutResolved.TargetObject = Actor;
		OutResolved.ResolvedOn = TEXT("Actor");
		OutResolved.RemainingPath = PropertyPath;
		OutResolved.QualifiedPath = PropertyPath;
		OutResolved.Note = FString();
		return true;
	}

	// Step 4: Check RootComponent
	USceneComponent* RootComp = Actor->GetRootComponent();
	if (RootComp && RootComp->GetClass()->FindPropertyByName(FName(*FirstSegment)))
	{
		OutResolved.TargetObject = RootComp;
		OutResolved.ResolvedOn = TEXT("RootComponent");
		OutResolved.RemainingPath = PropertyPath;
		OutResolved.QualifiedPath = RootComp->GetName() + TEXT(".") + PropertyPath;
		OutResolved.Note = TEXT("Property not found on actor root; found on RootComponent");
		return true;
	}

	// Step 5: Iterate all components (excluding RootComponent, already checked)
	for (UActorComponent* Component : AllComponents)
	{
		if (!Component || Component == RootComp)
		{
			continue;
		}
		if (Component->GetClass()->FindPropertyByName(FName(*FirstSegment)))
		{
			OutResolved.TargetObject = Component;
			OutResolved.ResolvedOn = Component->GetName();
			OutResolved.RemainingPath = PropertyPath;
			OutResolved.QualifiedPath = Component->GetName() + TEXT(".") + PropertyPath;
			OutResolved.Note = FString::Printf(TEXT("Property not found on actor root; found on component '%s'"), *Component->GetName());
			return true;
		}
	}

	// Step 6: Not found -- build descriptive error
	OutError = FString::Printf(TEXT("Property '%s' not found. Searched: %s (actor root)"), *FirstSegment, *Actor->GetClass()->GetName());
	if (RootComp)
	{
		OutError += FString::Printf(TEXT(", %s (RootComponent)"), *RootComp->GetClass()->GetName());
	}
	for (UActorComponent* Component : AllComponents)
	{
		if (!Component || Component == RootComp)
		{
			continue;
		}
		OutError += FString::Printf(TEXT(", %s (%s)"), *Component->GetClass()->GetName(), *Component->GetName());
	}
	return false;
}

FString ClaireonPropertyResolver::ReadPropertyOnActor(
	AActor* Actor,
	const FString& PropertyPath,
	FResolvedProperty& OutResolved,
	FString& OutError)
{
	if (!ResolvePropertyOnActor(Actor, PropertyPath, OutResolved, OutError))
	{
		return FString();
	}
	return ClaireonPropertyUtils::ReadPropertyByPath(OutResolved.TargetObject, OutResolved.RemainingPath, OutError);
}

bool ClaireonPropertyResolver::WritePropertyOnActor(
	AActor* Actor,
	const FString& PropertyPath,
	const FString& Value,
	FResolvedProperty& OutResolved,
	FString& OutError)
{
	if (!ResolvePropertyOnActor(Actor, PropertyPath, OutResolved, OutError))
	{
		return false;
	}
	// When the resolved target is a component (not the actor itself), call Modify()
	// on the component so the undo system captures its pre-modification state.
	// The caller is expected to have already called Actor->Modify() within a FScopedTransaction.
	if (OutResolved.TargetObject != Actor)
	{
		OutResolved.TargetObject->Modify();
	}
	return ClaireonPropertyUtils::WritePropertyByPath(OutResolved.TargetObject, OutResolved.RemainingPath, Value, OutError);
}

namespace ClaireonPropertyResolverInternal
{

// Strip an optional "[N]" suffix from a single path segment so that name lookups
// on component/property registries work for array-indexed first segments like
// "Waves[0]" or "MyArray[12]". Leaves non-indexed segments unchanged.
// Returns true if the input was well-formed (either no bracket or matched [digits]).
// If the input has an unmatched '[' or a non-numeric index, returns false and
// leaves OutBareName unchanged so the caller can forward the raw segment to the
// downstream parser (which emits the canonical "Malformed array index" error).
bool StripArrayIndexSuffix(const FString& Segment, FString& OutBareName)
{
	int32 BracketPos;
	if (!Segment.FindChar(TEXT('['), BracketPos))
	{
		OutBareName = Segment;
		return true;
	}
	int32 CloseBracket;
	if (!Segment.FindChar(TEXT(']'), CloseBracket) || CloseBracket <= BracketPos + 1)
	{
		// Malformed -- let the writer's parser produce the canonical error.
		return false;
	}
	const FString IndexStr = Segment.Mid(BracketPos + 1, CloseBracket - BracketPos - 1);
	if (!IndexStr.IsNumeric())
	{
		return false;
	}
	OutBareName = Segment.Left(BracketPos);
	return true;
}

}  // namespace ClaireonPropertyResolverInternal

bool ClaireonPropertyResolver::ResolvePropertyOnBlueprintCDO(
	UBlueprint* Blueprint,
	const FString& PropertyPath,
	FResolvedProperty& OutResolved,
	FString& OutError)
{
	// Step 1: Validate inputs and get CDO
	if (!Blueprint)
	{
		OutError = TEXT("Null Blueprint");
		return false;
	}
	if (!Blueprint->GeneratedClass)
	{
		OutError = TEXT("Blueprint has no GeneratedClass");
		return false;
	}
	UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject();
	if (!CDO)
	{
		OutError = TEXT("Failed to get CDO");
		return false;
	}

	// Step 2: Parse first segment and check for SCS component name prefix.
	// For paths like "MyArray[0].Member", the first segment ("MyArray[0]")
	// still needs to be looked up by its bare name ("MyArray") against the
	// CDO class and component templates -- the "[0]" index is forwarded to
	// the writer in RemainingPath.
	FString FirstSegment;
	FString Remainder;
	if (!PropertyPath.Split(TEXT("."), &FirstSegment, &Remainder))
	{
		FirstSegment = PropertyPath;
		Remainder = FString();
	}

	FString BareFirst;
	if (!ClaireonPropertyResolverInternal::StripArrayIndexSuffix(FirstSegment, BareFirst))
	{
		// Malformed bracket syntax on the first segment. Pass the whole path
		// straight through to the writer so ClaireonPropertyUtils::ParsePathSegments
		// emits the canonical "Malformed array index in '...'" error. We pick
		// the CDO as the target because we have no better choice; the writer
		// will fail during parse before touching any property state.
		OutResolved.TargetObject = CDO;
		OutResolved.ResolvedOn = TEXT("CDO");
		OutResolved.RemainingPath = PropertyPath;
		OutResolved.QualifiedPath = PropertyPath;
		OutResolved.Note = FString();
		return true;
	}

	if (Blueprint->SimpleConstructionScript)
	{
		for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
		{
			if (!Node || !Node->ComponentTemplate)
			{
				continue;
			}
			if (Node->GetVariableName().ToString() == BareFirst)
			{
				if (Remainder.IsEmpty())
				{
					OutError = FString::Printf(TEXT("'%s' is a component name, not a property. Use 'ComponentName.PropertyName' syntax."), *FirstSegment);
					return false;
				}
				OutResolved.TargetObject = Node->ComponentTemplate;
				OutResolved.ResolvedOn = Node->GetVariableName().ToString();
				OutResolved.RemainingPath = Remainder;
				OutResolved.QualifiedPath = PropertyPath;
				OutResolved.Note = TEXT("Resolved via explicit SCS component name prefix");
				return true;
			}
		}
	}

	// Step 2b: explicit native-subobject prefix. Walk the CDO's default
	// subobjects (which includes inherited native components declared on a
	// parent actor/class). Match by FName so that
	// `MyComponent.SomeProperty` resolves to the inherited template even when
	// the BP does not redeclare it in SCS.
	{
		TArray<UObject*> DefaultSubobjects;
		CDO->GetDefaultSubobjects(DefaultSubobjects);
		for (UObject* Subobject : DefaultSubobjects)
		{
			if (!Subobject)
			{
				continue;
			}
			if (Subobject->GetFName().ToString() == BareFirst)
			{
				if (Remainder.IsEmpty())
				{
					OutError = FString::Printf(TEXT("'%s' is a subobject name, not a property. Use 'SubobjectName.PropertyName' syntax."), *FirstSegment);
					return false;
				}
				OutResolved.TargetObject = Subobject;
				OutResolved.ResolvedOn = Subobject->GetName();
				OutResolved.RemainingPath = Remainder;
				OutResolved.QualifiedPath = PropertyPath;
				OutResolved.Note = TEXT("Resolved via explicit native subobject name prefix");
				return true;
			}
		}
	}

	// Step 3: Check CDO class
	if (CDO->GetClass()->FindPropertyByName(FName(*BareFirst)))
	{
		OutResolved.TargetObject = CDO;
		OutResolved.ResolvedOn = TEXT("CDO");
		OutResolved.RemainingPath = PropertyPath;
		OutResolved.QualifiedPath = PropertyPath;
		OutResolved.Note = FString();
		return true;
	}

	// Step 4: Check SCS component templates
	if (Blueprint->SimpleConstructionScript)
	{
		for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
		{
			if (!Node || !Node->ComponentTemplate)
			{
				continue;
			}
			if (Node->ComponentTemplate->GetClass()->FindPropertyByName(FName(*BareFirst)))
			{
				OutResolved.TargetObject = Node->ComponentTemplate;
				OutResolved.ResolvedOn = Node->GetVariableName().ToString();
				OutResolved.RemainingPath = PropertyPath;
				OutResolved.QualifiedPath = Node->GetVariableName().ToString() + TEXT(".") + PropertyPath;
				OutResolved.Note = FString::Printf(TEXT("Property not found on CDO; found on component template '%s'"), *Node->GetVariableName().ToString());
				return true;
			}
		}
	}

	// Step 4b (B31): walk CDO's native subobject templates as a fallback for
	// callers who name the property without the "Subobject." prefix. The
	// inherited native template owns the property on its own class; the
	// resolver redirects the write to that template.
	{
		TArray<UObject*> DefaultSubobjects;
		CDO->GetDefaultSubobjects(DefaultSubobjects);
		for (UObject* Subobject : DefaultSubobjects)
		{
			if (!Subobject)
			{
				continue;
			}
			if (Subobject->GetClass()->FindPropertyByName(FName(*BareFirst)))
			{
				const FString SubobjectName = Subobject->GetName();
				OutResolved.TargetObject = Subobject;
				OutResolved.ResolvedOn = SubobjectName;
				OutResolved.RemainingPath = PropertyPath;
				OutResolved.QualifiedPath = SubobjectName + TEXT(".") + PropertyPath;
				OutResolved.Note = FString::Printf(TEXT("Property not found on CDO; found on inherited native subobject '%s'"), *SubobjectName);
				return true;
			}
		}
	}

	// Step 5: Not found -- build descriptive error
	OutError = FString::Printf(TEXT("Property '%s' not found. Searched: %s (CDO)"), *FirstSegment, *CDO->GetClass()->GetName());
	if (Blueprint->SimpleConstructionScript)
	{
		for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
		{
			if (!Node || !Node->ComponentTemplate)
			{
				continue;
			}
			OutError += FString::Printf(TEXT(", %s (%s)"), *Node->ComponentTemplate->GetClass()->GetName(), *Node->GetVariableName().ToString());
		}
	}
	{
		TArray<UObject*> DefaultSubobjects;
		CDO->GetDefaultSubobjects(DefaultSubobjects);
		for (UObject* Subobject : DefaultSubobjects)
		{
			if (!Subobject)
			{
				continue;
			}
			OutError += FString::Printf(TEXT(", %s (%s) [inherited]"),
				*Subobject->GetClass()->GetName(), *Subobject->GetName());
		}
	}
	return false;
}
