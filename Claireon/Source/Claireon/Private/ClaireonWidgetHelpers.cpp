// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonWidgetHelpers.h"
#include "ClaireonLog.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"
#include "WidgetBlueprint.h"
#include "Animation/WidgetAnimation.h"
#include "MovieScene.h"
#include "MovieScenePossessable.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

// ============================================================================
// SerializeWidgetTree
// ============================================================================

TSharedPtr<FJsonObject> ClaireonWidgetHelpers::SerializeWidgetTree(UWidgetBlueprint* WidgetBP, const FWidgetSerializeOptions& Options)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	if (!WidgetBP)
	{
		Result->SetStringField(TEXT("error"), TEXT("WidgetBlueprint is null"));
		return Result;
	}

	// Top-level metadata
	Result->SetStringField(TEXT("asset_path"), WidgetBP->GetPathName());
	Result->SetStringField(TEXT("type"), TEXT("WidgetBlueprint"));

	if (WidgetBP->ParentClass)
	{
		Result->SetStringField(TEXT("parent_class"), WidgetBP->ParentClass->GetName());
	}
	else
	{
		Result->SetStringField(TEXT("parent_class"), TEXT("None"));
	}

	// Resolve starting widget for serialization
	UWidgetTree* Tree = WidgetBP->WidgetTree;
	if (!Tree)
	{
		Result->SetStringField(TEXT("error"), TEXT("WidgetTree is null"));
		return Result;
	}

	UWidget* RootWidget = Tree->RootWidget;

	// If a filter widget name is set, find and use that as the root
	if (Options.FilterWidgetName != NAME_None)
	{
		UWidget* FilterWidget = FindWidgetByName(Tree, Options.FilterWidgetName);
		if (FilterWidget)
		{
			RootWidget = FilterWidget;
		}
		else
		{
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Widget '%s' not found in tree"), *Options.FilterWidgetName.ToString()));
			return Result;
		}
	}

	// Serialize root widget subtree
	if (RootWidget)
	{
		TSharedPtr<FJsonObject> RootObj = SerializeWidget(RootWidget, Options, 0);
		Result->SetObjectField(TEXT("root"), RootObj);
	}
	else
	{
		Result->SetField(TEXT("root"), MakeShared<FJsonValueNull>());
	}

	// Optional: bindings
	if (Options.bIncludeBindings)
	{
		TArray<TSharedPtr<FJsonValue>> BindingsArray;
		for (const FDelegateEditorBinding& Binding : WidgetBP->Bindings)
		{
			TSharedPtr<FJsonObject> BindingObj = MakeShared<FJsonObject>();
			BindingObj->SetStringField(TEXT("ObjectName"), Binding.ObjectName);
			BindingObj->SetStringField(TEXT("PropertyName"), Binding.PropertyName.ToString());
			BindingObj->SetStringField(TEXT("FunctionName"), Binding.FunctionName.ToString());
			BindingObj->SetStringField(TEXT("SourceProperty"), Binding.SourceProperty.ToString());
			BindingsArray.Add(MakeShared<FJsonValueObject>(BindingObj));
		}
		Result->SetArrayField(TEXT("bindings"), BindingsArray);
	}

	// Optional: animations
	if (Options.bIncludeAnimations)
	{
		TArray<TSharedPtr<FJsonValue>> AnimationsArray;
		for (UWidgetAnimation* Anim : WidgetBP->Animations)
		{
			if (!Anim)
			{
				continue;
			}

			TSharedPtr<FJsonObject> AnimObj = MakeShared<FJsonObject>();
			AnimObj->SetStringField(TEXT("name"), Anim->GetDisplayName().ToString());
			AnimObj->SetNumberField(TEXT("start_time"), static_cast<double>(Anim->GetStartTime()));
			AnimObj->SetNumberField(TEXT("end_time"), static_cast<double>(Anim->GetEndTime()));

			AnimationsArray.Add(MakeShared<FJsonValueObject>(AnimObj));
		}
		Result->SetArrayField(TEXT("animations"), AnimationsArray);
	}

	return Result;
}

// ============================================================================
// SerializeWidget
// ============================================================================

