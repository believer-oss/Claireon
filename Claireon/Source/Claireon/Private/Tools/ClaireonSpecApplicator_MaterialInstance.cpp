// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSpecApplicator_MaterialInstance.h"

#include "Tools/ClaireonMaterialHelpers.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "ClaireonLog.h"

#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "MaterialEditingLibrary.h"
#include "Engine/Texture.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/SoftObjectPath.h"

namespace
{
	/** Accepted parameter type strings. */
	static bool IsAcceptedMICParameterType(const FString& Type)
	{
		return Type.Equals(TEXT("scalar"), ESearchCase::IgnoreCase)
			|| Type.Equals(TEXT("vector"), ESearchCase::IgnoreCase)
			|| Type.Equals(TEXT("texture"), ESearchCase::IgnoreCase)
			|| Type.Equals(TEXT("static_switch"), ESearchCase::IgnoreCase)
			|| Type.Equals(TEXT("static_component_mask"), ESearchCase::IgnoreCase);
	}

	/** Map a parameter_type string to EMaterialParameterType. Returns false if unknown. */
	static bool ParseParameterType(const FString& Type, EMaterialParameterType& OutType)
	{
		if (Type.Equals(TEXT("scalar"), ESearchCase::IgnoreCase))
		{
			OutType = EMaterialParameterType::Scalar;
			return true;
		}
		if (Type.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
		{
			OutType = EMaterialParameterType::Vector;
			return true;
		}
		if (Type.Equals(TEXT("texture"), ESearchCase::IgnoreCase))
		{
			OutType = EMaterialParameterType::Texture;
			return true;
		}
		if (Type.Equals(TEXT("static_switch"), ESearchCase::IgnoreCase))
		{
			OutType = EMaterialParameterType::StaticSwitch;
			return true;
		}
		if (Type.Equals(TEXT("static_component_mask"), ESearchCase::IgnoreCase))
		{
			OutType = EMaterialParameterType::StaticComponentMask;
			return true;
		}
		return false;
	}
} // anonymous namespace

bool FClaireonSpecApplicator_MaterialInstance::ValidateToolSpec(const TSharedPtr<FJsonObject>& Spec, TArray<FString>& OutErrors)
{
	if (!Spec.IsValid())
	{
		OutErrors.Add(TEXT("MaterialInstance spec is null"));
		return false;
	}

	bool bHasContent = false;

	// parent
	FString ParentPath;
	if (Spec->TryGetStringField(TEXT("parent"), ParentPath))
	{
		bHasContent = true;
		if (ParentPath.IsEmpty())
		{
			OutErrors.Add(TEXT("'parent' field is empty (must be a non-empty asset path)"));
		}
	}

	// parameters[]
	TSet<FString> SeenNameTypeKeys;
	const TArray<TSharedPtr<FJsonValue>>* ParametersArr = nullptr;
	if (Spec->TryGetArrayField(TEXT("parameters"), ParametersArr) && ParametersArr)
	{
		bHasContent = true;
		for (int32 i = 0; i < ParametersArr->Num(); ++i)
		{
			const TSharedPtr<FJsonValue>& Val = (*ParametersArr)[i];
			if (!Val.IsValid() || Val->Type != EJson::Object) continue;
			const TSharedPtr<FJsonObject>& Obj = Val->AsObject();

			FString ParamName, ParamType;
			if (!Obj->TryGetStringField(TEXT("name"), ParamName) || ParamName.IsEmpty())
			{
				OutErrors.Add(FString::Printf(TEXT("parameters[%d]: missing or empty 'name'"), i));
			}
			if (!Obj->TryGetStringField(TEXT("type"), ParamType) || ParamType.IsEmpty())
			{
				OutErrors.Add(FString::Printf(TEXT("parameters[%d]: missing or empty 'type'"), i));
				continue;
			}
			if (!IsAcceptedMICParameterType(ParamType))
			{
				OutErrors.Add(FString::Printf(TEXT("parameters[%d]: unknown type '%s' (expected scalar|vector|texture|static_switch|static_component_mask)"), i, *ParamType));
				continue;
			}

			if (!Obj->HasField(TEXT("value")))
			{
				OutErrors.Add(FString::Printf(TEXT("parameters[%d]: missing 'value'"), i));
			}

			// Reject duplicate name+type entries (ambiguous final state).
			if (!ParamName.IsEmpty())
			{
				const FString Key = FString::Printf(TEXT("%s|%s"),
					*ParamName.ToLower(), *ParamType.ToLower());
				if (SeenNameTypeKeys.Contains(Key))
				{
					OutErrors.Add(FString::Printf(TEXT("parameters[%d]: duplicate name+type '%s' (%s)"),
						i, *ParamName, *ParamType));
				}
				else
				{
					SeenNameTypeKeys.Add(Key);
				}
			}
		}
	}

	// clear_overrides[]
	const TArray<TSharedPtr<FJsonValue>>* ClearArr = nullptr;
	if (Spec->TryGetArrayField(TEXT("clear_overrides"), ClearArr) && ClearArr)
	{
		bHasContent = true;
		for (int32 i = 0; i < ClearArr->Num(); ++i)
		{
			const TSharedPtr<FJsonValue>& Val = (*ClearArr)[i];
			if (!Val.IsValid() || Val->Type != EJson::Object) continue;
			const TSharedPtr<FJsonObject>& Obj = Val->AsObject();

			FString Name, Type;
			if (!Obj->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
			{
				OutErrors.Add(FString::Printf(TEXT("clear_overrides[%d]: missing or empty 'name'"), i));
			}
			if (!Obj->TryGetStringField(TEXT("type"), Type) || Type.IsEmpty())
			{
				OutErrors.Add(FString::Printf(TEXT("clear_overrides[%d]: missing or empty 'type'"), i));
			}
			else if (!IsAcceptedMICParameterType(Type))
			{
				OutErrors.Add(FString::Printf(TEXT("clear_overrides[%d]: unknown type '%s'"), i, *Type));
			}
		}
	}

	if (!bHasContent)
	{
		OutErrors.Add(TEXT("MaterialInstance spec must contain at least one of: 'parent', 'parameters', 'clear_overrides'"));
		return false;
	}

	return OutErrors.Num() == 0;
}

bool FClaireonSpecApplicator_MaterialInstance::OpenOrCreateAsset(const FString& AssetPath, FString& OutSessionId, FString& OutError)
{
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		OutError = ResolveResult.Error;
		return false;
	}
	const FString ResolvedPath = ResolveResult.ResolvedPath.Path;

	UMaterialInstanceConstant* LoadedInstance = ClaireonMaterialHelpers::LoadMaterialInstanceAsset(ResolvedPath, OutError);
	if (!LoadedInstance)
	{
		return false;
	}

	const FString InstancePathName = LoadedInstance->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(
		InstancePathName, TEXT("claireon.material_instance_edit"));

	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		OutError = FString::Printf(TEXT("Asset is locked by %s session %s"),
			*Blocker.ToolName, *Blocker.SessionId);
		return false;
	}
	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		OutError = FString::Printf(TEXT("Invalid asset path: %s"), *InstancePathName);
		return false;
	}

	Instance = LoadedInstance;
	OutSessionId = OpenResult.SessionId;
	return true;
}

