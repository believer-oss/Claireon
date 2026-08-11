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
// call path. On SEH, bCaughtFatalException is true and OutClass is nullptr;
// callers should surface ExceptionDescription as a RuntimeError instead of
// crashing the editor.
struct FClaireonGeneratedClassLookupResult
{
	UClass* OutClass = nullptr;
	bool bCaughtFatalException = false;
	FString ExceptionDescription;
	uint32 ExceptionCode = 0;
};

namespace ClaireonSafeExec
{
	/**
	 * Reject arguments the tool's schema does not declare.
	 *
	 * Until this existed, an undeclared argument was silently dropped: the
	 * generated Python layer forwards unknown kwargs verbatim
	 * (payload.update(kwargs)) and the C++ side simply never reads them. A
	 * camel/snake-swapped or misspelled parameter was therefore a no-op reported
	 * as a success -- the mechanism behind most of the naming complaints in this
	 * catalog.
	 *
	 * Returns an empty string when the arguments are acceptable, otherwise the
	 * caller-facing error message. Validation is skipped (permissive) when the
	 * tool's schema has no "properties" object, since there is then nothing to
	 * check against; the registry-wide lint test is what catches those schemas.
	 *
	 * Exposed for tests.
	 */
	CLAIREON_API FString ValidateArgumentsAgainstSchema(
		const IClaireonTool* Tool,
		const TSharedPtr<FJsonObject>& Arguments);

	/** True for arguments every tool accepts regardless of its schema, because
	 *  the transport rather than the tool consumes them. */
	CLAIREON_API bool IsTransportLevelArgument(const FString& ArgumentName);

	FClaireonSafeExecResult ExecuteTool(IClaireonTool* Tool, const TSharedPtr<FJsonObject>& Arguments);
	FClaireonSafeActionResult ExecuteAction(TFunctionRef<void()> Action);

	// SEH-guarded wrapper around UBlueprintEditorLibrary::GeneratedClass.
	// Mirrors the Python-side unreal.BlueprintEditorLibrary.generated_class
	// call path that has been observed to raise SEH 0xC0000005 after
	// MulticastDelegate variable authoring + save sequences (see
	// Docs/llm/archive/bp-authoring-gaps-closure/GAP6_REPRO_ARTIFACT.md).
	// Callers that need the generated class from Python should route through
	// this helper via a Claireon tool, not call the BlueprintEditorLibrary
	// reflection path directly.
	FClaireonGeneratedClassLookupResult ExecuteGeneratedClassLookup(UBlueprint* Blueprint);

	bool DidLastExecutionCrash();
	void SetCrashFlag();
	void ClearCrashFlag();
} // namespace ClaireonSafeExec
