// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_Create.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonBlueprintHelpers.h"
#include "Dom/JsonObject.h"
#include "Tools/ClaireonSpecApplicator_Blueprint.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase_Internal.h"
#include "ClaireonLog.h"
#include "ClaireonSafeExec.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_CallDataTableFunction.h"
#include "K2Node_CallMaterialParameterCollectionFunction.h"
#include "K2Node_CommutativeAssociativeBinaryOperator.h"
#include "K2Node_Event.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_Timeline.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_Select.h"
#include "K2Node_MacroInstance.h"
#include "Engine/MemberReference.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_SpawnActorFromClass.h"
#include "K2Node_Knot.h"
#include "EdGraphNode_Comment.h"
#include "K2Node_Literal.h"
#include "K2Node_MakeArray.h"
#include "K2Node_MakeMap.h"
#include "K2Node_MakeSet.h"
#include "K2Node_GetArrayItem.h"
#include "K2Node_AddPinInterface.h"
#include "K2Node_Switch.h"
#include "K2Node_SwitchInteger.h"
#include "K2Node_SwitchString.h"
#include "K2Node_SwitchName.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_ForEachElementInEnum.h"
#include "K2Node_DoOnceMultiInput.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_RemoveDelegate.h"
#include "K2Node_ClearDelegate.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_AssignDelegate.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Engine/TimelineTemplate.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveVector.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraphUtilities.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ScopedTransaction.h"
#include "Animation/AnimBlueprint.h"
#include "AnimationGraph.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_Root.h"
#include "K2Node_Tunnel.h"
#include "ClaireonBlueprintNodeSerializer.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"
#include "ClaireonNameResolver.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "ClaireonBPInterfaceAuthor.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

#define LOCTEXT_NAMESPACE "ClaireonBlueprintGraphEditToolBase"

using FToolResult = IClaireonTool::FToolResult;


FString ClaireonBlueprintGraphTool_Create::GetOperation() const { return TEXT("create"); }

