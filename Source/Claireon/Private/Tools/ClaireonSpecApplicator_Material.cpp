// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSpecApplicator_Material.h"

#include "Tools/ClaireonMaterialHelpers.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "ClaireonLog.h"

#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionParameter.h"
#include "MaterialEditingLibrary.h"
#include "Engine/Texture.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "RenderUtils.h"
#include "UObject/UnrealType.h"

namespace
{
	/** Accepted parameter_type strings for parameter_defaults entries. */
	static bool SpecApplicatorMaterial_IsAcceptedParameterType(const FString& Type)
	{
		return Type.Equals(TEXT("scalar"), ESearchCase::IgnoreCase)
			|| Type.Equals(TEXT("vector"), ESearchCase::IgnoreCase)
			|| Type.Equals(TEXT("texture"), ESearchCase::IgnoreCase)
			|| Type.Equals(TEXT("static_switch"), ESearchCase::IgnoreCase)
			|| Type.Equals(TEXT("static_component_mask"), ESearchCase::IgnoreCase);
	}

	/** Parse a shading-model string (e.g. "MSM_DefaultLit") via UEnum. */
	static bool SpecApplicatorMaterial_ParseShadingModel(const FString& Str, EMaterialShadingModel& OutModel)
	{
		const UEnum* Enum = StaticEnum<EMaterialShadingModel>();
		if (!Enum)
		{
			return false;
		}
		const int64 Val = Enum->GetValueByNameString(Str);
		if (Val == INDEX_NONE)
		{
			return false;
		}
		OutModel = static_cast<EMaterialShadingModel>(Val);
		return true;
	}

	/** Parse a blend-mode string (e.g. "BLEND_Opaque") via UEnum. */
	static bool SpecApplicatorMaterial_ParseBlendMode(const FString& Str, EBlendMode& OutMode)
	{
		const UEnum* Enum = StaticEnum<EBlendMode>();
		if (!Enum)
		{
			return false;
		}
		const int64 Val = Enum->GetValueByNameString(Str);
		if (Val == INDEX_NONE)
		{
			return false;
		}
		OutMode = static_cast<EBlendMode>(Val);
		return true;
	}

	/** Accepted attribute strings for attribute_connections (legacy + Substrate). */
	static bool SpecApplicatorMaterial_IsKnownAttribute(const FString& Attr)
	{
		static const TSet<FString> Known = {
			TEXT("BaseColor"), TEXT("Metallic"), TEXT("Specular"), TEXT("Roughness"),
			TEXT("Anisotropy"), TEXT("EmissiveColor"), TEXT("Opacity"), TEXT("OpacityMask"),
			TEXT("Normal"), TEXT("Tangent"), TEXT("WorldPositionOffset"),
			TEXT("SubsurfaceColor"), TEXT("AmbientOcclusion"), TEXT("Refraction"),
			TEXT("PixelDepthOffset"),
			TEXT("CustomizedUVs0"), TEXT("CustomizedUVs1"), TEXT("CustomizedUVs2"), TEXT("CustomizedUVs3"),
			TEXT("CustomizedUVs4"), TEXT("CustomizedUVs5"), TEXT("CustomizedUVs6"), TEXT("CustomizedUVs7"),
			TEXT("FrontMaterial"),
		};
		return Known.Contains(Attr);
	}
} // anonymous namespace

