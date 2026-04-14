// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_LandscapeSplineEdit.h"
#include "Tools/ClaireonLandscapeHelpers.h"
#include "ClaireonSessionManager.h"
#include "ClaireonLog.h"
#include "Editor.h"
#include "LandscapeSplineControlPoint.h"
#include "LandscapeSplineSegment.h"
#include "LandscapeSplinesComponent.h"
#include "LandscapeProxy.h"
#include "LandscapeInfo.h"
#include "Dom/JsonValue.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

// Static member definitions
TMap<FString, FLandscapeSplineEditToolData> ClaireonTool_LandscapeSplineEdit::ToolData;
bool ClaireonTool_LandscapeSplineEdit::bDelegateRegistered = false;

void ClaireonTool_LandscapeSplineEdit::HandleSessionClosed(const FMCPSessionClosedInfo& Info)
{
	if (Info.ToolName == TEXT("claireon.landscape_spline_edit"))
	{
		ToolData.Remove(Info.SessionId);
	}
}

FString ClaireonTool_LandscapeSplineEdit::GetName() const
{
	return TEXT("claireon.landscape_spline_edit");
}

FString ClaireonTool_LandscapeSplineEdit::GetDescription() const
{
	return TEXT("Edit landscape splines: control points, segments, terrain deformation.");
}

FString ClaireonTool_LandscapeSplineEdit::GetFullDescription() const
{
	return TEXT(
		"Edit landscape splines: control points, segments, terrain deformation.\n\n"
		"Operations:\n"
		"  open - Open a session on a landscape's spline component (params: landscape_name)\n"
		"  close - Close the current session\n"
		"  status - Get current spline state (control points, segments)\n"
		"  add_control_point - Create a new control point (params: location{x,y,z}, rotation{pitch,yaw,roll}, width, side_falloff)\n"
		"  remove_control_point - Remove a control point and connected segments (params: index)\n"
		"  set_control_point - Modify control point properties (params: index, location, rotation, width, side_falloff)\n"
		"  add_segment - Connect two control points (params: start_index, end_index)\n"
		"  remove_segment - Remove a segment (params: index)\n"
		"  set_segment_property - Modify segment properties (params: index, layer_name, raise_terrain, lower_terrain)\n"
		"  apply_to_landscape - Apply spline deformation to heightmap and weightmap\n"
		"  save - Save the landscape package\n"
	);
}

