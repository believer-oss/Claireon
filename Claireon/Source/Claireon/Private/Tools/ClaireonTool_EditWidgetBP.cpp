// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_EditWidgetBP.h"
#include "ClaireonLog.h"
#include "ClaireonWidgetHelpers.h"
#include "ClaireonSessionManager.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/SavePackage.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"
#include "UObject/Package.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Animation/WidgetAnimation.h"
#include "WidgetBlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "MovieScene.h"
#include "MovieScenePossessable.h"
#include "MovieSceneBinding.h"
#include "Animation/WidgetAnimationBinding.h"
#include "Components/PanelSlot.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "Tracks/MovieSceneBoolTrack.h"
#include "Tracks/MovieSceneColorTrack.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Sections/MovieSceneBoolSection.h"
#include "Sections/MovieSceneColorSection.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Channels/MovieSceneBoolChannel.h"
#include "HAL/FileManager.h"
#include "UObject/UObjectIterator.h"
#include "MVVMBlueprintView.h"
#include "MVVMBlueprintViewBinding.h"
#include "MVVMBlueprintViewModelContext.h"
#include "MVVMWidgetBlueprintExtension_View.h"
#include "MVVMPropertyPath.h"
#include "MVVMBlueprintViewConversionFunction.h"
#include "MVVMViewModelBase.h"
#include "Types/MVVMBindingMode.h"
#include "Types/MVVMFieldVariant.h"

#define LOCTEXT_NAMESPACE "ClaireonTool_EditWidgetBP"

using FToolResult = IClaireonTool::FToolResult;

TMap<FString, FWidgetBPEditToolData> ClaireonTool_EditWidgetBP::ToolData;
bool ClaireonTool_EditWidgetBP::bDelegateRegistered = false;

FString ClaireonTool_EditWidgetBP::GetName() const
{
	return TEXT("claireon.widgetbp_edit");
}

FString ClaireonTool_EditWidgetBP::GetDescription() const
{
	return TEXT("Session-based Widget Blueprint editor. Add, remove, and configure widgets, manage properties and bindings, import/export widget hierarchies. Start with 'open' or 'create'.");
}

FString ClaireonTool_EditWidgetBP::GetFullDescription() const
{
	return TEXT("Session-based Widget Blueprint editor. Open a widget blueprint, then use operations to add/remove/move widgets, edit properties, manage bindings, and more. Start with 'open' or 'create'.\n\n"
				"Required params by operation:\n\n"
				"Lifecycle (no session_id needed):\n"
				"- open: params: asset_path (required)\n"
				"- create: params: asset_path (required), parent_class (optional, defaults to \"UserWidget\")\n\n"
				"Lifecycle (session_id required):\n"
				"- get_state, compile, save, close: (no params beyond session_id)\n"
				"- focus: params: widget_name (required)\n\n"
				"Widget CRUD (session_id required):\n"
				"- add_widget: params: widget_class (required), parent_name (optional), widget_name (optional), index (optional, insertion position)\n"
				"- remove_widget: params: widget_name (required)\n"
				"- move_widget: params: widget_name (required), new_parent_name (required), index (optional, insertion position)\n"
				"- replace_widget: params: widget_name (required), new_widget_class (required), preserve_children (optional, default true)\n"
				"- rename_widget: params: widget_name (required), new_name (required)\n\n"
				"Property operations (session_id required):\n"
				"- set_widget_property: params: widget_name (required), property_name (required), value (required)\n"
				"- set_slot_property: params: widget_name (required), property_name (required), value (required)\n"
				"- get_widget_details: params: widget_name (required)\n\n"
				"Advanced (session_id required):\n"
				"- add_binding: params: widget_name (required), property_name (required), function_name (required)\n"
				"- remove_binding: params: widget_name (required), property_name (required)\n"
				"- list_animations: params: include_details (optional, default false)\n"
				"- import_widgets: params: widget_text (required)\n"
				"- export_widgets: params: widget_names (required, array of strings)\n"
				"- list_widget_classes: (no params)\n\n"
				"Animation operations (session_id required):\n"
				"- create_animation: params: animation_name (required), duration (optional, default 5.0), display_rate (optional, default 20)\n"
				"- delete_animation: params: animation_name (required)\n"
				"- rename_animation: params: animation_name (required), new_name (required)\n"
				"- duplicate_animation: params: animation_name (required), new_name (optional)\n"
				"- get_animation_details: params: animation_name (required)\n"
				"- add_animation_binding: params: animation_name (required), widget_name (required), include_slot (optional, default false)\n"
				"- add_animation_track: params: animation_name (required), widget_name (required), property_path (required), target (optional, default \"widget\")\n"
				"- add_animation_keyframe: params: animation_name (required), widget_name (required), property_path (required), time (required), value (required), interpolation (optional, default \"cubic\"), target (optional, default \"widget\")\n"
				"- remove_animation_keyframe: params: animation_name (required), widget_name (required), property_path (required), time (required), target (optional, default \"widget\")\n"
				"- set_animation_property: params: animation_name (required), duration (optional), start_time (optional), display_rate (optional)\n\n"
				"MVVM ViewModel operations (session_id required):\n"
				"- list_mvvm_viewmodels: (no params)\n"
				"- add_mvvm_viewmodel: params: viewmodel_name (required), viewmodel_class (required), creation_type (optional, default \"Manual\"), optional (optional, default false)\n"
				"- remove_mvvm_viewmodel: params: viewmodel_name (required)\n\n"
				"MVVM Binding operations (session_id required):\n"
				"- list_mvvm_bindings: (no params)\n"
				"- add_mvvm_binding: params: viewmodel_name (required), viewmodel_property (required), widget_name (required), widget_property (required), mode (optional, default \"OneWayToDestination\"), enabled (optional, default true), conversion_function (optional)\n"
				"- edit_mvvm_binding: params: binding_id (required), mode (optional), enabled (optional), viewmodel_property (optional), widget_property (optional), conversion_function (optional, empty string to clear)\n"
				"- remove_mvvm_binding: params: binding_id (required)");
}

TSharedPtr<FJsonObject> ClaireonTool_EditWidgetBP::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// operation property
	TSharedPtr<FJsonObject> OperationProp = MakeShared<FJsonObject>();
	OperationProp->SetStringField(TEXT("type"), TEXT("string"));
	TArray<TSharedPtr<FJsonValue>> OperationEnum;
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("open")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("create")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("get_state")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("focus")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("compile")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("save")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("close")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("add_widget")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("remove_widget")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("move_widget")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("replace_widget")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("rename_widget")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("set_widget_property")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("set_slot_property")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("get_widget_details")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("add_binding")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("remove_binding")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("list_animations")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("import_widgets")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("export_widgets")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("list_widget_classes")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("create_animation")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("delete_animation")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("rename_animation")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("duplicate_animation")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("get_animation_details")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("add_animation_binding")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("add_animation_track")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("add_animation_keyframe")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("remove_animation_keyframe")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("set_animation_property")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("list_mvvm_viewmodels")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("add_mvvm_viewmodel")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("remove_mvvm_viewmodel")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("list_mvvm_bindings")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("add_mvvm_binding")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("edit_mvvm_binding")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("remove_mvvm_binding")));
	OperationProp->SetArrayField(TEXT("enum"), OperationEnum);
	OperationProp->SetStringField(TEXT("description"), TEXT("The editing operation to perform."));
	Properties->SetObjectField(TEXT("operation"), OperationProp);

	// session_id property
	TSharedPtr<FJsonObject> SessionIdProp = MakeShared<FJsonObject>();
	SessionIdProp->SetStringField(TEXT("type"), TEXT("string"));
	SessionIdProp->SetStringField(TEXT("description"), TEXT("Session identifier from a previous 'open' or 'create' operation. Required for all operations except 'open' and 'create'."));
	Properties->SetObjectField(TEXT("session_id"), SessionIdProp);

	// params property
	TSharedPtr<FJsonObject> ParamsProp = MakeShared<FJsonObject>();
	ParamsProp->SetStringField(TEXT("type"), TEXT("object"));
	ParamsProp->SetStringField(TEXT("description"), TEXT("Operation-specific parameters. See operation descriptions for details."));
	Properties->SetObjectField(TEXT("params"), ParamsProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("operation")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

FToolResult ClaireonTool_EditWidgetBP::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString Operation;
	if (!Arguments->TryGetStringField(TEXT("operation"), Operation) || Operation.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: operation"));
	}

	// Session-less operations
	if (Operation == TEXT("open"))
	{
		TSharedPtr<FJsonObject> Params = Arguments->HasField(TEXT("params"))
			? Arguments->GetObjectField(TEXT("params"))
			: MakeShared<FJsonObject>();
		return Operation_Open(Params);
	}
	if (Operation == TEXT("create"))
	{
		TSharedPtr<FJsonObject> Params = Arguments->HasField(TEXT("params"))
			? Arguments->GetObjectField(TEXT("params"))
			: MakeShared<FJsonObject>();
		return Operation_Create(Params);
	}

	// Session-required operations
	FString SessionId;
	if (!Arguments->TryGetStringField(TEXT("session_id"), SessionId) || SessionId.IsEmpty())
	{
		return MakeErrorResult(FString::Printf(TEXT("Operation '%s' requires session_id"), *Operation));
	}

	FMCPSession* Session = FClaireonSessionManager::Get().FindSession(SessionId);
	if (!Session)
	{
		return MakeErrorResult(FString::Printf(TEXT("Session not found or expired: %s"), *SessionId));
	}

	FWidgetBPEditToolData* Data = ToolData.Find(SessionId);
	if (!Data)
	{
		return MakeErrorResult(TEXT("Session tool data not found"));
	}

	FClaireonSessionManager::Get().TouchSession(SessionId);

	TSharedPtr<FJsonObject> Params = Arguments->HasField(TEXT("params"))
		? Arguments->GetObjectField(TEXT("params"))
		: MakeShared<FJsonObject>();

	if (Operation == TEXT("get_state"))
	{
		return Operation_GetState(SessionId, Data, Params);
	}
	if (Operation == TEXT("focus"))
	{
		return Operation_Focus(SessionId, Data, Params);
	}
	if (Operation == TEXT("compile"))
	{
		return Operation_Compile(SessionId, Data, Params);
	}
	if (Operation == TEXT("save"))
	{
		return Operation_Save(SessionId, Data, Params);
	}
	if (Operation == TEXT("close"))
	{
		return Operation_Close(SessionId, Data, Params);
	}
	if (Operation == TEXT("add_widget"))
	{
		return Operation_AddWidget(SessionId, Data, Params);
	}
	if (Operation == TEXT("remove_widget"))
	{
		return Operation_RemoveWidget(SessionId, Data, Params);
	}
	if (Operation == TEXT("move_widget"))
	{
		return Operation_MoveWidget(SessionId, Data, Params);
	}
	if (Operation == TEXT("replace_widget"))
	{
		return Operation_ReplaceWidget(SessionId, Data, Params);
	}
	if (Operation == TEXT("rename_widget"))
	{
		return Operation_RenameWidget(SessionId, Data, Params);
	}
	if (Operation == TEXT("set_widget_property"))
	{
		return Operation_SetWidgetProperty(SessionId, Data, Params);
	}
	if (Operation == TEXT("set_slot_property"))
	{
		return Operation_SetSlotProperty(SessionId, Data, Params);
	}
	if (Operation == TEXT("get_widget_details"))
	{
		return Operation_GetWidgetDetails(SessionId, Data, Params);
	}
	if (Operation == TEXT("add_binding"))
	{
		return Operation_AddBinding(SessionId, Data, Params);
	}
	if (Operation == TEXT("remove_binding"))
	{
		return Operation_RemoveBinding(SessionId, Data, Params);
	}
	if (Operation == TEXT("list_animations"))
	{
		return Operation_ListAnimations(SessionId, Data, Params);
	}
	if (Operation == TEXT("import_widgets"))
	{
		return Operation_ImportWidgets(SessionId, Data, Params);
	}
	if (Operation == TEXT("export_widgets"))
	{
		return Operation_ExportWidgets(SessionId, Data, Params);
	}
	if (Operation == TEXT("list_widget_classes"))
	{
		return Operation_ListWidgetClasses(SessionId, Data, Params);
	}
	// Animation operations
	if (Operation == TEXT("create_animation"))
	{
		return Operation_CreateAnimation(SessionId, Data, Params);
	}
	if (Operation == TEXT("delete_animation"))
	{
		return Operation_DeleteAnimation(SessionId, Data, Params);
	}
	if (Operation == TEXT("rename_animation"))
	{
		return Operation_RenameAnimation(SessionId, Data, Params);
	}
	if (Operation == TEXT("duplicate_animation"))
	{
		return Operation_DuplicateAnimation(SessionId, Data, Params);
	}
	if (Operation == TEXT("get_animation_details"))
	{
		return Operation_GetAnimationDetails(SessionId, Data, Params);
	}
	if (Operation == TEXT("add_animation_binding"))
	{
		return Operation_AddAnimationBinding(SessionId, Data, Params);
	}
	if (Operation == TEXT("add_animation_track"))
	{
		return Operation_AddAnimationTrack(SessionId, Data, Params);
	}
	if (Operation == TEXT("add_animation_keyframe"))
	{
		return Operation_AddAnimationKeyframe(SessionId, Data, Params);
	}
	if (Operation == TEXT("remove_animation_keyframe"))
	{
		return Operation_RemoveAnimationKeyframe(SessionId, Data, Params);
	}
	if (Operation == TEXT("set_animation_property"))
	{
		return Operation_SetAnimationProperty(SessionId, Data, Params);
	}
	if (Operation == TEXT("list_mvvm_viewmodels"))
	{
		return Operation_ListMVVMViewModels(SessionId, Data, Params);
	}
	if (Operation == TEXT("add_mvvm_viewmodel"))
	{
		return Operation_AddMVVMViewModel(SessionId, Data, Params);
	}
	if (Operation == TEXT("remove_mvvm_viewmodel"))
	{
		return Operation_RemoveMVVMViewModel(SessionId, Data, Params);
	}
	if (Operation == TEXT("list_mvvm_bindings"))
	{
		return Operation_ListMVVMBindings(SessionId, Data, Params);
	}
	if (Operation == TEXT("add_mvvm_binding"))
	{
		return Operation_AddMVVMBinding(SessionId, Data, Params);
	}
	if (Operation == TEXT("edit_mvvm_binding"))
	{
		return Operation_EditMVVMBinding(SessionId, Data, Params);
	}
	if (Operation == TEXT("remove_mvvm_binding"))
	{
		return Operation_RemoveMVVMBinding(SessionId, Data, Params);
	}

	return MakeErrorResult(FString::Printf(TEXT("Unknown operation: %s"), *Operation));
}

