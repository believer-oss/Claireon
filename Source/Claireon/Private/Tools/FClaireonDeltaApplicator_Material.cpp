// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/FClaireonDeltaApplicator_Material.h"
#include "Tools/ClaireonMaterialEditToolBase.h"
#include "Tools/ClaireonMaterialHelpers.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "MaterialEditingLibrary.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace ClaireonDeltaApplicator_Material_anon
{
	static bool MatDelta_TryGetObject(const TSharedPtr<FJsonValue>& Entry, TSharedPtr<FJsonObject>& OutObj)
	{
		if (!Entry.IsValid() || Entry->Type != EJson::Object) { return false; }
		OutObj = Entry->AsObject();
		return OutObj.IsValid();
	}

	// Resolve an expression ref: first via id-map, then via FindExpressionByIdentifier.
	static UMaterialExpression* MatDelta_ResolveExpr(
		UMaterial* Material,
		const FString& Ref,
		const TMap<FString, FString>& IdMap,
		FString& OutError)
	{
		FString Resolved = Ref;
		if (const FString* Found = IdMap.Find(Ref)) { Resolved = *Found; }
		int32 Index = INDEX_NONE;
		UMaterialExpression* Expr = ClaireonMaterialHelpers::FindExpressionByIdentifier(Material, Resolved, Index);
		if (!Expr)
		{
			OutError = FString::Printf(TEXT("expression not found: '%s' (resolved: '%s')"), *Ref, *Resolved);
			return nullptr;
		}
		return Expr;
	}
}

bool FClaireonDeltaApplicator_Material::ValidateArgs(const TSharedPtr<FJsonObject>& Args, TArray<FString>& OutErrors)
{
	(void)Args; (void)OutErrors;
	return true;
}

bool FClaireonDeltaApplicator_Material::OpenOrReuseSession(const TSharedPtr<FJsonObject>& Args, FString& OutSessionId, FString& OutError)
{
	CachedMaterial.Reset();
	CreatedExpressionsThisCall.Reset();

	FString SessionIdArg;
	const bool bHasSessionId = Args->TryGetStringField(TEXT("session_id"), SessionIdArg) && !SessionIdArg.IsEmpty();
	if (bHasSessionId)
	{
		FMaterialEditToolData* Data = ClaireonMaterialEditToolBase::ToolData.Find(SessionIdArg);
		if (!Data || !Data->IsValid())
		{
			OutError = FString::Printf(TEXT("material_apply_delta: session_id '%s' not found"), *SessionIdArg);
			return false;
		}
		CachedMaterial = Data->Material;
		OutSessionId = SessionIdArg;
		return true;
	}

	FString AssetPathArg;
	if (!Args->TryGetStringField(TEXT("asset_path"), AssetPathArg) || AssetPathArg.IsEmpty())
	{
		OutError = TEXT("material_apply_delta: missing asset_path");
		return false;
	}

	UMaterial* Material = ClaireonMaterialHelpers::LoadMaterialAsset(AssetPathArg, OutError);
	if (!Material) { return false; }

	ClaireonMaterialEditToolBase::EnsureDelegateRegistered();

	const FString ResolvedAssetPath = Material->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(
		ResolvedAssetPath, ClaireonMaterialEditToolBase::MaterialSessionToolName);
	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		OutError = FString::Printf(TEXT("material_apply_delta: asset is locked by %s session %s"),
			*Blocker.ToolName, *Blocker.SessionId);
		return false;
	}
	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		OutError = FString::Printf(TEXT("material_apply_delta: invalid asset path: %s"), *ResolvedAssetPath);
		return false;
	}

	FMaterialEditToolData NewData;
	NewData.Material = Material;
	NewData.LastOperationStatus = TEXT("apply_delta opened");
	ClaireonMaterialEditToolBase::ToolData.Add(OpenResult.SessionId, MoveTemp(NewData));

	CachedMaterial = Material;
	OutSessionId = OpenResult.SessionId;
	return true;
}

