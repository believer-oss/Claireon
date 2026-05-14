// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
// Test-only UCLASS fixtures for ClaireonPropertyUtils unit tests. Lives under
// Private/Tests so the types are not part of the module's public surface.
#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "ClaireonTestTypes.generated.h"

// Holder UCLASS used to verify CreateInstancedArrayElement rejects an array
// whose Inner FObjectProperty does not carry CPF_InstancedReference. Kept
// minimal and free of project-module dependencies so the test stays inside Claireon's module
// dependency surface.
UCLASS()
class UClaireonTestNonInstancedHolder : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category = "Test")
	TArray<UObject*> NonInstancedArray;
};