// ============================================================================
// Session Management
// ============================================================================

void ClaireonTool_EditWidgetBP::HandleSessionClosed(const FMCPSessionClosedInfo& Info)
{
	if (Info.ToolName == TEXT("claireon.widgetbp_edit"))
	{
		ToolData.Remove(Info.SessionId);
	}
}

// ============================================================================
// Session Lifecycle Operations
// ============================================================================

FToolResult ClaireonTool_EditWidgetBP::Operation_Open(const TSharedPtr<FJsonObject>& Params)
{
	// Extract asset_path (required)
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return MakeErrorResult(TEXT("Missing required field: asset_path. open requires: asset_path (e.g. \"/Game/UI/WBP_MyWidget.WBP_MyWidget\")"));
	}

	// Canonicalize path early to prevent malformed paths from reaching LoadObject
	// (e.g., double slashes cause a fatal error in CreatePackage)
	AssetPath = FClaireonSessionManager::CanonicalizePath(AssetPath);
	if (AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Invalid asset path. Path must start with /Game/."));
	}

	// Load the widget blueprint
	UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *AssetPath);
	if (!WBP)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load Widget Blueprint: %s"), *AssetPath));
	}

	// Register delegate if not done yet
	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonTool_EditWidgetBP::HandleSessionClosed);
		bDelegateRegistered = true;
	}

	// Use the canonical path from the loaded object as the lock key
	FString CanonicalPath = WBP->GetPathName();

	// Open session (acts as exclusive lock)
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(CanonicalPath, TEXT("claireon.widgetbp_edit"));
	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		const FTimespan Elapsed = FDateTime::UtcNow() - Blocker.LastAccessTime;
		return MakeErrorResult(FString::Printf(
			TEXT("Asset is locked by %s session %s (last activity %dm %ds ago). Close that session first."),
			*Blocker.ToolName, *Blocker.SessionId,
			static_cast<int32>(Elapsed.GetTotalMinutes()),
			static_cast<int32>(Elapsed.GetTotalSeconds()) % 60));
	}
	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid asset path: %s"), *CanonicalPath));
	}
	const FString SessionId = OpenResult.SessionId;

	// Create and populate tool data
	FWidgetBPEditToolData NewData;
	NewData.WidgetBlueprint = WBP;
	NewData.bModified = false;
	NewData.LastCommandTime = FDateTime::Now();

	// Set initial focus to root widget if one exists
	if (WBP->WidgetTree && WBP->WidgetTree->RootWidget)
	{
		NewData.FocusedWidget = WBP->WidgetTree->RootWidget->GetFName();
	}

	ToolData.Add(SessionId, NewData);

	UE_LOG(LogClaireon, Log, TEXT("[EditWidgetBP] Opened session %s for %s"), *SessionId, *AssetPath);

	FWidgetBPEditToolData* LiveData = ToolData.Find(SessionId);
	if (!LiveData)
	{
		return MakeErrorResult(TEXT("Failed to create session after open"));
	}

	return BuildStateResponse(SessionId, LiveData);
}

FToolResult ClaireonTool_EditWidgetBP::Operation_Create(const TSharedPtr<FJsonObject>& Params)
{
	// Extract asset_path (required)
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return MakeErrorResult(TEXT("Missing required field: asset_path. create requires: asset_path, parent_class (optional, defaults to \"UserWidget\")"));
	}

	// Validate asset path
	FString ValidationError;
	if (!ClaireonWidgetHelpers::ValidateWidgetBPAssetPath(AssetPath, ValidationError))
	{
		return MakeErrorResult(ValidationError);
	}

	// Extract parent_class (default: UserWidget)
	FString ParentClassName;
	if (!Params->TryGetStringField(TEXT("parent_class"), ParentClassName))
	{
		ParentClassName = TEXT("UserWidget");
	}

	// Extract root_widget_class (default: CanvasPanel)
	FString RootWidgetClassStr;
	if (!Params->TryGetStringField(TEXT("root_widget_class"), RootWidgetClassStr))
	{
		RootWidgetClassStr = TEXT("CanvasPanel");
	}

	// Resolve parent class â must be a subclass of UUserWidget
	UClass* ParentClass = FindFirstObject<UClass>(*ParentClassName);
	if (!ParentClass)
	{
		return MakeErrorResult(FString::Printf(TEXT("Parent class '%s' not found"), *ParentClassName));
	}
	if (!ParentClass->IsChildOf(UUserWidget::StaticClass()))
	{
		return MakeErrorResult(FString::Printf(TEXT("Parent class '%s' is not a subclass of UserWidget"), *ParentClassName));
	}

	// Extract package name and asset name from asset path
	FString PackageName = AssetPath;
	FString AssetName;
	if (AssetPath.Contains(TEXT(".")))
	{
		AssetPath.Split(TEXT("."), &PackageName, &AssetName);
	}
	else
	{
		int32 LastSlash;
		if (PackageName.FindLastChar('/', LastSlash))
		{
			AssetName = PackageName.Mid(LastSlash + 1);
		}
		else
		{
			AssetName = TEXT("NewWidgetBlueprint");
		}
	}

	// Delete pre-existing file to avoid partially-loaded errors
	FString PackageFileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	if (FPaths::FileExists(PackageFileName))
	{
		UE_LOG(LogClaireon, Warning, TEXT("[EditWidgetBP] Create: Deleting existing file %s"), *PackageFileName);
		IFileManager::Get().Delete(*PackageFileName, false, true);
	}

	// Create package
	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to create package: %s"), *PackageName));
	}

	// Create widget blueprint
	UWidgetBlueprint* NewWBP = CastChecked<UWidgetBlueprint>(
		FKismetEditorUtilities::CreateBlueprint(
			ParentClass,
			Package,
			FName(*AssetName),
			BPTYPE_Normal,
			UWidgetBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(),
			NAME_None));

	if (!NewWBP)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to create Widget Blueprint at %s"), *PackageName));
	}

	// Create root widget if requested
	if (!RootWidgetClassStr.IsEmpty() && RootWidgetClassStr != TEXT("none"))
	{
		FString RootClassError;
		UClass* RootClass = ClaireonWidgetHelpers::ResolveWidgetClass(RootWidgetClassStr, RootClassError);
		if (!RootClass)
		{
			// Non-fatal: log warning, continue without root widget
			UE_LOG(LogClaireon, Warning, TEXT("[EditWidgetBP] Could not resolve root widget class '%s': %s"), *RootWidgetClassStr, *RootClassError);
		}
		else if (NewWBP->WidgetTree)
		{
			UWidget* Root = NewWBP->WidgetTree->ConstructWidget<UWidget>(RootClass, FName(*RootClass->GetName()));
			if (Root)
			{
				NewWBP->WidgetTree->RootWidget = Root;
			}
		}
	}

	// Mark package as externally referenceable and dirty
	Package->SetIsExternallyReferenceable(true);
	Package->MarkPackageDirty();

	// Notify asset registry
	FAssetRegistryModule::AssetCreated(NewWBP);

	// Register delegate if not done yet
	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonTool_EditWidgetBP::HandleSessionClosed);
		bDelegateRegistered = true;
	}

	// Open session (acts as exclusive lock)
	FString FinalAssetPath = NewWBP->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(FinalAssetPath, TEXT("claireon.widgetbp_edit"));
	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		const FTimespan Elapsed = FDateTime::UtcNow() - Blocker.LastAccessTime;
		return MakeErrorResult(FString::Printf(
			TEXT("Asset is locked by %s session %s (last activity %dm %ds ago). Close that session first."),
			*Blocker.ToolName, *Blocker.SessionId,
			static_cast<int32>(Elapsed.GetTotalMinutes()),
			static_cast<int32>(Elapsed.GetTotalSeconds()) % 60));
	}
	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid asset path: %s"), *FinalAssetPath));
	}
	const FString SessionId = OpenResult.SessionId;

	// Create and register tool data
	FWidgetBPEditToolData NewData;
	NewData.WidgetBlueprint = NewWBP;
	NewData.bModified = true;
	NewData.LastCommandTime = FDateTime::Now();

	if (NewWBP->WidgetTree && NewWBP->WidgetTree->RootWidget)
	{
		NewData.FocusedWidget = NewWBP->WidgetTree->RootWidget->GetFName();
	}

	ToolData.Add(SessionId, NewData);

	UE_LOG(LogClaireon, Log, TEXT("[EditWidgetBP] Created session %s for new WBP %s"), *SessionId, *FinalAssetPath);

	FWidgetBPEditToolData* LiveData = ToolData.Find(SessionId);
	if (!LiveData)
	{
		return MakeErrorResult(TEXT("Failed to create session after create"));
	}

	return BuildStateResponse(SessionId, LiveData);
}

FToolResult ClaireonTool_EditWidgetBP::Operation_GetState(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_EditWidgetBP::Operation_Focus(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return MakeErrorResult(TEXT("Missing required field: widget_name"));
	}

	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		return MakeErrorResult(TEXT("Widget Blueprint or WidgetTree is no longer valid"));
	}

	UWidget* FoundWidget = ClaireonWidgetHelpers::FindWidgetByName(WBP->WidgetTree, FName(*WidgetName));
	if (!FoundWidget)
	{
		return MakeErrorResult(FString::Printf(TEXT("Widget '%s' not found in widget tree"), *WidgetName));
	}

	Data->FocusedWidget = FName(*WidgetName);

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_EditWidgetBP::Operation_Compile(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP)
	{
		return MakeErrorResult(TEXT("Widget Blueprint is no longer valid"));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	FKismetEditorUtilities::CompileBlueprint(WBP, EBlueprintCompileOptions::BatchCompile);

	const bool bCompileSuccess = (WBP->Status == BS_UpToDate || WBP->Status == BS_UpToDateWithWarnings);
	FString CompileStatus = bCompileSuccess ? TEXT("Success") : TEXT("Failed");

	TSharedPtr<FJsonObject> ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("session_id"), SessionId);
	ResultJson->SetStringField(TEXT("compile_status"), CompileStatus);
	ResultJson->SetBoolField(TEXT("success"), bCompileSuccess);

	FString ResultString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
	FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);

	return MakeErrorResult(ResultString);
}

FToolResult ClaireonTool_EditWidgetBP::Operation_Save(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP)
	{
		return MakeErrorResult(TEXT("Widget Blueprint is no longer valid"));
	}

	UPackage* Package = WBP->GetOutermost();
	FString PackageFilename = FPackageName::LongPackageNameToFilename(
		Package->GetName(),
		FPackageName::GetAssetPackageExtension());

	FSavePackageArgs SaveArgs;
	const bool bSaved = UPackage::SavePackage(Package, WBP, *PackageFilename, SaveArgs);

	if (!bSaved)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to save package: %s"), *PackageFilename));
	}

	Data->bModified = false;

	TSharedPtr<FJsonObject> ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("session_id"), SessionId);
	ResultJson->SetStringField(TEXT("asset_path"), WBP->GetPathName());
	ResultJson->SetBoolField(TEXT("success"), true);

	FString ResultString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
	FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);

	return MakeErrorResult(ResultString);
}

FToolResult ClaireonTool_EditWidgetBP::Operation_Close(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	bool bWasModified = Data->bModified;

	// Remove tool data before closing session (delegate will also try, but be explicit)
	ToolData.Remove(SessionId);
	FClaireonSessionManager::Get().CloseSession(SessionId);

	FString ResultMsg = TEXT("Session closed successfully.");
	if (bWasModified)
	{
		ResultMsg += TEXT(" Warning: Widget Blueprint had unsaved modifications.");
	}

	TSharedPtr<FJsonObject> ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("session_id"), SessionId);
	ResultJson->SetBoolField(TEXT("success"), true);
	ResultJson->SetStringField(TEXT("message"), ResultMsg);
	if (bWasModified)
	{
		ResultJson->SetBoolField(TEXT("unsaved_changes"), true);
	}

	FString ResultString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
	FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);

	return MakeErrorResult(ResultString);
}

// ============================================================================
// Helpers
// ============================================================================

FToolResult ClaireonTool_EditWidgetBP::BuildStateResponse(const FString& SessionId, FWidgetBPEditToolData* Data)
{
	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(TEXT("Invalid session"));
	}

	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();

	// Serialize widget tree with default options
	FWidgetSerializeOptions Options;
	Options.bIncludeProperties = false;
	Options.MaxDepth = -1;
	TSharedPtr<FJsonObject> TreeData = ClaireonWidgetHelpers::SerializeWidgetTree(WBP, Options);

	// Build response JSON
	TSharedPtr<FJsonObject> ResponseObj = MakeShared<FJsonObject>();
	ResponseObj->SetStringField(TEXT("session_id"), SessionId);
	ResponseObj->SetStringField(TEXT("asset_path"), WBP->GetPathName());
	ResponseObj->SetStringField(TEXT("focused_widget"), Data->FocusedWidget.ToString());
	ResponseObj->SetBoolField(TEXT("modified"), Data->bModified);

	if (TreeData.IsValid())
	{
		ResponseObj->SetObjectField(TEXT("widget_tree"), TreeData);
	}

	FString ResponseString;
	TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&ResponseString);
	FJsonSerializer::Serialize(ResponseObj.ToSharedRef(), Writer);

	return MakeErrorResult(ResponseString);
}

// ============================================================================
// Stage 009: Widget CRUD Operations
// ============================================================================