bool FClaireonDeltaApplicator_Material::ApplyPhase1_Disconnect(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries)
{
	using namespace ClaireonDeltaApplicator_Material_anon;
	(void)SessionId;
	UMaterial* Mat = CachedMaterial.Get();
	if (!Mat)
	{
		AddError(TEXT("material_apply_delta: material is no longer valid"));
		return false;
	}
	for (int32 i = 0; i < Entries.Num(); ++i)
	{
		TSharedPtr<FJsonObject> Obj;
		if (!MatDelta_TryGetObject(Entries[i], Obj))
		{
			AddError(FString::Printf(TEXT("material_apply_delta: disconnect[%d] must be an object"), i));
			return false;
		}
		FString FromRef, ToRef, FromOutput, ToInput;
		Obj->TryGetStringField(TEXT("from_expr_id"), FromRef);
		Obj->TryGetStringField(TEXT("to_expr_id"), ToRef);
		Obj->TryGetStringField(TEXT("from_output"), FromOutput);
		Obj->TryGetStringField(TEXT("to_input"), ToInput);
		if (ToRef.IsEmpty() || ToInput.IsEmpty())
		{
			AddError(FString::Printf(TEXT("material_apply_delta: disconnect[%d] requires 'to_expr_id' and 'to_input'"), i));
			return false;
		}
		FString ResolveErr;
		UMaterialExpression* ToExpr = MatDelta_ResolveExpr(Mat, ToRef, GetIdMap(), ResolveErr);
		if (!ToExpr)
		{
			AddError(FString::Printf(TEXT("material_apply_delta: disconnect[%d]: %s"), i, *ResolveErr));
			return false;
		}
		FString DisErr;
		if (!ClaireonMaterialHelpers::DisconnectExpressionInput(Mat, ToExpr, ToInput, DisErr))
		{
			AddError(FString::Printf(TEXT("material_apply_delta: disconnect[%d]: %s"), i, *DisErr));
			return false;
		}
		MarkRemoved();
		RecordAffected(ToRef);
	}
	return true;
}

bool FClaireonDeltaApplicator_Material::ApplyPhase2_Remove(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries)
{
	using namespace ClaireonDeltaApplicator_Material_anon;
	(void)SessionId;
	UMaterial* Mat = CachedMaterial.Get();
	if (!Mat)
	{
		AddError(TEXT("material_apply_delta: material is no longer valid"));
		return false;
	}
	for (int32 i = 0; i < Entries.Num(); ++i)
	{
		FString Ref;
		if (Entries[i].IsValid() && Entries[i]->Type == EJson::String)
		{
			Ref = Entries[i]->AsString();
		}
		else
		{
			TSharedPtr<FJsonObject> Obj;
			if (!MatDelta_TryGetObject(Entries[i], Obj))
			{
				AddError(FString::Printf(TEXT("material_apply_delta: remove_nodes[%d] must be a string or object"), i));
				return false;
			}
			Obj->TryGetStringField(TEXT("id"), Ref);
			if (Ref.IsEmpty()) { Obj->TryGetStringField(TEXT("name"), Ref); }
		}
		if (Ref.IsEmpty())
		{
			AddError(FString::Printf(TEXT("material_apply_delta: remove_nodes[%d] requires 'id' or 'name'"), i));
			return false;
		}
		FString ResolveErr;
		UMaterialExpression* Expr = MatDelta_ResolveExpr(Mat, Ref, GetIdMap(), ResolveErr);
		if (!Expr)
		{
			AddError(FString::Printf(TEXT("material_apply_delta: remove_nodes[%d]: %s"), i, *ResolveErr));
			return false;
		}
		UMaterialEditingLibrary::DeleteMaterialExpression(Mat, Expr);
		MarkRemoved();
		RecordAffected(Ref);
	}
	return true;
}

