// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSpecApplicator_Blackboard.h"
#include "Tools/ClaireonBehaviorTreeHelpers.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "ClaireonSafeExec.h"
#include "ClaireonLog.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Package.h"
#include "FileHelpers.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_String.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Name.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Rotator.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Class.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"

namespace
{
	UBlackboardKeyType* CreateKeyTypeForBBName(const FString& TypeName, UObject* Outer, FString& OutError)
	{
		static const TMap<FString, UClass*> TypeMap = {
			{ TEXT("Bool"), UBlackboardKeyType_Bool::StaticClass() },
			{ TEXT("Int"), UBlackboardKeyType_Int::StaticClass() },
			{ TEXT("Float"), UBlackboardKeyType_Float::StaticClass() },
			{ TEXT("String"), UBlackboardKeyType_String::StaticClass() },
			{ TEXT("Name"), UBlackboardKeyType_Name::StaticClass() },
			{ TEXT("Vector"), UBlackboardKeyType_Vector::StaticClass() },
			{ TEXT("Rotator"), UBlackboardKeyType_Rotator::StaticClass() },
			{ TEXT("Object"), UBlackboardKeyType_Object::StaticClass() },
			{ TEXT("Class"), UBlackboardKeyType_Class::StaticClass() },
			{ TEXT("Enum"), UBlackboardKeyType_Enum::StaticClass() },
		};

		const UClass* const* FoundClass = TypeMap.Find(TypeName);
		if (!FoundClass)
		{
			OutError = FString::Printf(TEXT("Unknown key type: '%s'. Supported: Bool, Int, Float, String, Name, Vector, Rotator, Object, Class, Enum"), *TypeName);
			return nullptr;
		}

		return NewObject<UBlackboardKeyType>(Outer, *FoundClass);
	}
} // namespace

bool FClaireonSpecApplicator_Blackboard::ValidateToolSpec(const TSharedPtr<FJsonObject>& Spec, TArray<FString>& OutErrors)
{
	const TArray<TSharedPtr<FJsonValue>>* KeysArray = nullptr;
	if (!Spec->TryGetArrayField(TEXT("keys"), KeysArray) || !KeysArray)
	{
		OutErrors.Add(TEXT("Blackboard spec must contain a 'keys' array"));
		return false;
	}

	if (KeysArray->Num() == 0)
	{
		OutErrors.Add(TEXT("Blackboard spec 'keys' array is empty"));
		return false;
	}

	for (int32 i = 0; i < KeysArray->Num(); ++i)
	{
		const TSharedPtr<FJsonValue>& KeyVal = (*KeysArray)[i];
		if (!KeyVal.IsValid() || KeyVal->Type != EJson::Object) continue;
		const TSharedPtr<FJsonObject>& KeyObj = KeyVal->AsObject();

		FString KeyId, KeyName, KeyType;
		if (!KeyObj->TryGetStringField(TEXT("id"), KeyId) || KeyId.IsEmpty())
		{
			OutErrors.Add(FString::Printf(TEXT("keys[%d]: missing or empty 'id'"), i));
		}
		if (!KeyObj->TryGetStringField(TEXT("name"), KeyName) || KeyName.IsEmpty())
		{
			OutErrors.Add(FString::Printf(TEXT("keys[%d]: missing or empty 'name'"), i));
		}
		if (!KeyObj->TryGetStringField(TEXT("type"), KeyType) || KeyType.IsEmpty())
		{
			OutErrors.Add(FString::Printf(TEXT("keys[%d]: missing or empty 'type'"), i));
		}
	}

	return OutErrors.Num() == 0;
}

bool FClaireonSpecApplicator_Blackboard::OpenOrCreateAsset(const FString& AssetPath, FString& OutSessionId, FString& OutError)
{
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		OutError = ResolveResult.Error;
		return false;
	}
	const FString ResolvedPath = ResolveResult.ResolvedPath.Path;

	UBlackboardData* BB = ClaireonBehaviorTreeHelpers::LoadBlackboardAsset(ResolvedPath, OutError);
	if (!BB)
	{
		return false;
	}

	const FString BBPathName = BB->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(
		BBPathName, TEXT("blackboard_edit"));

	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		OutError = FString::Printf(TEXT("Asset is locked by %s session %s"),
			*Blocker.ToolName, *Blocker.SessionId);
		return false;
	}
	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		OutError = FString::Printf(TEXT("Invalid asset path: %s"), *BBPathName);
		return false;
	}

	BlackboardData = BB;
	OutSessionId = OpenResult.SessionId;
	return true;
}

