// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "StateTreeSchema.h"
#include "UObject/UObjectIterator.h"

// Mirror-safe StateTree-schema discovery for tests.
//
// Claireon ships no UStateTreeSchema of its own -- concrete schemas are provided by
// the host engine/plugins or the game. Tests must therefore not hardcode a specific
// schema class (that would name a project-specific type and only run in one repo).
// Instead, discover any concrete UStateTreeSchema subclass present in the running
// project. Call sites use the first result as a statetree_create schema and skip
// gracefully when the list is empty (e.g. a bare host with no schema-providing plugin).
namespace ClaireonTestSchemaDiscovery
{
	// All concrete (instantiable) UStateTreeSchema subclasses currently loaded.
	// Engine base schemas are ordered after project/plugin ones so a test picks a
	// fully-featured game schema when one exists, without ever naming it.
	inline TArray<FString> FindConcreteStateTreeSchemaClassPaths()
	{
		TArray<FString> ProjectSchemas;
		TArray<FString> EngineSchemas;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Cls = *It;
			if (Cls == UStateTreeSchema::StaticClass()
				|| !Cls->IsChildOf(UStateTreeSchema::StaticClass())
				|| Cls->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
			{
				continue;
			}
			const FString Path = Cls->GetPathName();
			if (Path.StartsWith(TEXT("/Script/StateTreeModule"))
				|| Path.StartsWith(TEXT("/Script/GameplayStateTreeModule")))
			{
				EngineSchemas.Add(Path);
			}
			else
			{
				ProjectSchemas.Add(Path);
			}
		}
		ProjectSchemas.Sort();
		EngineSchemas.Sort();
		ProjectSchemas.Append(EngineSchemas);
		return ProjectSchemas;
	}

	// First available concrete schema class path, or empty if none exist.
	inline FString FindConcreteStateTreeSchemaClassPath()
	{
		const TArray<FString> All = FindConcreteStateTreeSchemaClassPaths();
		return All.Num() > 0 ? All[0] : FString();
	}
}