TSharedPtr<FJsonObject> ClaireonWidgetHelpers::SerializeWidget(UWidget* Widget, const FWidgetSerializeOptions& Options, int32 CurrentDepth)
{
	TSharedPtr<FJsonObject> WidgetObj = MakeShared<FJsonObject>();

	if (!Widget)
	{
		return WidgetObj;
	}

	WidgetObj->SetStringField(TEXT("name"), Widget->GetName());
	WidgetObj->SetStringField(TEXT("class"), Widget->GetClass()->GetName());

	// Serialize slot if present
	if (Widget->Slot)
	{
		TSharedPtr<FJsonObject> SlotObj = MakeShared<FJsonObject>();
		SlotObj->SetStringField(TEXT("slot_type"), Widget->Slot->GetClass()->GetName());

		TSharedPtr<FJsonObject> SlotPropsObj = MakeShared<FJsonObject>();
		for (TFieldIterator<FProperty> PropIt(Widget->Slot->GetClass()); PropIt; ++PropIt)
		{
			FProperty* Prop = *PropIt;
			if (!Prop || !(Prop->PropertyFlags & CPF_Edit))
			{
				continue;
			}

			FString Value;
			const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Widget->Slot);
			Prop->ExportText_Direct(Value, ValuePtr, ValuePtr, Widget->Slot, PPF_None);
			SlotPropsObj->SetStringField(Prop->GetName(), Value);
		}
		SlotObj->SetObjectField(TEXT("properties"), SlotPropsObj);
		WidgetObj->SetObjectField(TEXT("slot"), SlotObj);
	}

	// Serialize widget properties if requested
	if (Options.bIncludeProperties)
	{
		TSharedPtr<FJsonObject> PropsObj = MakeShared<FJsonObject>();
		for (TFieldIterator<FProperty> PropIt(Widget->GetClass()); PropIt; ++PropIt)
		{
			FProperty* Prop = *PropIt;
			if (!Prop || !(Prop->PropertyFlags & CPF_Edit))
			{
				continue;
			}

			FString Value;
			const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Widget);
			Prop->ExportText_Direct(Value, ValuePtr, ValuePtr, Widget, PPF_None);
			PropsObj->SetStringField(Prop->GetName(), Value);
		}
		WidgetObj->SetObjectField(TEXT("properties"), PropsObj);
	}

	// Recurse into children if this is a panel widget and depth allows
	const bool bDepthUnlimited = (Options.MaxDepth < 0);
	const bool bCanGoDeeper = bDepthUnlimited || (CurrentDepth < Options.MaxDepth);

	UPanelWidget* PanelWidget = Cast<UPanelWidget>(Widget);
	if (PanelWidget && bCanGoDeeper)
	{
		TArray<TSharedPtr<FJsonValue>> ChildrenArray;
		const int32 ChildCount = PanelWidget->GetChildrenCount();
		for (int32 i = 0; i < ChildCount; ++i)
		{
			UWidget* Child = PanelWidget->GetChildAt(i);
			if (Child)
			{
				TSharedPtr<FJsonObject> ChildObj = SerializeWidget(Child, Options, CurrentDepth + 1);
				ChildrenArray.Add(MakeShared<FJsonValueObject>(ChildObj));
			}
		}
		WidgetObj->SetArrayField(TEXT("children"), ChildrenArray);
	}

	return WidgetObj;
}

// ============================================================================
// FindWidgetByName
// ============================================================================

UWidget* ClaireonWidgetHelpers::FindWidgetByName(UWidgetTree* Tree, FName WidgetName)
{
	if (!Tree)
	{
		return nullptr;
	}

	// Use UWidgetTree's built-in FindWidget which searches by name
	UWidget* Found = Tree->FindWidget(WidgetName);
	return Found;
}

// ============================================================================
// ResolveWidgetClass
// ============================================================================

UClass* ClaireonWidgetHelpers::ResolveWidgetClass(const FString& WidgetClassStr, FString& OutError)
{
	if (WidgetClassStr.IsEmpty())
	{
		OutError = TEXT("Widget class string is empty");
		return nullptr;
	}

	UClass* Result = nullptr;

	// Stage 1: Blueprint path (contains '/')
	if (WidgetClassStr.Contains(TEXT("/")))
	{
		// Try direct LoadClass
		Result = LoadClass<UWidget>(nullptr, *WidgetClassStr);
		if (!Result)
		{
			// Try with _C suffix (generated blueprint class)
			FString WithSuffix = WidgetClassStr + TEXT("_C");
			Result = LoadClass<UWidget>(nullptr, *WithSuffix);
		}
	}

	// Stage 2: Native class lookup by name
	if (!Result)
	{
		// Try direct name
		Result = FindFirstObject<UClass>(*WidgetClassStr, EFindFirstObjectOptions::NativeFirst);

		// Try with 'U' prefix (e.g., "TextBlock" -> "UTextBlock")
		if (!Result)
		{
			FString WithPrefix = TEXT("U") + WidgetClassStr;
			Result = FindFirstObject<UClass>(*WithPrefix, EFindFirstObjectOptions::NativeFirst);
		}

		// Try common UMG module path
		if (!Result)
		{
			FString ScriptPath = FString::Printf(TEXT("/Script/UMG.%s"), *WidgetClassStr);
			Result = FindFirstObject<UClass>(*ScriptPath, EFindFirstObjectOptions::NativeFirst);
		}
	}

	// Stage 3: Validate it is a UWidget subclass
	if (Result)
	{
		if (!Result->IsChildOf(UWidget::StaticClass()))
		{
			OutError = FString::Printf(TEXT("Class '%s' is not a UWidget subclass (actual: %s)"),
				*WidgetClassStr, *Result->GetPathName());
			return nullptr;
		}
		return Result;
	}

	// Stage 4: Build suggestion list from partial name matches
	TArray<FString> Suggestions;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Cls = *It;
		if (!Cls || !Cls->IsChildOf(UWidget::StaticClass()))
		{
			continue;
		}

		const FString ClassName = Cls->GetName();
		if (ClassName.Contains(WidgetClassStr, ESearchCase::IgnoreCase))
		{
			Suggestions.Add(ClassName);
			if (Suggestions.Num() >= 10)
			{
				break;
			}
		}
	}

	if (Suggestions.Num() > 0)
	{
		OutError = FString::Printf(TEXT("Widget class '%s' not found. Did you mean: %s"),
			*WidgetClassStr, *FString::Join(Suggestions, TEXT(", ")));
	}
	else
	{
		OutError = FString::Printf(TEXT("Widget class '%s' not found. No similar UWidget subclasses found."), *WidgetClassStr);
	}

	return nullptr;
}

