// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSkeletonTools_Sockets.h"
#include "Animation/Skeleton.h"
#include "ReferenceSkeleton.h"
#include "Engine/SkeletalMeshSocket.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"

namespace
{
	TSharedPtr<FJsonObject> BuildSocketSnapshot(const USkeleton* Skeleton, const FString& LastOperation)
	{
		TSharedPtr<FJsonObject> Out = ClaireonSkeletonHelpers::BuildSockets(Skeleton);
		Out->SetStringField(TEXT("skeleton_path"), Skeleton->GetPathName());
		Out->SetStringField(TEXT("last_operation"), LastOperation);
		return Out;
	}
}

// ============================================================================
// claireon.skeleton_add_socket
// ============================================================================

FString ClaireonSkeletonTool_AddSocket::GetName() const { return TEXT("claireon.skeleton_add_socket"); }

FString ClaireonSkeletonTool_AddSocket::GetDescription() const
{
	return TEXT("Add a socket to the skeleton, attached to a specific bone. "
				"Transform fields are optional {x,y,z} / {pitch,yaw,roll} objects; defaults are zero location/rotation and unit scale.");
}

TSharedPtr<FJsonObject> ClaireonSkeletonTool_AddSocket::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("skeleton_path"), TEXT("Unreal asset path to the USkeleton"), true);
	S.AddString(TEXT("socket_name"), TEXT("Unique socket name"), true);
	S.AddString(TEXT("bone_name"), TEXT("Parent bone the socket attaches to (must exist on the skeleton)"), true);
	S.AddObject(TEXT("relative_location"), TEXT("Optional {x,y,z} offset from the bone (default 0,0,0)"));
	S.AddObject(TEXT("relative_rotation"), TEXT("Optional {pitch,yaw,roll} rotation (default 0,0,0)"));
	S.AddObject(TEXT("relative_scale"), TEXT("Optional {x,y,z} scale (default 1,1,1)"));
	S.AddBoolean(TEXT("force_always_animated"), TEXT("Keep this bone evaluated even if LOD-culled (default false)"));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonSkeletonTool_AddSocket::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SkeletonPath; Arguments->TryGetStringField(TEXT("skeleton_path"), SkeletonPath);
	FString LoadError;
	USkeleton* Skeleton = ClaireonSkeletonHelpers::LoadSkeleton(SkeletonPath, LoadError);
	if (!Skeleton) return MakeErrorResult(LoadError);

	FString SocketNameStr, BoneNameStr;
	if (!Arguments->TryGetStringField(TEXT("socket_name"), SocketNameStr) || SocketNameStr.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: socket_name"));
	if (!Arguments->TryGetStringField(TEXT("bone_name"), BoneNameStr) || BoneNameStr.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: bone_name"));

	const FName SocketName(*SocketNameStr);
	const FName BoneName(*BoneNameStr);

	if (Skeleton->FindSocket(SocketName) != nullptr)
	{
		return MakeErrorResult(FString::Printf(TEXT("Socket '%s' already exists"), *SocketNameStr));
	}
	if (Skeleton->GetReferenceSkeleton().FindBoneIndex(BoneName) == INDEX_NONE)
	{
		return MakeErrorResult(FString::Printf(TEXT("Bone '%s' not found on skeleton"), *BoneNameStr));
	}

	FVector Loc = FVector::ZeroVector;
	FRotator Rot = FRotator::ZeroRotator;
	FVector Scale = FVector(1.0f, 1.0f, 1.0f);

	const TSharedPtr<FJsonObject>* LocObj = nullptr;
	if (Arguments->TryGetObjectField(TEXT("relative_location"), LocObj) && LocObj)
	{
		ClaireonSkeletonHelpers::ReadVectorFromJson(*LocObj, FVector::ZeroVector, Loc);
	}
	const TSharedPtr<FJsonObject>* RotObj = nullptr;
	if (Arguments->TryGetObjectField(TEXT("relative_rotation"), RotObj) && RotObj)
	{
		ClaireonSkeletonHelpers::ReadRotatorFromJson(*RotObj, FRotator::ZeroRotator, Rot);
	}
	const TSharedPtr<FJsonObject>* ScaleObj = nullptr;
	if (Arguments->TryGetObjectField(TEXT("relative_scale"), ScaleObj) && ScaleObj)
	{
		ClaireonSkeletonHelpers::ReadVectorFromJson(*ScaleObj, FVector(1.0f, 1.0f, 1.0f), Scale);
	}

	bool bForceAnim = false;
	Arguments->TryGetBoolField(TEXT("force_always_animated"), bForceAnim);

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "SkeletonAddSocket", "MCP: Add Skeleton Socket"));
	Skeleton->Modify();

	USkeletalMeshSocket* NewSocket = NewObject<USkeletalMeshSocket>(Skeleton);
	if (!NewSocket)
	{
		return MakeErrorResult(TEXT("Failed to allocate new USkeletalMeshSocket"));
	}
	NewSocket->SocketName = SocketName;
	NewSocket->BoneName = BoneName;
	NewSocket->RelativeLocation = Loc;
	NewSocket->RelativeRotation = Rot;
	NewSocket->RelativeScale = Scale;
	NewSocket->bForceAlwaysAnimated = bForceAnim;
	Skeleton->Sockets.Add(NewSocket);

	Skeleton->PostEditChange();
	Skeleton->MarkPackageDirty();

	TSharedPtr<FJsonObject> Out = BuildSocketSnapshot(Skeleton,
		FString::Printf(TEXT("Added socket '%s' on bone '%s'"), *SocketNameStr, *BoneNameStr));
	return MakeSuccessResult(Out, FString::Printf(TEXT("Added socket '%s'"), *SocketNameStr));
}