FToolResult ClaireonTool_EditWidgetBP::Operation_AddWidget(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	// Extract required widget_class
	FString WidgetClassStr;
	if (!Params->TryGetStringField(TEXT("widget_class"), WidgetClassStr))
	{
		return MakeErrorResult(TEXT("Missing required field: widget_class"));
	}

	// Extract optional fields
	FString ParentName;
	Params->TryGetStringField(TEXT("parent_name"), ParentName);

	FString WidgetName;
	Params->TryGetStringField(TEXT("widget_name"), WidgetName);

	int32 InsertIndex = -1;
	Params->TryGetNumberField(TEXT("index"), InsertIndex);

	const TSharedPtr<FJsonObject>* SlotPropertiesPtr = nullptr;
	TSharedPtr<FJsonObject> SlotProperties;
	if (Params->TryGetObjectField(TEXT("slot_properties"), SlotPropertiesPtr) && SlotPropertiesPtr)
	{
		SlotProperties = *SlotPropertiesPtr;
	}

	// Get WBP and WidgetTree from session
	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		return MakeErrorResult(TEXT("Widget Blueprint or WidgetTree is no longer valid"));
	}
	UWidgetTree* Tree = WBP->WidgetTree;

	// Resolve class
	FString ClassError;
	UClass* ResolvedClass = ClaireonWidgetHelpers::ResolveWidgetClass(WidgetClassStr, ClassError);
	if (!ResolvedClass)
	{
		return MakeErrorResult(FString::Printf(TEXT("Could not resolve widget class '%s': %s"), *WidgetClassStr, *ClassError));
	}

	// Determine parent panel
	UPanelWidget* ParentPanel = nullptr;
	if (!ParentName.IsEmpty())
	{
		UWidget* ParentWidget = ClaireonWidgetHelpers::FindWidgetByName(Tree, FName(*ParentName));
		if (!ParentWidget)
		{
			return MakeErrorResult(FString::Printf(TEXT("Parent widget '%s' not found"), *ParentName));
		}
		ParentPanel = Cast<UPanelWidget>(ParentWidget);
		if (!ParentPanel)
		{
			return MakeErrorResult(FString::Printf(TEXT("Parent widget '%s' is not a panel widget"), *ParentName));
		}
	}
	else if (Tree->RootWidget)
	{
		// If no parent specified and root is a panel, use root as default parent
		ParentPanel = Cast<UPanelWidget>(Tree->RootWidget);
	}
	// If no root exists at all, we will set the new widget as root

	// Transaction
	FScopedTransaction Transaction(LOCTEXT("MCPAddWidget", "MCP: Add Widget"));
	Tree->SetFlags(RF_Transactional);
	Tree->Modify();

	// Determine widget name
	FName FinalWidgetName = WidgetName.IsEmpty() ? NAME_None : FName(*WidgetName);

	// Create the widget
	UWidget* NewWidget = ClaireonWidgetHelpers::CreateWidget(Tree, ResolvedClass, FinalWidgetName);
	if (!NewWidget)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to create widget of class '%s'"), *WidgetClassStr));
	}

	// Set as root if no root exists
	if (!Tree->RootWidget)
	{
		Tree->RootWidget = NewWidget;
	}
	else if (ParentPanel)
	{
		// Add to parent panel at specified index or end
		if (InsertIndex >= 0)
		{
			UPanelSlot* Slot = ParentPanel->InsertChildAt(InsertIndex, NewWidget);
			if (Slot && SlotProperties.IsValid())
			{
				for (auto& Pair : SlotProperties->Values)
				{
					FString Error;
					ClaireonWidgetHelpers::WriteSlotProperty(Slot, Pair.Key, Pair.Value->AsString(), Error);
				}
			}
		}
		else
		{
			ClaireonWidgetHelpers::AddChildToPanel(ParentPanel, NewWidget, SlotProperties);
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;
	Data->FocusedWidget = NewWidget->GetFName();

	UE_LOG(LogClaireon, Log, TEXT("[EditWidgetBP] Added widget '%s' (class: %s)"), *NewWidget->GetName(), *ResolvedClass->GetName());

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_EditWidgetBP::Operation_RemoveWidget(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return MakeErrorResult(TEXT("Missing required field: widget_name"));
	}

	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		return MakeErrorResult(TEXT("Widget Blueprint or WidgetTree is no longer valid"));
	}
	UWidgetTree* Tree = WBP->WidgetTree;

	UWidget* Widget = ClaireonWidgetHelpers::FindWidgetByName(Tree, FName(*WidgetName));
	if (!Widget)
	{
		return MakeErrorResult(FString::Printf(TEXT("Widget '%s' not found"), *WidgetName));
	}

	FScopedTransaction Transaction(LOCTEXT("MCPRemoveWidget", "MCP: Remove Widget"));
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
	Data->bModified = true;

	// Clear focus if the focused widget was removed
	if (Data->FocusedWidget == Widget->GetFName())
	{
		Data->FocusedWidget = Tree->RootWidget ? Tree->RootWidget->GetFName() : NAME_None;
	}

	UE_LOG(LogClaireon, Log, TEXT("[EditWidgetBP] Removed widget '%s'"), *WidgetName);

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_EditWidgetBP::Operation_MoveWidget(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return MakeErrorResult(TEXT("Missing required field: widget_name"));
	}

	FString NewParentName;
	if (!Params->TryGetStringField(TEXT("new_parent_name"), NewParentName))
	{
		return MakeErrorResult(TEXT("Missing required field: new_parent_name"));
	}

	int32 InsertIndex = -1;
	Params->TryGetNumberField(TEXT("index"), InsertIndex);

	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		return MakeErrorResult(TEXT("Widget Blueprint or WidgetTree is no longer valid"));
	}
	UWidgetTree* Tree = WBP->WidgetTree;

	UWidget* Widget = ClaireonWidgetHelpers::FindWidgetByName(Tree, FName(*WidgetName));
	if (!Widget)
	{
		return MakeErrorResult(FString::Printf(TEXT("Widget '%s' not found"), *WidgetName));
	}

	UWidget* NewParentWidget = ClaireonWidgetHelpers::FindWidgetByName(Tree, FName(*NewParentName));
	if (!NewParentWidget)
	{
		return MakeErrorResult(FString::Printf(TEXT("New parent widget '%s' not found"), *NewParentName));
	}

	UPanelWidget* NewParentPanel = Cast<UPanelWidget>(NewParentWidget);
	if (!NewParentPanel)
	{
		return MakeErrorResult(FString::Printf(TEXT("New parent widget '%s' is not a panel widget"), *NewParentName));
	}

	FScopedTransaction Transaction(LOCTEXT("MCPMoveWidget", "MCP: Move Widget"));
	Tree->SetFlags(RF_Transactional);
	Tree->Modify();

	// Remove from old parent if it has one
	if (UPanelWidget* OldParent = Cast<UPanelWidget>(Widget->GetParent()))
	{
		OldParent->RemoveChild(Widget);
	}

	// Add to new parent
	if (InsertIndex >= 0)
	{
		NewParentPanel->InsertChildAt(InsertIndex, Widget);
	}
	else
	{
		NewParentPanel->AddChild(Widget);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	UE_LOG(LogClaireon, Log, TEXT("[EditWidgetBP] Moved widget '%s' to parent '%s'"), *WidgetName, *NewParentName);

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_EditWidgetBP::Operation_ReplaceWidget(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return MakeErrorResult(TEXT("Missing required field: widget_name"));
	}

	FString NewWidgetClassStr;
	if (!Params->TryGetStringField(TEXT("new_widget_class"), NewWidgetClassStr))
	{
		return MakeErrorResult(TEXT("Missing required field: new_widget_class"));
	}

	bool bPreserveChildren = true;
	Params->TryGetBoolField(TEXT("preserve_children"), bPreserveChildren);

	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		return MakeErrorResult(TEXT("Widget Blueprint or WidgetTree is no longer valid"));
	}
	UWidgetTree* Tree = WBP->WidgetTree;

	UWidget* OldWidget = ClaireonWidgetHelpers::FindWidgetByName(Tree, FName(*WidgetName));
	if (!OldWidget)
	{
		return MakeErrorResult(FString::Printf(TEXT("Widget '%s' not found"), *WidgetName));
	}

	FString ClassError;
	UClass* NewClass = ClaireonWidgetHelpers::ResolveWidgetClass(NewWidgetClassStr, ClassError);
	if (!NewClass)
	{
		return MakeErrorResult(FString::Printf(TEXT("Could not resolve new widget class '%s': %s"), *NewWidgetClassStr, *ClassError));
	}

	FScopedTransaction Transaction(LOCTEXT("MCPReplaceWidget", "MCP: Replace Widget"));
	Tree->SetFlags(RF_Transactional);
	Tree->Modify();

	// Collect children from old widget before replacing (if it's a panel)
	TArray<UWidget*> OldChildren;
	UPanelWidget* OldPanel = Cast<UPanelWidget>(OldWidget);
	if (bPreserveChildren && OldPanel)
	{
		for (int32 i = 0; i < OldPanel->GetChildrenCount(); ++i)
		{
			OldChildren.Add(OldPanel->GetChildAt(i));
		}
	}

	// Create new widget
	UWidget* NewWidget = ClaireonWidgetHelpers::CreateWidget(Tree, NewClass, NAME_None);
	if (!NewWidget)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to create new widget of class '%s'"), *NewWidgetClassStr));
	}

	// Note old parent before replacing
	UPanelWidget* ParentPanel = Cast<UPanelWidget>(OldWidget->GetParent());

	if (Tree->RootWidget == OldWidget)
	{
		// Replace root
		Tree->RootWidget = NewWidget;
	}
	else if (ParentPanel)
	{
		// Use ReplaceChild to maintain slot position
		if (!ParentPanel->ReplaceChild(OldWidget, NewWidget))
		{
			// Fallback: remove old and add new
			ParentPanel->RemoveChild(OldWidget);
			ParentPanel->AddChild(NewWidget);
		}
	}

	// Reparent children to the new widget if it's a panel
	UPanelWidget* NewPanel = Cast<UPanelWidget>(NewWidget);
	if (OldChildren.Num() > 0 && NewPanel)
	{
		for (UWidget* Child : OldChildren)
		{
			if (OldPanel)
			{
				OldPanel->RemoveChild(Child);
			}
			NewPanel->AddChild(Child);
		}
		UE_LOG(LogClaireon, Log, TEXT("[EditWidgetBP] Reparented %d children to replacement widget"), OldChildren.Num());
	}
	else if (OldChildren.Num() > 0 && !NewPanel)
	{
		UE_LOG(LogClaireon, Warning, TEXT("[EditWidgetBP] Replacement widget '%s' is not a panel — %d children were lost"), *NewWidgetClassStr, OldChildren.Num());
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	// Update focus if needed
	if (Data->FocusedWidget == OldWidget->GetFName())
	{
		Data->FocusedWidget = NewWidget->GetFName();
	}

	UE_LOG(LogClaireon, Log, TEXT("[EditWidgetBP] Replaced widget '%s' with class '%s' -> '%s'"), *WidgetName, *NewWidgetClassStr, *NewWidget->GetName());

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_EditWidgetBP::Operation_RenameWidget(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return MakeErrorResult(TEXT("Missing required field: widget_name"));
	}

	FString NewName;
	if (!Params->TryGetStringField(TEXT("new_name"), NewName))
	{
		return MakeErrorResult(TEXT("Missing required field: new_name"));
	}

	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		return MakeErrorResult(TEXT("Widget Blueprint or WidgetTree is no longer valid"));
	}
	UWidgetTree* Tree = WBP->WidgetTree;

	UWidget* Widget = ClaireonWidgetHelpers::FindWidgetByName(Tree, FName(*WidgetName));
	if (!Widget)
	{
		return MakeErrorResult(FString::Printf(TEXT("Widget '%s' not found"), *WidgetName));
	}

	// Verify new name doesn't conflict with an existing widget
	if (ClaireonWidgetHelpers::FindWidgetByName(Tree, FName(*NewName)) != nullptr)
	{
		return MakeErrorResult(FString::Printf(TEXT("A widget named '%s' already exists in the tree"), *NewName));
	}

	FScopedTransaction Transaction(LOCTEXT("MCPRenameWidget", "MCP: Rename Widget"));
	Tree->SetFlags(RF_Transactional);
	Tree->Modify();
	Widget->Modify();

	FName OldFName = Widget->GetFName();

	// Rename the UObject
	Widget->Rename(*NewName, Widget->GetOuter());

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	// Update focus if the focused widget was renamed
	if (Data->FocusedWidget == OldFName)
	{
		Data->FocusedWidget = Widget->GetFName();
	}

	UE_LOG(LogClaireon, Log, TEXT("[EditWidgetBP] Renamed widget '%s' to '%s'"), *WidgetName, *Widget->GetName());

	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// Stage 010: Property Operations
// ============================================================================