bool FClaireonSpecApplicator_MaterialInstance::ApplyPass1_CreateEntities(const FString& /*SessionId*/, const TSharedPtr<FJsonObject>& Spec)
{
	UMaterialInstanceConstant* MIC = Instance.Get();
	if (!MIC)
	{
		AddError(TEXT("MaterialInstance is no longer valid"));
		return false;
	}

	// Stash spec for SaveAsset to consult save_at_end.
	CachedSpec = Spec;

	// Apply parent change (if present) before any parameter edits so overrides
	// settle on the final parent chain.
	FString ParentPath;
	if (Spec->TryGetStringField(TEXT("parent"), ParentPath) && !ParentPath.IsEmpty())
	{
		FSoftObjectPath SoftPath(ParentPath);
		UMaterialInterface* NewParent = Cast<UMaterialInterface>(SoftPath.TryLoad());
		if (!NewParent)
		{
			AddError(FString::Printf(TEXT("parent: failed to load '%s' as UMaterialInterface"), *ParentPath));
			return false;
		}

		// Cycle detection: walk the new parent chain; if any ancestor equals the
		// current instance, reject.
		{
			UMaterialInterface* Cursor = NewParent;
			int32 Safety = 0;
			while (Cursor && Safety++ < 64)
			{
				if (Cursor == MIC)
				{
					AddError(TEXT("parent: setting parent would create a cycle"));
					return false;
				}
				if (UMaterialInstance* AsInstance = Cast<UMaterialInstance>(Cursor))
				{
					Cursor = AsInstance->Parent;
				}
				else
				{
					break;
				}
			}
		}

		MIC->Modify();
		MIC->SetParentEditorOnly(NewParent);
		UMaterialEditingLibrary::UpdateMaterialInstance(MIC);

		RegisterIdMapping(TEXT("__parent__"), ParentPath);
		RecordEntrySuccess(TEXT("__parent__"), ParentPath);

		UE_LOG(LogClaireon, Log, TEXT("[apply_spec:MaterialInstance] Reparented to %s"), *ParentPath);
	}

	return !HasCriticalError();
}