bool FClaireonSpecApplicator_Material::ValidateToolSpec(const TSharedPtr<FJsonObject>& Spec, TArray<FString>& OutErrors)
{
	if (!Spec.IsValid())
	{
		OutErrors.Add(TEXT("Material spec is null"));
		return false;
	}

	bool bHasContent = false;

	// Collect spec-id set from expressions[] for connection cross-reference checks.
	TSet<FString> SpecIds;

	// expressions[]
	const TArray<TSharedPtr<FJsonValue>>* ExpressionsArr = nullptr;
	if (Spec->TryGetArrayField(TEXT("expressions"), ExpressionsArr) && ExpressionsArr)
	{
		bHasContent = true;
		for (int32 i = 0; i < ExpressionsArr->Num(); ++i)
		{
			const TSharedPtr<FJsonValue>& Val = (*ExpressionsArr)[i];
			if (!Val.IsValid() || Val->Type != EJson::Object) continue;
			const TSharedPtr<FJsonObject>& Obj = Val->AsObject();

			FString Id, ClassStr;
			if (!Obj->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty())
			{
				OutErrors.Add(FString::Printf(TEXT("expressions[%d]: missing or empty 'id'"), i));
			}
			else
			{
				SpecIds.Add(Id);
			}
			if (!Obj->TryGetStringField(TEXT("class"), ClassStr) || ClassStr.IsEmpty())
			{
				OutErrors.Add(FString::Printf(TEXT("expressions[%d]: missing or empty 'class'"), i));
				continue;
			}
			FString ResolveErr;
			if (!ClaireonMaterialHelpers::ResolveExpressionClass(ClassStr, ResolveErr))
			{
				OutErrors.Add(FString::Printf(TEXT("expressions[%d]: unknown expression class '%s'"), i, *ClassStr));
			}
		}
	}

	// connections[]
	const TArray<TSharedPtr<FJsonValue>>* ConnectionsArr = nullptr;
	if (Spec->TryGetArrayField(TEXT("connections"), ConnectionsArr) && ConnectionsArr)
	{
		bHasContent = true;
		for (int32 i = 0; i < ConnectionsArr->Num(); ++i)
		{
			const TSharedPtr<FJsonValue>& Val = (*ConnectionsArr)[i];
			if (!Val.IsValid() || Val->Type != EJson::Object) continue;
			const TSharedPtr<FJsonObject>& Obj = Val->AsObject();

			FString FromId, ToId;
			if (!Obj->TryGetStringField(TEXT("from_id"), FromId) || FromId.IsEmpty())
				OutErrors.Add(FString::Printf(TEXT("connections[%d]: missing 'from_id'"), i));
			if (!Obj->TryGetStringField(TEXT("to_id"), ToId) || ToId.IsEmpty())
				OutErrors.Add(FString::Printf(TEXT("connections[%d]: missing 'to_id'"), i));
			// 'from_output' and 'to_input' are optional; empty defaults to first output / first input
			// (matches per-op connect_expressions behavior).
		}
	}

	// attribute_connections[]
	const TArray<TSharedPtr<FJsonValue>>* AttrConnectionsArr = nullptr;
	if (Spec->TryGetArrayField(TEXT("attribute_connections"), AttrConnectionsArr) && AttrConnectionsArr)
	{
		bHasContent = true;
		const bool bSubstrateOn = Substrate::IsSubstrateEnabled();
		for (int32 i = 0; i < AttrConnectionsArr->Num(); ++i)
		{
			const TSharedPtr<FJsonValue>& Val = (*AttrConnectionsArr)[i];
			if (!Val.IsValid() || Val->Type != EJson::Object) continue;
			const TSharedPtr<FJsonObject>& Obj = Val->AsObject();

			FString FromId, Attribute;
			if (!Obj->TryGetStringField(TEXT("from_id"), FromId) || FromId.IsEmpty())
				OutErrors.Add(FString::Printf(TEXT("attribute_connections[%d]: missing 'from_id'"), i));
			// 'from_output' is optional; empty defaults to first output (matches per-op
			// connect_to_material_output behavior).
			if (!Obj->TryGetStringField(TEXT("attribute"), Attribute) || Attribute.IsEmpty())
			{
				OutErrors.Add(FString::Printf(TEXT("attribute_connections[%d]: missing 'attribute'"), i));
				continue;
			}
			if (!SpecApplicatorMaterial_IsKnownAttribute(Attribute))
			{
				OutErrors.Add(FString::Printf(TEXT("attribute_connections[%d]: unknown attribute '%s'"), i, *Attribute));
				continue;
			}
			if (Attribute.Equals(TEXT("FrontMaterial"), ESearchCase::IgnoreCase) && !bSubstrateOn)
			{
				OutErrors.Add(FString::Printf(TEXT("attribute_connections[%d]: 'FrontMaterial' requires Substrate to be enabled"), i));
			}
		}
	}

	// parameter_defaults[]
	const TArray<TSharedPtr<FJsonValue>>* ParamDefaultsArr = nullptr;
	if (Spec->TryGetArrayField(TEXT("parameter_defaults"), ParamDefaultsArr) && ParamDefaultsArr)
	{
		bHasContent = true;
		for (int32 i = 0; i < ParamDefaultsArr->Num(); ++i)
		{
			const TSharedPtr<FJsonValue>& Val = (*ParamDefaultsArr)[i];
			if (!Val.IsValid() || Val->Type != EJson::Object) continue;
			const TSharedPtr<FJsonObject>& Obj = Val->AsObject();

			FString ParamName, ParamType;
			if (!Obj->TryGetStringField(TEXT("parameter_name"), ParamName) || ParamName.IsEmpty())
				OutErrors.Add(FString::Printf(TEXT("parameter_defaults[%d]: missing 'parameter_name'"), i));
			if (!Obj->TryGetStringField(TEXT("parameter_type"), ParamType) || ParamType.IsEmpty())
				OutErrors.Add(FString::Printf(TEXT("parameter_defaults[%d]: missing 'parameter_type'"), i));
			else if (!SpecApplicatorMaterial_IsAcceptedParameterType(ParamType))
				OutErrors.Add(FString::Printf(TEXT("parameter_defaults[%d]: unknown parameter_type '%s' (expected scalar|vector|texture|static_switch|static_component_mask)"), i, *ParamType));

			if (!Obj->HasField(TEXT("value")))
				OutErrors.Add(FString::Printf(TEXT("parameter_defaults[%d]: missing 'value'"), i));
		}
	}

	// shading_model
	FString ShadingModelStr;
	if (Spec->TryGetStringField(TEXT("shading_model"), ShadingModelStr) && !ShadingModelStr.IsEmpty())
	{
		bHasContent = true;
		EMaterialShadingModel SM;
		if (!SpecApplicatorMaterial_ParseShadingModel(ShadingModelStr, SM))
		{
			OutErrors.Add(FString::Printf(TEXT("shading_model: '%s' is not a valid EMaterialShadingModel"), *ShadingModelStr));
		}
	}

	// blend_mode
	FString BlendModeStr;
	if (Spec->TryGetStringField(TEXT("blend_mode"), BlendModeStr) && !BlendModeStr.IsEmpty())
	{
		bHasContent = true;
		EBlendMode BM;
		if (!SpecApplicatorMaterial_ParseBlendMode(BlendModeStr, BM))
		{
			OutErrors.Add(FString::Printf(TEXT("blend_mode: '%s' is not a valid EBlendMode"), *BlendModeStr));
		}
	}

	if (!bHasContent)
	{
		OutErrors.Add(TEXT("Material spec must contain at least one of: 'expressions', 'connections', 'attribute_connections', 'parameter_defaults', 'shading_model', 'blend_mode'"));
		return false;
	}

	return OutErrors.Num() == 0;
}