FToolResult ClaireonTool_EditWidgetBP::Operation_SetWidgetProperty(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return MakeErrorResult(TEXT("Missing required field: widget_name"));
	}

	FString PropertyName;
	if (!Params->TryGetStringField(TEXT("property_name"), PropertyName))
	{
		return MakeErrorResult(TEXT("Missing required field: property_name"));
	}

	FString Value;
	if (!Params->TryGetStringField(TEXT("value"), Value))
	{
		return MakeErrorResult(TEXT("Missing required field: value"));
	}

	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		return MakeErrorResult(TEXT("Widget Blueprint or WidgetTree is no longer valid"));
	}

	UWidget* Widget = ClaireonWidgetHelpers::FindWidgetByName(WBP->WidgetTree, FName(*WidgetName));
	if (!Widget)
	{
		return MakeErrorResult(FString::Printf(TEXT("Widget '%s' not found"), *WidgetName));
	}

	FScopedTransaction Transaction(LOCTEXT("MCPSetWidgetProperty", "MCP: Set Widget Property"));
	Widget->Modify();

	FString WriteError;
	if (!ClaireonWidgetHelpers::WriteWidgetProperty(Widget, PropertyName, Value, WriteError))
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to set property '%s' on widget '%s': %s"), *PropertyName, *WidgetName, *WriteError));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_EditWidgetBP::Operation_SetSlotProperty(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return MakeErrorResult(TEXT("Missing required field: widget_name"));
	}

	FString PropertyName;
	if (!Params->TryGetStringField(TEXT("property_name"), PropertyName))
	{
		return MakeErrorResult(TEXT("Missing required field: property_name"));
	}

	FString Value;
	if (!Params->TryGetStringField(TEXT("value"), Value))
	{
		return MakeErrorResult(TEXT("Missing required field: value"));
	}

	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		return MakeErrorResult(TEXT("Widget Blueprint or WidgetTree is no longer valid"));
	}

	UWidget* Widget = ClaireonWidgetHelpers::FindWidgetByName(WBP->WidgetTree, FName(*WidgetName));
	if (!Widget)
	{
		return MakeErrorResult(FString::Printf(TEXT("Widget '%s' not found"), *WidgetName));
	}

	UPanelSlot* Slot = Widget->Slot;
	if (!Slot)
	{
		return MakeErrorResult(FString::Printf(TEXT("Widget '%s' has no slot (it may be the root widget)"), *WidgetName));
	}

	FScopedTransaction Transaction(LOCTEXT("MCPSetSlotProperty", "MCP: Set Slot Property"));
	Slot->Modify();

	FString WriteError;
	if (!ClaireonWidgetHelpers::WriteSlotProperty(Slot, PropertyName, Value, WriteError))
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to set slot property '%s' on widget '%s': %s"), *PropertyName, *WidgetName, *WriteError));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_EditWidgetBP::Operation_GetWidgetDetails(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return MakeErrorResult(TEXT("Missing required field: widget_name"));
	}

	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		return MakeErrorResult(TEXT("Widget Blueprint or WidgetTree is no longer valid"));
	}
	UWidgetTree* Tree = WBP->WidgetTree;

	UWidget* Widget = ClaireonWidgetHelpers::FindWidgetByName(Tree, FName(*WidgetName));
	if (!Widget)
	{
		return MakeErrorResult(FString::Printf(TEXT("Widget '%s' not found"), *WidgetName));
	}

	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetStringField(TEXT("name"), Widget->GetName());
	Details->SetStringField(TEXT("class"), Widget->GetClass()->GetPathName());

	// Parent info
	if (UWidget* Parent = Widget->GetParent())
	{
		Details->SetStringField(TEXT("parent_name"), Parent->GetName());
	}
	else
	{
		Details->SetStringField(TEXT("parent_name"), TEXT(""));
	}

	// Panel info
	UPanelWidget* AsPanel = Cast<UPanelWidget>(Widget);
	Details->SetBoolField(TEXT("is_panel"), AsPanel != nullptr);

	if (AsPanel)
	{
		Details->SetNumberField(TEXT("child_count"), AsPanel->GetChildrenCount());

		TArray<TSharedPtr<FJsonValue>> ChildNames;
		for (int32 i = 0; i < AsPanel->GetChildrenCount(); ++i)
		{
			if (UWidget* Child = AsPanel->GetChildAt(i))
			{
				ChildNames.Add(MakeShared<FJsonValueString>(Child->GetName()));
			}
		}
		Details->SetArrayField(TEXT("children"), ChildNames);
	}

	// Slot info
	if (UPanelSlot* Slot = Widget->Slot)
	{
		Details->SetStringField(TEXT("slot_type"), Slot->GetClass()->GetName());

		TSharedPtr<FJsonObject> SlotProps = MakeShared<FJsonObject>();
		for (TFieldIterator<FProperty> PropIt(Slot->GetClass()); PropIt; ++PropIt)
		{
			FProperty* Prop = *PropIt;
			if (!Prop->HasAnyPropertyFlags(CPF_Edit))
			{
				continue;
			}

			bool bSuccess = false;
			FString PropValue = ClaireonWidgetHelpers::ReadSlotProperty(Slot, Prop->GetName(), bSuccess);
			if (bSuccess)
			{
				SlotProps->SetStringField(Prop->GetName(), PropValue);
			}
		}
		Details->SetObjectField(TEXT("slot_properties"), SlotProps);
	}

	// All editable widget properties
	TSharedPtr<FJsonObject> WidgetProps = MakeShared<FJsonObject>();
	for (TFieldIterator<FProperty> PropIt(Widget->GetClass()); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;
		if (!Prop->HasAnyPropertyFlags(CPF_Edit))
		{
			continue;
		}

		bool bSuccess = false;
		FString PropValue = ClaireonWidgetHelpers::ReadWidgetProperty(Widget, Prop->GetName(), bSuccess);
		if (bSuccess)
		{
			WidgetProps->SetStringField(Prop->GetName(), PropValue);
		}
	}
	Details->SetObjectField(TEXT("properties"), WidgetProps);

	FString ResultString;
	TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&ResultString);
	FJsonSerializer::Serialize(Details.ToSharedRef(), Writer);

	return MakeErrorResult(ResultString);
}

// ============================================================================
// Stage 011: Advanced Operations
// ============================================================================

FToolResult ClaireonTool_EditWidgetBP::Operation_AddBinding(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString WidgetNameStr;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetNameStr))
	{
		return MakeErrorResult(TEXT("Missing required field: widget_name"));
	}

	FString PropertyNameStr;
	if (!Params->TryGetStringField(TEXT("property_name"), PropertyNameStr))
	{
		return MakeErrorResult(TEXT("Missing required field: property_name"));
	}

	FString FunctionNameStr;
	if (!Params->TryGetStringField(TEXT("function_name"), FunctionNameStr))
	{
		return MakeErrorResult(TEXT("Missing required field: function_name"));
	}

	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		return MakeErrorResult(TEXT("Widget Blueprint or WidgetTree is no longer valid"));
	}

	// Validate the widget exists
	UWidget* Widget = ClaireonWidgetHelpers::FindWidgetByName(WBP->WidgetTree, FName(*WidgetNameStr));
	if (!Widget)
	{
		return MakeErrorResult(FString::Printf(TEXT("Widget '%s' not found"), *WidgetNameStr));
	}

	// Build the binding
	FDelegateEditorBinding Binding;
	Binding.ObjectName = WidgetNameStr;
	Binding.PropertyName = FName(*PropertyNameStr);
	Binding.FunctionName = FName(*FunctionNameStr);
	Binding.Kind = EBindingKind::Function;

	// Remove any pre-existing binding for the same widget+property (only one allowed)
	WBP->Bindings.RemoveAll([&](const FDelegateEditorBinding& Existing)
	{
		return Existing.ObjectName == Binding.ObjectName && Existing.PropertyName == Binding.PropertyName;
	});

	WBP->Bindings.Add(Binding);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	ResultObj->SetStringField(TEXT("widget_name"), WidgetNameStr);
	ResultObj->SetStringField(TEXT("property_name"), PropertyNameStr);
	ResultObj->SetStringField(TEXT("function_name"), FunctionNameStr);

	FString ResultString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
	FJsonSerializer::Serialize(ResultObj.ToSharedRef(), Writer);

	return MakeErrorResult(ResultString);
}

FToolResult ClaireonTool_EditWidgetBP::Operation_RemoveBinding(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString WidgetNameStr;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetNameStr))
	{
		return MakeErrorResult(TEXT("Missing required field: widget_name"));
	}

	FString PropertyNameStr;
	if (!Params->TryGetStringField(TEXT("property_name"), PropertyNameStr))
	{
		return MakeErrorResult(TEXT("Missing required field: property_name"));
	}

	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP)
	{
		return MakeErrorResult(TEXT("Widget Blueprint is no longer valid"));
	}

	int32 RemovedCount = WBP->Bindings.RemoveAll([&](const FDelegateEditorBinding& Binding)
	{
		return Binding.ObjectName == WidgetNameStr && Binding.PropertyName == FName(*PropertyNameStr);
	});

	if (RemovedCount > 0)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
		Data->bModified = true;
	}

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	ResultObj->SetNumberField(TEXT("removed_count"), RemovedCount);

	FString ResultString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
	FJsonSerializer::Serialize(ResultObj.ToSharedRef(), Writer);

	return MakeErrorResult(ResultString);
}

FToolResult ClaireonTool_EditWidgetBP::Operation_ListAnimations(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP)
	{
		return MakeErrorResult(TEXT("Widget Blueprint is no longer valid"));
	}

	bool bIncludeDetails = false;
	Params->TryGetBoolField(TEXT("include_details"), bIncludeDetails);

	TArray<TSharedPtr<FJsonValue>> AnimArray;
	for (UWidgetAnimation* Anim : WBP->Animations)
	{
		if (!Anim)
		{
			continue;
		}

		TSharedPtr<FJsonObject> AnimObj;
		if (bIncludeDetails)
		{
			AnimObj = ClaireonWidgetHelpers::SerializeAnimationDetails(Anim);
		}
		else
		{
			AnimObj = MakeShared<FJsonObject>();
			AnimObj->SetStringField(TEXT("name"), Anim->GetDisplayName().ToString());
			AnimObj->SetNumberField(TEXT("start_time"), Anim->GetStartTime());
			AnimObj->SetNumberField(TEXT("end_time"), Anim->GetEndTime());
		}
		AnimArray.Add(MakeShared<FJsonValueObject>(AnimObj));
	}

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetArrayField(TEXT("animations"), AnimArray);
	ResultObj->SetNumberField(TEXT("count"), AnimArray.Num());

	return MakeSuccessResult(ResultObj, TEXT(""));
}

FToolResult ClaireonTool_EditWidgetBP::Operation_ImportWidgets(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString WidgetText;
	if (!Params->TryGetStringField(TEXT("widget_text"), WidgetText))
	{
		return MakeErrorResult(TEXT("Missing required field: widget_text"));
	}

	FString ParentName;
	Params->TryGetStringField(TEXT("parent_name"), ParentName);

	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		return MakeErrorResult(TEXT("Widget Blueprint or WidgetTree is no longer valid"));
	}

	TSet<UWidget*> ImportedWidgets;
	TMap<FName, UWidgetSlotPair*> PastedExtraSlotData;

	FScopedTransaction Transaction(LOCTEXT("MCPImportWidgets", "MCP: Import Widgets"));
	WBP->WidgetTree->SetFlags(RF_Transactional);
	WBP->WidgetTree->Modify();

	FWidgetBlueprintEditorUtils::ImportWidgetsFromText(WBP, WidgetText, ImportedWidgets, PastedExtraSlotData);

	if (ImportedWidgets.Num() == 0)
	{
		return MakeErrorResult(TEXT("No widgets were imported from the provided text"));
	}

	// If a parent is specified and imported widgets have no parent, add them to the named parent
	if (!ParentName.IsEmpty())
	{
		UWidget* ParentWidget = ClaireonWidgetHelpers::FindWidgetByName(WBP->WidgetTree, FName(*ParentName));
		UPanelWidget* ParentPanel = Cast<UPanelWidget>(ParentWidget);
		if (ParentPanel)
		{
			for (UWidget* Imported : ImportedWidgets)
			{
				if (Imported && !Imported->GetParent())
				{
					ParentPanel->AddChild(Imported);
				}
			}
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	// Collect imported widget names
	TArray<TSharedPtr<FJsonValue>> ImportedNames;
	for (UWidget* Imported : ImportedWidgets)
	{
		if (Imported)
		{
			ImportedNames.Add(MakeShared<FJsonValueString>(Imported->GetName()));
		}
	}

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	ResultObj->SetArrayField(TEXT("imported_widgets"), ImportedNames);
	ResultObj->SetNumberField(TEXT("count"), ImportedNames.Num());

	FString ResultString;
	TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&ResultString);
	FJsonSerializer::Serialize(ResultObj.ToSharedRef(), Writer);

	return MakeErrorResult(ResultString);
}

FToolResult ClaireonTool_EditWidgetBP::Operation_ExportWidgets(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* WidgetNamesArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("widget_names"), WidgetNamesArray) || !WidgetNamesArray)
	{
		return MakeErrorResult(TEXT("Missing required field: widget_names (array of strings)"));
	}

	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		return MakeErrorResult(TEXT("Widget Blueprint or WidgetTree is no longer valid"));
	}

	TArray<UWidget*> WidgetsToExport;
	for (const TSharedPtr<FJsonValue>& NameValue : *WidgetNamesArray)
	{
		FString Name = NameValue->AsString();
		UWidget* Widget = ClaireonWidgetHelpers::FindWidgetByName(WBP->WidgetTree, FName(*Name));
		if (!Widget)
		{
			return MakeErrorResult(FString::Printf(TEXT("Widget '%s' not found"), *Name));
		}
		WidgetsToExport.Add(Widget);
	}

	if (WidgetsToExport.Num() == 0)
	{
		return MakeErrorResult(TEXT("No widgets specified for export"));
	}

	FString ExportedText;
	FWidgetBlueprintEditorUtils::ExportWidgetsToText(WidgetsToExport, ExportedText);

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	ResultObj->SetStringField(TEXT("widget_text"), ExportedText);
	ResultObj->SetNumberField(TEXT("widget_count"), WidgetsToExport.Num());

	FString ResultString;
	TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&ResultString);
	FJsonSerializer::Serialize(ResultObj.ToSharedRef(), Writer);

	return MakeErrorResult(ResultString);
}

