// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSpecApplicator_WidgetBP.h"
#include "ClaireonWidgetHelpers.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "ClaireonSafeExec.h"
#include "ClaireonLog.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "FileHelpers.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

#include "Animation/WidgetAnimation.h"
#include "Animation/WidgetAnimationBinding.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneTrack.h"
#include "MovieSceneSection.h"
#include "ClaireonSequenceEditHandlers.h"
#include "ClaireonWidgetAnimationHandlers.h"
#include "KeyParams.h"

bool FClaireonSpecApplicator_WidgetBP::ValidateToolSpec(const TSharedPtr<FJsonObject>& Spec, TArray<FString>& OutErrors)
{
	const TArray<TSharedPtr<FJsonValue>>* WidgetsArray = nullptr;
	const bool bHasWidgets = Spec->TryGetArrayField(TEXT("widgets"), WidgetsArray) && WidgetsArray && WidgetsArray->Num() > 0;

	const TArray<TSharedPtr<FJsonValue>>* AnimationsArray = nullptr;
	const bool bHasAnimations = Spec->TryGetArrayField(TEXT("animations"), AnimationsArray) && AnimationsArray && AnimationsArray->Num() > 0;

	// Relaxation for #0000: accept specs that have animations[] even without widgets[]
	// so animation-only specs can run against an already-existing WBP.
	if (!bHasWidgets && !bHasAnimations)
	{
		OutErrors.Add(TEXT("WidgetBP spec must contain a non-empty 'widgets' or 'animations' array"));
		return false;
	}

	if (bHasWidgets)
	{
		int32 RootCount = 0;
		for (int32 i = 0; i < WidgetsArray->Num(); ++i)
		{
			const TSharedPtr<FJsonValue>& Val = (*WidgetsArray)[i];
			if (!Val.IsValid() || Val->Type != EJson::Object) continue;
			const TSharedPtr<FJsonObject>& Obj = Val->AsObject();

			FString Id, Type;
			if (!Obj->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty())
				OutErrors.Add(FString::Printf(TEXT("widgets[%d]: missing or empty 'id'"), i));
			if (!Obj->TryGetStringField(TEXT("type"), Type) || Type.IsEmpty())
				OutErrors.Add(FString::Printf(TEXT("widgets[%d]: missing or empty 'type'"), i));

			if (!Obj->HasField(TEXT("parent")) || Obj->GetField<EJson::None>(TEXT("parent"))->IsNull())
			{
				RootCount++;
			}
		}
		if (RootCount == 0)
		{
			OutErrors.Add(TEXT("WidgetBP spec must have exactly one root widget (parent: null)"));
		}
		else if (RootCount > 1)
		{
			OutErrors.Add(FString::Printf(TEXT("WidgetBP spec has %d root widgets (expected exactly 1)"), RootCount));
		}
	}

	if (bHasAnimations)
	{
		TSet<FString> SeenNames;
		for (int32 i = 0; i < AnimationsArray->Num(); ++i)
		{
			const TSharedPtr<FJsonValue>& Val = (*AnimationsArray)[i];
			if (!Val.IsValid() || Val->Type != EJson::Object) continue;
			const TSharedPtr<FJsonObject>& Obj = Val->AsObject();

			FString AnimName;
			if (!Obj->TryGetStringField(TEXT("name"), AnimName) || AnimName.IsEmpty())
			{
				OutErrors.Add(FString::Printf(TEXT("animations[%d]: missing or empty 'name'"), i));
				continue;
			}
			if (SeenNames.Contains(AnimName))
			{
				OutErrors.Add(FString::Printf(TEXT("animations[%d]: duplicate name '%s'"), i, *AnimName));
			}
			SeenNames.Add(AnimName);

			const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
			if (Obj->TryGetArrayField(TEXT("bindings"), Bindings) && Bindings)
			{
				for (int32 b = 0; b < Bindings->Num(); ++b)
				{
					const TSharedPtr<FJsonObject>& BindObj = (*Bindings)[b]->AsObject();
					if (!BindObj.IsValid()) continue;
					FString Slot;
					if (BindObj->TryGetStringField(TEXT("slot_widget"), Slot) && Slot.IsEmpty())
					{
						OutErrors.Add(FString::Printf(
							TEXT("animations[%d].bindings[%d].slot_widget must be a non-empty string when present (NAME_None is the absent-key default)"), i, b));
					}
				}
			}
		}
	}

	return OutErrors.Num() == 0;
}