bool FClaireonSpecApplicator_Material::OpenOrCreateAsset(const FString& AssetPath, FString& OutSessionId, FString& OutError)
{
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		OutError = ResolveResult.Error;
		return false;
	}
	const FString ResolvedPath = ResolveResult.ResolvedPath.Path;

	UMaterial* LoadedMaterial = ClaireonMaterialHelpers::LoadMaterialAsset(ResolvedPath, OutError);
	if (!LoadedMaterial)
	{
		return false;
	}

	const FString MaterialPathName = LoadedMaterial->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(
		MaterialPathName, TEXT("material_edit"));

	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		OutError = FString::Printf(TEXT("Asset is locked by %s session %s"),
			*Blocker.ToolName, *Blocker.SessionId);
		return false;
	}
	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		OutError = FString::Printf(TEXT("Invalid asset path: %s"), *MaterialPathName);
		return false;
	}

	Material = LoadedMaterial;
	OutSessionId = OpenResult.SessionId;
	return true;
}

bool FClaireonSpecApplicator_Material::ApplyPass1_CreateEntities(const FString& /*SessionId*/, const TSharedPtr<FJsonObject>& Spec)
{
	UMaterial* Mat = Material.Get();
	if (!Mat)
	{
		AddError(TEXT("Material is no longer valid"));
		return false;
	}

	// Stash spec for CompileAsset / SaveAsset to consult compile_at_end / save_at_end.
	CachedSpec = Spec;

	const TArray<TSharedPtr<FJsonValue>>* ExpressionsArr = nullptr;
	if (!Spec->TryGetArrayField(TEXT("expressions"), ExpressionsArr) || !ExpressionsArr)
	{
		// No expressions to create; Pass 2 may still wire pre-existing expressions.
		return true;
	}

	for (int32 i = 0; i < ExpressionsArr->Num(); ++i)
	{
		const TSharedPtr<FJsonValue>& Val = (*ExpressionsArr)[i];
		if (!Val.IsValid() || Val->Type != EJson::Object) continue;
		const TSharedPtr<FJsonObject>& Obj = Val->AsObject();

		FString SpecId, ClassStr;
		Obj->TryGetStringField(TEXT("id"), SpecId);
		Obj->TryGetStringField(TEXT("class"), ClassStr);

		if (SpecId.IsEmpty() || ClassStr.IsEmpty())
		{
			RecordEntryFailure(SpecId, TEXT("expression entry requires non-empty 'id' and 'class'"));
			continue;
		}

		FString ResolveErr;
		UClass* ExprClass = ClaireonMaterialHelpers::ResolveExpressionClass(ClassStr, ResolveErr);
		if (!ExprClass)
		{
			RecordEntryFailure(SpecId, ResolveErr);
			continue;
		}

		double X = -400.0, Y = static_cast<double>(i) * 120.0;
		Obj->TryGetNumberField(TEXT("x"), X);
		Obj->TryGetNumberField(TEXT("y"), Y);

		UMaterialExpression* Expr = UMaterialEditingLibrary::CreateMaterialExpression(
			Mat, ExprClass, static_cast<int32>(X), static_cast<int32>(Y));
		if (!Expr)
		{
			RecordEntryFailure(SpecId,
				FString::Printf(TEXT("CreateMaterialExpression returned null for class '%s'"), *ClassStr));
			continue;
		}

		// Optional parameter_name.
		FString ParameterName;
		if (Obj->TryGetStringField(TEXT("parameter_name"), ParameterName) && !ParameterName.IsEmpty())
		{
			if (UMaterialExpressionParameter* AsParam = Cast<UMaterialExpressionParameter>(Expr))
			{
				AsParam->Modify();
				AsParam->ParameterName = FName(*ParameterName);
				FPropertyChangedEvent Event(nullptr);
				AsParam->PostEditChangeProperty(Event);
			}
		}

		// Optional properties{} (string -> string ImportText).
		const TSharedPtr<FJsonObject>* PropsObjPtr = nullptr;
		if (Obj->TryGetObjectField(TEXT("properties"), PropsObjPtr) && PropsObjPtr && PropsObjPtr->IsValid())
		{
			for (const auto& Pair : (*PropsObjPtr)->Values)
			{
				if (!Pair.Value.IsValid()) continue;
				FString TextValue;
				if (Pair.Value->Type == EJson::String)
				{
					TextValue = Pair.Value->AsString();
				}
				else
				{
					TextValue = Pair.Value->AsString(); // best-effort coercion
				}
				FString PropErr;
				if (!ClaireonMaterialHelpers::SetExpressionProperty(Mat, Expr, FString(*Pair.Key), TextValue, PropErr))
				{
					AddWarning(FString::Printf(TEXT("expression '%s': failed to set property '%s' = '%s' (%s)"),
						*SpecId, *Pair.Key, *TextValue, *PropErr));
				}
			}
		}

		// Resolve the actual index of the new expression in the material's expression list.
		TConstArrayView<TObjectPtr<UMaterialExpression>> AllExprs = Mat->GetExpressions();
		const int32 ActualIndex = AllExprs.IndexOfByKey(Expr);
		const FString ActualIdStr = ActualIndex != INDEX_NONE
			? FString::FromInt(ActualIndex)
			: FString();

		RegisterIdMapping(SpecId, ActualIdStr);
		RecordEntrySuccess(SpecId, ActualIdStr);

		UE_LOG(LogClaireon, Log, TEXT("[apply_spec:Material] Created expression '%s' (%s) at index %s"),
			*SpecId, *ClassStr, *ActualIdStr);
	}

	return !HasCriticalError();
}

