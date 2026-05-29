// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_UClassCheckAsyncActionDelegateSignatures.h"

#include "ClaireonPathResolver.h"
#include "Tools/FToolSchemaBuilder.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/CancellableAsyncAction.h"
#include "UObject/Class.h"
#include "UObject/Object.h"
#include "UObject/UnrealType.h"

FString ClaireonTool_UClassCheckAsyncActionDelegateSignatures::GetOperation() const
{
	return TEXT("check_async_action_delegate_signatures");
}

FString ClaireonTool_UClassCheckAsyncActionDelegateSignatures::GetDescription() const
{
	return TEXT(
		"Static-analysis lint for UCancellableAsyncAction subclasses (B33). "
		"K2Node_BaseAsyncTask emits one then-pin per BlueprintAssignable "
		"multicast delegate but only allocates pin-type metadata for the "
		"FIRST delegate signature it encounters -- subsequent delegates with "
		"heterogeneous signatures silently lose their typed output pins. "
		"This tool walks UCancellableAsyncAction-derived classes, groups "
		"BlueprintAssignable delegates by signature, and reports a warning "
		"when more than one distinct signature is present. Accepts a "
		"Blueprint asset path or a native class path "
		"(/Script/Module.ClassName). Read-only. Immediate-mode tool: no "
		"session required.");
}

TArray<FString> ClaireonTool_UClassCheckAsyncActionDelegateSignatures::GetSearchKeywords() const
{
	return {
		TEXT("uclass"),
		TEXT("static_analysis"),
		TEXT("lint"),
		TEXT("async_action"),
		TEXT("cancellable_async_action"),
		TEXT("delegate"),
		TEXT("signature"),
		TEXT("blueprint_assignable"),
		TEXT("then_pin"),
	};
}

TSharedPtr<FJsonObject> ClaireonTool_UClassCheckAsyncActionDelegateSignatures::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(
		TEXT("asset_or_class_path"),
		TEXT("Blueprint asset path (/Game/...) or native class path (/Script/Module.ClassName). Resolved to a UClass."),
		true);
	return Builder.Build();
}

namespace ClaireonToolUClassCheckAsyncActionDelegateSignatures_Internal
{
	UClass* ResolveTargetClass(const FString& AssetOrClassPath, FString& OutError)
	{
		const ClaireonPathResolver::FResolveResult Resolved = ClaireonPathResolver::Resolve(AssetOrClassPath);
		if (!Resolved.bSuccess)
		{
			OutError = Resolved.Error;
			return nullptr;
		}

		const FString& Path = Resolved.ResolvedPath.Path;

		if (Resolved.ResolvedPath.Kind == ClaireonPathResolver::EPathKind::NativeClassPath)
		{
			UClass* NativeClass = FindObject<UClass>(nullptr, *Path);
			if (!NativeClass)
			{
				NativeClass = LoadObject<UClass>(nullptr, *Path);
			}
			if (!NativeClass)
			{
				OutError = FString::Printf(
					TEXT("Could not resolve native class '%s'."),
					*Path);
				return nullptr;
			}
			return NativeClass;
		}

		// Blueprint asset path: load the Blueprint, return its GeneratedClass.
		if (UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *Path))
		{
			if (Blueprint->GeneratedClass)
			{
				return Blueprint->GeneratedClass;
			}
			OutError = FString::Printf(
				TEXT("Blueprint '%s' has no GeneratedClass (compile it first)."),
				*Path);
			return nullptr;
		}

		OutError = FString::Printf(
			TEXT("Could not load Blueprint at '%s' and path is not a native class."),
			*Path);
		return nullptr;
	}
}