// ============================================================================
// claireon.skeleton_remove_socket
// ============================================================================

FString ClaireonSkeletonTool_RemoveSocket::GetName() const { return TEXT("claireon.skeleton_remove_socket"); }

FString ClaireonSkeletonTool_RemoveSocket::GetDescription() const
{
	return TEXT("Remove a socket from the skeleton by name.");
}

TSharedPtr<FJsonObject> ClaireonSkeletonTool_RemoveSocket::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("skeleton_path"), TEXT("Unreal asset path to the USkeleton"), true);
	S.AddString(TEXT("socket_name"), TEXT("Name of the socket to remove"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonSkeletonTool_RemoveSocket::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SkeletonPath; Arguments->TryGetStringField(TEXT("skeleton_path"), SkeletonPath);
	FString LoadError;
	USkeleton* Skeleton = ClaireonSkeletonHelpers::LoadSkeleton(SkeletonPath, LoadError);
	if (!Skeleton) return MakeErrorResult(LoadError);

	FString SocketNameStr;
	if (!Arguments->TryGetStringField(TEXT("socket_name"), SocketNameStr) || SocketNameStr.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: socket_name"));

	const FName SocketName(*SocketNameStr);
	USkeletalMeshSocket* Socket = Skeleton->FindSocket(SocketName);
	if (!Socket)
	{
		return MakeErrorResult(FString::Printf(TEXT("Socket '%s' not found"), *SocketNameStr));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "SkeletonRemoveSocket", "MCP: Remove Skeleton Socket"));
	Skeleton->Modify();
	Skeleton->Sockets.Remove(Socket);
	Skeleton->PostEditChange();
	Skeleton->MarkPackageDirty();

	TSharedPtr<FJsonObject> Out = BuildSocketSnapshot(Skeleton,
		FString::Printf(TEXT("Removed socket '%s'"), *SocketNameStr));
	return MakeSuccessResult(Out, FString::Printf(TEXT("Removed socket '%s'"), *SocketNameStr));
}

// ============================================================================
// claireon.skeleton_rename_socket
// ============================================================================

FString ClaireonSkeletonTool_RenameSocket::GetName() const { return TEXT("claireon.skeleton_rename_socket"); }

FString ClaireonSkeletonTool_RenameSocket::GetDescription() const
{
	return TEXT("Rename a socket on the skeleton. Fails if new_name is already used by another socket.");
}

TSharedPtr<FJsonObject> ClaireonSkeletonTool_RenameSocket::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("skeleton_path"), TEXT("Unreal asset path to the USkeleton"), true);
	S.AddString(TEXT("old_name"), TEXT("Current socket name"), true);
	S.AddString(TEXT("new_name"), TEXT("New socket name"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonSkeletonTool_RenameSocket::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SkeletonPath; Arguments->TryGetStringField(TEXT("skeleton_path"), SkeletonPath);
	FString LoadError;
	USkeleton* Skeleton = ClaireonSkeletonHelpers::LoadSkeleton(SkeletonPath, LoadError);
	if (!Skeleton) return MakeErrorResult(LoadError);

	FString OldStr, NewStr;
	if (!Arguments->TryGetStringField(TEXT("old_name"), OldStr) || OldStr.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: old_name"));
	if (!Arguments->TryGetStringField(TEXT("new_name"), NewStr) || NewStr.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: new_name"));

	const FName OldName(*OldStr);
	const FName NewName(*NewStr);

	USkeletalMeshSocket* Socket = Skeleton->FindSocket(OldName);
	if (!Socket) return MakeErrorResult(FString::Printf(TEXT("Socket '%s' not found"), *OldStr));
	if (Skeleton->FindSocket(NewName) != nullptr)
	{
		return MakeErrorResult(FString::Printf(TEXT("A socket named '%s' already exists"), *NewStr));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "SkeletonRenameSocket", "MCP: Rename Skeleton Socket"));
	Skeleton->Modify();
	Socket->Modify();
	Socket->SocketName = NewName;
	Skeleton->PostEditChange();
	Skeleton->MarkPackageDirty();

	TSharedPtr<FJsonObject> Out = BuildSocketSnapshot(Skeleton,
		FString::Printf(TEXT("Renamed socket '%s' -> '%s'"), *OldStr, *NewStr));
	return MakeSuccessResult(Out, FString::Printf(TEXT("Renamed socket '%s' -> '%s'"), *OldStr, *NewStr));
}

// ============================================================================
// claireon.skeleton_modify_socket
// ============================================================================

FString ClaireonSkeletonTool_ModifySocket::GetName() const { return TEXT("claireon.skeleton_modify_socket"); }

FString ClaireonSkeletonTool_ModifySocket::GetDescription() const
{
	return TEXT("Modify an existing socket in place. Any combination of bone_name, relative_location, relative_rotation, relative_scale, "
				"and force_always_animated may be supplied; fields that are omitted are left unchanged.");
}

TSharedPtr<FJsonObject> ClaireonSkeletonTool_ModifySocket::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("skeleton_path"), TEXT("Unreal asset path to the USkeleton"), true);
	S.AddString(TEXT("socket_name"), TEXT("Name of the socket to modify"), true);
	S.AddString(TEXT("bone_name"), TEXT("Optional new parent bone name"));
	S.AddObject(TEXT("relative_location"), TEXT("Optional {x,y,z} replacement"));
	S.AddObject(TEXT("relative_rotation"), TEXT("Optional {pitch,yaw,roll} replacement"));
	S.AddObject(TEXT("relative_scale"), TEXT("Optional {x,y,z} replacement"));
	S.AddBoolean(TEXT("force_always_animated"), TEXT("Optional new value for bForceAlwaysAnimated"));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonSkeletonTool_ModifySocket::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SkeletonPath; Arguments->TryGetStringField(TEXT("skeleton_path"), SkeletonPath);
	FString LoadError;
	USkeleton* Skeleton = ClaireonSkeletonHelpers::LoadSkeleton(SkeletonPath, LoadError);
	if (!Skeleton) return MakeErrorResult(LoadError);

	FString SocketNameStr;
	if (!Arguments->TryGetStringField(TEXT("socket_name"), SocketNameStr) || SocketNameStr.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: socket_name"));

	USkeletalMeshSocket* Socket = Skeleton->FindSocket(FName(*SocketNameStr));
	if (!Socket) return MakeErrorResult(FString::Printf(TEXT("Socket '%s' not found"), *SocketNameStr));

	FString BoneNameStr;
	const bool bHasBone = Arguments->TryGetStringField(TEXT("bone_name"), BoneNameStr) && !BoneNameStr.IsEmpty();
	if (bHasBone && Skeleton->GetReferenceSkeleton().FindBoneIndex(FName(*BoneNameStr)) == INDEX_NONE)
	{
		return MakeErrorResult(FString::Printf(TEXT("Bone '%s' not found on skeleton"), *BoneNameStr));
	}

	TArray<FString> Changes;

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "SkeletonModifySocket", "MCP: Modify Skeleton Socket"));
	Skeleton->Modify();
	Socket->Modify();

	if (bHasBone)
	{
		Socket->BoneName = FName(*BoneNameStr);
		Changes.Add(FString::Printf(TEXT("bone_name=%s"), *BoneNameStr));
	}

	const TSharedPtr<FJsonObject>* LocObj = nullptr;
	if (Arguments->TryGetObjectField(TEXT("relative_location"), LocObj) && LocObj)
	{
		FVector NewLoc;
		ClaireonSkeletonHelpers::ReadVectorFromJson(*LocObj, Socket->RelativeLocation, NewLoc);
		Socket->RelativeLocation = NewLoc;
		Changes.Add(FString::Printf(TEXT("relative_location=(%.3f,%.3f,%.3f)"), NewLoc.X, NewLoc.Y, NewLoc.Z));
	}
	const TSharedPtr<FJsonObject>* RotObj = nullptr;
	if (Arguments->TryGetObjectField(TEXT("relative_rotation"), RotObj) && RotObj)
	{
		FRotator NewRot;
		ClaireonSkeletonHelpers::ReadRotatorFromJson(*RotObj, Socket->RelativeRotation, NewRot);
		Socket->RelativeRotation = NewRot;
		Changes.Add(FString::Printf(TEXT("relative_rotation=(P=%.3f,Y=%.3f,R=%.3f)"), NewRot.Pitch, NewRot.Yaw, NewRot.Roll));
	}
	const TSharedPtr<FJsonObject>* ScaleObj = nullptr;
	if (Arguments->TryGetObjectField(TEXT("relative_scale"), ScaleObj) && ScaleObj)
	{
		FVector NewScale;
		ClaireonSkeletonHelpers::ReadVectorFromJson(*ScaleObj, Socket->RelativeScale, NewScale);
		Socket->RelativeScale = NewScale;
		Changes.Add(FString::Printf(TEXT("relative_scale=(%.3f,%.3f,%.3f)"), NewScale.X, NewScale.Y, NewScale.Z));
	}
	bool bForceAnim = false;
	if (Arguments->TryGetBoolField(TEXT("force_always_animated"), bForceAnim))
	{
		Socket->bForceAlwaysAnimated = bForceAnim;
		Changes.Add(FString::Printf(TEXT("force_always_animated=%s"), bForceAnim ? TEXT("true") : TEXT("false")));
	}

	if (Changes.Num() == 0)
	{
		return MakeErrorResult(TEXT("No modifiable fields were supplied (bone_name / relative_location / relative_rotation / relative_scale / force_always_animated)"));
	}

	Socket->PostEditChange();
	Skeleton->PostEditChange();
	Skeleton->MarkPackageDirty();

	TSharedPtr<FJsonObject> Out = BuildSocketSnapshot(Skeleton,
		FString::Printf(TEXT("Modified socket '%s': %s"), *SocketNameStr, *FString::Join(Changes, TEXT(", "))));
	return MakeSuccessResult(Out, FString::Printf(TEXT("Modified socket '%s' (%d field(s))"), *SocketNameStr, Changes.Num()));
}
