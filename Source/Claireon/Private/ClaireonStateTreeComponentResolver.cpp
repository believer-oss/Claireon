// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonStateTreeComponentResolver.h"

#include "Components/ActorComponent.h"
#include "Components/StateTreeComponent.h"
#include "GameFramework/Actor.h"

namespace ClaireonStateTreeComponentResolver
{
UActorComponent* FindStateTreeComponent(AActor* Actor, const FString& OptionalComponentClass)
{
	if (!IsValid(Actor))
	{
		return nullptr;
	}

	TArray<UActorComponent*> Components;
	Actor->GetComponents(Components);

	for (UActorComponent* Component : Components)
	{
		if (!IsValid(Component))
		{
			continue;
		}

		if (!OptionalComponentClass.IsEmpty())
		{
			if (Component->GetClass()->GetName().Contains(OptionalComponentClass, ESearchCase::IgnoreCase))
			{
				return Component;
			}
		}
		else if (Component->IsA(UStateTreeComponent::StaticClass()))
		{
			return Component;
		}
	}

	return nullptr;
}

FString DescribeComponents(AActor* Actor)
{
	FString Description;
	if (!IsValid(Actor))
	{
		return Description;
	}

	TArray<UActorComponent*> Components;
	Actor->GetComponents(Components);
	for (UActorComponent* Component : Components)
	{
		if (IsValid(Component))
		{
			Description += FString::Printf(TEXT("\n  - %s (%s)"), *Component->GetName(), *Component->GetClass()->GetName());
		}
	}
	return Description;
}
} // namespace ClaireonStateTreeComponentResolver
