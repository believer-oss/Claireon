// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/FClaireonDeltaApplicator_WidgetBP.h"
#include "Tools/ClaireonWidgetBPEditToolBase.h"
#include "ClaireonWidgetHelpers.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace ClaireonDeltaApplicator_WidgetBP_anon
{
	static bool WBPDelta_TryGetObject(const TSharedPtr<FJsonValue>& Entry, TSharedPtr<FJsonObject>& OutObj)
	{
		if (!Entry.IsValid() || Entry->Type != EJson::Object) { return false; }
		OutObj = Entry->AsObject();
		return OutObj.IsValid();
	}

	// Resolve a widget ref: first via id-map, then by name in the widget tree.
	static UWidget* WBPDelta_ResolveWidget(
		UWidgetTree* Tree,
		const FString& Ref,
		const TMap<FString, FString>& IdMap,
		FString& OutError)
	{
		FString Resolved = Ref;
		if (const FString* Found = IdMap.Find(Ref)) { Resolved = *Found; }
		UWidget* Widget = ClaireonWidgetHelpers::FindWidgetByName(Tree, FName(*Resolved));
		if (!Widget)
		{
			OutError = FString::Printf(TEXT("widget not found: '%s' (resolved: '%s')"), *Ref, *Resolved);
			return nullptr;
		}
		return Widget;
	}
}

bool FClaireonDeltaApplicator_WidgetBP::ValidateArgs(const TSharedPtr<FJsonObject>& Args, TArray<FString>& OutErrors)
{
	(void)Args; (void)OutErrors;
	return true;
}

bool FClaireonDeltaApplicator_WidgetBP::OpenOrReuseSession(const TSharedPtr<FJsonObject>& Args, FString& OutSessionId, FString& OutError)
{
	CachedWBP.Reset();
	CreatedWidgetsThisCall.Reset();

	FString SessionIdArg;
	const bool bHasSessionId = Args->TryGetStringField(TEXT("session_id"), SessionIdArg) && !SessionIdArg.IsEmpty();
	if (bHasSessionId)
	{
		FWidgetBPEditToolData* Data = ClaireonWidgetBPEditToolBase::ToolData.Find(SessionIdArg);
		if (!Data || !Data->IsValid())
		{
			OutError = FString::Printf(TEXT("widgetbp_apply_delta: session_id '%s' not found"), *SessionIdArg);
			return false;
		}
		CachedWBP = Data->WidgetBlueprint;
		OutSessionId = SessionIdArg;
		return true;
	}

	FString AssetPathArg;
	if (!Args->TryGetStringField(TEXT("asset_path"), AssetPathArg) || AssetPathArg.IsEmpty())
	{
		OutError = TEXT("widgetbp_apply_delta: missing asset_path");
		return false;
	}

	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPathArg);
	if (!ResolveResult.bSuccess)
	{
		OutError = ResolveResult.Error;
		return false;
	}
	const FString ResolvedPath = ResolveResult.ResolvedPath.Path;

	UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *ResolvedPath);
	if (!WBP)
	{
		OutError = FString::Printf(TEXT("widgetbp_apply_delta: failed to load Widget Blueprint: %s"), *ResolvedPath);
		return false;
	}

	const FString CanonicalPath = WBP->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(
		CanonicalPath, TEXT("widgetbp_edit"));
	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		OutError = FString::Printf(TEXT("widgetbp_apply_delta: asset is locked by %s session %s"),
			*Blocker.ToolName, *Blocker.SessionId);
		return false;
	}
	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		OutError = FString::Printf(TEXT("widgetbp_apply_delta: invalid asset path: %s"), *CanonicalPath);
		return false;
	}

	FWidgetBPEditToolData NewData;
	NewData.WidgetBlueprint = WBP;
	NewData.bModified = false;
	NewData.LastCommandTime = FDateTime::Now();
	ClaireonWidgetBPEditToolBase::ToolData.Add(OpenResult.SessionId, MoveTemp(NewData));

	CachedWBP = WBP;
	OutSessionId = OpenResult.SessionId;
	return true;
}

