// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UWidgetBlueprint;
class UWidgetTree;
class UWidget;
class UWidgetAnimation;
class UPanelWidget;
class UPanelSlot;

/**
 * Options for widget tree serialization.
 */
struct FWidgetSerializeOptions
{
	int32 MaxDepth = -1; // -1 = unlimited
	bool bIncludeProperties = false;
	bool bIncludeBindings = false;
	bool bIncludeAnimations = false;
	FName FilterWidgetName = NAME_None; // If set, only serialize subtree rooted at this widget
};

/**
 * Helper functions for widget blueprint MCP tools.
 */
namespace ClaireonWidgetHelpers
{
	/** Serialize the full widget tree to a JSON object. */
	TSharedPtr<FJsonObject> SerializeWidgetTree(UWidgetBlueprint* WidgetBP, const FWidgetSerializeOptions& Options);

	/** Serialize a single widget and its children to JSON. */
	TSharedPtr<FJsonObject> SerializeWidget(UWidget* Widget, const FWidgetSerializeOptions& Options, int32 CurrentDepth = 0);

	/** Find a widget by name in the widget tree. */
	UWidget* FindWidgetByName(UWidgetTree* Tree, FName WidgetName);

	/** Resolve a widget class from a string (supports shorthand, full names, and blueprint paths). */
	UClass* ResolveWidgetClass(const FString& WidgetClassStr, FString& OutError);

	/** Create a widget in a widget tree with RF_Transactional. */
	UWidget* CreateWidget(UWidgetTree* Tree, TSubclassOf<UWidget> WidgetClass, FName WidgetName = NAME_None);

	/** Add a child widget to a panel widget and configure slot properties. */
	UPanelSlot* AddChildToPanel(UPanelWidget* Parent, UWidget* Child, const TSharedPtr<FJsonObject>& SlotProperties = nullptr);

	/** Read a UPROPERTY value from a widget as a string. */
	FString ReadWidgetProperty(UWidget* Widget, const FString& PropertyName, bool& bOutSuccess);

	/** Write a UPROPERTY value on a widget from a string. */
	bool WriteWidgetProperty(UWidget* Widget, const FString& PropertyName, const FString& Value, FString& OutError);

	/** Read a slot property value as a string. */
	FString ReadSlotProperty(UPanelSlot* Slot, const FString& PropertyName, bool& bOutSuccess);

	/** Write a slot property value from a string. */
	bool WriteSlotProperty(UPanelSlot* Slot, const FString& PropertyName, const FString& Value, FString& OutError);

	/** Validate an asset path for widget blueprint operations. */
	bool ValidateWidgetBPAssetPath(const FString& AssetPath, FString& OutError);

	/** Serialize detailed animation info (bindings, tracks, keyframes) to JSON. */
	TSharedPtr<FJsonObject> SerializeAnimationDetails(UWidgetAnimation* Anim);
} // namespace ClaireonWidgetHelpers