bool FClaireonSpecApplicator_WidgetBP::OpenOrCreateAsset(const FString& AssetPath, FString& OutSessionId, FString& OutError)
{
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		OutError = ResolveResult.Error;
		return false;
	}
	const FString ResolvedPath = ResolveResult.ResolvedPath.Path;

	// Try to load existing
	UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *ResolvedPath);
	if (!WBP)
	{
		// WidgetBP supports creation -- but it requires the full widget factory
		// For now, require the asset to exist
		OutError = FString::Printf(TEXT("Widget Blueprint not found: %s"), *ResolvedPath);
		return false;
	}

	const FString WBPPathName = WBP->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(
		WBPPathName, TEXT("widgetbp_edit"));

	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		OutError = FString::Printf(TEXT("Asset is locked by %s session %s"),
			*Blocker.ToolName, *Blocker.SessionId);
		return false;
	}
	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		OutError = FString::Printf(TEXT("Invalid asset path: %s"), *WBPPathName);
		return false;
	}

	WidgetBlueprint = WBP;
	OutSessionId = OpenResult.SessionId;
	return true;
}

bool FClaireonSpecApplicator_WidgetBP::ApplyPass1_CreateEntities(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec)
{
	UWidgetBlueprint* WBP = WidgetBlueprint.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		AddError(TEXT("WidgetBlueprint or WidgetTree is no longer valid"));
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* WidgetsArray = nullptr;
	if (!Spec->TryGetArrayField(TEXT("widgets"), WidgetsArray) || !WidgetsArray)
	{
		return true;
	}

	// Build dependency-ordered list (parents first)
	TMap<FString, TSharedPtr<FJsonObject>> WidgetMap;
	for (const TSharedPtr<FJsonValue>& Val : *WidgetsArray)
	{
		const TSharedPtr<FJsonObject>& Obj = Val->AsObject();
		if (!Obj.IsValid()) continue;
		FString Id;
		Obj->TryGetStringField(TEXT("id"), Id);
		WidgetMap.Add(Id, Obj);
	}

	TArray<FString> Ordered;
	TSet<FString> Visited;
	TFunction<void(const FString&)> Visit = [&](const FString& Id)
	{
		if (Visited.Contains(Id)) return;
		Visited.Add(Id);

		const TSharedPtr<FJsonObject>* Found = WidgetMap.Find(Id);
		if (!Found) return;

		FString ParentId;
		if ((*Found)->TryGetStringField(TEXT("parent"), ParentId) && !ParentId.IsEmpty())
		{
			Visit(ParentId);
		}
		Ordered.Add(Id);
	};
	for (const auto& Pair : WidgetMap)
	{
		Visit(Pair.Key);
	}

	int32 SuccessCount = 0;
	UWidgetTree* WidgetTree = WBP->WidgetTree;

	// Reset the per-apply-spec widget map so re-entrant runs don't see stale entries
	WidgetsBySpecId.Reset();

	for (const FString& WidgetId : Ordered)
	{
		const TSharedPtr<FJsonObject>& WidgetObj = WidgetMap[WidgetId];

		FString WidgetType;
		WidgetObj->TryGetStringField(TEXT("type"), WidgetType);

		FString Error;
		UClass* WidgetClass = ClaireonWidgetHelpers::ResolveWidgetClass(WidgetType, Error);
		if (!WidgetClass)
		{
			RecordEntryFailure(WidgetId, Error);
			continue;
		}

		FName WidgetName(*WidgetId);
		// Generate a unique name for the widget
		UWidget* NewWidget = ClaireonWidgetHelpers::CreateWidget(WidgetTree, WidgetClass, WidgetName);
		if (!NewWidget)
		{
			RecordEntryFailure(WidgetId, FString::Printf(TEXT("Failed to create widget of type: %s"), *WidgetType));
			continue;
		}

		// Check if this is the root widget
		FString ParentId;
		bool bHasParent = WidgetObj->TryGetStringField(TEXT("parent"), ParentId) && !ParentId.IsEmpty();
		if (!bHasParent)
		{
			// Set as root widget
			WidgetTree->RootWidget = NewWidget;
		}

		// Track every newly created widget so Pass 2 can find it even before it
		// has been attached to the tree. Tree->FindWidget walks from RootWidget,
		// which doesn't see un-parented widgets created in this pass.
		WidgetsBySpecId.Add(WidgetId, NewWidget);

		FString ActualName = NewWidget->GetName();
		RegisterIdMapping(WidgetId, ActualName);
		RecordEntrySuccess(WidgetId, ActualName);
		SuccessCount++;
	}

	UE_LOG(LogClaireon, Log, TEXT("[apply_spec:WidgetBP] Pass 1: Created %d/%d widgets"),
		SuccessCount, WidgetsArray->Num());

	return true;
}