bool FClaireonDeltaApplicator_WidgetBP::ApplyPhase2_Remove(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries)
{
	using namespace ClaireonDeltaApplicator_WidgetBP_anon;
	(void)SessionId;
	UWidgetBlueprint* WBP = CachedWBP.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		AddError(TEXT("widgetbp_apply_delta: widget blueprint is no longer valid"));
		return false;
	}
	UWidgetTree* Tree = WBP->WidgetTree;
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
			if (!WBPDelta_TryGetObject(Entries[i], Obj))
			{
				AddError(FString::Printf(TEXT("widgetbp_apply_delta: remove_nodes[%d] must be a string or object"), i));
				return false;
			}
			Obj->TryGetStringField(TEXT("name"), Ref);
			if (Ref.IsEmpty()) { Obj->TryGetStringField(TEXT("id"), Ref); }
		}
		if (Ref.IsEmpty())
		{
			AddError(FString::Printf(TEXT("widgetbp_apply_delta: remove_nodes[%d] requires 'name' or 'id'"), i));
			return false;
		}
		FString ResolveErr;
		UWidget* Widget = WBPDelta_ResolveWidget(Tree, Ref, GetIdMap(), ResolveErr);
		if (!Widget)
		{
			AddError(FString::Printf(TEXT("widgetbp_apply_delta: remove_nodes[%d]: %s"), i, *ResolveErr));
			return false;
		}
		Tree->SetFlags(RF_Transactional);
		Tree->Modify();
		if (Tree->RootWidget == Widget)
		{
			Tree->RootWidget = nullptr;
		}
		else
		{
			Tree->RemoveWidget(Widget);
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
		MarkRemoved();
		RecordAffected(Ref);
	}
	return true;
}

bool FClaireonDeltaApplicator_WidgetBP::ApplyPhase3_Create(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries)
{
	using namespace ClaireonDeltaApplicator_WidgetBP_anon;
	(void)SessionId;
	UWidgetBlueprint* WBP = CachedWBP.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		AddError(TEXT("widgetbp_apply_delta: widget blueprint is no longer valid"));
		return false;
	}
	UWidgetTree* Tree = WBP->WidgetTree;
	for (int32 i = 0; i < Entries.Num(); ++i)
	{
		TSharedPtr<FJsonObject> Obj;
		if (!WBPDelta_TryGetObject(Entries[i], Obj))
		{
			AddError(FString::Printf(TEXT("widgetbp_apply_delta: nodes[%d] must be an object"), i));
			return false;
		}
		FString LocalId, ClassStr, ParentRef, WidgetName;
		Obj->TryGetStringField(TEXT("id"), LocalId);
		Obj->TryGetStringField(TEXT("type"), ClassStr);
		if (ClassStr.IsEmpty()) { Obj->TryGetStringField(TEXT("class"), ClassStr); }
		Obj->TryGetStringField(TEXT("parent_id"), ParentRef);
		Obj->TryGetStringField(TEXT("name"), WidgetName);
		if (LocalId.IsEmpty() || ClassStr.IsEmpty())
		{
			AddError(FString::Printf(TEXT("widgetbp_apply_delta: nodes[%d] requires 'id' and 'type'"), i));
			return false;
		}
		FString ClassErr;
		UClass* WidgetClass = ClaireonWidgetHelpers::ResolveWidgetClass(ClassStr, ClassErr);
		if (!WidgetClass)
		{
			AddError(FString::Printf(TEXT("widgetbp_apply_delta: nodes[%d]: %s"), i, *ClassErr));
			return false;
		}

		Tree->SetFlags(RF_Transactional);
		Tree->Modify();

		const FName FinalName = WidgetName.IsEmpty() ? NAME_None : FName(*WidgetName);
		UWidget* NewWidget = ClaireonWidgetHelpers::CreateWidget(Tree, WidgetClass, FinalName);
		if (!NewWidget)
		{
			AddError(FString::Printf(TEXT("widgetbp_apply_delta: nodes[%d]: failed to create widget of class '%s'"),
				i, *ClassStr));
			return false;
		}
		CreatedWidgetsThisCall.Add(NewWidget);

		// Optional slot_properties{}.
		const TSharedPtr<FJsonObject>* SlotPropsPtr = nullptr;
		TSharedPtr<FJsonObject> SlotProps;
		if (Obj->TryGetObjectField(TEXT("slot_properties"), SlotPropsPtr) && SlotPropsPtr)
		{
			SlotProps = *SlotPropsPtr;
		}

		if (!Tree->RootWidget)
		{
			// First widget becomes root if no root exists.
			Tree->RootWidget = NewWidget;
		}
		else
		{
			UPanelWidget* ParentPanel = nullptr;
			if (!ParentRef.IsEmpty())
			{
				FString ResolveErr;
				UWidget* ParentWidget = WBPDelta_ResolveWidget(Tree, ParentRef, GetIdMap(), ResolveErr);
				if (!ParentWidget)
				{
					AddError(FString::Printf(TEXT("widgetbp_apply_delta: nodes[%d]: parent_id %s"), i, *ResolveErr));
					return false;
				}
				ParentPanel = Cast<UPanelWidget>(ParentWidget);
				if (!ParentPanel)
				{
					AddError(FString::Printf(TEXT("widgetbp_apply_delta: nodes[%d]: parent_id '%s' is not a panel"), i, *ParentRef));
					return false;
				}
			}
			else
			{
				ParentPanel = Cast<UPanelWidget>(Tree->RootWidget);
				if (!ParentPanel)
				{
					AddError(FString::Printf(TEXT("widgetbp_apply_delta: nodes[%d]: root widget is not a panel and no parent_id provided"), i));
					return false;
				}
			}

			ClaireonWidgetHelpers::AddChildToPanel(ParentPanel, NewWidget, SlotProps);
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

		const FString CreatedName = NewWidget->GetName();
		RegisterIdMapping(LocalId, CreatedName);
		MarkCreated();
		RecordAffected(CreatedName);
	}
	return true;
}