FToolResult ClaireonTool_EditWidgetBP::Operation_ListWidgetClasses(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Filter;
	Params->TryGetStringField(TEXT("filter"), Filter);

	bool bPanelsOnly = false;
	Params->TryGetBoolField(TEXT("panels_only"), bPanelsOnly);

	TArray<UClass*> FoundClasses;

	for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
	{
		UClass* Class = *ClassIt;

		// Must be a non-abstract, non-deprecated UWidget subclass
		if (!Class->IsChildOf(UWidget::StaticClass()))
		{
			continue;
		}
		if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
		{
			continue;
		}

		// panels_only filter
		if (bPanelsOnly && !Class->IsChildOf(UPanelWidget::StaticClass()))
		{
			continue;
		}

		// string filter (case-insensitive match on class name)
		if (!Filter.IsEmpty())
		{
			FString ClassName = Class->GetName();
			if (!ClassName.Contains(Filter, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		FoundClasses.Add(Class);
	}

	// Sort alphabetically by class name
	FoundClasses.Sort([](const UClass& A, const UClass& B)
	{
		return A.GetName() < B.GetName();
	});

	TArray<TSharedPtr<FJsonValue>> ClassArray;
	for (UClass* Class : FoundClasses)
	{
		TSharedPtr<FJsonObject> ClassObj = MakeShared<FJsonObject>();
		ClassObj->SetStringField(TEXT("name"), Class->GetName());
		ClassObj->SetBoolField(TEXT("is_panel"), Class->IsChildOf(UPanelWidget::StaticClass()));
		ClassArray.Add(MakeShared<FJsonValueObject>(ClassObj));
	}

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetArrayField(TEXT("classes"), ClassArray);
	ResultObj->SetNumberField(TEXT("count"), ClassArray.Num());

	FString ResultString;
	TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&ResultString);
	FJsonSerializer::Serialize(ResultObj.ToSharedRef(), Writer);

	return MakeErrorResult(ResultString);
}

// ============================================================================
// MVVM Helpers (local)
// ============================================================================

static bool ParseBindingMode(const FString& ModeStr, EMVVMBindingMode& OutMode)
{
	if (ModeStr == TEXT("OneWayToDestination")) { OutMode = EMVVMBindingMode::OneWayToDestination; return true; }
	if (ModeStr == TEXT("OneWayToSource")) { OutMode = EMVVMBindingMode::OneWayToSource; return true; }
	if (ModeStr == TEXT("TwoWay")) { OutMode = EMVVMBindingMode::TwoWay; return true; }
	if (ModeStr == TEXT("OneTimeToDestination")) { OutMode = EMVVMBindingMode::OneTimeToDestination; return true; }
	if (ModeStr == TEXT("OneTimeToSource")) { OutMode = EMVVMBindingMode::OneTimeToSource; return true; }
	return false;
}

static bool ParseCreationType(const FString& TypeStr, EMVVMBlueprintViewModelContextCreationType& OutType)
{
	if (TypeStr == TEXT("Manual")) { OutType = EMVVMBlueprintViewModelContextCreationType::Manual; return true; }
	if (TypeStr == TEXT("CreateInstance")) { OutType = EMVVMBlueprintViewModelContextCreationType::CreateInstance; return true; }
	if (TypeStr == TEXT("GlobalViewModelCollection")) { OutType = EMVVMBlueprintViewModelContextCreationType::GlobalViewModelCollection; return true; }
	if (TypeStr == TEXT("PropertyPath")) { OutType = EMVVMBlueprintViewModelContextCreationType::PropertyPath; return true; }
	if (TypeStr == TEXT("Resolver")) { OutType = EMVVMBlueprintViewModelContextCreationType::Resolver; return true; }
	return false;
}

/**
 * Resolve a property path string (dot-separated) against a starting UClass.
 * Sets up the FMVVMBlueprintPropertyPath fields using SetPropertyPath/AppendPropertyPath.
 * Returns false and sets OutError on failure.
 */
static bool ResolvePropertyPath(
	UWidgetBlueprint* WBP,
	FMVVMBlueprintPropertyPath& Path,
	UClass* StartClass,
	const FString& PropertyPathStr,
	FString& OutError)
{
	TArray<FString> Segments;
	PropertyPathStr.ParseIntoArray(Segments, TEXT("."), true);

	if (Segments.Num() == 0)
	{
		OutError = FString::Printf(TEXT("Property path is empty"));
		return false;
	}

	UStruct* CurrentStruct = StartClass;
	for (int32 i = 0; i < Segments.Num(); ++i)
	{
		if (!CurrentStruct)
		{
			OutError = FString::Printf(TEXT("Cannot resolve segment '%s' — no struct context at depth %d"), *Segments[i], i);
			return false;
		}

		const FProperty* Prop = CurrentStruct->FindPropertyByName(FName(*Segments[i]));
		if (!Prop)
		{
			OutError = FString::Printf(TEXT("Property '%s' not found on %s"), *Segments[i], *CurrentStruct->GetName());
			return false;
		}

		UE::MVVM::FMVVMConstFieldVariant FieldVariant(Prop);
		if (i == 0)
		{
			Path.SetPropertyPath(WBP, FieldVariant);
		}
		else
		{
			Path.AppendPropertyPath(WBP, FieldVariant);
		}

		// Advance current struct for next segment
		if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
		{
			CurrentStruct = StructProp->Struct;
		}
		else if (const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
		{
			CurrentStruct = ObjProp->PropertyClass;
		}
		else
		{
			// Primitive — no further nesting possible
			CurrentStruct = nullptr;
		}
	}

	return true;
}

/**
 * Resolve a conversion function name to a UFunction*.
 * Tries: full path, Class::Function, self-context.
 */
static const UFunction* ResolveConversionFunction(
	UWidgetBlueprint* WBP,
	const FString& FunctionNameStr,
	FString& OutError)
{
	// 1. Full path
	const UFunction* Func = FindObject<UFunction>(nullptr, *FunctionNameStr);
	if (Func)
	{
		if (UMVVMBlueprintViewConversionFunction::IsValidConversionFunction(WBP, Func))
		{
			return Func;
		}
		OutError = FString::Printf(TEXT("Function '%s' found but is not a valid MVVM conversion function"), *FunctionNameStr);
		return nullptr;
	}

	// 2. Class::Function format
	FString ClassName, FuncName;
	if (FunctionNameStr.Split(TEXT("::"), &ClassName, &FuncName))
	{
		// Strip leading 'U' if present for class lookup
		UClass* FoundClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
		if (!FoundClass && ClassName.StartsWith(TEXT("U")))
		{
			FoundClass = FindFirstObject<UClass>(*ClassName.Mid(1), EFindFirstObjectOptions::NativeFirst);
		}
		if (FoundClass)
		{
			Func = FoundClass->FindFunctionByName(FName(*FuncName));
			if (Func)
			{
				if (UMVVMBlueprintViewConversionFunction::IsValidConversionFunction(WBP, Func))
				{
					return Func;
				}
				OutError = FString::Printf(TEXT("Function '%s::%s' found but is not a valid MVVM conversion function"), *ClassName, *FuncName);
				return nullptr;
			}
		}
	}

	// 3. Self-context: search WBP generated class hierarchy
	if (WBP->GeneratedClass)
	{
		Func = WBP->GeneratedClass->FindFunctionByName(FName(*FunctionNameStr));
		if (Func)
		{
			if (UMVVMBlueprintViewConversionFunction::IsValidConversionFunction(WBP, Func))
			{
				return Func;
			}
			OutError = FString::Printf(TEXT("Function '%s' found on self but is not a valid MVVM conversion function"), *FunctionNameStr);
			return nullptr;
		}
	}

	OutError = FString::Printf(TEXT("Conversion function '%s' not found"), *FunctionNameStr);
	return nullptr;
}

// ============================================================================
// MVVM ViewModel Operations
// ============================================================================

FToolResult ClaireonTool_EditWidgetBP::Operation_ListMVVMViewModels(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP)
	{
		return MakeErrorResult(TEXT("Widget Blueprint is no longer valid"));
	}

	TSharedPtr<FJsonObject> Result = ClaireonWidgetHelpers::SerializeMVVMViewModelContexts(WBP);
	int32 Count = 0;
	if (Result.IsValid())
	{
		Count = static_cast<int32>(Result->GetNumberField(TEXT("count")));
	}

	return MakeSuccessResult(Result, FString::Printf(TEXT("Found %d MVVM ViewModel context(s)"), Count));
}

FToolResult ClaireonTool_EditWidgetBP::Operation_AddMVVMViewModel(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP)
	{
		return MakeErrorResult(TEXT("Widget Blueprint is no longer valid"));
	}

	FString ViewModelName;
	if (!Params->TryGetStringField(TEXT("viewmodel_name"), ViewModelName) || ViewModelName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required param: viewmodel_name"));
	}

	FString ViewModelClassStr;
	if (!Params->TryGetStringField(TEXT("viewmodel_class"), ViewModelClassStr) || ViewModelClassStr.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required param: viewmodel_class"));
	}

	// Resolve viewmodel class
	UClass* VMClass = FindFirstObject<UClass>(*ViewModelClassStr, EFindFirstObjectOptions::NativeFirst);
	if (!VMClass)
	{
		VMClass = LoadClass<UMVVMViewModelBase>(nullptr, *ViewModelClassStr);
	}
	if (!VMClass)
	{
		// Try with 'U' prefix
		VMClass = FindFirstObject<UClass>(*FString::Printf(TEXT("U%s"), *ViewModelClassStr), EFindFirstObjectOptions::NativeFirst);
	}
	if (!VMClass || !VMClass->IsChildOf(UMVVMViewModelBase::StaticClass()))
	{
		return MakeErrorResult(FString::Printf(TEXT("Could not resolve '%s' to a UMVVMViewModelBase subclass"), *ViewModelClassStr));
	}

	// Parse optional params
	FString CreationTypeStr;
	EMVVMBlueprintViewModelContextCreationType CreationType = EMVVMBlueprintViewModelContextCreationType::Manual;
	if (Params->TryGetStringField(TEXT("creation_type"), CreationTypeStr) && !CreationTypeStr.IsEmpty())
	{
		if (!ParseCreationType(CreationTypeStr, CreationType))
		{
			return MakeErrorResult(FString::Printf(TEXT("Invalid creation_type: '%s'. Valid values: Manual, CreateInstance, GlobalViewModelCollection, PropertyPath, Resolver"), *CreationTypeStr));
		}
	}

	bool bOptional = false;
	Params->TryGetBoolField(TEXT("optional"), bOptional);

	// Get or create MVVM view
	UMVVMBlueprintView* View = ClaireonWidgetHelpers::GetOrCreateMVVMBlueprintView(WBP);
	if (!View)
	{
		return MakeErrorResult(TEXT("Failed to get or create MVVM Blueprint View"));
	}

	// Check for duplicate
	if (View->FindViewModel(FName(*ViewModelName)) != nullptr)
	{
		return MakeErrorResult(FString::Printf(TEXT("ViewModel with name '%s' already exists"), *ViewModelName));
	}

	// Construct and add context
	FScopedTransaction Transaction(LOCTEXT("MCPAddMVVMViewModel", "MCP: Add MVVM ViewModel"));

	FMVVMBlueprintViewModelContext Context;
	Context.ViewModelName = FName(*ViewModelName);
	Context.NotifyFieldValueClass = VMClass;
	Context.CreationType = CreationType;
	Context.bOptional = bOptional;

	View->AddViewModel(Context);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	// Return the created context info
	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetStringField(TEXT("viewmodel_name"), ViewModelName);
	ResultObj->SetStringField(TEXT("viewmodel_class"), VMClass->GetPathName());
	ResultObj->SetStringField(TEXT("creation_type"), CreationTypeStr.IsEmpty() ? TEXT("Manual") : *CreationTypeStr);
	ResultObj->SetBoolField(TEXT("optional"), bOptional);

	// Try to get the assigned GUID
	const FMVVMBlueprintViewModelContext* Added = View->FindViewModel(FName(*ViewModelName));
	if (Added)
	{
		ResultObj->SetStringField(TEXT("id"), Added->GetViewModelId().ToString());
	}

	return MakeSuccessResult(ResultObj, FString::Printf(TEXT("Added MVVM ViewModel '%s' (%s)"), *ViewModelName, *VMClass->GetName()));
}

FToolResult ClaireonTool_EditWidgetBP::Operation_RemoveMVVMViewModel(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP)
	{
		return MakeErrorResult(TEXT("Widget Blueprint is no longer valid"));
	}

	FString ViewModelName;
	if (!Params->TryGetStringField(TEXT("viewmodel_name"), ViewModelName) || ViewModelName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required param: viewmodel_name"));
	}

	const UMVVMBlueprintView* View = ClaireonWidgetHelpers::GetMVVMBlueprintView(WBP);
	if (!View)
	{
		return MakeErrorResult(TEXT("No MVVM Blueprint View exists on this widget blueprint"));
	}

	const FMVVMBlueprintViewModelContext* Context = View->FindViewModel(FName(*ViewModelName));
	if (!Context)
	{
		return MakeErrorResult(FString::Printf(TEXT("ViewModel '%s' not found"), *ViewModelName));
	}

	FGuid VMId = Context->GetViewModelId();

	FScopedTransaction Transaction(LOCTEXT("MCPRemoveMVVMViewModel", "MCP: Remove MVVM ViewModel"));

	// Need non-const view for mutation
	UMVVMBlueprintView* MutableView = ClaireonWidgetHelpers::GetOrCreateMVVMBlueprintView(WBP);
	bool bRemoved = MutableView->RemoveViewModel(VMId);
	if (!bRemoved)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to remove ViewModel '%s'"), *ViewModelName));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetStringField(TEXT("removed_viewmodel"), ViewModelName);

	return MakeSuccessResult(ResultObj, FString::Printf(TEXT("Removed MVVM ViewModel '%s'"), *ViewModelName));
}

// ============================================================================
// MVVM Binding Operations
// ============================================================================

FToolResult ClaireonTool_EditWidgetBP::Operation_ListMVVMBindings(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP)
	{
		return MakeErrorResult(TEXT("Widget Blueprint is no longer valid"));
	}

	TSharedPtr<FJsonObject> Result = ClaireonWidgetHelpers::SerializeMVVMBindings(WBP);
	int32 Count = 0;
	if (Result.IsValid())
	{
		Count = static_cast<int32>(Result->GetNumberField(TEXT("count")));
	}

	return MakeSuccessResult(Result, FString::Printf(TEXT("Found %d MVVM binding(s)"), Count));
}

