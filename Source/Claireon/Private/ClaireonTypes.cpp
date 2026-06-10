// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonTypes.h"

bool FMCPJsonRpcRequest::FromJsonObject(const TSharedPtr<FJsonObject>& JsonObject, FMCPJsonRpcRequest& OutRequest)
{
	if (!JsonObject.IsValid())
	{
		return false;
	}

	// "jsonrpc" must be "2.0"
	if (!JsonObject->TryGetStringField(TEXT("jsonrpc"), OutRequest.JsonRpc) || OutRequest.JsonRpc != TEXT("2.0"))
	{
		return false;
	}

	// "method" is required
	if (!JsonObject->TryGetStringField(TEXT("method"), OutRequest.Method) || OutRequest.Method.IsEmpty())
	{
		return false;
	}

	// "id" is optional (notifications don't have it)
	if (JsonObject->HasField(TEXT("id")))
	{
		OutRequest.Id = JsonObject->TryGetField(TEXT("id"));
	}

	// "params" is optional
	if (JsonObject->HasField(TEXT("params")))
	{
		OutRequest.Params = JsonObject->GetObjectField(TEXT("params"));
	}

	return true;
}

TSharedPtr<FJsonObject> FMCPJsonRpcResponse::MakeResult(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonValue>& Result)
{
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	if (Id.IsValid())
	{
		Response->SetField(TEXT("id"), Id);
	}
	else
	{
		Response->SetField(TEXT("id"), MakeShared<FJsonValueNull>());
	}
	Response->SetField(TEXT("result"), Result);
	return Response;
}

TSharedPtr<FJsonObject> FMCPJsonRpcResponse::MakeResult(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Result)
{
	return MakeResult(Id, MakeShared<FJsonValueObject>(Result));
}

TSharedPtr<FJsonObject> FMCPJsonRpcResponse::MakeError(const TSharedPtr<FJsonValue>& Id, int32 Code, const FString& Message)
{
	TSharedPtr<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
	ErrorObj->SetNumberField(TEXT("code"), Code);
	ErrorObj->SetStringField(TEXT("message"), Message);

	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	if (Id.IsValid())
	{
		Response->SetField(TEXT("id"), Id);
	}
	else
	{
		Response->SetField(TEXT("id"), MakeShared<FJsonValueNull>());
	}
	Response->SetObjectField(TEXT("error"), ErrorObj);
	return Response;
}
