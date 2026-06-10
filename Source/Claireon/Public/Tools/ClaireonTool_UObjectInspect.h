// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Stateless tool: reflection-based property inspector for any loaded UObject.
 *
 * Walks FProperty reflection directly to read UPROPERTY values, including
 * those that lack a Blueprint accessor specifier (BlueprintReadOnly /
 * BlueprintReadWrite / EditAnywhere) and those declared protected or
 * private in C++.
 *
 * Listing mode (no property_path): enumerates every reflected, non-skipped
 * UPROPERTY on the object with name, kind, cpp_type, access, bp_access, and
 * editor_access. The value field is included only when include_values=true.
 *
 * Targeted mode (property_path provided): returns the single property's
 * serialized value, supporting array indexing (Foo[0]) and dot-path nested
 * struct / object access. Component sub-properties are reached by traversing
 * the actor's component UPROPERTY by name (e.g. property_path
 * "MyComp.SomeField" on an actor object_path).
 *
 * Read-only. No setter surface is exposed by this tool; a write counterpart
 * is deferred to a separate work item.
 *
 * Object-path resolution covers:
 *  - Loaded assets (FindObject / StaticFindObject).
 *  - CDOs (native class path resolved to UClass, then GetDefaultObject).
 *  - World actors / sub-objects (FindObject by full path or ANY_PACKAGE
 *    fallback).
 *  - On-disk assets (StaticLoadObject) when allow_load=true (default).
 *
 * When allow_load=false and FindObject returns null for an asset path, the
 * tool reports an explicit "object not loaded" error rather than attempting
 * to load.
 */
class CLAIREON_API ClaireonTool_UObjectInspect : public IClaireonTool
{
public:
	FString GetCategory() const override { return TEXT("uobject"); }
	FString GetOperation() const override;
	FString GetDescription() const override;
	TArray<FString> GetSearchKeywords() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