FToolResult ClaireonTool_EditWidgetBP::Operation_AddMVVMBinding(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP)
	{
		return MakeErrorResult(TEXT("Widget Blueprint is no longer valid"));
	}

	// Required params
	FString ViewModelName, ViewModelProperty, WidgetNameStr, WidgetProperty;
	if (!Params->TryGetStringField(TEXT("viewmodel_name"), ViewModelName) || ViewModelName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required param: viewmodel_name"));
	}
	if (!Params->TryGetStringField(TEXT("viewmodel_property"), ViewModelProperty) || ViewModelProperty.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required param: viewmodel_property"));
	}
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetNameStr) || WidgetNameStr.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required param: widget_name"));
	}
	if (!Params->TryGetStringField(TEXT("widget_property"), WidgetProperty) || WidgetProperty.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required param: widget_property"));
	}

	// Optional params
	FString ModeStr = TEXT("OneWayToDestination");
	Params->TryGetStringField(TEXT("mode"), ModeStr);
	EMVVMBindingMode BindingMode;
	if (!ParseBindingMode(ModeStr, BindingMode))
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid mode: '%s'. Valid: OneWayToDestination, OneWayToSource, TwoWay, OneTimeToDestination, OneTimeToSource"), *ModeStr));
	}

	bool bEnabled = true;
	Params->TryGetBoolField(TEXT("enabled"), bEnabled);

	FString ConversionFunctionStr;
	Params->TryGetStringField(TEXT("conversion_function"), ConversionFunctionStr);

	// Validate MVVM view exists
	UMVVMBlueprintView* View = ClaireonWidgetHelpers::GetOrCreateMVVMBlueprintView(WBP);
	if (!View)
	{
		return MakeErrorResult(TEXT("Failed to get or create MVVM Blueprint View"));
	}

	// Find ViewModel context
	const FMVVMBlueprintViewModelContext* VMContext = View->FindViewModel(FName(*ViewModelName));
	if (!VMContext)
	{
		return MakeErrorResult(FString::Printf(TEXT("ViewModel '%s' not found. Add it first with add_mvvm_viewmodel."), *ViewModelName));
	}

	// Validate widget exists
	UWidget* Widget = ClaireonWidgetHelpers::FindWidgetByName(WBP->WidgetTree, FName(*WidgetNameStr));
	if (!Widget)
	{
		return MakeErrorResult(FString::Printf(TEXT("Widget '%s' not found in the widget tree"), *WidgetNameStr));
	}

	FScopedTransaction Transaction(LOCTEXT("MCPAddMVVMBinding", "MCP: Add MVVM Binding"));

	FMVVMBlueprintViewBinding& NewBinding = View->AddDefaultBinding();

	// Configure source path (ViewModel)
	FMVVMBlueprintPropertyPath SourcePath;
	SourcePath.SetViewModelId(VMContext->GetViewModelId());
	{
		FString Error;
		UClass* VMClass = VMContext->GetViewModelClass();
		if (!VMClass)
		{
			View->RemoveBinding(&NewBinding);
			return MakeErrorResult(TEXT("ViewModel class is null"));
		}
		if (!ResolvePropertyPath(WBP, SourcePath, VMClass, ViewModelProperty, Error))
		{
			View->RemoveBinding(&NewBinding);
			return MakeErrorResult(FString::Printf(TEXT("Failed to resolve viewmodel property path '%s': %s"), *ViewModelProperty, *Error));
		}
	}
	NewBinding.SourcePath = SourcePath;

	// Configure destination path (Widget)
	FMVVMBlueprintPropertyPath DestPath;
	DestPath.SetWidgetName(FName(*WidgetNameStr));
	{
		FString Error;
		if (!ResolvePropertyPath(WBP, DestPath, Widget->GetClass(), WidgetProperty, Error))
		{
			View->RemoveBinding(&NewBinding);
			return MakeErrorResult(FString::Printf(TEXT("Failed to resolve widget property path '%s': %s"), *WidgetProperty, *Error));
		}
	}
	NewBinding.DestinationPath = DestPath;

	NewBinding.BindingType = BindingMode;
	NewBinding.bEnabled = bEnabled;
	NewBinding.bCompile = true;

	// Stage 004: Conversion function support
	if (!ConversionFunctionStr.IsEmpty())
	{
		FString ConvError;
		const UFunction* ConvFunc = ResolveConversionFunction(WBP, ConversionFunctionStr, ConvError);
		if (!ConvFunc)
		{
			View->RemoveBinding(&NewBinding);
			return MakeErrorResult(FString::Printf(TEXT("Failed to resolve conversion function '%s': %s"), *ConversionFunctionStr, *ConvError));
		}

		UMVVMBlueprintViewConversionFunction* ConvObj = NewObject<UMVVMBlueprintViewConversionFunction>(View);
		FName GraphName = MakeUniqueObjectName(WBP, UEdGraph::StaticClass(), TEXT("MVVM_Conv"));
		ConvObj->InitializeFromFunction(WBP, GraphName, ConvFunc);
		NewBinding.Conversion.SourceToDestinationConversion = ConvObj;
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	TSharedPtr<FJsonObject> ResultObj = ClaireonWidgetHelpers::SerializeMVVMBinding(WBP, NewBinding);
	return MakeSuccessResult(ResultObj, FString::Printf(TEXT("Added MVVM binding: %s.%s -> %s.%s"), *ViewModelName, *ViewModelProperty, *WidgetNameStr, *WidgetProperty));
}

FToolResult ClaireonTool_EditWidgetBP::Operation_EditMVVMBinding(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP)
	{
		return MakeErrorResult(TEXT("Widget Blueprint is no longer valid"));
	}

	FString BindingIdStr;
	if (!Params->TryGetStringField(TEXT("binding_id"), BindingIdStr) || BindingIdStr.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required param: binding_id"));
	}

	FGuid BindingId;
	if (!FGuid::Parse(BindingIdStr, BindingId))
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid binding_id GUID: '%s'"), *BindingIdStr));
	}

	UMVVMBlueprintView* View = ClaireonWidgetHelpers::GetOrCreateMVVMBlueprintView(WBP);
	if (!View)
	{
		return MakeErrorResult(TEXT("No MVVM Blueprint View exists"));
	}

	FMVVMBlueprintViewBinding* Binding = View->GetBinding(BindingId);
	if (!Binding)
	{
		return MakeErrorResult(FString::Printf(TEXT("Binding with id '%s' not found"), *BindingIdStr));
	}

	FScopedTransaction Transaction(LOCTEXT("MCPEditMVVMBinding", "MCP: Edit MVVM Binding"));

	// Optional: mode
	FString ModeStr;
	if (Params->TryGetStringField(TEXT("mode"), ModeStr) && !ModeStr.IsEmpty())
	{
		EMVVMBindingMode NewMode;
		if (!ParseBindingMode(ModeStr, NewMode))
		{
			return MakeErrorResult(FString::Printf(TEXT("Invalid mode: '%s'"), *ModeStr));
		}
		Binding->BindingType = NewMode;
	}

	// Optional: enabled
	bool bEnabled;
	if (Params->TryGetBoolField(TEXT("enabled"), bEnabled))
	{
		Binding->bEnabled = bEnabled;
	}

	// Optional: viewmodel_property
	FString ViewModelProperty;
	if (Params->TryGetStringField(TEXT("viewmodel_property"), ViewModelProperty) && !ViewModelProperty.IsEmpty())
	{
		// We need the ViewModel class from the source path
		FGuid VMId = Binding->SourcePath.GetViewModelId();
		const FMVVMBlueprintViewModelContext* VMContext = View->FindViewModel(VMId);
		if (!VMContext)
		{
			return MakeErrorResult(TEXT("Cannot update viewmodel_property: ViewModel context not found for this binding's source"));
		}
		UClass* VMClass = VMContext->GetViewModelClass();
		if (!VMClass)
		{
			return MakeErrorResult(TEXT("ViewModel class is null"));
		}

		FMVVMBlueprintPropertyPath NewSourcePath;
		NewSourcePath.SetViewModelId(VMId);
		FString Error;
		if (!ResolvePropertyPath(WBP, NewSourcePath, VMClass, ViewModelProperty, Error))
		{
			return MakeErrorResult(FString::Printf(TEXT("Failed to resolve viewmodel property path '%s': %s"), *ViewModelProperty, *Error));
		}
		Binding->SourcePath = NewSourcePath;
	}

	// Optional: widget_property
	FString WidgetProperty;
	if (Params->TryGetStringField(TEXT("widget_property"), WidgetProperty) && !WidgetProperty.IsEmpty())
	{
		FName WidgetName = Binding->DestinationPath.GetWidgetName();
		UWidget* Widget = ClaireonWidgetHelpers::FindWidgetByName(WBP->WidgetTree, WidgetName);
		if (!Widget)
		{
			return MakeErrorResult(FString::Printf(TEXT("Widget '%s' not found in the tree for property path update"), *WidgetName.ToString()));
		}

		FMVVMBlueprintPropertyPath NewDestPath;
		NewDestPath.SetWidgetName(WidgetName);
		FString Error;
		if (!ResolvePropertyPath(WBP, NewDestPath, Widget->GetClass(), WidgetProperty, Error))
		{
			return MakeErrorResult(FString::Printf(TEXT("Failed to resolve widget property path '%s': %s"), *WidgetProperty, *Error));
		}
		Binding->DestinationPath = NewDestPath;
	}

	// Optional: conversion_function (Stage 004)
	FString ConversionFunctionStr;
	if (Params->TryGetStringField(TEXT("conversion_function"), ConversionFunctionStr))
	{
		if (ConversionFunctionStr.IsEmpty())
		{
			// Clear conversion
			UMVVMBlueprintViewConversionFunction* Existing = Binding->Conversion.GetConversionFunction(/*bSourceToDestination=*/ true);
			if (Existing)
			{
				Existing->RemoveWrapperGraph(WBP);
			}
			Binding->Conversion.SourceToDestinationConversion = nullptr;
		}
		else
		{
			FString ConvError;
			const UFunction* ConvFunc = ResolveConversionFunction(WBP, ConversionFunctionStr, ConvError);
			if (!ConvFunc)
			{
				return MakeErrorResult(FString::Printf(TEXT("Failed to resolve conversion function '%s': %s"), *ConversionFunctionStr, *ConvError));
			}

			UMVVMBlueprintViewConversionFunction* ConvObj = NewObject<UMVVMBlueprintViewConversionFunction>(View);
			FName GraphName = MakeUniqueObjectName(WBP, UEdGraph::StaticClass(), TEXT("MVVM_Conv"));
			ConvObj->InitializeFromFunction(WBP, GraphName, ConvFunc);
			Binding->Conversion.SourceToDestinationConversion = ConvObj;
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	TSharedPtr<FJsonObject> ResultObj = ClaireonWidgetHelpers::SerializeMVVMBinding(WBP, *Binding);
	return MakeSuccessResult(ResultObj, FString::Printf(TEXT("Updated MVVM binding '%s'"), *BindingIdStr));
}

FToolResult ClaireonTool_EditWidgetBP::Operation_RemoveMVVMBinding(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP)
	{
		return MakeErrorResult(TEXT("Widget Blueprint is no longer valid"));
	}

	FString BindingIdStr;
	if (!Params->TryGetStringField(TEXT("binding_id"), BindingIdStr) || BindingIdStr.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required param: binding_id"));
	}

	FGuid BindingId;
	if (!FGuid::Parse(BindingIdStr, BindingId))
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid binding_id GUID: '%s'"), *BindingIdStr));
	}

	UMVVMBlueprintView* View = ClaireonWidgetHelpers::GetOrCreateMVVMBlueprintView(WBP);
	if (!View)
	{
		return MakeErrorResult(TEXT("No MVVM Blueprint View exists"));
	}

	FMVVMBlueprintViewBinding* Binding = View->GetBinding(BindingId);
	if (!Binding)
	{
		return MakeErrorResult(FString::Printf(TEXT("Binding with id '%s' not found"), *BindingIdStr));
	}

	FScopedTransaction Transaction(LOCTEXT("MCPRemoveMVVMBinding", "MCP: Remove MVVM Binding"));

	View->RemoveBinding(Binding);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetStringField(TEXT("removed_binding_id"), BindingIdStr);

	return MakeSuccessResult(ResultObj, FString::Printf(TEXT("Removed MVVM binding '%s'"), *BindingIdStr));
}

// ============================================================================
// Animation Operations — Helpers
// ============================================================================

static UWidgetAnimation* FindAnimationByName(UWidgetBlueprint* WBP, const FString& AnimationName)
{
	for (UWidgetAnimation* Anim : WBP->Animations)
	{
		if (Anim && Anim->GetDisplayName().ToString() == AnimationName)
		{
			return Anim;
		}
	}
	return nullptr;
}

static FString GetAnimationNameList(UWidgetBlueprint* WBP)
{
	TArray<FString> Names;
	for (UWidgetAnimation* Anim : WBP->Animations)
	{
		if (Anim)
		{
			Names.Add(Anim->GetDisplayName().ToString());
		}
	}
	return Names.Num() > 0 ? FString::Join(Names, TEXT(", ")) : TEXT("(none)");
}

static void AddFloatKey(FMovieSceneFloatChannel& Channel, FFrameNumber Frame, float Value, const FString& Interpolation)
{
	if (Interpolation == TEXT("linear"))
	{
		Channel.AddLinearKey(Frame, Value);
	}
	else if (Interpolation == TEXT("constant"))
	{
		Channel.AddConstantKey(Frame, Value);
	}
	else
	{
		Channel.AddCubicKey(Frame, Value);
	}
}

static UMovieSceneTrack* FindTrackByPropertyPath(UMovieScene* MovieScene, const FGuid& PossessableGuid, const FString& PropertyPath)
{
	const FMovieSceneBinding* Binding = MovieScene->FindBinding(PossessableGuid);
	if (!Binding)
	{
		return nullptr;
	}

	for (UMovieSceneTrack* Track : Binding->GetTracks())
	{
		if (UMovieScenePropertyTrack* PropTrack = Cast<UMovieScenePropertyTrack>(Track))
		{
			if (PropTrack->GetPropertyName() == FName(*PropertyPath))
			{
				return Track;
			}
		}
	}
	return nullptr;
}

static FGuid FindPossessableGuidForWidget(UWidgetAnimation* Animation, const FString& WidgetName, const FString& Target)
{
	for (const FWidgetAnimationBinding& Binding : Animation->AnimationBindings)
	{
		if (Binding.WidgetName == FName(*WidgetName))
		{
			if (Target == TEXT("slot") && Binding.SlotWidgetName != NAME_None)
			{
				return Binding.AnimationGuid;
			}
			else if (Target == TEXT("widget") && Binding.SlotWidgetName == NAME_None)
			{
				return Binding.AnimationGuid;
			}
		}
	}
	return FGuid();
}

// ============================================================================
// Animation Operations — Lifecycle
// ============================================================================