bool FClaireonDeltaApplicator_Material::ApplyPhase3_Create(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries)
{
	using namespace ClaireonDeltaApplicator_Material_anon;
	(void)SessionId;
	UMaterial* Mat = CachedMaterial.Get();
	if (!Mat)
	{
		AddError(TEXT("material_apply_delta: material is no longer valid"));
		return false;
	}
	for (int32 i = 0; i < Entries.Num(); ++i)
	{
		TSharedPtr<FJsonObject> Obj;
		if (!MatDelta_TryGetObject(Entries[i], Obj))
		{
			AddError(FString::Printf(TEXT("material_apply_delta: nodes[%d] must be an object"), i));
			return false;
		}
		FString LocalId, ClassStr;
		Obj->TryGetStringField(TEXT("id"), LocalId);
		Obj->TryGetStringField(TEXT("type"), ClassStr);
		// Accept 'class' as legacy alias for 'type'
		if (ClassStr.IsEmpty()) { Obj->TryGetStringField(TEXT("class"), ClassStr); }
		if (LocalId.IsEmpty() || ClassStr.IsEmpty())
		{
			AddError(FString::Printf(TEXT("material_apply_delta: nodes[%d] requires 'id' and 'type'"), i));
			return false;
		}

		FString ResolveErr;
		UClass* ExprClass = ClaireonMaterialHelpers::ResolveExpressionClass(ClassStr, ResolveErr);
		if (!ExprClass)
		{
			AddError(FString::Printf(TEXT("material_apply_delta: nodes[%d]: %s"), i, *ResolveErr));
			return false;
		}

		double X = -400.0;
		double Y = static_cast<double>(i) * 120.0;
		Obj->TryGetNumberField(TEXT("x"), X);
		Obj->TryGetNumberField(TEXT("y"), Y);

		UMaterialExpression* Expr = UMaterialEditingLibrary::CreateMaterialExpression(
			Mat, ExprClass, static_cast<int32>(X), static_cast<int32>(Y));
		if (!Expr)
		{
			AddError(FString::Printf(TEXT("material_apply_delta: nodes[%d]: CreateMaterialExpression returned null for class '%s'"),
				i, *ClassStr));
			return false;
		}
		CreatedExpressionsThisCall.Add(Expr);

		// Optional properties{} (string -> string ImportText)
		const TSharedPtr<FJsonObject>* PropsObjPtr = nullptr;
		if (Obj->TryGetObjectField(TEXT("properties"), PropsObjPtr) && PropsObjPtr && PropsObjPtr->IsValid())
		{
			for (const auto& Pair : (*PropsObjPtr)->Values)
			{
				if (!Pair.Value.IsValid()) { continue; }
				const FString TextValue = Pair.Value->AsString();
				FString PropErr;
				if (!ClaireonMaterialHelpers::SetExpressionProperty(Mat, Expr, Pair.Key, TextValue, PropErr))
				{
					AddWarning(FString::Printf(TEXT("material_apply_delta: nodes[%d] property '%s': %s"),
						i, *Pair.Key, *PropErr));
				}
			}
		}

		// Resolve actual index for id_map
		TConstArrayView<TObjectPtr<UMaterialExpression>> AllExprs = Mat->GetExpressions();
		const int32 ActualIndex = AllExprs.IndexOfByKey(Expr);
		const FString ActualIdStr = ActualIndex != INDEX_NONE
			? FString::Printf(TEXT("#%d"), ActualIndex)
			: Expr->GetName();
		RegisterIdMapping(LocalId, ActualIdStr);
		MarkCreated();
		RecordAffected(ActualIdStr);
	}
	return true;
}