// ============================================================================
// CreateWidget
// ============================================================================

UWidget* ClaireonWidgetHelpers::CreateWidget(UWidgetTree* Tree, TSubclassOf<UWidget> WidgetClass, FName WidgetName)
{
	if (!Tree || !WidgetClass)
	{
		return nullptr;
	}

	return Tree->ConstructWidget<UWidget>(WidgetClass, WidgetName);
}

// ============================================================================
// AddChildToPanel
// ============================================================================

UPanelSlot* ClaireonWidgetHelpers::AddChildToPanel(UPanelWidget* Parent, UWidget* Child, const TSharedPtr<FJsonObject>& SlotProperties)
{
	if (!Parent || !Child)
	{
		return nullptr;
	}

	UPanelSlot* Slot = Parent->AddChild(Child);
	if (Slot && SlotProperties.IsValid())
	{
		for (auto& Pair : SlotProperties->Values)
		{
			FString Error;
			WriteSlotProperty(Slot, Pair.Key, Pair.Value->AsString(), Error);
		}
	}

	return Slot;
}

// ============================================================================
// ReadWidgetProperty
// ============================================================================

FString ClaireonWidgetHelpers::ReadWidgetProperty(UWidget* Widget, const FString& PropertyName, bool& bOutSuccess)
{
	if (!Widget)
	{
		bOutSuccess = false;
		return FString();
	}

	FProperty* Prop = Widget->GetClass()->FindPropertyByName(*PropertyName);
	if (!Prop)
	{
		bOutSuccess = false;
		return FString();
	}

	FString Value;
	const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Widget);
	Prop->ExportText_Direct(Value, ValuePtr, ValuePtr, Widget, PPF_None);
	bOutSuccess = true;
	return Value;
}

// ============================================================================
// WriteWidgetProperty
// ============================================================================

bool ClaireonWidgetHelpers::WriteWidgetProperty(UWidget* Widget, const FString& PropertyName, const FString& Value, FString& OutError)
{
	if (!Widget)
	{
		OutError = TEXT("Widget is null");
		return false;
	}

	FProperty* Prop = Widget->GetClass()->FindPropertyByName(*PropertyName);
	if (!Prop)
	{
		OutError = FString::Printf(TEXT("Property '%s' not found on %s"), *PropertyName, *Widget->GetClass()->GetName());
		return false;
	}

	void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Widget);
	const TCHAR* ImportResult = Prop->ImportText_Direct(*Value, ValuePtr, Widget, PPF_None);
	if (!ImportResult)
	{
		OutError = FString::Printf(TEXT("Failed to parse value '%s' for property '%s'"), *Value, *PropertyName);
		return false;
	}

	return true;
}

// ============================================================================
// ReadSlotProperty
// ============================================================================

FString ClaireonWidgetHelpers::ReadSlotProperty(UPanelSlot* Slot, const FString& PropertyName, bool& bOutSuccess)
{
	if (!Slot)
	{
		bOutSuccess = false;
		return FString();
	}

	FProperty* Prop = Slot->GetClass()->FindPropertyByName(*PropertyName);
	if (!Prop)
	{
		bOutSuccess = false;
		return FString();
	}

	FString Value;
	const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Slot);
	Prop->ExportText_Direct(Value, ValuePtr, ValuePtr, Slot, PPF_None);
	bOutSuccess = true;
	return Value;
}

// ============================================================================
// WriteSlotProperty
// ============================================================================