FToolResult ClaireonTool_EditWidgetBP::Operation_CreateAnimation(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP)
	{
		return MakeErrorResult(TEXT("Widget Blueprint is no longer valid"));
	}

	FString AnimationName;
	if (!Params->TryGetStringField(TEXT("animation_name"), AnimationName))
	{
		return MakeErrorResult(TEXT("Missing required field: animation_name"));
	}

	double Duration = 5.0;
	Params->TryGetNumberField(TEXT("duration"), Duration);
	if (Duration <= 0.0)
	{
		return MakeErrorResult(TEXT("Duration must be positive"));
	}

	int32 DisplayRate = 20;
	{
		double DisplayRateDouble = 20.0;
		if (Params->TryGetNumberField(TEXT("display_rate"), DisplayRateDouble))
		{
			DisplayRate = static_cast<int32>(DisplayRateDouble);
		}
	}
	if (DisplayRate <= 0)
	{
		return MakeErrorResult(TEXT("Display rate must be positive"));
	}

	// Ensure unique name
	FString UniqueName = AnimationName;
	int32 NameIndex = 1;
	while (FindAnimationByName(WBP, UniqueName) != nullptr)
	{
		UniqueName = FString::Printf(TEXT("%s_%d"), *AnimationName, NameIndex);
		NameIndex++;
	}

	const FScopedTransaction Transaction(LOCTEXT("MCPCreateAnimation", "MCP: Create Animation"));
	WBP->Modify();

	UWidgetAnimation* NewAnimation = NewObject<UWidgetAnimation>(WBP, FName(), RF_Transactional);
	NewAnimation->SetDisplayLabel(UniqueName);
	NewAnimation->Rename(*UniqueName);

	const FName NewFName = FName(*UniqueName);
	NewAnimation->MovieScene = NewObject<UMovieScene>(NewAnimation, NewFName, RF_Transactional);
	NewAnimation->MovieScene->SetDisplayRate(FFrameRate(DisplayRate, 1));

	// Convert duration to frames using tick resolution
	const FFrameRate TickResolution = NewAnimation->MovieScene->GetTickResolution();
	const FFrameTime EndFrame = Duration * TickResolution;
	NewAnimation->MovieScene->SetPlaybackRange(TRange<FFrameNumber>(FFrameNumber(0), EndFrame.FrameNumber + 1));
	NewAnimation->MovieScene->GetEditorData().WorkStart = 0.0f;
	NewAnimation->MovieScene->GetEditorData().WorkEnd = static_cast<float>(Duration);

	WBP->Animations.Add(NewAnimation);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	TSharedPtr<FJsonObject> ResultObj = ClaireonWidgetHelpers::SerializeAnimationDetails(NewAnimation);
	ResultObj->SetBoolField(TEXT("success"), true);

	return MakeSuccessResult(ResultObj, TEXT(""));
}

FToolResult ClaireonTool_EditWidgetBP::Operation_DeleteAnimation(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP)
	{
		return MakeErrorResult(TEXT("Widget Blueprint is no longer valid"));
	}

	FString AnimationName;
	if (!Params->TryGetStringField(TEXT("animation_name"), AnimationName))
	{
		return MakeErrorResult(TEXT("Missing required field: animation_name"));
	}

	UWidgetAnimation* Animation = FindAnimationByName(WBP, AnimationName);
	if (!Animation)
	{
		return MakeErrorResult(FString::Printf(TEXT("Animation '%s' not found. Available: %s"), *AnimationName, *GetAnimationNameList(WBP)));
	}

	{
		const FScopedTransaction Transaction(LOCTEXT("MCPDeleteAnimation", "MCP: Delete Animation"));
		WBP->Modify();
		Animation->Rename(nullptr, GetTransientPackage());
		WBP->Animations.Remove(Animation);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	ResultObj->SetStringField(TEXT("deleted_animation"), AnimationName);

	return MakeSuccessResult(ResultObj, TEXT(""));
}

FToolResult ClaireonTool_EditWidgetBP::Operation_RenameAnimation(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP)
	{
		return MakeErrorResult(TEXT("Widget Blueprint is no longer valid"));
	}

	FString AnimationName;
	if (!Params->TryGetStringField(TEXT("animation_name"), AnimationName))
	{
		return MakeErrorResult(TEXT("Missing required field: animation_name"));
	}

	FString NewName;
	if (!Params->TryGetStringField(TEXT("new_name"), NewName))
	{
		return MakeErrorResult(TEXT("Missing required field: new_name"));
	}

	UWidgetAnimation* Animation = FindAnimationByName(WBP, AnimationName);
	if (!Animation)
	{
		return MakeErrorResult(FString::Printf(TEXT("Animation '%s' not found. Available: %s"), *AnimationName, *GetAnimationNameList(WBP)));
	}

	// Check new name is unique
	if (FindAnimationByName(WBP, NewName) != nullptr)
	{
		return MakeErrorResult(FString::Printf(TEXT("An animation named '%s' already exists"), *NewName));
	}

	const FName OldFName = Animation->GetFName();
	const FName NewFName = FName(*NewName);

	{
		const FScopedTransaction Transaction(LOCTEXT("MCPRenameAnimation", "MCP: Rename Animation"));
		Animation->Modify();
		Animation->GetMovieScene()->Modify();

		Animation->SetDisplayLabel(NewName);
		Animation->Rename(*NewName);
		Animation->GetMovieScene()->Rename(*NewName);
	}

	// ReplaceVariableReferences must be called outside the transaction
	FBlueprintEditorUtils::ReplaceVariableReferences(WBP, OldFName, NewFName);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	TSharedPtr<FJsonObject> ResultObj = ClaireonWidgetHelpers::SerializeAnimationDetails(Animation);
	ResultObj->SetBoolField(TEXT("success"), true);

	return MakeSuccessResult(ResultObj, TEXT(""));
}

FToolResult ClaireonTool_EditWidgetBP::Operation_DuplicateAnimation(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP)
	{
		return MakeErrorResult(TEXT("Widget Blueprint is no longer valid"));
	}

	FString AnimationName;
	if (!Params->TryGetStringField(TEXT("animation_name"), AnimationName))
	{
		return MakeErrorResult(TEXT("Missing required field: animation_name"));
	}

	UWidgetAnimation* SourceAnimation = FindAnimationByName(WBP, AnimationName);
	if (!SourceAnimation)
	{
		return MakeErrorResult(FString::Printf(TEXT("Animation '%s' not found. Available: %s"), *AnimationName, *GetAnimationNameList(WBP)));
	}

	FString NewName;
	if (!Params->TryGetStringField(TEXT("new_name"), NewName))
	{
		NewName = AnimationName + TEXT("_Copy");
	}

	// Ensure unique name
	FString UniqueName = NewName;
	int32 NameIndex = 1;
	while (FindAnimationByName(WBP, UniqueName) != nullptr)
	{
		UniqueName = FString::Printf(TEXT("%s_%d"), *NewName, NameIndex);
		NameIndex++;
	}

	const FScopedTransaction Transaction(LOCTEXT("MCPDuplicateAnimation", "MCP: Duplicate Animation"));
	WBP->Modify();

	UWidgetAnimation* NewAnimation = DuplicateObject<UWidgetAnimation>(
		SourceAnimation,
		WBP,
		MakeUniqueObjectName(WBP, UWidgetAnimation::StaticClass(), FName(*UniqueName)));

	NewAnimation->MovieScene->Rename(*UniqueName, nullptr, REN_DontCreateRedirectors);
	NewAnimation->SetDisplayLabel(UniqueName);
	WBP->Animations.Add(NewAnimation);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	TSharedPtr<FJsonObject> ResultObj = ClaireonWidgetHelpers::SerializeAnimationDetails(NewAnimation);
	ResultObj->SetBoolField(TEXT("success"), true);

	return MakeSuccessResult(ResultObj, TEXT(""));
}

FToolResult ClaireonTool_EditWidgetBP::Operation_GetAnimationDetails(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP)
	{
		return MakeErrorResult(TEXT("Widget Blueprint is no longer valid"));
	}

	FString AnimationName;
	if (!Params->TryGetStringField(TEXT("animation_name"), AnimationName))
	{
		return MakeErrorResult(TEXT("Missing required field: animation_name"));
	}

	UWidgetAnimation* Animation = FindAnimationByName(WBP, AnimationName);
	if (!Animation)
	{
		return MakeErrorResult(FString::Printf(TEXT("Animation '%s' not found. Available: %s"), *AnimationName, *GetAnimationNameList(WBP)));
	}

	TSharedPtr<FJsonObject> ResultObj = ClaireonWidgetHelpers::SerializeAnimationDetails(Animation);

	return MakeSuccessResult(ResultObj, TEXT(""));
}

FToolResult ClaireonTool_EditWidgetBP::Operation_AddAnimationBinding(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		return MakeErrorResult(TEXT("Widget Blueprint or WidgetTree is no longer valid"));
	}

	FString AnimationName;
	if (!Params->TryGetStringField(TEXT("animation_name"), AnimationName))
	{
		return MakeErrorResult(TEXT("Missing required field: animation_name"));
	}

	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return MakeErrorResult(TEXT("Missing required field: widget_name"));
	}

	bool bIncludeSlot = false;
	Params->TryGetBoolField(TEXT("include_slot"), bIncludeSlot);

	UWidgetAnimation* Animation = FindAnimationByName(WBP, AnimationName);
	if (!Animation)
	{
		return MakeErrorResult(FString::Printf(TEXT("Animation '%s' not found. Available: %s"), *AnimationName, *GetAnimationNameList(WBP)));
	}

	UMovieScene* MovieScene = Animation->GetMovieScene();
	if (!MovieScene)
	{
		return MakeErrorResult(TEXT("Animation has no MovieScene"));
	}

	// Verify widget exists
	UWidget* Widget = ClaireonWidgetHelpers::FindWidgetByName(WBP->WidgetTree, FName(*WidgetName));
	if (!Widget)
	{
		return MakeErrorResult(FString::Printf(TEXT("Widget '%s' not found in widget tree"), *WidgetName));
	}

	// Check if widget is already bound
	for (const FWidgetAnimationBinding& ExistingBinding : Animation->AnimationBindings)
	{
		if (ExistingBinding.WidgetName == FName(*WidgetName) && ExistingBinding.SlotWidgetName == NAME_None)
		{
			return MakeErrorResult(FString::Printf(TEXT("Widget '%s' is already bound in animation '%s'"), *WidgetName, *AnimationName));
		}
	}

	const FScopedTransaction Transaction(LOCTEXT("MCPAddAnimationBinding", "MCP: Add Animation Binding"));
	WBP->Modify();

	// Add widget possessable
	FGuid WidgetGuid = MovieScene->AddPossessable(WidgetName, UWidget::StaticClass());

	FWidgetAnimationBinding WidgetBinding;
	WidgetBinding.WidgetName = FName(*WidgetName);
	WidgetBinding.AnimationGuid = WidgetGuid;
	Animation->AnimationBindings.Add(WidgetBinding);

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	ResultObj->SetStringField(TEXT("widget_name"), WidgetName);
	ResultObj->SetStringField(TEXT("widget_guid"), WidgetGuid.ToString());

	// Optionally add slot possessable
	if (bIncludeSlot && Widget->Slot)
	{
		FString SlotName = WidgetName + TEXT("_Slot");
		FGuid SlotGuid = MovieScene->AddPossessable(SlotName, UPanelSlot::StaticClass());
		FMovieScenePossessable* SlotPossessable = MovieScene->FindPossessable(SlotGuid);
		if (SlotPossessable)
		{
			SlotPossessable->SetParent(WidgetGuid, MovieScene);
		}

		FWidgetAnimationBinding SlotAnimBinding;
		SlotAnimBinding.WidgetName = FName(*WidgetName);
		SlotAnimBinding.SlotWidgetName = FName(*SlotName);
		SlotAnimBinding.AnimationGuid = SlotGuid;
		Animation->AnimationBindings.Add(SlotAnimBinding);

		ResultObj->SetStringField(TEXT("slot_guid"), SlotGuid.ToString());
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	return MakeSuccessResult(ResultObj, TEXT(""));
}

FToolResult ClaireonTool_EditWidgetBP::Operation_AddAnimationTrack(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		return MakeErrorResult(TEXT("Widget Blueprint or WidgetTree is no longer valid"));
	}

	FString AnimationName;
	if (!Params->TryGetStringField(TEXT("animation_name"), AnimationName))
	{
		return MakeErrorResult(TEXT("Missing required field: animation_name"));
	}

	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return MakeErrorResult(TEXT("Missing required field: widget_name"));
	}

	FString PropertyPath;
	if (!Params->TryGetStringField(TEXT("property_path"), PropertyPath))
	{
		return MakeErrorResult(TEXT("Missing required field: property_path"));
	}

	FString Target = TEXT("widget");
	Params->TryGetStringField(TEXT("target"), Target);

	UWidgetAnimation* Animation = FindAnimationByName(WBP, AnimationName);
	if (!Animation)
	{
		return MakeErrorResult(FString::Printf(TEXT("Animation '%s' not found. Available: %s"), *AnimationName, *GetAnimationNameList(WBP)));
	}

	UMovieScene* MovieScene = Animation->GetMovieScene();
	if (!MovieScene)
	{
		return MakeErrorResult(TEXT("Animation has no MovieScene"));
	}

	FGuid PossessableGuid = FindPossessableGuidForWidget(Animation, WidgetName, Target);
	if (!PossessableGuid.IsValid())
	{
		return MakeErrorResult(FString::Printf(TEXT("Widget '%s' (target: %s) is not bound in animation '%s'. Call add_animation_binding first."), *WidgetName, *Target, *AnimationName));
	}

	// Map property path to track class
	UClass* TrackClass = nullptr;
	if (PropertyPath == TEXT("RenderOpacity"))
	{
		TrackClass = UMovieSceneFloatTrack::StaticClass();
	}
	else if (PropertyPath == TEXT("ColorAndOpacity"))
	{
		TrackClass = UMovieSceneColorTrack::StaticClass();
	}
	else if (PropertyPath == TEXT("bIsEnabled"))
	{
		TrackClass = UMovieSceneBoolTrack::StaticClass();
	}
	else
	{
		return MakeErrorResult(FString::Printf(TEXT("Unsupported property_path: '%s'. Supported: RenderOpacity (float), ColorAndOpacity (color), bIsEnabled (bool)"), *PropertyPath));
	}

	const FScopedTransaction Transaction(LOCTEXT("MCPAddAnimationTrack", "MCP: Add Animation Track"));
	MovieScene->Modify();

	UMovieSceneTrack* Track = MovieScene->AddTrack(TrackClass, PossessableGuid);
	if (!Track)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to create track for property '%s'"), *PropertyPath));
	}

	// Set property binding
	if (UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(Track))
	{
		PropertyTrack->SetPropertyNameAndPath(FName(*PropertyPath), PropertyPath);
	}

	// Create section spanning the full playback range
	UMovieSceneSection* Section = Track->CreateNewSection();
	if (Section)
	{
		Section->SetRange(MovieScene->GetPlaybackRange());
		Track->AddSection(*Section);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	ResultObj->SetStringField(TEXT("animation_name"), AnimationName);
	ResultObj->SetStringField(TEXT("widget_name"), WidgetName);
	ResultObj->SetStringField(TEXT("property_path"), PropertyPath);
	ResultObj->SetStringField(TEXT("track_type"), Track->GetClass()->GetName());

	return MakeSuccessResult(ResultObj, TEXT(""));
}