TSharedPtr<FJsonObject> ClaireonTool_LandscapeSplineEdit::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> OpProp = MakeShared<FJsonObject>();
	OpProp->SetStringField(TEXT("type"), TEXT("string"));
	OpProp->SetStringField(TEXT("description"), TEXT("Operation to perform"));
	{
		TArray<TSharedPtr<FJsonValue>> EnumValues;
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("open")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("close")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("status")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("add_control_point")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("remove_control_point")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("set_control_point")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("add_segment")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("remove_segment")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("set_segment_property")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("apply_to_landscape")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("save")));
		OpProp->SetArrayField(TEXT("enum"), EnumValues);
	}
	Properties->SetObjectField(TEXT("operation"), OpProp);

	TSharedPtr<FJsonObject> SessionProp = MakeShared<FJsonObject>();
	SessionProp->SetStringField(TEXT("type"), TEXT("string"));
	SessionProp->SetStringField(TEXT("description"), TEXT("Session ID from open. Required for all operations except open."));
	Properties->SetObjectField(TEXT("session_id"), SessionProp);

	TSharedPtr<FJsonObject> ParamsProp = MakeShared<FJsonObject>();
	ParamsProp->SetStringField(TEXT("type"), TEXT("object"));
	ParamsProp->SetStringField(TEXT("description"), TEXT("Operation-specific parameters."));
	Properties->SetObjectField(TEXT("params"), ParamsProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("operation")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_LandscapeSplineEdit::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString Operation;
	if (!Arguments->TryGetStringField(TEXT("operation"), Operation) || Operation.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: operation"));
	}

	bool bSuppressOutput = false;
	Arguments->TryGetBoolField(TEXT("suppress_output"), bSuppressOutput);

	if (Operation == TEXT("open"))
	{
		TSharedPtr<FJsonObject> Params = Arguments->HasField(TEXT("params"))
			? Arguments->GetObjectField(TEXT("params"))
			: MakeShared<FJsonObject>();
		return Operation_Open(Params);
	}

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

	FLandscapeSplineEditToolData* Data = ToolData.Find(SessionId);
	if (!Data)
	{
		return MakeErrorResult(TEXT("Session tool data not found"));
	}

	if (!Data->SplinesComponent.IsValid())
	{
		return MakeErrorResult(TEXT("Splines component no longer valid. Reopen session."));
	}

	Data->bSuppressOutput = bSuppressOutput;

	TSharedPtr<FJsonObject> Params = Arguments->HasField(TEXT("params"))
		? Arguments->GetObjectField(TEXT("params"))
		: MakeShared<FJsonObject>();

	if (Operation == TEXT("close")) return Operation_Close(SessionId, Data, Params);
	if (Operation == TEXT("status")) return Operation_Status(SessionId, Data, Params);
	if (Operation == TEXT("add_control_point")) return Operation_AddControlPoint(SessionId, Data, Params);
	if (Operation == TEXT("remove_control_point")) return Operation_RemoveControlPoint(SessionId, Data, Params);
	if (Operation == TEXT("set_control_point")) return Operation_SetControlPoint(SessionId, Data, Params);
	if (Operation == TEXT("add_segment")) return Operation_AddSegment(SessionId, Data, Params);
	if (Operation == TEXT("remove_segment")) return Operation_RemoveSegment(SessionId, Data, Params);
	if (Operation == TEXT("set_segment_property")) return Operation_SetSegmentProperty(SessionId, Data, Params);
	if (Operation == TEXT("apply_to_landscape")) return Operation_ApplyToLandscape(SessionId, Data, Params);
	if (Operation == TEXT("save")) return Operation_Save(SessionId, Data, Params);

	return MakeErrorResult(FString::Printf(TEXT("Unknown operation: %s"), *Operation));
}

// ---------------------------------------------------------------------------
// Session Management
// ---------------------------------------------------------------------------

IClaireonTool::FToolResult ClaireonTool_LandscapeSplineEdit::Operation_Open(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor)
	{
		return MakeErrorResult(TEXT("Editor not available"));
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return MakeErrorResult(TEXT("No editor world loaded"));
	}

	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&HandleSessionClosed);
		bDelegateRegistered = true;
	}

	FString LandscapeName;
	if (!Params->TryGetStringField(TEXT("landscape_name"), LandscapeName) || LandscapeName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: landscape_name"));
	}

	TArray<TPair<ULandscapeInfo*, ALandscapeProxy*>> Landscapes =
		ClaireonLandscapeHelpers::FindLandscapeInWorld(World, LandscapeName);

	if (Landscapes.Num() == 0)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("No landscape matching '%s' found in the current world"), *LandscapeName));
	}
	if (Landscapes.Num() > 1)
	{
		TArray<FString> Names;
		for (const auto& L : Landscapes) Names.Add(L.Value->GetActorLabel());
		return MakeErrorResult(FString::Printf(
			TEXT("Multiple landscapes match '%s': %s. Provide a more specific name."),
			*LandscapeName, *FString::Join(Names, TEXT(", "))));
	}

	ULandscapeInfo* LandscapeInfo = Landscapes[0].Key;
	ALandscapeProxy* Proxy = Landscapes[0].Value;

	// Get or create splines component
	ULandscapeSplinesComponent* SplinesComp = Proxy->GetSplinesComponent();
	if (!SplinesComp)
	{
		Proxy->CreateSplineComponent();
		SplinesComp = Proxy->GetSplinesComponent();
		if (!SplinesComp)
		{
			return MakeErrorResult(TEXT("Failed to create splines component on landscape"));
		}
	}

	const FString ActorPath = Proxy->GetPathName();
	FMCPOpenSessionResult SessionResult = FClaireonSessionManager::Get().OpenSession(ActorPath, TEXT("claireon.landscape_spline_edit"));
	if (SessionResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		FString BlockInfo = TEXT("another tool");
		if (SessionResult.BlockingSession.IsSet())
		{
			BlockInfo = FString::Printf(TEXT("%s (session %s)"),
				*SessionResult.BlockingSession->ToolName, *SessionResult.BlockingSession->SessionId);
		}
		return MakeErrorResult(FString::Printf(TEXT("Landscape splines locked by %s"), *BlockInfo));
	}

	const FString SessionId = SessionResult.SessionId;
	FLandscapeSplineEditToolData& Data = ToolData.FindOrAdd(SessionId);
	Data.SplinesComponent = SplinesComp;
	Data.LandscapeProxy = Proxy;
	Data.LandscapeInfo = LandscapeInfo;
	Data.FocusedControlPointIndex = INDEX_NONE;
	Data.LastOperationStatus = TEXT("Session opened");

	return BuildStateResponse(SessionId, &Data);
}

