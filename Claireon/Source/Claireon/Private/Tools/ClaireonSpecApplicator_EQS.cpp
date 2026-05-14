// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSpecApplicator_EQS.h"
#include "Tools/ClaireonBehaviorTreeHelpers.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "ClaireonSafeExec.h"
#include "ClaireonLog.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Package.h"
#include "FileHelpers.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryTest.h"

namespace
{
	TArray<UEnvQueryOption*>& GetOptionsMutable(UEnvQuery* InQuery)
	{
		static FArrayProperty* OptionsProp = nullptr;
		if (!OptionsProp)
		{
			OptionsProp = CastField<FArrayProperty>(FindFProperty<FProperty>(UEnvQuery::StaticClass(), TEXT("Options")));
		}
		check(OptionsProp);
		return *OptionsProp->ContainerPtrToValuePtr<TArray<UEnvQueryOption*>>(InQuery);
	}

	UClass* ResolveEQSNodeClass(const FString& ClassName, UClass* BaseClass, const FString& BasePrefix, FString& OutError)
	{
		UClass* FoundClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
		if (FoundClass && FoundClass->IsChildOf(BaseClass))
		{
			return FoundClass;
		}

		FString PrefixedName = BasePrefix + ClassName;
		FoundClass = FindFirstObject<UClass>(*PrefixedName, EFindFirstObjectOptions::NativeFirst);
		if (FoundClass && FoundClass->IsChildOf(BaseClass))
		{
			return FoundClass;
		}

		if (ClassName.StartsWith(TEXT("U")))
		{
			FString WithoutU = ClassName.Mid(1);
			FoundClass = FindFirstObject<UClass>(*WithoutU, EFindFirstObjectOptions::NativeFirst);
			if (FoundClass && FoundClass->IsChildOf(BaseClass))
			{
				return FoundClass;
			}
		}

		OutError = FString::Printf(TEXT("Could not resolve EQS class: %s (expected subclass of %s)"), *ClassName, *BaseClass->GetName());
		return nullptr;
	}

	bool SpecApplicatorEQS_SetEQSProperty(UObject* Node, const FString& PropertyName, const FString& PropertyValue, FString& OutError)
	{
		if (!Node)
		{
			OutError = TEXT("Node is null");
			return false;
		}

		FProperty* Property = FindFProperty<FProperty>(Node->GetClass(), *PropertyName);
		if (!Property)
		{
			OutError = FString::Printf(TEXT("Property '%s' not found on %s"), *PropertyName, *Node->GetClass()->GetName());
			return false;
		}

		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Node);
		const TCHAR* Result = Property->ImportText_Direct(*PropertyValue, ValuePtr, Node, PPF_None);
		if (!Result)
		{
			OutError = FString::Printf(TEXT("Failed to set property '%s' to '%s'"), *PropertyName, *PropertyValue);
			return false;
		}

		return true;
	}
} // namespace

bool FClaireonSpecApplicator_EQS::ValidateToolSpec(const TSharedPtr<FJsonObject>& Spec, TArray<FString>& OutErrors)
{
	const TArray<TSharedPtr<FJsonValue>>* OptionsArray = nullptr;
	if (!Spec->TryGetArrayField(TEXT("options"), OptionsArray) || !OptionsArray)
	{
		OutErrors.Add(TEXT("EQS spec must contain an 'options' array"));
		return false;
	}

	for (int32 i = 0; i < OptionsArray->Num(); ++i)
	{
		const TSharedPtr<FJsonValue>& OptVal = (*OptionsArray)[i];
		if (!OptVal.IsValid() || OptVal->Type != EJson::Object) continue;
		const TSharedPtr<FJsonObject>& OptObj = OptVal->AsObject();

		FString OptId;
		if (!OptObj->TryGetStringField(TEXT("id"), OptId) || OptId.IsEmpty())
		{
			OutErrors.Add(FString::Printf(TEXT("options[%d]: missing or empty 'id'"), i));
		}

		// Generator type is required
		const TSharedPtr<FJsonObject>* GenPtr = nullptr;
		if (OptObj->TryGetObjectField(TEXT("generator"), GenPtr) && GenPtr && (*GenPtr).IsValid())
		{
			FString GenType;
			if (!(*GenPtr)->TryGetStringField(TEXT("type"), GenType) || GenType.IsEmpty())
			{
				OutErrors.Add(FString::Printf(TEXT("options[%d].generator: missing 'type'"), i));
			}
		}

		// Validate tests
		const TArray<TSharedPtr<FJsonValue>>* TestsArray = nullptr;
		if (OptObj->TryGetArrayField(TEXT("tests"), TestsArray) && TestsArray)
		{
			for (int32 t = 0; t < TestsArray->Num(); ++t)
			{
				const TSharedPtr<FJsonObject>& TestObj = (*TestsArray)[t]->AsObject();
				if (!TestObj.IsValid()) continue;

				FString TestId, TestType;
				if (!TestObj->TryGetStringField(TEXT("id"), TestId) || TestId.IsEmpty())
					OutErrors.Add(FString::Printf(TEXT("options[%d].tests[%d]: missing 'id'"), i, t));
				if (!TestObj->TryGetStringField(TEXT("type"), TestType) || TestType.IsEmpty())
					OutErrors.Add(FString::Printf(TEXT("options[%d].tests[%d]: missing 'type'"), i, t));
			}
		}
	}

	return OutErrors.Num() == 0;
}