IClaireonTool::FToolResult ClaireonTool_UClassCheckAsyncActionDelegateSignatures::Execute(
	const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetOrClassPath;
	if (!Arguments->TryGetStringField(TEXT("asset_or_class_path"), AssetOrClassPath))
	{
		return MakeErrorResult(TEXT("Missing required field: asset_or_class_path"));
	}

	FString ResolveError;
	UClass* TargetClass =
		ClaireonToolUClassCheckAsyncActionDelegateSignatures_Internal::ResolveTargetClass(
			AssetOrClassPath, ResolveError);
	if (!TargetClass)
	{
		return MakeErrorResult(ResolveError);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("class"), TargetClass->GetPathName());

	const bool bIsAsyncAction =
		TargetClass->IsChildOf(UCancellableAsyncAction::StaticClass());
	Data->SetBoolField(TEXT("is_cancellable_async_action"), bIsAsyncAction);

	if (!bIsAsyncAction)
	{
		Data->SetStringField(
			TEXT("note"),
			TEXT("Class is not a UCancellableAsyncAction subclass; lint does not apply."));
		return MakeSuccessResult(
			Data,
			FString::Printf(
				TEXT("check_async_action_delegate_signatures: %s is not a UCancellableAsyncAction subclass; skipped"),
				*TargetClass->GetName()));
	}

	// Group BlueprintAssignable multicast delegates by signature function.
	TMap<UFunction*, TArray<FString>> SignatureToDelegateNames;
	for (TFieldIterator<FProperty> PropIt(TargetClass); PropIt; ++PropIt)
	{
		FProperty* Property = *PropIt;
		if (!Property)
		{
			continue;
		}
		if (!Property->HasAnyPropertyFlags(CPF_BlueprintAssignable))
		{
			continue;
		}
		FMulticastDelegateProperty* MulticastProp = CastField<FMulticastDelegateProperty>(Property);
		if (!MulticastProp)
		{
			continue;
		}
		UFunction* SignatureFunction = MulticastProp->SignatureFunction;
		if (!SignatureFunction)
		{
			continue;
		}
		SignatureToDelegateNames.FindOrAdd(SignatureFunction).Add(Property->GetName());
	}

	TArray<TSharedPtr<FJsonValue>> SignatureGroupsArray;
	for (const TPair<UFunction*, TArray<FString>>& Pair : SignatureToDelegateNames)
	{
		TSharedPtr<FJsonObject> GroupObj = MakeShared<FJsonObject>();
		GroupObj->SetStringField(TEXT("signature_function"), Pair.Key->GetPathName());
		TArray<TSharedPtr<FJsonValue>> DelegateNamesArray;
		for (const FString& DelegateName : Pair.Value)
		{
			DelegateNamesArray.Add(MakeShared<FJsonValueString>(DelegateName));
		}
		GroupObj->SetArrayField(TEXT("delegates"), DelegateNamesArray);
		SignatureGroupsArray.Add(MakeShared<FJsonValueObject>(GroupObj));
	}
	Data->SetArrayField(TEXT("signature_groups"), SignatureGroupsArray);
	Data->SetNumberField(TEXT("distinct_signature_count"), SignatureToDelegateNames.Num());

	IClaireonTool::FToolResult Result;
	if (SignatureToDelegateNames.Num() > 1)
	{
		// Emit a single multi-line warning naming every heterogeneous delegate
		// so callers do not have to walk signature_groups themselves to
		// understand the problem.
		FString WarningMsg = FString::Printf(
			TEXT("UCancellableAsyncAction subclass '%s' declares %d distinct BlueprintAssignable delegate signatures; K2Node_BaseAsyncTask will silently drop typed pins for the non-primary delegates. Groups:"),
			*TargetClass->GetName(),
			SignatureToDelegateNames.Num());
		for (const TPair<UFunction*, TArray<FString>>& Pair : SignatureToDelegateNames)
		{
			WarningMsg += FString::Printf(
				TEXT("\n  - %s => %s"),
				*Pair.Key->GetName(),
				*FString::Join(Pair.Value, TEXT(", ")));
		}
		Data->SetBoolField(TEXT("has_heterogeneous_signatures"), true);
		Result = MakeSuccessResult(
			Data,
			FString::Printf(
				TEXT("check_async_action_delegate_signatures: %s has %d distinct signatures (heterogeneous)"),
				*TargetClass->GetName(),
				SignatureToDelegateNames.Num()));
		Result.Warnings.Add(WarningMsg);
	}
	else
	{
		Data->SetBoolField(TEXT("has_heterogeneous_signatures"), false);
		Result = MakeSuccessResult(
			Data,
			FString::Printf(
				TEXT("check_async_action_delegate_signatures: %s has %d delegate signature(s) (homogeneous)"),
				*TargetClass->GetName(),
				SignatureToDelegateNames.Num()));
	}

	return Result;
}
