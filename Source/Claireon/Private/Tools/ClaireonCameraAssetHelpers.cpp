// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonCameraAssetHelpers.h"

#include "Misc/EngineVersionComparison.h"

// Entire helper is camera-only; compiles to nothing when GameplayCameras is absent.
#if WITH_GAMEPLAY_CAMERAS

#include "Core/CameraAsset.h"
#if UE_VERSION_OLDER_THAN(5, 6, 0)
#include "Core/CameraBuildLog.h"
#else
#include "Build/CameraBuildLog.h"
#endif
#if !UE_VERSION_OLDER_THAN(5, 7, 0)
#include "Core/CameraDirector.h"
#endif
#include "Core/CameraNode.h"
#include "Core/CameraRigAsset.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Logging/TokenizedMessage.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Tools/ClaireonPropertyUtils.h"
#include "UObject/Class.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"

namespace ClaireonCameraAssetHelpers
{
	TArray<UCameraRigAsset*> GetCameraRigs(const UCameraAsset* Asset)
	{
		TArray<UCameraRigAsset*> Out;
		if (!Asset)
		{
			return Out;
		}
#if UE_VERSION_OLDER_THAN(5, 6, 0)
		// <=5.5: rigs are owned directly by the asset.
		for (const TObjectPtr<UCameraRigAsset>& Rig : Asset->GetCameraRigs())
		{
			Out.Add(Rig);
		}
#elif UE_VERSION_OLDER_THAN(5, 7, 0)
		// 5.6: GetCameraRigs() was removed, but the rigs still live in the deprecated
		// backing array; reach them by reflecting on CameraRigs_DEPRECATED.
		if (const FArrayProperty* Prop = FindFProperty<FArrayProperty>(UCameraAsset::StaticClass(), TEXT("CameraRigs_DEPRECATED")))
		{
			FScriptArrayHelper Helper(Prop, Prop->ContainerPtrToValuePtr<void>(Asset));
			for (int32 i = 0; i < Helper.Num(); ++i)
			{
				Out.Add(*reinterpret_cast<TObjectPtr<UCameraRigAsset>*>(Helper.GetRawPtr(i)));
			}
		}
#else
		// 5.7+: rigs are owned by the camera director.
		if (const UCameraDirector* Dir = Asset->GetCameraDirector())
		{
			FCameraDirectorRigUsageInfo Usage;
			Dir->GatherRigUsageInfo(Usage);
			Out = Usage.CameraRigs;
		}
#endif
		return Out;
	}

	UCameraNode* ResolveNode(UCameraRigAsset* Rig, const FString& NodeId, FString& OutError)
	{
		if (!Rig)
		{
			OutError = TEXT("Null rig");
			return nullptr;
		}
		if (NodeId.IsEmpty())
		{
			OutError = TEXT("Empty NodeId");
			return nullptr;
		}

		// Translate "Root" / "Root.Children[N]" / etc. into a property path on the rig.
		// "Root" is the rig's RootNode; the rest of the segments map 1:1.
		FString Translated = NodeId;
		if (Translated.Equals(TEXT("Root")) || Translated.StartsWith(TEXT("Root.")))
		{
			Translated = TEXT("RootNode") + Translated.Mid(4);
		}

		void* Container = nullptr;
		FProperty* Prop = ClaireonPropertyUtils::ResolvePropertyByPath(Rig, Translated, Container, OutError);
		if (!Prop)
		{
			return nullptr;
		}

		// ResolvePropertyByPath returns the *container* the leaf FProperty lives on.
		// To read the actual value we must apply ContainerPtrToValuePtr<>. Passing
		// the bare container into GetObjectPropertyValue reinterprets the rig's
		// memory as a UObject* and crashes with a TArray-OOB assertion when the
		// engine tries to operate on the resulting garbage pointer.
		FProperty* LeafProp = Prop;
		FObjectProperty* ObjProp = nullptr;
		void* ValuePtr = nullptr;

		// For "Root.Children[N]", the resolver descends into the array and the
		// returned LeafProp is the FArrayProperty's Inner (an FObjectProperty),
		// with Container being the element address. For a singular FObjectProperty
		// like RootNode, Container is the parent UObject and we need to apply
		// ContainerPtrToValuePtr.
		if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(LeafProp))
		{
			ObjProp = CastField<FObjectProperty>(ArrayProp->Inner);
			ValuePtr = Container;
		}
		else
		{
			ObjProp = CastField<FObjectProperty>(LeafProp);
			ValuePtr = ObjProp ? ObjProp->ContainerPtrToValuePtr<void>(Container) : nullptr;
		}