bool FClaireonSpecApplicator_MaterialInstance::ApplyPass2_WireRelationships(const FString& /*SessionId*/, const TSharedPtr<FJsonObject>& Spec)
{
	UMaterialInstanceConstant* MIC = Instance.Get();
	if (!MIC)
	{
		AddError(TEXT("MaterialInstance is no longer valid"));
		return false;
	}

	// 1. parameters[]
	const TArray<TSharedPtr<FJsonValue>>* ParametersArr = nullptr;
	if (Spec->TryGetArrayField(TEXT("parameters"), ParametersArr) && ParametersArr)
	{
		for (int32 i = 0; i < ParametersArr->Num(); ++i)
		{
			const TSharedPtr<FJsonValue>& Val = (*ParametersArr)[i];
			if (!Val.IsValid() || Val->Type != EJson::Object) continue;
			const TSharedPtr<FJsonObject>& Obj = Val->AsObject();

			FString ParamName, ParamType, SpecId;
			Obj->TryGetStringField(TEXT("name"), ParamName);
			Obj->TryGetStringField(TEXT("type"), ParamType);
			Obj->TryGetStringField(TEXT("id"), SpecId);

			const FString EntryKey = SpecId.IsEmpty()
				? FString::Printf(TEXT("parameters[%d] %s (%s)"), i, *ParamName, *ParamType)
				: SpecId;

			const FName ParamFName(*ParamName);
			const TSharedPtr<FJsonValue> ValueField = Obj->TryGetField(TEXT("value"));
			if (!ValueField.IsValid())
			{
				RecordEntryFailure(EntryKey, TEXT("missing 'value'"));
				continue;
			}

			FString SetErr;
			bool bOk = false;

			if (ParamType.Equals(TEXT("scalar"), ESearchCase::IgnoreCase))
			{
				const float Value = static_cast<float>(ValueField->AsNumber());
				bOk = ClaireonMaterialHelpers::SetMICScalar(MIC, ParamFName, Value, SetErr);
			}
			else if (ParamType.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
			{
				FLinearColor LC(ForceInit);
				if (ValueField->Type == EJson::Array)
				{
					TArray<TSharedPtr<FJsonValue>> Arr = ValueField->AsArray();
					if (Arr.Num() >= 3)
					{
						LC.R = static_cast<float>(Arr[0]->AsNumber());
						LC.G = static_cast<float>(Arr[1]->AsNumber());
						LC.B = static_cast<float>(Arr[2]->AsNumber());
						LC.A = Arr.Num() >= 4 ? static_cast<float>(Arr[3]->AsNumber()) : 1.0f;
					}
					else
					{
						SetErr = TEXT("vector value must be array of length 3 or 4");
					}
				}
				else if (ValueField->Type == EJson::Object)
				{
					TSharedPtr<FJsonObject> ColObj = ValueField->AsObject();
					double R = 0.0, G = 0.0, B = 0.0, A = 1.0;
					ColObj->TryGetNumberField(TEXT("R"), R);
					ColObj->TryGetNumberField(TEXT("G"), G);
					ColObj->TryGetNumberField(TEXT("B"), B);
					ColObj->TryGetNumberField(TEXT("A"), A);
					LC.R = static_cast<float>(R);
					LC.G = static_cast<float>(G);
					LC.B = static_cast<float>(B);
					LC.A = static_cast<float>(A);
				}
				else
				{
					SetErr = TEXT("vector value must be JSON array or object");
				}
				if (SetErr.IsEmpty())
				{
					bOk = ClaireonMaterialHelpers::SetMICVector(MIC, ParamFName, LC, SetErr);
				}
			}
			else if (ParamType.Equals(TEXT("texture"), ESearchCase::IgnoreCase))
			{
				const FString TexPath = ValueField->AsString();
				UTexture* Tex = nullptr;
				if (!TexPath.IsEmpty() && !TexPath.Equals(TEXT("None"), ESearchCase::IgnoreCase))
				{
					FSoftObjectPath SoftPath(TexPath);
					Tex = Cast<UTexture>(SoftPath.TryLoad());
					if (!Tex)
					{
						SetErr = FString::Printf(TEXT("failed to load texture '%s'"), *TexPath);
					}
				}
				if (SetErr.IsEmpty())
				{
					bOk = ClaireonMaterialHelpers::SetMICTexture(MIC, ParamFName, Tex, SetErr);
				}
			}
			else if (ParamType.Equals(TEXT("static_switch"), ESearchCase::IgnoreCase))
			{
				const bool Value = ValueField->AsBool();
				bOk = ClaireonMaterialHelpers::SetMICStaticSwitch(MIC, ParamFName, Value, SetErr);
			}
			else if (ParamType.Equals(TEXT("static_component_mask"), ESearchCase::IgnoreCase))
			{
				bool R = false, G = false, B = false, A = false;
				if (ValueField->Type == EJson::Object)
				{
					TSharedPtr<FJsonObject> Mask = ValueField->AsObject();
					Mask->TryGetBoolField(TEXT("R"), R);
					Mask->TryGetBoolField(TEXT("G"), G);
					Mask->TryGetBoolField(TEXT("B"), B);
					Mask->TryGetBoolField(TEXT("A"), A);
				}
				else if (ValueField->Type == EJson::Array)
				{
					TArray<TSharedPtr<FJsonValue>> Arr = ValueField->AsArray();
					if (Arr.Num() >= 1) R = Arr[0]->AsBool();
					if (Arr.Num() >= 2) G = Arr[1]->AsBool();
					if (Arr.Num() >= 3) B = Arr[2]->AsBool();
					if (Arr.Num() >= 4) A = Arr[3]->AsBool();
				}
				bOk = ClaireonMaterialHelpers::SetMICStaticComponentMask(MIC, ParamFName, R, G, B, A, SetErr);
			}
			else
			{
				SetErr = FString::Printf(TEXT("unknown type '%s'"), *ParamType);
			}

			if (!bOk)
			{
				RecordEntryFailure(EntryKey, SetErr);
			}
			else
			{
				RecordEntrySuccess(EntryKey, ParamName);
			}
		}
	}

	// 2. clear_overrides[]
	const TArray<TSharedPtr<FJsonValue>>* ClearArr = nullptr;
	if (Spec->TryGetArrayField(TEXT("clear_overrides"), ClearArr) && ClearArr)
	{
		for (int32 i = 0; i < ClearArr->Num(); ++i)
		{
			const TSharedPtr<FJsonValue>& Val = (*ClearArr)[i];
			if (!Val.IsValid() || Val->Type != EJson::Object) continue;
			const TSharedPtr<FJsonObject>& Obj = Val->AsObject();

			FString Name, TypeStr;
			Obj->TryGetStringField(TEXT("name"), Name);
			Obj->TryGetStringField(TEXT("type"), TypeStr);

			const FString EntryKey = FString::Printf(TEXT("clear_overrides[%d] %s (%s)"),
				i, *Name, *TypeStr);

			EMaterialParameterType ParamType;
			if (!ParseParameterType(TypeStr, ParamType))
			{
				RecordEntryFailure(EntryKey, FString::Printf(TEXT("unknown type '%s'"), *TypeStr));
				continue;
			}

			FString ClearErr;
			if (!ClaireonMaterialHelpers::ClearMICOverride(MIC, FName(*Name), ParamType, ClearErr))
			{
				RecordEntryFailure(EntryKey, ClearErr);
				continue;
			}
			RecordEntrySuccess(EntryKey, Name);
		}
	}

	return !HasCriticalError();
}

bool FClaireonSpecApplicator_MaterialInstance::CompileAsset(const FString& /*SessionId*/, FString& OutError)
{
	UMaterialInstanceConstant* MIC = Instance.Get();
	if (!MIC)
	{
		OutError = TEXT("MaterialInstance is no longer valid");
		return false;
	}
	MIC->PostEditChange();
	return true;
}

bool FClaireonSpecApplicator_MaterialInstance::SaveAsset(const FString& /*SessionId*/, FString& OutError)
{
	UMaterialInstanceConstant* MIC = Instance.Get();
	if (!MIC)
	{
		OutError = TEXT("MaterialInstance is no longer valid");
		return false;
	}

	bool bSaveAtEnd = false;
	if (CachedSpec.IsValid())
	{
		CachedSpec->TryGetBoolField(TEXT("save_at_end"), bSaveAtEnd);
	}

	if (!bSaveAtEnd)
	{
		return true;
	}

	return ClaireonMaterialHelpers::SaveMaterialInstanceAsset(MIC, OutError);
}

void FClaireonSpecApplicator_MaterialInstance::CloseSession(const FString& SessionId)
{
	FClaireonSessionManager::Get().CloseSession(SessionId);
}