bool FClaireonSpecApplicator_WidgetBP::ApplyPass2_WireRelationships(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec)
{
	UWidgetBlueprint* WBP = WidgetBlueprint.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		AddError(TEXT("WidgetBlueprint or WidgetTree is no longer valid"));
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* WidgetsArray = nullptr;
	if (!Spec->TryGetArrayField(TEXT("widgets"), WidgetsArray) || !WidgetsArray)
	{
		return true;
	}

	UWidgetTree* WidgetTree = WBP->WidgetTree;

	for (const TSharedPtr<FJsonValue>& Val : *WidgetsArray)
	{
		const TSharedPtr<FJsonObject>& WidgetObj = Val->AsObject();
		if (!WidgetObj.IsValid()) continue;

		FString SpecId;
		WidgetObj->TryGetStringField(TEXT("id"), SpecId);
		if (!IsIdCreated(SpecId)) continue;

		FString ParentId;
		bool bHasParent = WidgetObj->TryGetStringField(TEXT("parent"), ParentId) && !ParentId.IsEmpty();

		if (bHasParent)
		{
			FString ChildName = ResolveId(SpecId);
			FString ParentName = ResolveId(ParentId);

			if (ParentName.IsEmpty())
			{
				AddWarning(FString::Printf(TEXT("Widget '%s' parent '%s' not found"), *SpecId, *ParentId));
				continue;
			}

			// Pass 1 stashes every created widget in WidgetsBySpecId. Use that map
			// here so we can find children that haven't been attached to the tree yet
			// (Tree->FindWidget only walks from RootWidget and would miss them).
			auto LookupWidget = [&](const FString& Id, const FString& Name) -> UWidget*
			{
				if (TWeakObjectPtr<UWidget>* Stashed = WidgetsBySpecId.Find(Id))
				{
					if (UWidget* W = Stashed->Get()) return W;
				}
				return ClaireonWidgetHelpers::FindWidgetByName(WidgetTree, FName(*Name));
			};

			UWidget* ChildWidget = LookupWidget(SpecId, ChildName);
			UWidget* ParentWidget = LookupWidget(ParentId, ParentName);

			if (!ChildWidget || !ParentWidget)
			{
				AddWarning(FString::Printf(TEXT("Could not find widgets for parent-child: '%s' -> '%s'"), *ParentId, *SpecId));
				continue;
			}

			// Add child to parent panel
			TSharedPtr<FJsonObject> SlotPropsObj;
			const TSharedPtr<FJsonObject>* SlotPropsPtr = nullptr;
			if (WidgetObj->TryGetObjectField(TEXT("slot_properties"), SlotPropsPtr) && SlotPropsPtr)
			{
				SlotPropsObj = *SlotPropsPtr;
			}

			UPanelWidget* ParentPanel = Cast<UPanelWidget>(ParentWidget);
			if (!ParentPanel)
			{
				AddWarning(FString::Printf(TEXT("Parent '%s' is not a panel widget, cannot add children"), *ParentId));
				continue;
			}

			UPanelSlot* Slot = ClaireonWidgetHelpers::AddChildToPanel(ParentPanel, ChildWidget, SlotPropsObj);
			if (!Slot)
			{
				AddWarning(FString::Printf(TEXT("Failed to add '%s' as child of '%s'"), *SpecId, *ParentId));
			}
			else if (SlotPropsObj.IsValid())
			{
				// Set slot properties
				for (const auto& Prop : SlotPropsObj->Values)
				{
					FString PropValue;
					if (Prop.Value->TryGetString(PropValue))
					{
						FString PropError;
						ClaireonWidgetHelpers::WriteSlotProperty(Slot, Prop.Key, PropValue, PropError);
					}
				}
			}
		}

		// --- Set widget properties ---
		const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
		if (WidgetObj->TryGetObjectField(TEXT("properties"), PropsPtr) && PropsPtr && (*PropsPtr).IsValid())
		{
			FString WidgetName = ResolveId(SpecId);
			UWidget* Widget = nullptr;
			if (TWeakObjectPtr<UWidget>* Stashed = WidgetsBySpecId.Find(SpecId))
			{
				Widget = Stashed->Get();
			}
			if (!Widget)
			{
				Widget = ClaireonWidgetHelpers::FindWidgetByName(WidgetTree, FName(*WidgetName));
			}
			if (Widget)
			{
				for (const auto& Prop : (*PropsPtr)->Values)
				{
					FString PropValue;
					if (Prop.Value->TryGetString(PropValue))
					{
						FString PropError;
						if (!ClaireonWidgetHelpers::WriteWidgetProperty(Widget, Prop.Key, PropValue, PropError))
						{
							AddWarning(FString::Printf(TEXT("Widget '%s' property '%s': %s"), *SpecId, *Prop.Key, *PropError));
						}
					}
				}
			}
		}
	}

	// --- #0000 animations[] branch ---
	const TArray<TSharedPtr<FJsonValue>>* AnimationsArray = nullptr;
	if (Spec->TryGetArrayField(TEXT("animations"), AnimationsArray) && AnimationsArray)
	{
		for (const TSharedPtr<FJsonValue>& AnimVal : *AnimationsArray)
		{
			const TSharedPtr<FJsonObject> AnimObj = AnimVal.IsValid() ? AnimVal->AsObject() : nullptr;
			if (!AnimObj.IsValid()) continue;

			FString AnimName;
			AnimObj->TryGetStringField(TEXT("name"), AnimName);
			if (AnimName.IsEmpty())
			{
				RecordEntryFailure(TEXT("animations[?]"), TEXT("missing 'name'"));
				continue;
			}

			double DurationD = 5.0;
			AnimObj->TryGetNumberField(TEXT("duration"), DurationD);
			FString DisplayLabel;
			AnimObj->TryGetStringField(TEXT("display_label"), DisplayLabel);

			UWidgetAnimation* Anim = nullptr;
			FString CreateError;
			if (!ApplyCreateAnimation(WBP, AnimName, static_cast<float>(DurationD), DisplayLabel, Anim, CreateError))
			{
				RecordEntryFailure(AnimName, CreateError);
				continue;
			}

			const TArray<TSharedPtr<FJsonValue>>* BindingsArr = nullptr;
			if (!AnimObj->TryGetArrayField(TEXT("bindings"), BindingsArr) || !BindingsArr)
			{
				RecordEntrySuccess(AnimName, AnimName);
				continue;
			}

			for (const TSharedPtr<FJsonValue>& BindVal : *BindingsArr)
			{
				const TSharedPtr<FJsonObject> BindObj = BindVal.IsValid() ? BindVal->AsObject() : nullptr;
				if (!BindObj.IsValid()) continue;

				FString BindWidget;
				BindObj->TryGetStringField(TEXT("widget"), BindWidget);
				if (BindWidget.IsEmpty())
				{
					RecordEntryFailure(AnimName, TEXT("binding missing 'widget'"));
					continue;
				}

				FString SlotWidget;
				const TSharedPtr<FJsonValue>& SlotField = BindObj->TryGetField(TEXT("slot_widget"));
				if (SlotField.IsValid() && SlotField->Type == EJson::String)
				{
					SlotField->TryGetString(SlotWidget);
				}

				UWidget* Widget = nullptr;
				if (TWeakObjectPtr<UWidget>* Stashed = WidgetsBySpecId.Find(BindWidget))
				{
					Widget = Stashed->Get();
				}
				if (!Widget)
				{
					Widget = ClaireonWidgetHelpers::FindWidgetByName(WidgetTree, FName(*BindWidget));
				}
				if (!Widget)
				{
					RecordEntryFailure(AnimName, FString::Printf(TEXT("binding widget '%s' not found"), *BindWidget));
					continue;
				}

				FGuid BindingGuid;
				FString BindError;
				if (!ApplyAddAnimationBinding(Anim, Widget, SlotWidget, BindingGuid, BindError))
				{
					RecordEntryFailure(AnimName, BindError);
					continue;
				}

				const TArray<TSharedPtr<FJsonValue>>* TracksArr = nullptr;
				if (!BindObj->TryGetArrayField(TEXT("tracks"), TracksArr) || !TracksArr)
				{
					continue;
				}

				for (const TSharedPtr<FJsonValue>& TrackVal : *TracksArr)
				{
					const TSharedPtr<FJsonObject> TrackObj = TrackVal.IsValid() ? TrackVal->AsObject() : nullptr;
					if (!TrackObj.IsValid()) continue;

					FString TrackType;
					TrackObj->TryGetStringField(TEXT("type"), TrackType);
					FString TrackProp;
					TrackObj->TryGetStringField(TEXT("property"), TrackProp);

					UMovieSceneTrack* NewTrack = nullptr;
					FString TrackError;
					if (!ApplyAddAnimationTrack(Anim, BindingGuid, TrackType, TrackProp, NewTrack, TrackError))
					{
						RecordEntryFailure(AnimName, TrackError);
						continue;
					}

					if (!NewTrack || NewTrack->GetAllSections().Num() == 0)
					{
						RecordEntryFailure(AnimName, TEXT("track has no sections"));
						continue;
					}
					UMovieSceneSection* Section = NewTrack->GetAllSections()[0];
					UMovieScene* MS = Anim->GetMovieScene();
					if (!Section || !MS) continue;

					const FFrameRate TickResolution = MS->GetTickResolution();
					const TArray<TSharedPtr<FJsonValue>>* KeysArr = nullptr;
					if (!TrackObj->TryGetArrayField(TEXT("keyframes"), KeysArr) || !KeysArr)
					{
						continue;
					}

					for (const TSharedPtr<FJsonValue>& KeyVal : *KeysArr)
					{
						const TSharedPtr<FJsonObject> KeyObj = KeyVal.IsValid() ? KeyVal->AsObject() : nullptr;
						if (!KeyObj.IsValid()) continue;

						double TimeSec = 0.0;
						KeyObj->TryGetNumberField(TEXT("time"), TimeSec);
						const FFrameNumber Frame = (TimeSec * TickResolution).FloorToFrame();

						FString ValueJson;
						const TSharedPtr<FJsonValue>& ValField = KeyObj->TryGetField(TEXT("value"));
						if (ValField.IsValid())
						{
							if (ValField->Type == EJson::Number)
							{
								ValueJson = FString::SanitizeFloat(ValField->AsNumber());
							}
							else if (ValField->Type == EJson::Boolean)
							{
								ValueJson = ValField->AsBool() ? TEXT("true") : TEXT("false");
							}
							else
							{
								ValField->TryGetString(ValueJson);
							}
						}

						FString KeyError;
						if (!ApplyAddKeyframe(Section, Frame, ValueJson, KeyError))
						{
							RecordEntryFailure(AnimName,
								FString::Printf(TEXT("add_keyframe t=%.3f: %s"), TimeSec, *KeyError));
							continue;
						}

						FString InterpStr;
						if (KeyObj->TryGetStringField(TEXT("interpolation"), InterpStr) && !InterpStr.IsEmpty())
						{
							EMovieSceneKeyInterpolation InterpMode = EMovieSceneKeyInterpolation::Auto;
							const FString Norm = InterpStr.ToLower();
							if (Norm == TEXT("linear")) InterpMode = EMovieSceneKeyInterpolation::Linear;
							else if (Norm == TEXT("constant")) InterpMode = EMovieSceneKeyInterpolation::Constant;
							else if (Norm == TEXT("cubic") || Norm == TEXT("auto")) InterpMode = EMovieSceneKeyInterpolation::Auto;
							else if (Norm == TEXT("break")) InterpMode = EMovieSceneKeyInterpolation::Break;
							else if (Norm == TEXT("user")) InterpMode = EMovieSceneKeyInterpolation::User;
							else
							{
								RecordEntryFailure(AnimName,
									FString::Printf(TEXT("unknown interpolation '%s'; supported: Constant, Linear, Cubic"), *InterpStr));
								continue;
							}
							FString InterpError;
							if (!ApplySetKeyInterpMode(Section, Frame, InterpMode, InterpError))
							{
								RecordEntryFailure(AnimName, InterpError);
								continue;
							}
						}
					}
				}
			}

			RecordEntrySuccess(AnimName, AnimName);
		}
	}

	return true;
}

