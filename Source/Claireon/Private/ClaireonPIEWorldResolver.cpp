// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonPIEWorldResolver.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "UObject/WeakObjectPtr.h"

namespace ClaireonPIEWorldResolverInternal
{
	// Named file-local namespace (NOT a raw anonymous namespace): under
	// linux-build-server-v2 unity batching, anonymous namespaces from separate
	// .cpp merge into one TU and collide -- a named namespace is the isolation.
	TWeakObjectPtr<UWorld> GClaireonPIEWorldResolver_TestOverrideWorld;

	FString ClaireonPIEWorldResolver_DescribeCandidates(TArrayView<const ClaireonPIEWorldResolver::FPIEContextCandidate> Candidates)
	{
		TArray<FString> Parts;
		for (const ClaireonPIEWorldResolver::FPIEContextCandidate& Candidate : Candidates)
		{
			if (!Candidate.bHasWorld)
			{
				continue;
			}
			FString Role;
			if (Candidate.bIsServer && Candidate.bIsClient)
			{
				Role = TEXT("standalone");
			}
			else if (Candidate.bIsServer)
			{
				Role = TEXT("server");
			}
			else if (Candidate.bIsClient)
			{
				Role = TEXT("client");
			}
			else
			{
				Role = TEXT("unknown");
			}
			Parts.Add(FString::Printf(TEXT("instance %d (%s)"), Candidate.PIEInstance, *Role));
		}
		return Parts.Num() > 0 ? FString::Join(Parts, TEXT(", ")) : FString(TEXT("none"));
	}
} // namespace ClaireonPIEWorldResolverInternal

using namespace ClaireonPIEWorldResolverInternal;