FToolResult ClaireonTool_EditWidgetBP::Operation_AddAnimationKeyframe(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP)
	{
		return MakeErrorResult(TEXT("Widget Blueprint is no longer valid"));
	}

	FString AnimationName, WidgetName, PropertyPath;
	if (!Params->TryGetStringField(TEXT("animation_name"), AnimationName))
	{
		return MakeErrorResult(TEXT("Missing required field: animation_name"));
	}
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return MakeErrorResult(TEXT("Missing required field: widget_name"));
	}
	if (!Params->TryGetStringField(TEXT("property_path"), PropertyPath))
	{
		return MakeErrorResult(TEXT("Missing required field: property_path"));
	}

	double Time = 0.0;
	if (!Params->TryGetNumberField(TEXT("time"), Time))
	{
		return MakeErrorResult(TEXT("Missing required field: time"));
	}

	FString Interpolation = TEXT("cubic");
	Params->TryGetStringField(TEXT("interpolation"), Interpolation);

	FString Target = TEXT("widget");
	Params->TryGetStringField(TEXT("target"), Target);

	UWidgetAnimation* Animation = FindAnimationByName(WBP, AnimationName);
	if (!Animation)
	{
		return MakeErrorResult(FString::Printf(TEXT("Animation '%s' not found. Available: %s"), *AnimationName, *GetAnimationNameList(WBP)));
	}

	UMovieScene* MovieScene = Animation->GetMovieScene();
	if (!MovieScene)
	{
		return MakeErrorResult(TEXT("Animation has no MovieScene"));
	}

	FGuid PossessableGuid = FindPossessableGuidForWidget(Animation, WidgetName, Target);
	if (!PossessableGuid.IsValid())
	{
		return MakeErrorResult(FString::Printf(TEXT("Widget '%s' (target: %s) is not bound in animation '%s'"), *WidgetName, *Target, *AnimationName));
	}

	UMovieSceneTrack* Track = FindTrackByPropertyPath(MovieScene, PossessableGuid, PropertyPath);
	if (!Track)
	{
		return MakeErrorResult(FString::Printf(TEXT("No track for property '%s' on widget '%s'. Call add_animation_track first."), *PropertyPath, *WidgetName));
	}

	const TArray<UMovieSceneSection*>& Sections = Track->GetAllSections();
	if (Sections.Num() == 0)
	{
		return MakeErrorResult(TEXT("Track has no sections"));
	}
	UMovieSceneSection* Section = Sections[0];

	const FFrameRate TickResolution = MovieScene->GetTickResolution();
	FFrameNumber FrameNumber = (Time * TickResolution).FloorToFrame();

	const FScopedTransaction Transaction(LOCTEXT("MCPAddAnimationKeyframe", "MCP: Add Animation Keyframe"));
	Section->Modify();

	// Dispatch by track type
	if (UMovieSceneFloatSection* FloatSection = Cast<UMovieSceneFloatSection>(Section))
	{
		double FloatValue = 0.0;
		if (!Params->TryGetNumberField(TEXT("value"), FloatValue))
		{
			return MakeErrorResult(TEXT("Missing required field: value (number expected for float track)"));
		}
		AddFloatKey(FloatSection->GetChannel(), FrameNumber, static_cast<float>(FloatValue), Interpolation);
	}
	else if (UMovieSceneColorSection* ColorSection = Cast<UMovieSceneColorSection>(Section))
	{
		const TSharedPtr<FJsonObject>* ColorObjPtr = nullptr;
		if (!Params->TryGetObjectField(TEXT("value"), ColorObjPtr) || !ColorObjPtr)
		{
			return MakeErrorResult(TEXT("Missing required field: value (object with r,g,b,a expected for color track)"));
		}
		const TSharedPtr<FJsonObject>& ColorObj = *ColorObjPtr;

		double R = 1.0, G = 1.0, B = 1.0, A = 1.0;
		ColorObj->TryGetNumberField(TEXT("r"), R);
		ColorObj->TryGetNumberField(TEXT("g"), G);
		ColorObj->TryGetNumberField(TEXT("b"), B);
		ColorObj->TryGetNumberField(TEXT("a"), A);

		AddFloatKey(ColorSection->GetRedChannel(), FrameNumber, static_cast<float>(R), Interpolation);
		AddFloatKey(ColorSection->GetGreenChannel(), FrameNumber, static_cast<float>(G), Interpolation);
		AddFloatKey(ColorSection->GetBlueChannel(), FrameNumber, static_cast<float>(B), Interpolation);
		AddFloatKey(ColorSection->GetAlphaChannel(), FrameNumber, static_cast<float>(A), Interpolation);
	}
	else if (UMovieSceneBoolSection* BoolSection = Cast<UMovieSceneBoolSection>(Section))
	{
		bool bValue = false;
		if (!Params->TryGetBoolField(TEXT("value"), bValue))
		{
			return MakeErrorResult(TEXT("Missing required field: value (boolean expected for bool track)"));
		}
		BoolSection->GetChannel().GetData().UpdateOrAddKey(FrameNumber, bValue);
	}
	else
	{
		return MakeErrorResult(TEXT("Unsupported section type for keyframe insertion"));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	ResultObj->SetStringField(TEXT("animation_name"), AnimationName);
	ResultObj->SetStringField(TEXT("property_path"), PropertyPath);
	ResultObj->SetNumberField(TEXT("time"), Time);

	return MakeSuccessResult(ResultObj, TEXT(""));
}

FToolResult ClaireonTool_EditWidgetBP::Operation_RemoveAnimationKeyframe(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP)
	{
		return MakeErrorResult(TEXT("Widget Blueprint is no longer valid"));
	}

	FString AnimationName, WidgetName, PropertyPath;
	if (!Params->TryGetStringField(TEXT("animation_name"), AnimationName))
	{
		return MakeErrorResult(TEXT("Missing required field: animation_name"));
	}
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return MakeErrorResult(TEXT("Missing required field: widget_name"));
	}
	if (!Params->TryGetStringField(TEXT("property_path"), PropertyPath))
	{
		return MakeErrorResult(TEXT("Missing required field: property_path"));
	}

	double Time = 0.0;
	if (!Params->TryGetNumberField(TEXT("time"), Time))
	{
		return MakeErrorResult(TEXT("Missing required field: time"));
	}

	FString Target = TEXT("widget");
	Params->TryGetStringField(TEXT("target"), Target);

	UWidgetAnimation* Animation = FindAnimationByName(WBP, AnimationName);
	if (!Animation)
	{
		return MakeErrorResult(FString::Printf(TEXT("Animation '%s' not found"), *AnimationName));
	}

	UMovieScene* MovieScene = Animation->GetMovieScene();
	if (!MovieScene)
	{
		return MakeErrorResult(TEXT("Animation has no MovieScene"));
	}

	FGuid PossessableGuid = FindPossessableGuidForWidget(Animation, WidgetName, Target);
	if (!PossessableGuid.IsValid())
	{
		return MakeErrorResult(FString::Printf(TEXT("Widget '%s' is not bound in animation '%s'"), *WidgetName, *AnimationName));
	}

	UMovieSceneTrack* Track = FindTrackByPropertyPath(MovieScene, PossessableGuid, PropertyPath);
	if (!Track)
	{
		return MakeErrorResult(FString::Printf(TEXT("No track for property '%s' on widget '%s'"), *PropertyPath, *WidgetName));
	}

	const TArray<UMovieSceneSection*>& Sections = Track->GetAllSections();
	if (Sections.Num() == 0)
	{
		return MakeErrorResult(TEXT("Track has no sections"));
	}
	UMovieSceneSection* Section = Sections[0];

	const FFrameRate TickResolution = MovieScene->GetTickResolution();
	FFrameNumber FrameNumber = (Time * TickResolution).FloorToFrame();

	const FScopedTransaction Transaction(LOCTEXT("MCPRemoveAnimationKeyframe", "MCP: Remove Animation Keyframe"));
	Section->Modify();

	int32 RemovedCount = 0;

	// Helper lambda to remove key at frame from a float channel
	auto RemoveFloatKeyAtFrame = [&](FMovieSceneFloatChannel& Channel)
	{
		TArrayView<const FFrameNumber> Times = Channel.GetData().GetTimes();
		for (int32 i = Times.Num() - 1; i >= 0; --i)
		{
			if (Times[i] == FrameNumber)
			{
				Channel.GetData().RemoveKey(i);
				RemovedCount++;
			}
		}
	};

	if (UMovieSceneFloatSection* FloatSection = Cast<UMovieSceneFloatSection>(Section))
	{
		RemoveFloatKeyAtFrame(FloatSection->GetChannel());
	}
	else if (UMovieSceneColorSection* ColorSection = Cast<UMovieSceneColorSection>(Section))
	{
		RemoveFloatKeyAtFrame(ColorSection->GetRedChannel());
		RemoveFloatKeyAtFrame(ColorSection->GetGreenChannel());
		RemoveFloatKeyAtFrame(ColorSection->GetBlueChannel());
		RemoveFloatKeyAtFrame(ColorSection->GetAlphaChannel());
	}
	else if (UMovieSceneBoolSection* BoolSection = Cast<UMovieSceneBoolSection>(Section))
	{
		TArrayView<const FFrameNumber> Times = BoolSection->GetChannel().GetData().GetTimes();
		for (int32 i = Times.Num() - 1; i >= 0; --i)
		{
			if (Times[i] == FrameNumber)
			{
				BoolSection->GetChannel().GetData().RemoveKey(i);
				RemovedCount++;
			}
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), RemovedCount > 0);
	ResultObj->SetNumberField(TEXT("removed_count"), RemovedCount);
	ResultObj->SetStringField(TEXT("animation_name"), AnimationName);
	ResultObj->SetStringField(TEXT("property_path"), PropertyPath);
	ResultObj->SetNumberField(TEXT("time"), Time);

	return MakeSuccessResult(ResultObj, TEXT(""));
}

FToolResult ClaireonTool_EditWidgetBP::Operation_SetAnimationProperty(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP)
	{
		return MakeErrorResult(TEXT("Widget Blueprint is no longer valid"));
	}

	FString AnimationName;
	if (!Params->TryGetStringField(TEXT("animation_name"), AnimationName))
	{
		return MakeErrorResult(TEXT("Missing required field: animation_name"));
	}

	UWidgetAnimation* Animation = FindAnimationByName(WBP, AnimationName);
	if (!Animation)
	{
		return MakeErrorResult(FString::Printf(TEXT("Animation '%s' not found. Available: %s"), *AnimationName, *GetAnimationNameList(WBP)));
	}

	UMovieScene* MovieScene = Animation->GetMovieScene();
	if (!MovieScene)
	{
		return MakeErrorResult(TEXT("Animation has no MovieScene"));
	}

	double Duration = -1.0;
	double StartTime = -1.0;
	double DisplayRateDouble = -1.0;
	bool bHasDuration = Params->TryGetNumberField(TEXT("duration"), Duration);
	bool bHasStartTime = Params->TryGetNumberField(TEXT("start_time"), StartTime);
	bool bHasDisplayRate = Params->TryGetNumberField(TEXT("display_rate"), DisplayRateDouble);

	if (!bHasDuration && !bHasStartTime && !bHasDisplayRate)
	{
		return MakeErrorResult(TEXT("At least one property must be specified: duration, start_time, display_rate"));
	}

	const FScopedTransaction Transaction(LOCTEXT("MCPSetAnimationProperty", "MCP: Set Animation Property"));
	MovieScene->Modify();

	if (bHasDisplayRate)
	{
		int32 NewRate = static_cast<int32>(DisplayRateDouble);
		if (NewRate <= 0)
		{
			return MakeErrorResult(TEXT("Display rate must be positive"));
		}
		MovieScene->SetDisplayRate(FFrameRate(NewRate, 1));
	}

	if (bHasDuration || bHasStartTime)
	{
		const FFrameRate TickResolution = MovieScene->GetTickResolution();
		TRange<FFrameNumber> CurrentRange = MovieScene->GetPlaybackRange();

		double CurrentStart = 0.0;
		if (CurrentRange.HasLowerBound())
		{
			CurrentStart = static_cast<double>(CurrentRange.GetLowerBoundValue().Value) / TickResolution.AsDecimal();
		}

		double CurrentEnd = 0.0;
		if (CurrentRange.HasUpperBound())
		{
			CurrentEnd = static_cast<double>(CurrentRange.GetUpperBoundValue().Value) / TickResolution.AsDecimal();
		}

		double NewStart = bHasStartTime ? StartTime : CurrentStart;
		double NewEnd = bHasDuration ? (NewStart + Duration) : CurrentEnd;

		if (NewEnd <= NewStart)
		{
			return MakeErrorResult(TEXT("End time must be greater than start time"));
		}

		const FFrameTime StartFrame = NewStart * TickResolution;
		const FFrameTime EndFrame = NewEnd * TickResolution;
		MovieScene->SetPlaybackRange(TRange<FFrameNumber>(StartFrame.FrameNumber, EndFrame.FrameNumber + 1));
		MovieScene->GetEditorData().WorkStart = static_cast<float>(NewStart);
		MovieScene->GetEditorData().WorkEnd = static_cast<float>(NewEnd);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	TSharedPtr<FJsonObject> ResultObj = ClaireonWidgetHelpers::SerializeAnimationDetails(Animation);
	ResultObj->SetBoolField(TEXT("success"), true);

	return MakeSuccessResult(ResultObj, TEXT(""));
}

#undef LOCTEXT_NAMESPACE