bool FClaireonDeltaApplicator_WidgetBP::ApplyPhase4_Connect(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries)
{
	using namespace ClaireonDeltaApplicator_WidgetBP_anon;
	(void)SessionId;
	UWidgetBlueprint* WBP = CachedWBP.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		AddError(TEXT("widgetbp_apply_delta: widget blueprint is no longer valid"));
		return false;
	}
	UWidgetTree* Tree = WBP->WidgetTree;

	for (int32 i = 0; i < Entries.Num(); ++i)
	{
		TSharedPtr<FJsonObject> Obj;
		if (!WBPDelta_TryGetObject(Entries[i], Obj))
		{
			AddError(FString::Printf(TEXT("widgetbp_apply_delta: connections[%d] must be an object"), i));
			return false;
		}
		FString WidgetRef, NewParentRef;
		Obj->TryGetStringField(TEXT("widget_id"), WidgetRef);
		Obj->TryGetStringField(TEXT("new_parent_id"), NewParentRef);
		if (WidgetRef.IsEmpty() || NewParentRef.IsEmpty())
		{
			AddError(FString::Printf(TEXT("widgetbp_apply_delta: connections[%d] requires 'widget_id' and 'new_parent_id'"), i));
			return false;
		}
		// new_parent_slot_id is interpreted as an insertion index when numeric; otherwise -1 (append).
		int32 InsertIndex = -1;
		double NumericIndex = -1.0;
		if (Obj->TryGetNumberField(TEXT("new_parent_slot_id"), NumericIndex))
		{
			InsertIndex = static_cast<int32>(NumericIndex);
		}
		FString WidgetErr, ParentErr;
		UWidget* Widget = WBPDelta_ResolveWidget(Tree, WidgetRef, GetIdMap(), WidgetErr);
		if (!Widget)
		{
			AddError(FString::Printf(TEXT("widgetbp_apply_delta: connections[%d]: %s"), i, *WidgetErr));
			return false;
		}
		UWidget* NewParent = WBPDelta_ResolveWidget(Tree, NewParentRef, GetIdMap(), ParentErr);
		if (!NewParent)
		{
			AddError(FString::Printf(TEXT("widgetbp_apply_delta: connections[%d]: new_parent %s"), i, *ParentErr));
			return false;
		}

		// H4: delegate to the shared helper (same code path as the discrete move_widget tool).
		FString MoveErr;
		if (!ClaireonWidgetHelpers::MoveWidget(WBP, Widget, NewParent, InsertIndex, MoveErr))
		{
			AddError(FString::Printf(TEXT("widgetbp_apply_delta: connections[%d]: %s"), i, *MoveErr));
			return false;
		}
		MarkConnection();
		RecordAffected(Widget->GetName());
	}
	return true;
}

void FClaireonDeltaApplicator_WidgetBP::FinalizeSession(const FString& SessionId)
{
	(void)SessionId;
	UWidgetBlueprint* WBP = CachedWBP.Get();
	if (WBP) { WBP->MarkPackageDirty(); }
}

void FClaireonDeltaApplicator_WidgetBP::CloseSessionIfOwned(const FString& SessionId)
{
	if (DoesOwnSession() && !SessionId.IsEmpty())
	{
		ClaireonWidgetBPEditToolBase::ToolData.Remove(SessionId);
		FClaireonSessionManager::Get().CloseSession(SessionId);
	}
}

void FClaireonDeltaApplicator_WidgetBP::Phase3CleanupOnFailure(const FString& SessionId)
{
	(void)SessionId;
	UWidgetBlueprint* WBP = CachedWBP.Get();
	if (!WBP || !WBP->WidgetTree) { return; }
	UWidgetTree* Tree = WBP->WidgetTree;
	for (const TWeakObjectPtr<UWidget>& Weak : CreatedWidgetsThisCall)
	{
		UWidget* W = Weak.Get();
		if (W)
		{
			if (Tree->RootWidget == W) { Tree->RootWidget = nullptr; }
			else { Tree->RemoveWidget(W); }
		}
	}
	CreatedWidgetsThisCall.Reset();
}