bool FClaireonSpecApplicator_WidgetBP::CompileAsset(const FString& SessionId, FString& OutError)
{
	UWidgetBlueprint* WBP = WidgetBlueprint.Get();
	if (!WBP)
	{
		OutError = TEXT("Widget Blueprint is no longer valid");
		return false;
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	FKismetEditorUtilities::CompileBlueprint(WBP, EBlueprintCompileOptions::BatchCompile);

	if (WBP->Status == BS_Error)
	{
		OutError = TEXT("Widget Blueprint compilation failed with errors");
		return false;
	}

	return true;
}

bool FClaireonSpecApplicator_WidgetBP::SaveAsset(const FString& SessionId, FString& OutError)
{
	UWidgetBlueprint* WBP = WidgetBlueprint.Get();
	if (!WBP)
	{
		OutError = TEXT("Widget Blueprint is no longer valid");
		return false;
	}

	UPackage* Package = WBP->GetOutermost();
	FString PackageFileName = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());

	if (ClaireonSafeExec::DidLastExecutionCrash())
	{
		OutError = TEXT("Save blocked: editor state may be corrupted after a previous crash");
		return false;
	}

	FSavePackageArgs SaveArgs;
	if (!UPackage::SavePackage(Package, WBP, *PackageFileName, SaveArgs))
	{
		OutError = FString::Printf(TEXT("Failed to save Widget Blueprint to %s"), *PackageFileName);
		return false;
	}

	return true;
}

void FClaireonSpecApplicator_WidgetBP::CloseSession(const FString& SessionId)
{
	FClaireonSessionManager::Get().CloseSession(SessionId);
}
