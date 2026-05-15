// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Tools/IClaireonTool.h"

class UBlueprint;
class UClass;

struct FClaireonSafeExecResult
{
	IClaireonTool::FToolResult ToolResult;
	bool bCaughtFatalException = false;
	FString ExceptionDescription;
	uint32 ExceptionCode = 0;
};

struct FClaireonSafeActionResult
{
	bool bSuccess = true;
	bool bCaughtFatalException = false;
	FString ExceptionDescription;
};

// Result envelope for the guarded BlueprintEditorLibrary::GeneratedClass
// call path (Gap 6 of #0000).  On SEH, bCaughtFatalException is true and
// OutClass is nullptr; callers should surface ExceptionDescription as a
// RuntimeError instead of crashing the editor.
struct FClaireonGeneratedClassLookupResult
{
	UClass* OutClass = nullptr;
	bool bCaughtFatalException = false;
	FString ExceptionDescription;
	uint32 ExceptionCode = 0;
};

namespace ClaireonSafeExec
{
	FClaireonSafeExecResult ExecuteTool(IClaireonTool* Tool, const TSharedPtr<FJsonObject>& Arguments);
	FClaireonSafeActionResult ExecuteAction(TFunctionRef<void()> Action);

	// SEH-guarded wrapper around UBlueprintEditorLibrary::GeneratedClass.
	// Mirrors the Python-side unreal.BlueprintEditorLibrary.generated_class
	// call path that has been observed to raise SEH 0xC0000005 after
	// MulticastDelegate variable authoring + save sequences (see
	// BP_AUTHORING_GAPS_CLOSURE/GAP6_REPRO_ARTIFACT.md).  Callers that need
	// the generated class from Python should route through this helper via
	// a Claireon tool, not call the BlueprintEditorLibrary reflection path
	// directly.
	FClaireonGeneratedClassLookupResult ExecuteGeneratedClassLookup(UBlueprint* Blueprint);

	bool DidLastExecutionCrash();
	void SetCrashFlag();
	void ClearCrashFlag();
} // namespace ClaireonSafeExec