		if (!ObjProp || !ValuePtr)
		{
			OutError = FString::Printf(TEXT("'%s' does not resolve to a UObject property"), *NodeId);
			return nullptr;
		}

		UObject* Obj = ObjProp->GetObjectPropertyValue(ValuePtr);
		UCameraNode* AsNode = Cast<UCameraNode>(Obj);
		if (!AsNode)
		{
			OutError = FString::Printf(TEXT("'%s' resolves to %s, not UCameraNode"),
				*NodeId,
				Obj ? *Obj->GetClass()->GetName() : TEXT("nullptr"));
			return nullptr;
		}
		return AsNode;
	}

	UClass* ResolveNodeClass(const FString& Name)
	{
		UClass* Cls = UClass::TryFindTypeSlow<UClass>(Name);
		if (!Cls)
		{
			Cls = UClass::TryFindTypeSlow<UClass>(TEXT("U") + Name);
		}
		if (Cls && !Cls->IsChildOf(UCameraNode::StaticClass()))
		{
			return nullptr;
		}
		return Cls;
	}

	TArray<UClass*> EnumerateCameraNodeClasses()
	{
		TArray<UClass*> Out;
		GetDerivedClasses(UCameraNode::StaticClass(), Out, /*bRecursive=*/true);
		return Out.FilterByPredicate(
			[](UClass* C)
		{
			return !C->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated);
		});
	}

	TSharedPtr<FJsonObject> BuildLogToJson(const UE::Cameras::FCameraBuildLog& Log)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Messages;

		for (const UE::Cameras::FCameraBuildLogMessage& Msg : Log.GetMessages())
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();

			FString SeverityStr;
			switch (Msg.Severity)
			{
				case EMessageSeverity::Error:
					SeverityStr = TEXT("Error");
					break;
				case EMessageSeverity::PerformanceWarning:
					SeverityStr = TEXT("PerformanceWarning");
					break;
				case EMessageSeverity::Warning:
					SeverityStr = TEXT("Warning");
					break;
				case EMessageSeverity::Info:
					SeverityStr = TEXT("Info");
					break;
				default:
					SeverityStr = TEXT("Info");
					break;
			}
			Entry->SetStringField(TEXT("severity"), SeverityStr);
			Entry->SetStringField(TEXT("text"), Msg.Text.ToString());
			Entry->SetStringField(TEXT("object"), Msg.Object ? Msg.Object->GetPathName() : FString());

			Messages.Add(MakeShared<FJsonValueObject>(Entry));
		}

		Root->SetArrayField(TEXT("messages"), Messages);
		return Root;
	}

	void CloseEditorToolkitForAsset(UCameraAsset* Asset)
	{
		if (!Asset || !GEditor)
		{
			return;
		}
		if (UAssetEditorSubsystem* Sys = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			Sys->CloseAllEditorsForAsset(Asset);
		}
	}

	int32 ParseTrailingIndex(const FString& NodeId)
	{
		if (NodeId.IsEmpty() || !NodeId.EndsWith(TEXT("]")))
		{
			return -1;
		}
		int32 OpenBracket = INDEX_NONE;
		if (!NodeId.FindLastChar(TEXT('['), OpenBracket))
		{
			return -1;
		}
		const FString Inner = NodeId.Mid(OpenBracket + 1, NodeId.Len() - OpenBracket - 2);
		if (Inner.IsEmpty() || !Inner.IsNumeric())
		{
			return -1;
		}
		return FCString::Atoi(*Inner);
	}

	FString GetParentPath(const FString& NodeId)
	{
		if (NodeId.IsEmpty())
		{
			return FString();
		}
		// "Root" has no parent path (the rig is not a node).
		int32 LastDot = INDEX_NONE;
		if (!NodeId.FindLastChar(TEXT('.'), LastDot))
		{
			return FString();
		}
		return NodeId.Left(LastDot);
	}

	FString ComputeNodeIdForChildOf(const FString& ParentId, int32 ChildIndex)
	{
		return FString::Printf(TEXT("%s.Children[%d]"), *ParentId, ChildIndex);
	}
} // namespace ClaireonCameraAssetHelpers

#endif // WITH_GAMEPLAY_CAMERAS