IClaireonTool::FToolResult ClaireonTool_LandscapeSplineEdit::Operation_Close(const FString& SessionId, FLandscapeSplineEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FClaireonSessionManager::Get().CloseSession(SessionId);
	ToolData.Remove(SessionId);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("status"), TEXT("closed"));
	return MakeSuccessResult(ResultData, TEXT("Session closed"));
}

IClaireonTool::FToolResult ClaireonTool_LandscapeSplineEdit::Operation_Status(const FString& SessionId, FLandscapeSplineEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	return BuildStateResponse(SessionId, Data);
}

// ---------------------------------------------------------------------------
// Control Point Operations
// ---------------------------------------------------------------------------

IClaireonTool::FToolResult ClaireonTool_LandscapeSplineEdit::Operation_AddControlPoint(const FString& SessionId, FLandscapeSplineEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	ULandscapeSplinesComponent* SplinesComp = Data->SplinesComponent.Get();

	// Extract location
	const TSharedPtr<FJsonObject>* LocationObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("location"), LocationObj) || !LocationObj)
	{
		return MakeErrorResult(TEXT("Missing required parameter: location {x, y, z}"));
	}
	FVector Location = FVector::ZeroVector;
	double Val;
	if ((*LocationObj)->TryGetNumberField(TEXT("x"), Val)) Location.X = Val;
	if ((*LocationObj)->TryGetNumberField(TEXT("y"), Val)) Location.Y = Val;
	if ((*LocationObj)->TryGetNumberField(TEXT("z"), Val)) Location.Z = Val;

	// Optional rotation
	FRotator Rotation = FRotator::ZeroRotator;
	const TSharedPtr<FJsonObject>* RotObj = nullptr;
	if (Params->TryGetObjectField(TEXT("rotation"), RotObj) && RotObj)
	{
		if ((*RotObj)->TryGetNumberField(TEXT("pitch"), Val)) Rotation.Pitch = Val;
		if ((*RotObj)->TryGetNumberField(TEXT("yaw"), Val)) Rotation.Yaw = Val;
		if ((*RotObj)->TryGetNumberField(TEXT("roll"), Val)) Rotation.Roll = Val;
	}

	double Width = 1000.0;
	Params->TryGetNumberField(TEXT("width"), Width);

	double SideFalloff = 1000.0;
	Params->TryGetNumberField(TEXT("side_falloff"), SideFalloff);

	// Create control point
	ULandscapeSplineControlPoint* NewPoint = NewObject<ULandscapeSplineControlPoint>(SplinesComp);
	NewPoint->Location = Location;
	NewPoint->Rotation = Rotation;
	NewPoint->Width = static_cast<float>(Width);
	NewPoint->SideFalloff = static_cast<float>(SideFalloff);

	SplinesComp->GetControlPoints().Add(NewPoint);
	SplinesComp->MarkRenderStateDirty();

	const int32 NewIndex = SplinesComp->GetControlPoints().Num() - 1;
	Data->LastOperationStatus = FString::Printf(TEXT("Added control point at index %d"), NewIndex);

	return BuildStateResponse(SessionId, Data);
}