bool FClaireonSpecApplicator_Material::ApplyPass2_WireRelationships(const FString& /*SessionId*/, const TSharedPtr<FJsonObject>& Spec)
{
	UMaterial* Mat = Material.Get();
	if (!Mat)
	{
		AddError(TEXT("Material is no longer valid"));
		return false;
	}

	// Helper: resolve a spec id (or pre-existing identifier) to a UMaterialExpression*.
	auto ResolveExpression = [this, Mat](const FString& SpecId, FString& OutErr) -> UMaterialExpression*
	{
		const FString MappedId = ResolveId(SpecId);
		const FString Identifier = MappedId.IsEmpty() ? SpecId : MappedId;
		int32 OutIndex = INDEX_NONE;
		UMaterialExpression* Expr = ClaireonMaterialHelpers::FindExpressionByIdentifier(Mat, Identifier, OutIndex);
		if (!Expr)
		{
			OutErr = FString::Printf(TEXT("expression not found: '%s' (resolved id: '%s')"), *SpecId, *Identifier);
		}
		return Expr;
	};

	// 1. connections[]
	const TArray<TSharedPtr<FJsonValue>>* ConnectionsArr = nullptr;
	if (Spec->TryGetArrayField(TEXT("connections"), ConnectionsArr) && ConnectionsArr)
	{
		for (int32 i = 0; i < ConnectionsArr->Num(); ++i)
		{
			const TSharedPtr<FJsonValue>& Val = (*ConnectionsArr)[i];
			if (!Val.IsValid() || Val->Type != EJson::Object) continue;
			const TSharedPtr<FJsonObject>& Obj = Val->AsObject();

			FString FromId, ToId, FromOutput, ToInput;
			Obj->TryGetStringField(TEXT("from_id"), FromId);
			Obj->TryGetStringField(TEXT("to_id"), ToId);
			Obj->TryGetStringField(TEXT("from_output"), FromOutput);
			Obj->TryGetStringField(TEXT("to_input"), ToInput);

			const FString EntryKey = FString::Printf(TEXT("connections[%d] %s.%s -> %s.%s"),
				i, *FromId, *FromOutput, *ToId, *ToInput);

			FString FromErr, ToErr;
			UMaterialExpression* FromExpr = ResolveExpression(FromId, FromErr);
			UMaterialExpression* ToExpr = ResolveExpression(ToId, ToErr);
			if (!FromExpr || !ToExpr)
			{
				RecordEntryFailure(EntryKey, !FromErr.IsEmpty() ? FromErr : ToErr);
				continue;
			}

			FString ConnErr;
			if (!ClaireonMaterialHelpers::ConnectExpressions(Mat, FromExpr, FromOutput, ToExpr, ToInput, ConnErr))
			{
				RecordEntryFailure(EntryKey, ConnErr);
				continue;
			}
			RecordEntrySuccess(EntryKey, EntryKey);
		}
	}

	// 2. attribute_connections[]
	const TArray<TSharedPtr<FJsonValue>>* AttrConnectionsArr = nullptr;
	if (Spec->TryGetArrayField(TEXT("attribute_connections"), AttrConnectionsArr) && AttrConnectionsArr)
	{
		for (int32 i = 0; i < AttrConnectionsArr->Num(); ++i)
		{
			const TSharedPtr<FJsonValue>& Val = (*AttrConnectionsArr)[i];
			if (!Val.IsValid() || Val->Type != EJson::Object) continue;
			const TSharedPtr<FJsonObject>& Obj = Val->AsObject();

			FString FromId, FromOutput, Attribute;
			Obj->TryGetStringField(TEXT("from_id"), FromId);
			Obj->TryGetStringField(TEXT("from_output"), FromOutput);
			Obj->TryGetStringField(TEXT("attribute"), Attribute);

			const FString EntryKey = FString::Printf(TEXT("attribute_connections[%d] %s.%s -> %s"),
				i, *FromId, *FromOutput, *Attribute);

			FString FromErr;
			UMaterialExpression* FromExpr = ResolveExpression(FromId, FromErr);
			if (!FromExpr)
			{
				RecordEntryFailure(EntryKey, FromErr);
				continue;
			}

			FString AttrErr;
			if (!ClaireonMaterialHelpers::ConnectToMaterialAttribute(Mat, FromExpr, Attribute, FromOutput, AttrErr))
			{
				RecordEntryFailure(EntryKey, AttrErr);
				continue;
			}
			RecordEntrySuccess(EntryKey, EntryKey);
		}
	}

	// 3. parameter_defaults[]
	const TArray<TSharedPtr<FJsonValue>>* ParamDefaultsArr = nullptr;
	if (Spec->TryGetArrayField(TEXT("parameter_defaults"), ParamDefaultsArr) && ParamDefaultsArr)
	{
		for (int32 i = 0; i < ParamDefaultsArr->Num(); ++i)
		{
			const TSharedPtr<FJsonValue>& Val = (*ParamDefaultsArr)[i];
			if (!Val.IsValid() || Val->Type != EJson::Object) continue;
			const TSharedPtr<FJsonObject>& Obj = Val->AsObject();

			FString ParamName, ParamType;
			Obj->TryGetStringField(TEXT("parameter_name"), ParamName);
			Obj->TryGetStringField(TEXT("parameter_type"), ParamType);

			const FString EntryKey = FString::Printf(TEXT("parameter_defaults[%d] %s (%s)"),
				i, *ParamName, *ParamType);

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
				bOk = ClaireonMaterialHelpers::SetScalarParameterDefault(Mat, ParamFName, Value, SetErr);
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
					bOk = ClaireonMaterialHelpers::SetVectorParameterDefault(Mat, ParamFName, LC, SetErr);
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
					bOk = ClaireonMaterialHelpers::SetTextureParameterDefault(Mat, ParamFName, Tex, SetErr);
				}
			}
			else if (ParamType.Equals(TEXT("static_switch"), ESearchCase::IgnoreCase))
			{
				const bool Value = ValueField->AsBool();
				bOk = ClaireonMaterialHelpers::SetStaticSwitchParameterDefault(Mat, ParamFName, Value, SetErr);
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
				bOk = ClaireonMaterialHelpers::SetStaticComponentMaskParameterDefault(Mat, ParamFName, R, G, B, A, SetErr);
			}
			else
			{
				SetErr = FString::Printf(TEXT("unknown parameter_type '%s'"), *ParamType);
			}

			if (!bOk)
			{
				RecordEntryFailure(EntryKey, SetErr);
			}
			else
			{
				RecordEntrySuccess(EntryKey, EntryKey);
			}
		}
	}

	// 4. shading_model
	FString ShadingModelStr;
	if (Spec->TryGetStringField(TEXT("shading_model"), ShadingModelStr) && !ShadingModelStr.IsEmpty())
	{
		EMaterialShadingModel SM;
		if (!SpecApplicatorMaterial_ParseShadingModel(ShadingModelStr, SM))
		{
			AddError(FString::Printf(TEXT("shading_model: invalid value '%s'"), *ShadingModelStr));
		}
		else
		{
			FString SmErr;
			if (!ClaireonMaterialHelpers::SetShadingModel(Mat, SM, SmErr))
			{
				AddError(SmErr);
			}
		}
	}

	// 5. blend_mode
	FString BlendModeStr;
	if (Spec->TryGetStringField(TEXT("blend_mode"), BlendModeStr) && !BlendModeStr.IsEmpty())
	{
		EBlendMode BM;
		if (!SpecApplicatorMaterial_ParseBlendMode(BlendModeStr, BM))
		{
			AddError(FString::Printf(TEXT("blend_mode: invalid value '%s'"), *BlendModeStr));
		}
		else
		{
			FString BmErr;
			if (!ClaireonMaterialHelpers::SetBlendMode(Mat, BM, BmErr))
			{
				AddError(BmErr);
			}
		}
	}

	return !HasCriticalError();
}

bool FClaireonSpecApplicator_Material::CompileAsset(const FString& /*SessionId*/, FString& OutError)
{
	UMaterial* Mat = Material.Get();
	if (!Mat)
	{
		OutError = TEXT("Material is no longer valid");
		return false;
	}

	bool bCompileAtEnd = true;
	if (CachedSpec.IsValid())
	{
		CachedSpec->TryGetBoolField(TEXT("compile_at_end"), bCompileAtEnd);
	}

	if (!bCompileAtEnd)
	{
		return true;
	}

	UMaterialEditingLibrary::RecompileMaterial(Mat);
	return true;
}

bool FClaireonSpecApplicator_Material::SaveAsset(const FString& /*SessionId*/, FString& OutError)
{
	UMaterial* Mat = Material.Get();
	if (!Mat)
	{
		OutError = TEXT("Material is no longer valid");
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

	return ClaireonMaterialHelpers::SaveMaterialAsset(Mat, OutError);
}

void FClaireonSpecApplicator_Material::CloseSession(const FString& SessionId)
{
	FClaireonSessionManager::Get().CloseSession(SessionId);
}