bool ClaireonWidgetHelpers::WriteSlotProperty(UPanelSlot* Slot, const FString& PropertyName, const FString& Value, FString& OutError)
{
	if (!Slot)
	{
		OutError = TEXT("Slot is null");
		return false;
	}

	FProperty* Prop = Slot->GetClass()->FindPropertyByName(*PropertyName);
	if (!Prop)
	{
		OutError = FString::Printf(TEXT("Property '%s' not found on %s"), *PropertyName, *Slot->GetClass()->GetName());
		return false;
	}

	void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Slot);
	const TCHAR* ImportResult = Prop->ImportText_Direct(*Value, ValuePtr, Slot, PPF_None);
	if (!ImportResult)
	{
		OutError = FString::Printf(TEXT("Failed to parse value '%s' for property '%s'"), *Value, *PropertyName);
		return false;
	}

	return true;
}

// ============================================================================
// ValidateWidgetBPAssetPath
// ============================================================================

bool ClaireonWidgetHelpers::ValidateWidgetBPAssetPath(const FString& AssetPath, FString& OutError)
{
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("Asset path is empty");
		return false;
	}

	// Must start with '/'
	if (!AssetPath.StartsWith(TEXT("/")))
	{
		OutError = FString::Printf(TEXT("Asset path must start with '/'. Got: %s"), *AssetPath);
		return false;
	}

	// Check for invalid characters (backslashes, consecutive dots, spaces)
	if (AssetPath.Contains(TEXT("\\")) || AssetPath.Contains(TEXT("..")) || AssetPath.Contains(TEXT(" ")))
	{
		OutError = FString::Printf(TEXT("Asset path contains invalid characters (backslash, '..', or space): %s"), *AssetPath);
		return false;
	}

	// Must contain at least one segment beyond the initial slash
	if (AssetPath.Len() < 2)
	{
		OutError = FString::Printf(TEXT("Asset path is too short to be a valid UE asset path: %s"), *AssetPath);
		return false;
	}

	return true;
}

// ============================================================================
// SerializeAnimationDetails
// ============================================================================

TSharedPtr<FJsonObject> ClaireonWidgetHelpers::SerializeAnimationDetails(UWidgetAnimation* Anim)
{
	TSharedPtr<FJsonObject> AnimObj = MakeShared<FJsonObject>();
	if (!Anim)
	{
		return AnimObj;
	}

	AnimObj->SetStringField(TEXT("name"), Anim->GetDisplayName().ToString());
	AnimObj->SetNumberField(TEXT("start_time"), Anim->GetStartTime());
	AnimObj->SetNumberField(TEXT("end_time"), Anim->GetEndTime());

	UMovieScene* MovieScene = Anim->GetMovieScene();
	if (!MovieScene)
	{
		return AnimObj;
	}

	// Duration
	FFrameRate TickResolution = MovieScene->GetTickResolution();
	TRange<FFrameNumber> PlaybackRange = MovieScene->GetPlaybackRange();
	if (PlaybackRange.HasLowerBound() && PlaybackRange.HasUpperBound())
	{
		double Duration = static_cast<double>(PlaybackRange.Size<FFrameNumber>().Value) / TickResolution.AsDecimal();
		AnimObj->SetNumberField(TEXT("duration"), Duration);
	}
	AnimObj->SetNumberField(TEXT("display_rate"), MovieScene->GetDisplayRate().AsDecimal());

	// Bindings
	TArray<TSharedPtr<FJsonValue>> BindingsArray;
	for (int32 i = 0; i < MovieScene->GetPossessableCount(); ++i)
	{
		const FMovieScenePossessable& Possessable = MovieScene->GetPossessable(i);
		TSharedPtr<FJsonObject> BindingObj = MakeShared<FJsonObject>();
		BindingObj->SetStringField(TEXT("name"), Possessable.GetName());
		BindingObj->SetStringField(TEXT("guid"), Possessable.GetGuid().ToString());

		// Tracks for this binding
		TArray<TSharedPtr<FJsonValue>> TracksArray;
		const FMovieSceneBinding* Binding = MovieScene->FindBinding(Possessable.GetGuid());
		if (Binding)
		{
			for (UMovieSceneTrack* Track : Binding->GetTracks())
			{
				if (!Track)
				{
					continue;
				}

				TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
				TrackObj->SetStringField(TEXT("type"), Track->GetClass()->GetName());
				TrackObj->SetArrayField(TEXT("keyframes"), TArray<TSharedPtr<FJsonValue>>());

				TracksArray.Add(MakeShared<FJsonValueObject>(TrackObj));
			}
		}
		BindingObj->SetArrayField(TEXT("tracks"), TracksArray);

		BindingsArray.Add(MakeShared<FJsonValueObject>(BindingObj));
	}
	AnimObj->SetArrayField(TEXT("bindings"), BindingsArray);

	return AnimObj;
}