IClaireonTool::FToolResult ClaireonTool_LandscapeSplineEdit::Operation_RemoveControlPoint(const FString& SessionId, FLandscapeSplineEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	ULandscapeSplinesComponent* SplinesComp = Data->SplinesComponent.Get();
	auto& ControlPoints = SplinesComp->GetControlPoints();

	int32 Index = INDEX_NONE;
	if (!Params->TryGetNumberField(TEXT("index"), Index))
	{
		return MakeErrorResult(TEXT("Missing required parameter: index"));
	}

	if (Index < 0 || Index >= ControlPoints.Num())
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Control point index %d out of range [0, %d)"), Index, ControlPoints.Num()));
	}

	ULandscapeSplineControlPoint* PointToRemove = ControlPoints[Index];

	// Collect and remove connected segments
	TArray<ULandscapeSplineSegment*> SegmentsToRemove;
	for (const FLandscapeSplineConnection& Connection : PointToRemove->ConnectedSegments)
	{
		SegmentsToRemove.AddUnique(Connection.Segment);
	}

	auto& Segments = SplinesComp->GetSegments();
	for (ULandscapeSplineSegment* Segment : SegmentsToRemove)
	{
		// Clean up connected segments on the other end
		for (int32 EndIdx = 0; EndIdx < 2; ++EndIdx)
		{
			ULandscapeSplineControlPoint* OtherPoint = Segment->Connections[EndIdx].ControlPoint;
			if (OtherPoint && OtherPoint != PointToRemove)
			{
				OtherPoint->ConnectedSegments.RemoveAll(
					[Segment](const FLandscapeSplineConnection& Conn) { return Conn.Segment == Segment; });
			}
		}
		Segments.Remove(Segment);
	}

	ControlPoints.RemoveAt(Index);

	// Update focused index
	if (Data->FocusedControlPointIndex == Index)
	{
		Data->FocusedControlPointIndex = INDEX_NONE;
	}
	else if (Data->FocusedControlPointIndex > Index)
	{
		--Data->FocusedControlPointIndex;
	}

	SplinesComp->MarkRenderStateDirty();
	Data->LastOperationStatus = FString::Printf(
		TEXT("Removed control point %d and %d connected segment(s)"), Index, SegmentsToRemove.Num());

	return BuildStateResponse(SessionId, Data);
}

IClaireonTool::FToolResult ClaireonTool_LandscapeSplineEdit::Operation_SetControlPoint(const FString& SessionId, FLandscapeSplineEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	ULandscapeSplinesComponent* SplinesComp = Data->SplinesComponent.Get();
	const auto& ControlPoints = SplinesComp->GetControlPoints();

	int32 Index = INDEX_NONE;
	if (!Params->TryGetNumberField(TEXT("index"), Index))
	{
		return MakeErrorResult(TEXT("Missing required parameter: index"));
	}

	if (Index < 0 || Index >= ControlPoints.Num())
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Control point index %d out of range [0, %d)"), Index, ControlPoints.Num()));
	}

	ULandscapeSplineControlPoint* Point = ControlPoints[Index];
	bool bLocationOrRotationChanged = false;
	double Val;

	// Apply optional properties
	const TSharedPtr<FJsonObject>* LocationObj = nullptr;
	if (Params->TryGetObjectField(TEXT("location"), LocationObj) && LocationObj)
	{
		if ((*LocationObj)->TryGetNumberField(TEXT("x"), Val)) { Point->Location.X = Val; bLocationOrRotationChanged = true; }
		if ((*LocationObj)->TryGetNumberField(TEXT("y"), Val)) { Point->Location.Y = Val; bLocationOrRotationChanged = true; }
		if ((*LocationObj)->TryGetNumberField(TEXT("z"), Val)) { Point->Location.Z = Val; bLocationOrRotationChanged = true; }
	}

	const TSharedPtr<FJsonObject>* RotObj = nullptr;
	if (Params->TryGetObjectField(TEXT("rotation"), RotObj) && RotObj)
	{
		if ((*RotObj)->TryGetNumberField(TEXT("pitch"), Val)) { Point->Rotation.Pitch = Val; bLocationOrRotationChanged = true; }
		if ((*RotObj)->TryGetNumberField(TEXT("yaw"), Val)) { Point->Rotation.Yaw = Val; bLocationOrRotationChanged = true; }
		if ((*RotObj)->TryGetNumberField(TEXT("roll"), Val)) { Point->Rotation.Roll = Val; bLocationOrRotationChanged = true; }
	}

	if (Params->TryGetNumberField(TEXT("width"), Val)) Point->Width = static_cast<float>(Val);
	if (Params->TryGetNumberField(TEXT("side_falloff"), Val)) Point->SideFalloff = static_cast<float>(Val);

	if (bLocationOrRotationChanged)
	{
		Point->UpdateSplinePoints(true /*bUpdateCollision*/, true /*bUpdateAttachedSegments*/);
	}

	SplinesComp->MarkRenderStateDirty();
	Data->LastOperationStatus = FString::Printf(TEXT("Updated control point %d"), Index);

	return BuildStateResponse(SessionId, Data);
}

