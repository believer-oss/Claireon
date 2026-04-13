// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonAnimTools_Curve.h"
#include "Tools/ClaireonAnimHelpers.h"
#include "ClaireonLog.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Curves/RichCurve.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ============================================================================
// claireon.anim_add_curve
// ============================================================================

FString ClaireonAnimTool_AddCurve::GetName() const { return TEXT("claireon.anim_add_curve"); }

FString ClaireonAnimTool_AddCurve::GetDescription() const
{
	return TEXT("Add a new float curve to the animation.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_AddCurve::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddString(TEXT("curve_name"), TEXT("Name of the float curve to add"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_AddCurve::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	FString CurveName;
	if (!Arguments->TryGetStringField(TEXT("curve_name"), CurveName) || CurveName.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: curve_name"));

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimAddCurve", "MCP: Add Curve"));
	Data->Animation->Modify();

	FString OpError;
	if (!ClaireonAnimHelpers::AddCurve(Data->Animation.Get(), CurveName, OpError))
		return MakeErrorResult(OpError);

	Data->LastOperationStatus = FString::Printf(TEXT("add_curve -> Added curve '%s'"), *CurveName);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// claireon.anim_remove_curve
// ============================================================================

FString ClaireonAnimTool_RemoveCurve::GetName() const { return TEXT("claireon.anim_remove_curve"); }

FString ClaireonAnimTool_RemoveCurve::GetDescription() const
{
	return TEXT("Remove a float curve from the animation.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_RemoveCurve::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddString(TEXT("curve_name"), TEXT("Name of the float curve to remove"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_RemoveCurve::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	FString CurveName;
	if (!Arguments->TryGetStringField(TEXT("curve_name"), CurveName) || CurveName.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: curve_name"));

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimRemoveCurve", "MCP: Remove Curve"));
	Data->Animation->Modify();

	FString OpError;
	if (!ClaireonAnimHelpers::RemoveCurve(Data->Animation.Get(), CurveName, OpError))
		return MakeErrorResult(OpError);

	Data->LastOperationStatus = FString::Printf(TEXT("remove_curve -> Removed curve '%s'"), *CurveName);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// claireon.anim_add_curve_key
// ============================================================================

FString ClaireonAnimTool_AddCurveKey::GetName() const { return TEXT("claireon.anim_add_curve_key"); }

FString ClaireonAnimTool_AddCurveKey::GetDescription() const
{
	return TEXT("Add a key to a float curve at a specific time or frame.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_AddCurveKey::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddString(TEXT("curve_name"), TEXT("Name of the target float curve"), true);
	S.AddNumber(TEXT("value"), TEXT("Key value"), true);
	S.AddNumber(TEXT("time"), TEXT("Time in seconds for the key"));
	S.AddInteger(TEXT("frame"), TEXT("Frame number for the key (converted to time using asset frame rate)"));
	S.AddEnum(TEXT("interp_mode"), TEXT("Interpolation mode for the key"),
		{TEXT("linear"), TEXT("cubic"), TEXT("constant")});
	S.AddEnum(TEXT("tangent_mode"), TEXT("Tangent mode for the key"),
		{TEXT("auto"), TEXT("user"), TEXT("break")});
	S.AddNumber(TEXT("arrive_tangent"), TEXT("Arrive tangent value"));
	S.AddNumber(TEXT("leave_tangent"), TEXT("Leave tangent value"));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_AddCurveKey::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	FString CurveName;
	if (!Arguments->TryGetStringField(TEXT("curve_name"), CurveName) || CurveName.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: curve_name"));

	double Value = 0.0;
	if (!Arguments->TryGetNumberField(TEXT("value"), Value))
		return MakeErrorResult(TEXT("Missing required parameter: value"));

	double Time = -1.0;
	Arguments->TryGetNumberField(TEXT("time"), Time);

	double FrameD = -1.0;
	Arguments->TryGetNumberField(TEXT("frame"), FrameD);
	int32 Frame = static_cast<int32>(FrameD);

	if (Frame >= 0 && Time < 0.0)
	{
		const IAnimationDataModel* Model = Data->Animation->GetDataModel();
		double Fps = Model ? Model->GetFrameRate().AsDecimal() : 30.0;
		Time = Frame / Fps;
	}

	if (Time < 0.0)
		return MakeErrorResult(TEXT("Missing required parameter: time or frame"));

	FRichCurveKey NewKey;
	NewKey.Time = static_cast<float>(Time);
	NewKey.Value = static_cast<float>(Value);

	// Interpolation mode
	FString InterpMode;
	if (Arguments->TryGetStringField(TEXT("interp_mode"), InterpMode) && !InterpMode.IsEmpty())
	{
		if (InterpMode == TEXT("linear"))
			NewKey.InterpMode = RCIM_Linear;
		else if (InterpMode == TEXT("cubic"))
			NewKey.InterpMode = RCIM_Cubic;
		else if (InterpMode == TEXT("constant"))
			NewKey.InterpMode = RCIM_Constant;
		else
			return MakeErrorResult(FString::Printf(TEXT("Invalid interp_mode: '%s'. Must be linear, cubic, or constant."), *InterpMode));
	}

	// Tangent mode
	FString TangentMode;
	if (Arguments->TryGetStringField(TEXT("tangent_mode"), TangentMode) && !TangentMode.IsEmpty())
	{
		if (TangentMode == TEXT("auto"))
			NewKey.TangentMode = RCTM_Auto;
		else if (TangentMode == TEXT("user"))
			NewKey.TangentMode = RCTM_User;
		else if (TangentMode == TEXT("break"))
			NewKey.TangentMode = RCTM_Break;
		else
			return MakeErrorResult(FString::Printf(TEXT("Invalid tangent_mode: '%s'. Must be auto, user, or break."), *TangentMode));
	}

	// Optional tangent values
	double ArriveTangent = 0.0;
	if (Arguments->TryGetNumberField(TEXT("arrive_tangent"), ArriveTangent))
		NewKey.ArriveTangent = static_cast<float>(ArriveTangent);

	double LeaveTangent = 0.0;
	if (Arguments->TryGetNumberField(TEXT("leave_tangent"), LeaveTangent))
		NewKey.LeaveTangent = static_cast<float>(LeaveTangent);

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimAddCurveKey", "MCP: Add Curve Key"));
	Data->Animation->Modify();

	FString OpError;
	if (!ClaireonAnimHelpers::AddCurveKey(Data->Animation.Get(), CurveName, NewKey, OpError))
		return MakeErrorResult(OpError);

	Data->LastOperationStatus = FString::Printf(TEXT("add_curve_key -> Added key at t=%.4f v=%.4f on curve '%s'"),
		Time, Value, *CurveName);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// claireon.anim_remove_curve_key
// ============================================================================

FString ClaireonAnimTool_RemoveCurveKey::GetName() const { return TEXT("claireon.anim_remove_curve_key"); }

FString ClaireonAnimTool_RemoveCurveKey::GetDescription() const
{
	return TEXT("Remove a key from a float curve at a specific time or frame.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_RemoveCurveKey::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddString(TEXT("curve_name"), TEXT("Name of the target float curve"), true);
	S.AddNumber(TEXT("time"), TEXT("Time in seconds of the key to remove"));
	S.AddInteger(TEXT("frame"), TEXT("Frame number of the key to remove (converted to time using asset frame rate)"));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_RemoveCurveKey::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	FString CurveName;
	if (!Arguments->TryGetStringField(TEXT("curve_name"), CurveName) || CurveName.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: curve_name"));

	double Time = -1.0;
	Arguments->TryGetNumberField(TEXT("time"), Time);

	double FrameD = -1.0;
	Arguments->TryGetNumberField(TEXT("frame"), FrameD);
	int32 Frame = static_cast<int32>(FrameD);

	if (Frame >= 0 && Time < 0.0)
	{
		const IAnimationDataModel* Model = Data->Animation->GetDataModel();
		double Fps = Model ? Model->GetFrameRate().AsDecimal() : 30.0;
		Time = Frame / Fps;
	}

	if (Time < 0.0)
		return MakeErrorResult(TEXT("Missing required parameter: time or frame"));

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimRemoveCurveKey", "MCP: Remove Curve Key"));
	Data->Animation->Modify();

	FString OpError;
	if (!ClaireonAnimHelpers::RemoveCurveKey(Data->Animation.Get(), CurveName, static_cast<float>(Time), OpError))
		return MakeErrorResult(OpError);

	Data->LastOperationStatus = FString::Printf(TEXT("remove_curve_key -> Removed key at t=%.4f on curve '%s'"),
		Time, *CurveName);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// claireon.anim_set_curve_key_property
// ============================================================================

FString ClaireonAnimTool_SetCurveKeyProperty::GetName() const { return TEXT("claireon.anim_set_curve_key_property"); }

FString ClaireonAnimTool_SetCurveKeyProperty::GetDescription() const
{
	return TEXT("Set a property on an existing curve key (e.g. interp_mode, tangent values).");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_SetCurveKeyProperty::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddString(TEXT("curve_name"), TEXT("Name of the target float curve"), true);
	S.AddString(TEXT("property_name"), TEXT("Property to set (e.g. interp_mode, tangent_mode, arrive_tangent, leave_tangent)"), true);
	S.AddString(TEXT("value"), TEXT("New value for the property"), true);
	S.AddNumber(TEXT("time"), TEXT("Time in seconds of the key to modify"));
	S.AddInteger(TEXT("frame"), TEXT("Frame number of the key to modify (converted to time using asset frame rate)"));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_SetCurveKeyProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	FString CurveName;
	if (!Arguments->TryGetStringField(TEXT("curve_name"), CurveName) || CurveName.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: curve_name"));

	FString PropertyName;
	if (!Arguments->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: property_name"));

	FString Value;
	if (!Arguments->TryGetStringField(TEXT("value"), Value) || Value.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: value"));

	double Time = -1.0;
	Arguments->TryGetNumberField(TEXT("time"), Time);

	double FrameD = -1.0;
	Arguments->TryGetNumberField(TEXT("frame"), FrameD);
	int32 Frame = static_cast<int32>(FrameD);

	if (Frame >= 0 && Time < 0.0)
	{
		const IAnimationDataModel* Model = Data->Animation->GetDataModel();
		double Fps = Model ? Model->GetFrameRate().AsDecimal() : 30.0;
		Time = Frame / Fps;
	}

	if (Time < 0.0)
		return MakeErrorResult(TEXT("Missing required parameter: time or frame"));

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimSetCurveKeyProp", "MCP: Set Curve Key Property"));
	Data->Animation->Modify();

	FString OpError;
	if (!ClaireonAnimHelpers::SetCurveKeyProperty(Data->Animation.Get(), CurveName, static_cast<float>(Time), PropertyName, Value, OpError))
		return MakeErrorResult(OpError);

	Data->LastOperationStatus = FString::Printf(TEXT("set_curve_key_property -> Set '%s' = '%s' on key at t=%.4f on curve '%s'"),
		*PropertyName, *Value, Time, *CurveName);
	return BuildStateResponse(SessionId, Data);
}