bool FClaireonSpecApplicator_EQS::OpenOrCreateAsset(const FString& AssetPath, FString& OutSessionId, FString& OutError)
{
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		OutError = ResolveResult.Error;
		return false;
	}
	const FString ResolvedPath = ResolveResult.ResolvedPath.Path;

	UEnvQuery* EQS = ClaireonBehaviorTreeHelpers::LoadEQSAsset(ResolvedPath, OutError);
	if (!EQS)
	{
		return false;
	}

	const FString EQSPathName = EQS->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(
		EQSPathName, TEXT("eqs_edit"));

	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		OutError = FString::Printf(TEXT("Asset is locked by %s session %s"),
			*Blocker.ToolName, *Blocker.SessionId);
		return false;
	}
	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		OutError = FString::Printf(TEXT("Invalid asset path: %s"), *EQSPathName);
		return false;
	}

	Query = EQS;
	OutSessionId = OpenResult.SessionId;
	return true;
}

bool FClaireonSpecApplicator_EQS::ApplyPass1_CreateEntities(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec)
{
	UEnvQuery* EQS = Query.Get();
	if (!EQS)
	{
		AddError(TEXT("EQS Query is no longer valid"));
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* OptionsArray = nullptr;
	if (!Spec->TryGetArrayField(TEXT("options"), OptionsArray) || !OptionsArray)
	{
		return true;
	}

	int32 SuccessCount = 0;

	for (int32 i = 0; i < OptionsArray->Num(); ++i)
	{
		const TSharedPtr<FJsonObject>& OptObj = (*OptionsArray)[i]->AsObject();
		if (!OptObj.IsValid()) continue;

		FString SpecId;
		OptObj->TryGetStringField(TEXT("id"), SpecId);

		// Resolve generator class
		FString GeneratorClassName;
		const TSharedPtr<FJsonObject>* GenPtr = nullptr;
		if (OptObj->TryGetObjectField(TEXT("generator"), GenPtr) && GenPtr && (*GenPtr).IsValid())
		{
			(*GenPtr)->TryGetStringField(TEXT("type"), GeneratorClassName);
		}

		UClass* GeneratorClass = nullptr;
		if (!GeneratorClassName.IsEmpty())
		{
			FString Error;
			GeneratorClass = ResolveEQSNodeClass(GeneratorClassName, UEnvQueryGenerator::StaticClass(), TEXT("EnvQueryGenerator_"), Error);
			if (!GeneratorClass)
			{
				RecordEntryFailure(SpecId, Error);
				continue;
			}
		}

		UEnvQueryOption* NewOption = NewObject<UEnvQueryOption>(EQS);
		if (GeneratorClass)
		{
			NewOption->Generator = NewObject<UEnvQueryGenerator>(NewOption, GeneratorClass);
		}

		GetOptionsMutable(EQS).Add(NewOption);

		FString OptionIndexStr = FString::FromInt(GetOptionsMutable(EQS).Num() - 1);
		RegisterIdMapping(SpecId, OptionIndexStr);
		RecordEntrySuccess(SpecId, OptionIndexStr);
		SuccessCount++;
	}

	EQS->MarkPackageDirty();

	UE_LOG(LogClaireon, Log, TEXT("[apply_spec:EQS] Pass 1: Created %d/%d options"),
		SuccessCount, OptionsArray->Num());

	return true;
}

bool FClaireonSpecApplicator_EQS::ApplyPass2_WireRelationships(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec)
{
	UEnvQuery* EQS = Query.Get();
	if (!EQS)
	{
		AddError(TEXT("EQS Query is no longer valid"));
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* OptionsArray = nullptr;
	if (!Spec->TryGetArrayField(TEXT("options"), OptionsArray) || !OptionsArray)
	{
		return true;
	}

	for (int32 i = 0; i < OptionsArray->Num(); ++i)
	{
		const TSharedPtr<FJsonObject>& OptObj = (*OptionsArray)[i]->AsObject();
		if (!OptObj.IsValid()) continue;

		FString SpecId;
		OptObj->TryGetStringField(TEXT("id"), SpecId);
		if (!IsIdCreated(SpecId)) continue;

		FString OptionIndexStr = ResolveId(SpecId);
		int32 OptionIndex = FCString::Atoi(*OptionIndexStr);

		if (OptionIndex < 0 || OptionIndex >= EQS->GetOptions().Num())
		{
			AddWarning(FString::Printf(TEXT("Option index %d out of range for '%s'"), OptionIndex, *SpecId));
			continue;
		}

		UEnvQueryOption* Option = GetOptionsMutable(EQS)[OptionIndex];

		// --- Set generator properties ---
		const TSharedPtr<FJsonObject>* GenPtr = nullptr;
		if (OptObj->TryGetObjectField(TEXT("generator"), GenPtr) && GenPtr && (*GenPtr).IsValid() && Option->Generator)
		{
			const TSharedPtr<FJsonObject>* GenPropsPtr = nullptr;
			if ((*GenPtr)->TryGetObjectField(TEXT("properties"), GenPropsPtr) && GenPropsPtr && (*GenPropsPtr).IsValid())
			{
				for (const auto& Prop : (*GenPropsPtr)->Values)
				{
					FString PropValue;
					if (Prop.Value->TryGetString(PropValue))
					{
						FString PropError;
						if (!SpecApplicatorEQS_SetEQSProperty(Option->Generator, Prop.Key, PropValue, PropError))
						{
							AddWarning(FString::Printf(TEXT("Option '%s' generator property '%s': %s"), *SpecId, *Prop.Key, *PropError));
						}
					}
				}
			}
		}

		// --- Add tests ---
		const TArray<TSharedPtr<FJsonValue>>* TestsArray = nullptr;
		if (OptObj->TryGetArrayField(TEXT("tests"), TestsArray) && TestsArray)
		{
			for (int32 t = 0; t < TestsArray->Num(); ++t)
			{
				const TSharedPtr<FJsonObject>& TestObj = (*TestsArray)[t]->AsObject();
				if (!TestObj.IsValid()) continue;

				FString TestId, TestType;
				TestObj->TryGetStringField(TEXT("id"), TestId);
				TestObj->TryGetStringField(TEXT("type"), TestType);

				FString Error;
				UClass* TestClass = ResolveEQSNodeClass(TestType, UEnvQueryTest::StaticClass(), TEXT("EnvQueryTest_"), Error);
				if (!TestClass)
				{
					RecordEntryFailure(TestId, Error);
					continue;
				}

				UEnvQueryTest* NewTest = NewObject<UEnvQueryTest>(Option, TestClass);
				Option->Tests.Add(NewTest);

				FString TestIndexStr = FString::Printf(TEXT("%d:%d"), OptionIndex, Option->Tests.Num() - 1);
				RegisterIdMapping(TestId, TestIndexStr);
				RecordEntrySuccess(TestId, TestIndexStr);

				// Set test properties
				const TSharedPtr<FJsonObject>* TestPropsPtr = nullptr;
				if (TestObj->TryGetObjectField(TEXT("properties"), TestPropsPtr) && TestPropsPtr && (*TestPropsPtr).IsValid())
				{
					for (const auto& Prop : (*TestPropsPtr)->Values)
					{
						FString PropValue;
						if (Prop.Value->TryGetString(PropValue))
						{
							FString PropError;
							if (!SpecApplicatorEQS_SetEQSProperty(NewTest, Prop.Key, PropValue, PropError))
							{
								AddWarning(FString::Printf(TEXT("Test '%s' property '%s': %s"), *TestId, *Prop.Key, *PropError));
							}
						}
					}
				}
			}
		}
	}

	EQS->MarkPackageDirty();

	return true;
}

bool FClaireonSpecApplicator_EQS::CompileAsset(const FString& SessionId, FString& OutError)
{
	// EQS queries do not need explicit compilation. No-op.
	return true;
}

bool FClaireonSpecApplicator_EQS::SaveAsset(const FString& SessionId, FString& OutError)
{
	UEnvQuery* EQS = Query.Get();
	if (!EQS)
	{
		OutError = TEXT("EQS Query is no longer valid");
		return false;
	}

	UPackage* Package = EQS->GetPackage();
	Package->SetDirtyFlag(true);

	TArray<UPackage*> PackagesToSave;
	PackagesToSave.Add(Package);

	if (ClaireonSafeExec::DidLastExecutionCrash())
	{
		OutError = TEXT("Save blocked: editor state may be corrupted after a previous crash");
		return false;
	}

	bool bSuccess = UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, true);
	if (!bSuccess)
	{
		OutError = TEXT("Failed to save EQS package");
		return false;
	}

	return true;
}

void FClaireonSpecApplicator_EQS::CloseSession(const FString& SessionId)
{
	FClaireonSessionManager::Get().CloseSession(SessionId);
}