// ---------------------------------------------------------------------------
// Segment Operations
// ---------------------------------------------------------------------------

IClaireonTool::FToolResult ClaireonTool_LandscapeSplineEdit::Operation_AddSegment(const FString& SessionId, FLandscapeSplineEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	ULandscapeSplinesComponent* SplinesComp = Data->SplinesComponent.Get();
	auto& ControlPoints = SplinesComp->GetControlPoints();

	int32 StartIndex = INDEX_NONE, EndIndex = INDEX_NONE;
	if (!Params->TryGetNumberField(TEXT("start_index"), StartIndex))
	{
		return MakeErrorResult(TEXT("Missing required parameter: start_index"));
	}
	if (!Params->TryGetNumberField(TEXT("end_index"), EndIndex))
	{
		return MakeErrorResult(TEXT("Missing required parameter: end_index"));
	}

	if (StartIndex < 0 || StartIndex >= ControlPoints.Num())
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Start control point index %d out of range [0, %d)"), StartIndex, ControlPoints.Num()));
	}
	if (EndIndex < 0 || EndIndex >= ControlPoints.Num())
	{
		return MakeErrorResult(FString::Printf(
			TEXT("End control point index %d out of range [0, %d)"), EndIndex, ControlPoints.Num()));
	}
	if (StartIndex == EndIndex)
	{
		return MakeErrorResult(TEXT("Cannot connect a control point to itself"));
	}

	ULandscapeSplineControlPoint* StartPoint = ControlPoints[StartIndex];
	ULandscapeSplineControlPoint* EndPoint = ControlPoints[EndIndex];

	// Create segment
	ULandscapeSplineSegment* NewSegment = NewObject<ULandscapeSplineSegment>(SplinesComp);
	NewSegment->Connections[0].ControlPoint = StartPoint;
	NewSegment->Connections[1].ControlPoint = EndPoint;

	// Wire up both control points
	FLandscapeSplineConnection StartConn;
	StartConn.Segment = NewSegment;
	StartConn.End = 0;
	StartPoint->ConnectedSegments.Add(StartConn);

	FLandscapeSplineConnection EndConn;
	EndConn.Segment = NewSegment;
	EndConn.End = 1;
	EndPoint->ConnectedSegments.Add(EndConn);

	// Compute tangent data
	NewSegment->UpdateSplinePoints();

	SplinesComp->GetSegments().Add(NewSegment);
	SplinesComp->MarkRenderStateDirty();

	Data->LastOperationStatus = FString::Printf(
		TEXT("Added segment connecting control points %d and %d"), StartIndex, EndIndex);

	return BuildStateResponse(SessionId, Data);
}

