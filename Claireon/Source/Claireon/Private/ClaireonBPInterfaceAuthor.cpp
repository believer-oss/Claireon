// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonBPInterfaceAuthor.h"
#include "ClaireonNameResolver.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "ClaireonBPInterfaceAuthor"

namespace ClaireonBPInterfaceAuthor
{
	/** Collect post-op interface short names into the result. */
	static void CollectPostOpNames(UBlueprint* Blueprint, FInterfaceOpResult& Out)
	{
		if (!Blueprint)
		{
			return;
		}
		for (const FBPInterfaceDescription& I : Blueprint->ImplementedInterfaces)
		{
			if (I.Interface)
			{
				Out.PostOpInterfaceNames.Add(I.Interface->GetName());
			}
		}
	}

	FInterfaceOpResult Implement(UBlueprint* Blueprint, const FString& InterfaceName)
	{
		FInterfaceOpResult Out;

		if (!Blueprint)
		{
			Out.Error = TEXT("Blueprint is null");
			return Out;
		}
		if (InterfaceName.IsEmpty())
		{
			Out.Error = TEXT("Interface class name is empty");
			return Out;
		}

		ClaireonNameResolver::FNameResolveResult ResolveResult;
		UClass* Class = ClaireonNameResolver::ResolveClassName(InterfaceName, nullptr, ResolveResult);
		Out.ResolutionNote = ResolveResult.ResolutionNote;

		if (!Class)
		{
			Out.Error = FString::Printf(TEXT("Could not resolve interface class '%s': %s"),
				*InterfaceName,
				*ResolveResult.Error);
			return Out;
		}

		if (!Class->HasAnyClassFlags(CLASS_Interface))
		{
			Out.Error = FString::Printf(TEXT("'%s' is not an interface class"), *Class->GetName());
			return Out;
		}

		// Pre-check: already implemented?
		for (const FBPInterfaceDescription& Existing : Blueprint->ImplementedInterfaces)
		{
			if (Existing.Interface == Class)
			{
				Out.Error = FString::Printf(TEXT("Interface '%s' is already implemented"), *Class->GetName());
				return Out;
			}
		}

		FScopedTransaction Transaction(LOCTEXT("ImplementInterface", "Implement Blueprint Interface"));
		Blueprint->Modify();

		const bool bAdded = FBlueprintEditorUtils::ImplementNewInterface(Blueprint, Class->GetClassPathName());
		if (!bAdded)
		{
			// Engine-level boolean failure -- pre-checks above should have caught the
			// known modes on UE 5.5, so surface a generic error here.
			Out.Error = FString::Printf(TEXT("Failed to implement interface '%s'"), *Class->GetName());
			return Out;
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		FKismetEditorUtilities::CompileBlueprint(Blueprint);

		Out.ResolvedClassName = Class->GetName();
		CollectPostOpNames(Blueprint, Out);
		Out.bSucceeded = true;
		return Out;
	}

	FInterfaceOpResult Remove(UBlueprint* Blueprint, const FString& InterfaceName)
	{
		FInterfaceOpResult Out;

		if (!Blueprint)
		{
			Out.Error = TEXT("Blueprint is null");
			return Out;
		}
		if (InterfaceName.IsEmpty())
		{
			Out.Error = TEXT("Interface class name is empty");
			return Out;
		}

		ClaireonNameResolver::FNameResolveResult ResolveResult;
		UClass* Class = ClaireonNameResolver::ResolveClassName(InterfaceName, nullptr, ResolveResult);
		Out.ResolutionNote = ResolveResult.ResolutionNote;

		if (!Class)
		{
			Out.Error = FString::Printf(TEXT("Could not resolve interface class '%s': %s"),
				*InterfaceName,
				*ResolveResult.Error);
			return Out;
		}

		if (!Class->HasAnyClassFlags(CLASS_Interface))
		{
			Out.Error = FString::Printf(TEXT("'%s' is not an interface class"), *Class->GetName());
			return Out;
		}

		// Pre-check: is this class actually currently implemented?
		bool bFound = false;
		for (const FBPInterfaceDescription& Existing : Blueprint->ImplementedInterfaces)
		{
			if (Existing.Interface == Class)
			{
				bFound = true;
				break;
			}
		}
		if (!bFound)
		{
			Out.Error = FString::Printf(TEXT("Interface '%s' is not implemented on this Blueprint"), *Class->GetName());
			return Out;
		}

		FScopedTransaction Transaction(LOCTEXT("RemoveInterface", "Remove Blueprint Interface"));
		Blueprint->Modify();

		FBlueprintEditorUtils::RemoveInterface(Blueprint, Class->GetClassPathName(), /*bPreserveFunctions=*/false);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		FKismetEditorUtilities::CompileBlueprint(Blueprint);

		Out.ResolvedClassName = Class->GetName();
		CollectPostOpNames(Blueprint, Out);
		Out.bSucceeded = true;
		return Out;
	}
}

#undef LOCTEXT_NAMESPACE