namespace ClaireonPIEWorldResolver
{

bool ParseRequest(const TSharedPtr<FJsonObject>& Arguments, FPIEWorldRequest& OutRequest, FString& OutError)
{
	OutRequest = FPIEWorldRequest();

	if (!Arguments.IsValid())
	{
		return true;
	}

	if (Arguments->HasField(TEXT("pie_instance")))
	{
		if (!Arguments->HasTypedField<EJson::Number>(TEXT("pie_instance")))
		{
			OutError = TEXT("Invalid pie_instance: expected an integer.");
			return false;
		}
		OutRequest.PIEInstance = static_cast<int32>(Arguments->GetNumberField(TEXT("pie_instance")));
	}

	FString NetModeValue;
	if (Arguments->TryGetStringField(TEXT("net_mode"), NetModeValue) && !NetModeValue.IsEmpty())
	{
		const FString NetModeLower = NetModeValue.ToLower();
		if (NetModeLower != TEXT("server") && NetModeLower != TEXT("client"))
		{
			OutError = FString::Printf(
				TEXT("Invalid net_mode '%s'. Valid values: 'server', 'client'."), *NetModeValue);
			return false;
		}
		OutRequest.NetMode = NetModeLower;
	}

	return true;
}

int32 SelectPIEContextIndex(TArrayView<const FPIEContextCandidate> Candidates, const FPIEWorldRequest& Request, FString& OutError)
{
	TArray<int32> Surviving;
	for (int32 Index = 0; Index < Candidates.Num(); ++Index)
	{
		if (Candidates[Index].bHasWorld)
		{
			Surviving.Add(Index);
		}
	}

	if (Surviving.Num() == 0)
	{
		OutError = TEXT("No active PIE session. Start Play-in-Editor first.");
		return INDEX_NONE;
	}

	if (Request.PIEInstance.IsSet())
	{
		const int32 WantedInstance = Request.PIEInstance.GetValue();
		TArray<int32> InstanceMatched;
		for (int32 Index : Surviving)
		{
			if (Candidates[Index].PIEInstance == WantedInstance)
			{
				InstanceMatched.Add(Index);
			}
		}
		if (InstanceMatched.Num() == 0)
		{
			OutError = FString::Printf(
				TEXT("No PIE world context matched pie_instance=%d. Available PIE contexts: %s."),
				WantedInstance,
				*ClaireonPIEWorldResolver_DescribeCandidates(Candidates));
			return INDEX_NONE;
		}
		Surviving = MoveTemp(InstanceMatched);
	}

	if (Request.NetMode.IsSet())
	{
		const bool bWantServer = Request.NetMode.GetValue() == TEXT("server");
		TArray<int32> RoleMatched;
		for (int32 Index : Surviving)
		{
			const FPIEContextCandidate& Candidate = Candidates[Index];
			if ((bWantServer && Candidate.bIsServer) || (!bWantServer && Candidate.bIsClient))
			{
				RoleMatched.Add(Index);
			}
		}
		if (RoleMatched.Num() == 0)
		{
			OutError = FString::Printf(
				TEXT("No PIE world context matched net_mode='%s'. Available PIE contexts: %s."),
				*Request.NetMode.GetValue(),
				*ClaireonPIEWorldResolver_DescribeCandidates(Candidates));
			return INDEX_NONE;
		}
		Surviving = MoveTemp(RoleMatched);
	}

	return Surviving[0];
}

UWorld* ResolvePIEWorld(const TSharedPtr<FJsonObject>& Arguments, FString& OutError)
{
	if (UWorld* OverrideWorld = GClaireonPIEWorldResolver_TestOverrideWorld.Get())
	{
		return OverrideWorld;
	}

	FPIEWorldRequest Request;
	if (!ParseRequest(Arguments, Request, OutError))
	{
		return nullptr;
	}

	if (!GEngine)
	{
		OutError = TEXT("GEngine is not available; cannot enumerate PIE world contexts.");
		return nullptr;
	}

	TArray<FPIEContextCandidate> Candidates;
	TArray<UWorld*> CandidateWorlds;
	for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
	{
		if (WorldContext.WorldType != EWorldType::PIE)
		{
			continue;
		}

		FPIEContextCandidate Candidate;
		Candidate.PIEInstance = WorldContext.PIEInstance;

		UWorld* ContextWorld = WorldContext.World();
		Candidate.bHasWorld = (ContextWorld != nullptr);
		if (ContextWorld)
		{
			const ENetMode NetMode = ContextWorld->GetNetMode();
			// A standalone PIE world is both the authority and the local
			// client, so it satisfies either net_mode filter.
			Candidate.bIsServer =
				NetMode == NM_DedicatedServer || NetMode == NM_ListenServer || NetMode == NM_Standalone;
			Candidate.bIsClient = NetMode == NM_Client || NetMode == NM_Standalone;
		}

		Candidates.Add(Candidate);
		CandidateWorlds.Add(ContextWorld);
	}

	const int32 SelectedIndex = SelectPIEContextIndex(Candidates, Request, OutError);
	if (SelectedIndex == INDEX_NONE)
	{
		return nullptr;
	}

	return CandidateWorlds[SelectedIndex];
}

void AddSchemaParams(const TSharedPtr<FJsonObject>& Properties)
{
	if (!Properties.IsValid())
	{
		return;
	}

	TSharedPtr<FJsonObject> PieInstanceProp = MakeShared<FJsonObject>();
	PieInstanceProp->SetStringField(TEXT("type"), TEXT("integer"));
	PieInstanceProp->SetStringField(TEXT("description"),
		TEXT("Optional PIE instance index to target when multiple PIE world contexts exist "
			 "(multi-client PIE). Default: the first PIE context (historical behavior)."));
	Properties->SetObjectField(TEXT("pie_instance"), PieInstanceProp);

	TSharedPtr<FJsonObject> NetModeProp = MakeShared<FJsonObject>();
	NetModeProp->SetStringField(TEXT("type"), TEXT("string"));
	NetModeProp->SetStringField(TEXT("description"),
		TEXT("Optional network role of the PIE world to target: 'server' picks the authority world, "
			 "'client' picks a client world (required to reach client-side actors under Play-as-Client). "
			 "A standalone (non-networked) PIE world satisfies either value. "
			 "Default: the first PIE context, which under Play-as-Client is the server world."));
	TArray<TSharedPtr<FJsonValue>> NetModeEnum;
	NetModeEnum.Add(MakeShared<FJsonValueString>(TEXT("server")));
	NetModeEnum.Add(MakeShared<FJsonValueString>(TEXT("client")));
	NetModeProp->SetArrayField(TEXT("enum"), NetModeEnum);
	Properties->SetObjectField(TEXT("net_mode"), NetModeProp);
}

void SetPIEWorldOverrideForTesting(UWorld* World)
{
	GClaireonPIEWorldResolver_TestOverrideWorld = World;
}

} // namespace ClaireonPIEWorldResolver