IClaireonTool::FToolResult ClaireonTool_LandscapeSplineEdit::Operation_RemoveSegment(const FString& SessionId, FLandscapeSplineEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	ULandscapeSplinesComponent* SplinesComp = Data->SplinesComponent.Get();
	auto& Segments = SplinesComp->GetSegments();

	int32 Index = INDEX_NONE;
	if (!Params->TryGetNumberField(TEXT("index"), Index))
	{
		return MakeErrorResult(TEXT("Missing required parameter: index"));
	}

	if (Index < 0 || Index >= Segments.Num())
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Segment index %d out of range [0, %d)"), Index, Segments.Num()));
	}

	ULandscapeSplineSegment* Segment = Segments[Index];

	// Remove from connected control points
	for (int32 EndIdx = 0; EndIdx < 2; ++EndIdx)
	{
		ULandscapeSplineControlPoint* Point = Segment->Connections[EndIdx].ControlPoint;
		if (Point)
		{
			Point->ConnectedSegments.RemoveAll(
				[Segment](const FLandscapeSplineConnection& Conn) { return Conn.Segment == Segment; });
		}
	}

	Segments.RemoveAt(Index);
	SplinesComp->MarkRenderStateDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("Removed segment %d"), Index);

	return BuildStateResponse(SessionId, Data);
}

IClaireonTool::FToolResult ClaireonTool_LandscapeSplineEdit::Operation_SetSegmentProperty(const FString& SessionId, FLandscapeSplineEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	ULandscapeSplinesComponent* SplinesComp = Data->SplinesComponent.Get();
	const auto& Segments = SplinesComp->GetSegments();

	int32 Index = INDEX_NONE;
	if (!Params->TryGetNumberField(TEXT("index"), Index))
	{
		return MakeErrorResult(TEXT("Missing required parameter: index"));
	}

	if (Index < 0 || Index >= Segments.Num())
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Segment index %d out of range [0, %d)"), Index, Segments.Num()));
	}

	ULandscapeSplineSegment* Segment = Segments[Index];

	FString LayerName;
	if (Params->TryGetStringField(TEXT("layer_name"), LayerName))
	{
		Segment->LayerName = FName(*LayerName);
	}

	bool bVal;
	if (Params->TryGetBoolField(TEXT("raise_terrain"), bVal))
	{
		Segment->bRaiseTerrain = bVal;
	}
	if (Params->TryGetBoolField(TEXT("lower_terrain"), bVal))
	{
		Segment->bLowerTerrain = bVal;
	}

	SplinesComp->MarkRenderStateDirty();
	Data->LastOperationStatus = FString::Printf(TEXT("Updated segment %d properties"), Index);

	return BuildStateResponse(SessionId, Data);
}

// ---------------------------------------------------------------------------
// Landscape Application
// ---------------------------------------------------------------------------

IClaireonTool::FToolResult ClaireonTool_LandscapeSplineEdit::Operation_ApplyToLandscape(const FString& SessionId, FLandscapeSplineEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	ULandscapeInfo* LandscapeInfo = Data->LandscapeInfo.Get();
	if (!LandscapeInfo)
	{
		return MakeErrorResult(TEXT("No landscape info available for spline application"));
	}

	TSet<TObjectPtr<ULandscapeComponent>> ModifiedComponents;
	const bool bResult = LandscapeInfo->ApplySplines(false /*bOnlySelected*/, &ModifiedComponents);

	const int32 ModifiedCount = ModifiedComponents.Num();
	Data->LastOperationStatus = FString::Printf(
		TEXT("Applied splines to landscape (%d components modified)"), ModifiedCount);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("operation"), TEXT("apply_to_landscape"));
	ResultData->SetBoolField(TEXT("success"), bResult);
	ResultData->SetNumberField(TEXT("modified_components"), ModifiedCount);
	return MakeSuccessResult(ResultData, Data->LastOperationStatus);
}

IClaireonTool::FToolResult ClaireonTool_LandscapeSplineEdit::Operation_Save(const FString& SessionId, FLandscapeSplineEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	ALandscapeProxy* Proxy = Data->LandscapeProxy.Get();
	if (!Proxy)
	{
		return MakeErrorResult(TEXT("Landscape proxy no longer valid"));
	}

	UPackage* Package = Proxy->GetOutermost();
	if (!Package)
	{
		return MakeErrorResult(TEXT("Failed to get landscape package"));
	}

	const FString PackageFilename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Standalone;
	const FSavePackageResultStruct SaveResult = UPackage::Save(Package, nullptr, *PackageFilename, SaveArgs);

	if (SaveResult.Result != ESavePackageResult::Success)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to save package: %s"), *PackageFilename));
	}

	Data->LastOperationStatus = TEXT("Landscape saved");

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("operation"), TEXT("save"));
	return MakeSuccessResult(ResultData, Data->LastOperationStatus);
}