bool FClaireonSpecApplicator_Blackboard::ApplyPass1_CreateEntities(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec)
{
	UBlackboardData* BB = BlackboardData.Get();
	if (!BB)
	{
		AddError(TEXT("BlackboardData is no longer valid"));
		return false;
	}

	// --- Set parent blackboard ---
	FString ParentPath;
	if (Spec->TryGetStringField(TEXT("parent_blackboard"), ParentPath) && !ParentPath.IsEmpty())
	{
		FString Error;
		UBlackboardData* ParentBB = ClaireonBehaviorTreeHelpers::LoadBlackboardAsset(ParentPath, Error);
		if (ParentBB && ParentBB != BB)
		{
			BB->Parent = ParentBB;
			BB->UpdateKeyIDs();
		}
		else if (!ParentBB)
		{
			AddWarning(FString::Printf(TEXT("Could not load parent blackboard: %s"), *Error));
		}
	}

	// --- Create keys ---
	const TArray<TSharedPtr<FJsonValue>>* KeysArray = nullptr;
	if (!Spec->TryGetArrayField(TEXT("keys"), KeysArray) || !KeysArray)
	{
		return true;
	}

	int32 SuccessCount = 0;

	for (int32 i = 0; i < KeysArray->Num(); ++i)
	{
		const TSharedPtr<FJsonObject>& KeyObj = (*KeysArray)[i]->AsObject();
		if (!KeyObj.IsValid()) continue;

		FString SpecId, KeyName, KeyType;
		KeyObj->TryGetStringField(TEXT("id"), SpecId);
		KeyObj->TryGetStringField(TEXT("name"), KeyName);
		KeyObj->TryGetStringField(TEXT("type"), KeyType);

		// Check for duplicate
		bool bDuplicate = false;
		for (const FBlackboardEntry& Entry : BB->Keys)
		{
			if (Entry.EntryName == FName(*KeyName))
			{
				bDuplicate = true;
				break;
			}
		}
		for (const FBlackboardEntry& Entry : BB->ParentKeys)
		{
			if (Entry.EntryName == FName(*KeyName))
			{
				bDuplicate = true;
				break;
			}
		}
		if (bDuplicate)
		{
			RecordEntryFailure(SpecId, FString::Printf(TEXT("Key '%s' already exists"), *KeyName));
			continue;
		}

		FString Error;
		UBlackboardKeyType* NewKeyType = CreateKeyTypeForBBName(KeyType, BB, Error);
		if (!NewKeyType)
		{
			RecordEntryFailure(SpecId, Error);
			continue;
		}

		FBlackboardEntry NewEntry;
		NewEntry.EntryName = FName(*KeyName);
		NewEntry.KeyType = NewKeyType;

		BB->Keys.Add(NewEntry);
		RegisterIdMapping(SpecId, KeyName);
		RecordEntrySuccess(SpecId, KeyName);
		SuccessCount++;
	}

	BB->UpdateKeyIDs();
	BB->MarkPackageDirty();

	UE_LOG(LogClaireon, Log, TEXT("[apply_spec:Blackboard] Pass 1: Created %d/%d keys"),
		SuccessCount, KeysArray->Num());

	return true;
}

bool FClaireonSpecApplicator_Blackboard::ApplyPass2_WireRelationships(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec)
{
	// Blackboard has no relationships to wire. No-op.
	return true;
}

bool FClaireonSpecApplicator_Blackboard::CompileAsset(const FString& SessionId, FString& OutError)
{
	// Blackboard does not need compilation. No-op.
	return true;
}

bool FClaireonSpecApplicator_Blackboard::SaveAsset(const FString& SessionId, FString& OutError)
{
	UBlackboardData* BB = BlackboardData.Get();
	if (!BB)
	{
		OutError = TEXT("BlackboardData is no longer valid");
		return false;
	}

	UPackage* Package = BB->GetPackage();
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
		OutError = TEXT("Failed to save Blackboard package");
		return false;
	}

	return true;
}

void FClaireonSpecApplicator_Blackboard::CloseSession(const FString& SessionId)
{
	FClaireonSessionManager::Get().CloseSession(SessionId);
}