bool FClaireonDeltaApplicator_Material::ApplyPhase4_Connect(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries)
{
	using namespace ClaireonDeltaApplicator_Material_anon;
	(void)SessionId;
	UMaterial* Mat = CachedMaterial.Get();
	if (!Mat)
	{
		AddError(TEXT("material_apply_delta: material is no longer valid"));
		return false;
	}
	for (int32 i = 0; i < Entries.Num(); ++i)
	{
		TSharedPtr<FJsonObject> Obj;
		if (!MatDelta_TryGetObject(Entries[i], Obj))
		{
			AddError(FString::Printf(TEXT("material_apply_delta: connections[%d] must be an object"), i));
			return false;
		}
		FString FromRef, FromOutput;
		Obj->TryGetStringField(TEXT("from"), FromRef);
		if (FromRef.IsEmpty()) { Obj->TryGetStringField(TEXT("from_id"), FromRef); }
		Obj->TryGetStringField(TEXT("from_output"), FromOutput);
		if (FromRef.IsEmpty())
		{
			AddError(FString::Printf(TEXT("material_apply_delta: connections[%d] requires 'from'"), i));
			return false;
		}
		FString FromErr;
		UMaterialExpression* FromExpr = MatDelta_ResolveExpr(Mat, FromRef, GetIdMap(), FromErr);
		if (!FromExpr)
		{
			AddError(FString::Printf(TEXT("material_apply_delta: connections[%d]: %s"), i, *FromErr));
			return false;
		}

		// Phase 4 attribute dispatch: presence of 'attribute' field selects expression-to-attribute path.
		FString Attribute;
		if (Obj->TryGetStringField(TEXT("attribute"), Attribute) && !Attribute.IsEmpty())
		{
			FString AttrErr;
			if (!ClaireonMaterialHelpers::ConnectToMaterialAttribute(Mat, FromExpr, Attribute, FromOutput, AttrErr))
			{
				AddError(FString::Printf(TEXT("material_apply_delta: connections[%d] attribute '%s': %s"),
					i, *Attribute, *AttrErr));
				return false;
			}
			MarkConnection();
			RecordAffected(FString::Printf(TEXT("attribute:%s"), *Attribute));
			continue;
		}

		// expression-to-expression path
		FString ToRef, ToInput;
		Obj->TryGetStringField(TEXT("to"), ToRef);
		if (ToRef.IsEmpty()) { Obj->TryGetStringField(TEXT("to_id"), ToRef); }
		Obj->TryGetStringField(TEXT("to_input"), ToInput);
		if (ToRef.IsEmpty())
		{
			AddError(FString::Printf(TEXT("material_apply_delta: connections[%d] requires 'to' (or 'attribute')"), i));
			return false;
		}
		FString ToErr;
		UMaterialExpression* ToExpr = MatDelta_ResolveExpr(Mat, ToRef, GetIdMap(), ToErr);
		if (!ToExpr)
		{
			AddError(FString::Printf(TEXT("material_apply_delta: connections[%d]: %s"), i, *ToErr));
			return false;
		}
		FString ConnErr;
		if (!ClaireonMaterialHelpers::ConnectExpressions(Mat, FromExpr, FromOutput, ToExpr, ToInput, ConnErr))
		{
			AddError(FString::Printf(TEXT("material_apply_delta: connections[%d]: %s"), i, *ConnErr));
			return false;
		}
		MarkConnection();
		RecordAffected(ToRef);
	}
	return true;
}

void FClaireonDeltaApplicator_Material::FinalizeSession(const FString& SessionId)
{
	(void)SessionId;
	UMaterial* Mat = CachedMaterial.Get();
	if (Mat) { Mat->MarkPackageDirty(); }
}

void FClaireonDeltaApplicator_Material::CloseSessionIfOwned(const FString& SessionId)
{
	if (DoesOwnSession() && !SessionId.IsEmpty())
	{
		ClaireonMaterialEditToolBase::ToolData.Remove(SessionId);
		FClaireonSessionManager::Get().CloseSession(SessionId);
	}
}

void FClaireonDeltaApplicator_Material::Phase3CleanupOnFailure(const FString& SessionId)
{
	(void)SessionId;
	UMaterial* Mat = CachedMaterial.Get();
	if (!Mat) { return; }
	for (const TWeakObjectPtr<UMaterialExpression>& Weak : CreatedExpressionsThisCall)
	{
		UMaterialExpression* Expr = Weak.Get();
		if (Expr)
		{
			UMaterialEditingLibrary::DeleteMaterialExpression(Mat, Expr);
		}
	}
	CreatedExpressionsThisCall.Reset();
}