FString ClaireonBlueprintGraphTool_Create::GetDescription() const
{
    return TEXT("Create a new blueprint asset at asset_path and open a session for editing. Returns session_id; pair with bp_close to release the session, or let it expire on idle timeout. Auto-opens a session as a side effect of creation. Common pitfall: parent_class must be a Blueprintable native class; the package directory portion of asset_path must already exist. Accepts either session_id or asset_path; auto-opens a session when asset_path is supplied.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_Create::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("asset_path"), TEXT("New Blueprint asset path."), true);
    Builder.AddString(TEXT("parent_class"), TEXT("Parent class path (e.g. /Script/Engine.Actor)."), true);
    Builder.AddString(TEXT("blueprint_type"), TEXT("Optional: 'Normal', 'MacroLibrary', 'Interface', 'LevelScript' (default 'Normal')."));
    Builder.AddNumber(TEXT("timeout_minutes"), TEXT("Session timeout in minutes (default 60)."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_Create::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    // create is stateless; it opens its own session internally.
    TSharedPtr<FJsonObject> Params = Arguments.IsValid() ? Arguments : MakeShared<FJsonObject>();
    if (Params->HasField(TEXT("params")))
    {
        const TSharedPtr<FJsonObject>* NestedObj = nullptr;
        if (Params->TryGetObjectField(TEXT("params"), NestedObj) && NestedObj && NestedObj->IsValid())
        {
            Params = *NestedObj;
        }
    }
	// Get asset_path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return MakeErrorResult(TEXT("Missing required field: asset_path"));
	}

	// Validate asset path
	FString ValidationError;
	if (!ClaireonBlueprintHelpers::ValidateAssetPath(AssetPath, ValidationError))
	{
		return MakeErrorResult(ValidationError);
	}

	// Get parent_class (optional, defaults to Actor)
	FString ParentClassName;
	if (!Params->TryGetStringField(TEXT("parent_class"), ParentClassName))
	{
		ParentClassName = TEXT("Actor");
	}

	// Find parent class
	ClaireonNameResolver::FNameResolveResult ParentClassResult;
	UClass* ParentClass = ClaireonNameResolver::ResolveClassName(ParentClassName, nullptr, ParentClassResult);
	if (!ParentClass)
	{
		return MakeErrorResult(ParentClassResult.Error);
	}
	TArray<FString> ResolutionWarnings;
	if (!ParentClassResult.ResolutionNote.IsEmpty())
	{
		ResolutionWarnings.Add(ParentClassResult.ResolutionNote);
	}

	// Extract package and asset name from path
	FString PackageName = AssetPath;
	FString AssetName;
	if (AssetPath.Contains(TEXT(".")))
	{
		AssetPath.Split(TEXT("."), &PackageName, &AssetName);
	}
	else
	{
		// Asset name from last path component
		int32 LastSlash;
		if (PackageName.FindLastChar('/', LastSlash))
		{
			AssetName = PackageName.Mid(LastSlash + 1);
		}
		else
		{
			AssetName = TEXT("NewBlueprint");
		}
	}

	// Check if package already exists on disk (e.g., from previous test run)
	// If it does, we must delete it first to avoid "partially loaded" errors
	FString PackageFileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	if (FPaths::FileExists(PackageFileName))
	{
		UE_LOG(LogClaireon, Warning, TEXT("[EditBlueprintGraph] Create: Deleting existing file %s"), *PackageFileName);
		IFileManager::Get().Delete(*PackageFileName, false, true);
	}

	// Create package
	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to create package: %s"), *PackageName));
	}

	// Create Blueprint with proper flags matching editor workflow
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		ParentClass,
		Package,
		FName(*AssetName),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(),
		NAME_None);

	if (!Blueprint)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to create Blueprint at %s"), *AssetPath));
	}

	// Configure package to match editor workflow (see UBlueprintFactory and FAssetToolsImpl::CreateAsset)
	Package->SetIsExternallyReferenceable(true); // Mark as externally referenceable asset
	Package->MarkPackageDirty();

	// Notify asset registry
	FAssetRegistryModule::AssetCreated(Blueprint);

	// Get EventGraph (created by default)
	UEdGraph* EventGraph = nullptr;
	if (Blueprint->UbergraphPages.Num() > 0)
	{
		EventGraph = Blueprint->UbergraphPages[0];
	}

	if (!EventGraph)
	{
		return MakeErrorResult(TEXT("Failed to find EventGraph in newly created Blueprint"));
	}

	// Register delegate on first use
	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonBlueprintGraphEditToolBase::HandleSessionClosed);
		bDelegateRegistered = true;
	}

	// Open session via the manager (handles locking)
	double TimeoutMinutes = 60.0;
	Params->TryGetNumberField(TEXT("timeout_minutes"), TimeoutMinutes);
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(Blueprint->GetPathName(), TEXT("bp"), TimeoutMinutes);

	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		const FTimespan Elapsed = FDateTime::UtcNow() - Blocker.LastAccessTime;
		return MakeErrorResult(FString::Printf(
			TEXT("Asset is locked by %s session %s (last activity %dm %ds ago). Close that session first, or use mcp_release_sessions(asset_path='%s') to force-release it."),
			*Blocker.ToolName, *Blocker.SessionId,
			static_cast<int32>(Elapsed.GetTotalMinutes()),
			static_cast<int32>(Elapsed.GetTotalSeconds()) % 60,
			*Blueprint->GetPathName()));
	}

	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid asset path for created Blueprint: %s"), *Blueprint->GetPathName()));
	}

	const FString& SessionId = OpenResult.SessionId;

	// Create tool-specific data
	FBlueprintEditToolData NewData;
	NewData.Blueprint = Blueprint;
	NewData.Graph = EventGraph;
	NewData.Cursor.GraphName = EventGraph->GetName();
	NewData.Cursor.ViewportCenter = FVector2D(0.0f, 0.0f);

	// Find first event node to focus cursor
	TArray<UEdGraphNode*> RootNodes = ClaireonBlueprintHelpers::FindRootNodes(EventGraph);
	if (RootNodes.Num() > 0)
	{
		UEdGraphNode* FirstNode = RootNodes[0];
		NewData.Cursor.FocusedNodeGuid = FirstNode->NodeGuid;
		UEdGraphPin* FirstOutput = ClaireonBlueprintHelpers::GetFirstOutputPin(FirstNode);
		if (FirstOutput)
		{
			NewData.Cursor.FocusedPinName = FirstOutput->PinName;
			NewData.Cursor.FocusedPinDirection = FirstOutput->Direction;
		}
	}

	ToolData.Add(SessionId, MoveTemp(NewData));
	FBlueprintEditToolData* Data = ToolData.Find(SessionId);

	UE_LOG(LogClaireon, Log, TEXT("[EditBlueprintGraph] Created session %s for new Blueprint %s"), *SessionId, *Blueprint->GetPathName());

	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Created new Blueprint %s with parent class %s"), *AssetPath, *ParentClassName);

	// "create" always returns the full state regardless of response_mode so the
	// caller can parse the newly-minted Session ID out of the response (mirrors
	// Operation_Open behavior).
	Data->ResponseMode = TEXT("full");
	Data->bSuppressOutput = false;

	FToolResult CreateResult = BuildStateResponse(SessionId, Data);
	CreateResult.Warnings.Append(ResolutionWarnings);
	return CreateResult;
}

#undef LOCTEXT_NAMESPACE