// ---------------------------------------------------------------------------
// BuildStateResponse
// ---------------------------------------------------------------------------

IClaireonTool::FToolResult ClaireonTool_LandscapeSplineEdit::BuildStateResponse(const FString& SessionId, FLandscapeSplineEditToolData* Data)
{
	ULandscapeSplinesComponent* SplinesComp = Data->SplinesComponent.Get();
	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("session_id"), SessionId);
	ResultData->SetStringField(TEXT("status"), Data->LastOperationStatus);

	if (Data->bSuppressOutput)
	{
		return MakeSuccessResult(ResultData, Data->LastOperationStatus);
	}

	// Control points
	const auto& ControlPoints = SplinesComp->GetControlPoints();
	TArray<TSharedPtr<FJsonValue>> CPArray;
	for (int32 i = 0; i < ControlPoints.Num(); ++i)
	{
		ULandscapeSplineControlPoint* CP = ControlPoints[i];
		TSharedPtr<FJsonObject> CPJson = MakeShared<FJsonObject>();
		CPJson->SetNumberField(TEXT("index"), i);

		TSharedPtr<FJsonObject> Loc = MakeShared<FJsonObject>();
		Loc->SetNumberField(TEXT("x"), CP->Location.X);
		Loc->SetNumberField(TEXT("y"), CP->Location.Y);
		Loc->SetNumberField(TEXT("z"), CP->Location.Z);
		CPJson->SetObjectField(TEXT("location"), Loc);

		TSharedPtr<FJsonObject> Rot = MakeShared<FJsonObject>();
		Rot->SetNumberField(TEXT("pitch"), CP->Rotation.Pitch);
		Rot->SetNumberField(TEXT("yaw"), CP->Rotation.Yaw);
		Rot->SetNumberField(TEXT("roll"), CP->Rotation.Roll);
		CPJson->SetObjectField(TEXT("rotation"), Rot);

		CPJson->SetNumberField(TEXT("width"), CP->Width);
		CPJson->SetNumberField(TEXT("side_falloff"), CP->SideFalloff);
		CPArray.Add(MakeShared<FJsonValueObject>(CPJson));
	}
	ResultData->SetArrayField(TEXT("control_points"), CPArray);

	// Segments
	const auto& Segments = SplinesComp->GetSegments();
	TArray<TSharedPtr<FJsonValue>> SegArray;
	for (int32 i = 0; i < Segments.Num(); ++i)
	{
		ULandscapeSplineSegment* Seg = Segments[i];
		TSharedPtr<FJsonObject> SegJson = MakeShared<FJsonObject>();
		SegJson->SetNumberField(TEXT("index"), i);

		// Find control point indices
		for (int32 EndIdx = 0; EndIdx < 2; ++EndIdx)
		{
			ULandscapeSplineControlPoint* ConnPoint = Seg->Connections[EndIdx].ControlPoint;
			int32 CPIndex = INDEX_NONE;
			if (ConnPoint)
			{
				CPIndex = ControlPoints.Find(ConnPoint);
			}
			SegJson->SetNumberField(
				EndIdx == 0 ? TEXT("start_index") : TEXT("end_index"), CPIndex);
		}

		SegJson->SetStringField(TEXT("layer_name"), Seg->LayerName.ToString());
		SegJson->SetBoolField(TEXT("raise_terrain"), Seg->bRaiseTerrain);
		SegJson->SetBoolField(TEXT("lower_terrain"), Seg->bLowerTerrain);
		SegArray.Add(MakeShared<FJsonValueObject>(SegJson));
	}
	ResultData->SetArrayField(TEXT("segments"), SegArray);

	ResultData->SetNumberField(TEXT("focused_control_point_index"), Data->FocusedControlPointIndex);

	const FString Summary = FString::Printf(
		TEXT("Session %s: %d control points, %d segments -- %s"),
		*SessionId, ControlPoints.Num(), Segments.Num(), *Data->LastOperationStatus);
	return MakeSuccessResult(ResultData, Summary);
}
