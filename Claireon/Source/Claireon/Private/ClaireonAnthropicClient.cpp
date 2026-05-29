// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonAnthropicClient.h"
#include "ClaireonBridge.h"
#include "ClaireonSettings.h"
#include "ClaireonServer.h"
#include "ClaireonModule.h"
#include "ClaireonREPLLogger.h"
#include "ClaireonLog.h"
#include "ClaireonOutputGate.h"
#include "Tools/IClaireonTool.h"
#include "ClaireonXmlFormatter.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonReader.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Async/Async.h"
#include "Interfaces/IHttpBase.h"
#include "Modules/ModuleManager.h"

FClaireonAnthropicClient::FClaireonAnthropicClient(
	FClaireonServer* InServer, TSharedPtr<FClaireonREPLLogger> InLogger)
	: Server(InServer), Logger(InLogger)
{
	ResetConversation();
}

FClaireonServer* FClaireonAnthropicClient::GetCurrentServer() const
{
	// Always resolve from the module so we get the live server even if it was
	// started after this client was constructed (e.g. tab opened before Start Server).
	FClaireonModule* Mod = FModuleManager::GetModulePtr<FClaireonModule>(TEXT("Claireon"));
	return Mod ? Mod->GetServer() : nullptr;
}

void FClaireonAnthropicClient::ResetConversation()
{
	++ConversationCounter;
	CurrentConversationId = FString::Printf(TEXT("conv_%03d"), ConversationCounter);
	ConversationMessages.Empty();
}

void FClaireonAnthropicClient::SendMessage(const FString& UserText,
	const FString& ConversationId)
{
	if (bRequestInFlight)
	{
		UE_LOG(LogClaireon, Warning, TEXT("[REPLClient] SendMessage called while request in flight"));
		return;
	}

	// Connect-time stale-spill sweep (once per client lifetime, game-thread sync).
	// See CLAIREON_DISK_RESULTS/cleanup-retention.md.  Benign double-fire on hot-reload.
	if (!bHasSwept)
	{
		bHasSwept = true;
		if (const UClaireonSettings* Settings = UClaireonSettings::Get())
		{
			if (!Settings->bKeepResultSpills)
			{
				FClaireonOutputGate::SweepStaleSpills(Settings->ResultSpillRetentionDays);
			}
		}
	}

	// Log user message
	if (Logger)
		Logger->LogUserMessage(UserText, CurrentConversationId);

	// Append user message to history
	TSharedPtr<FJsonObject> UserMsg = MakeShared<FJsonObject>();
	UserMsg->SetStringField(TEXT("role"), TEXT("user"));

	TSharedPtr<FJsonObject> TextContent = MakeShared<FJsonObject>();
	TextContent->SetStringField(TEXT("type"), TEXT("text"));
	TextContent->SetStringField(TEXT("text"), UserText);

	TArray<TSharedPtr<FJsonValue>> ContentArr;
	ContentArr.Add(MakeShared<FJsonValueObject>(TextContent));
	UserMsg->SetArrayField(TEXT("content"), ContentArr);
	ConversationMessages.Add(UserMsg);

	// Create cancellation token for this request cycle
	ActiveCancelToken = MakeShared<FThreadSafeBool>(false);
	CurrentRetryAttempt = 0;
	bRequestInFlight = true;

	PostToAPI(ActiveCancelToken, 0);
}

void FClaireonAnthropicClient::CancelActiveRequest()
{
	if (!bRequestInFlight)
		return;

	UE_LOG(LogClaireon, Log, TEXT("[REPLClient] Cancel requested"));

	if (ActiveCancelToken.IsValid())
	{
		*ActiveCancelToken = true;
	}
	if (ActiveHttpRequest.IsValid())
	{
		ActiveHttpRequest->CancelRequest();
		ActiveHttpRequest.Reset();
	}
}

void FClaireonAnthropicClient::PostToAPI(
	TSharedPtr<FThreadSafeBool> CancelToken, int32 Depth)
{
	SanitizeConversationHistory();

	// Check cancel before sending
	if (CancelToken.IsValid() && *CancelToken)
	{
		FREPLEvent Ev;
		Ev.Type = EREPLEventType::Cancelled;
		Ev.Text = TEXT("Stopped by user");
		BroadcastEvent(MoveTemp(Ev));
		if (Logger)
			Logger->LogCancelled(TEXT("pre_send"), {}, {}, CurrentConversationId);
		FinalizeTurn();
		return;
	}

	const UClaireonSettings* Settings = UClaireonSettings::Get();

	// Rate limiter: enforce minimum interval between API calls
	double Now = FPlatformTime::Seconds();
	double Elapsed = Now - LastRequestTimestamp;
	if (Elapsed < Settings->MinRequestIntervalSeconds && LastRequestTimestamp > 0.0)
	{
		float RemainingDelay = static_cast<float>(Settings->MinRequestIntervalSeconds - Elapsed);

		TWeakPtr<FClaireonAnthropicClient> WeakSelfRL = AsShared();
		RateLimitTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda(
				[WeakSelfRL, CancelToken, Depth](float) -> bool
		{
			TSharedPtr<FClaireonAnthropicClient> Self = WeakSelfRL.Pin();
			if (!Self.IsValid())
				return false;

			if (CancelToken.IsValid() && *CancelToken)
			{
				FREPLEvent Ev;
				Ev.Type = EREPLEventType::Cancelled;
				Ev.Text = TEXT("Stopped by user");
				Self->BroadcastEvent(MoveTemp(Ev));
				Self->FinalizeTurn();
				return false;
			}

			Self->PostToAPI(CancelToken, Depth);
			return false; // one-shot
		}),
			RemainingDelay);
		return; // Defer — PostToAPI will be called again by the ticker
	}
	LastRequestTimestamp = FPlatformTime::Seconds();

	TSharedPtr<FJsonObject> Body = BuildRequestBody();

	// Log how many tools we're sending so stale-server issues are immediately visible
	{
		const TArray<TSharedPtr<FJsonValue>>* ToolsArr = nullptr;
		Body->TryGetArrayField(TEXT("tools"), ToolsArr);
		int32 ToolCount = ToolsArr ? ToolsArr->Num() : 0;
		UE_LOG(LogClaireon, Log, TEXT("[REPLClient] Sending request: model=%s depth=%d tools=%d"),
			*Settings->ModelId, Depth, ToolCount);
	}

	FString BodyStr = SerializeJson(Body);

	if (Settings->bLogAllToolCalls)
	{
		UE_LOG(LogClaireon, Log, TEXT("[REPLClient][DEBUG] === API REQUEST (depth=%d) ==="), Depth);
		// Log messages array (skip tools to avoid spamming the full schema)
		const TArray<TSharedPtr<FJsonValue>>* MsgArr = nullptr;
		if (Body->TryGetArrayField(TEXT("messages"), MsgArr))
		{
			for (int32 i = 0; i < MsgArr->Num(); ++i)
			{
				FString MsgJson = SerializeJson((*MsgArr)[i]->AsObject());
				// UE_LOG has a max buffer; chunk long strings into 4K segments
				int32 Offset = 0;
				while (Offset < MsgJson.Len())
				{
					UE_LOG(LogClaireon, Log, TEXT("[REPLClient][DEBUG]   messages[%d]%s: %s"),
						i, Offset > 0 ? TEXT(" (cont)") : TEXT(""),
						*MsgJson.Mid(Offset, 4000));
					Offset += 4000;
				}
			}
		}
	}

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request =
		FHttpModule::Get().CreateRequest();
	Request->SetURL(Settings->ApiEndpointUrl);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("content-type"), TEXT("application/json"));
	Request->SetHeader(TEXT("x-api-key"), Settings->AnthropicApiKey);
	Request->SetHeader(TEXT("anthropic-version"), Settings->AnthropicVersion);
	Request->SetContentAsString(BodyStr);
	Request->SetTimeout(Settings->RequestTimeoutSeconds);

	ActiveHttpRequest = Request;

	TWeakPtr<FClaireonAnthropicClient> WeakSelf = AsShared();
	Request->OnProcessRequestComplete().BindLambda(
		[WeakSelf, CancelToken, Depth](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bSuccess)
	{
		// Always marshal to game thread
		TSharedPtr<IHttpResponse, ESPMode::ThreadSafe> RespCopy = Resp;
		AsyncTask(ENamedThreads::GameThread,
			[WeakSelf, Req, RespCopy, bSuccess, CancelToken, Depth]()
		{
			TSharedPtr<FClaireonAnthropicClient> Self = WeakSelf.Pin();
			if (Self.IsValid())
			{
				Self->OnHTTPResponse(Req, RespCopy, bSuccess, CancelToken, Depth);
			}
		});
	});

	Request->ProcessRequest();
}

void FClaireonAnthropicClient::OnHTTPResponse(
	FHttpRequestPtr Request, FHttpResponsePtr Response,
	bool bConnectedSuccessfully, TSharedPtr<FThreadSafeBool> CancelToken, int32 Depth)
{
	ActiveHttpRequest.Reset();

	// Check cancel
	if (CancelToken.IsValid() && *CancelToken)
	{
		FREPLEvent Ev;
		Ev.Type = EREPLEventType::Cancelled;
		Ev.Text = TEXT("Stopped by user");
		BroadcastEvent(MoveTemp(Ev));
		if (Logger)
			Logger->LogCancelled(TEXT("post_response"), {}, {}, CurrentConversationId);
		FinalizeTurn();
		return;
	}

	// --- Error Classification ---
	const int32 HttpCode = Response.IsValid() ? Response->GetResponseCode() : 0;
	const FString ResponseStr = Response.IsValid() ? Response->GetContentAsString() : FString();
	const EAnthropicErrorCategory ErrorCategory = ClassifyHTTPResponse(
		bConnectedSuccessfully, HttpCode, Request->GetStatus(), Request->GetFailureReason());

	if (ErrorCategory != EAnthropicErrorCategory::Success)
	{
		const UClaireonSettings* Settings = UClaireonSettings::Get();

		// Build category description for messages
		FString CategoryDesc;
		switch (ErrorCategory)
		{
			case EAnthropicErrorCategory::Network:
				CategoryDesc = TEXT("Connection failed");
				break;
			case EAnthropicErrorCategory::Timeout:
				CategoryDesc = TEXT("Request timed out");
				break;
			case EAnthropicErrorCategory::RateLimit:
				CategoryDesc = TEXT("Rate limited");
				break;
			case EAnthropicErrorCategory::Overloaded:
				CategoryDesc = TEXT("API overloaded");
				break;
			case EAnthropicErrorCategory::ServerError:
				CategoryDesc = TEXT("Server error");
				break;
			case EAnthropicErrorCategory::AuthError:
				CategoryDesc = TEXT("Authentication failed");
				break;
			case EAnthropicErrorCategory::ClientError:
				CategoryDesc = TEXT("Bad request");
				break;
			default:
				CategoryDesc = TEXT("Unknown error");
				break;
		}

		// Attempt retry if retryable
		if (IsRetryable(ErrorCategory) && CurrentRetryAttempt < Settings->MaxRetryAttempts)
		{
			const FString RetryAfterHeader = Response.IsValid()
				? Response->GetHeader(TEXT("Retry-After"))
				: FString();
			const float Delay = CalculateRetryDelay(
				ErrorCategory, RetryAfterHeader, CurrentRetryAttempt,
				Settings->InitialRetryDelaySeconds, Settings->MaxRetryDelaySeconds);

			// Log the retry attempt
			if (Logger.IsValid())
			{
				Logger->LogRetryAttempt(
					CategoryDesc, HttpCode, CurrentRetryAttempt + 1,
					Delay, CurrentConversationId);
			}

			// Broadcast retry event to widget
			{
				const FString FailureReasonStr = LexToString(Request->GetFailureReason());
				FREPLEvent RetryEv;
				RetryEv.Type = EREPLEventType::Retrying;
				if (HttpCode > 0)
				{
					RetryEv.Text = FString::Printf(
						TEXT("%s (%d). Retrying in %.1fs... (attempt %d/%d)"),
						*CategoryDesc, HttpCode, Delay,
						CurrentRetryAttempt + 1, Settings->MaxRetryAttempts);
				}
				else
				{
					RetryEv.Text = FString::Printf(
						TEXT("%s (%s). Retrying in %.1fs... (attempt %d/%d)"),
						*CategoryDesc, *FailureReasonStr, Delay,
						CurrentRetryAttempt + 1, Settings->MaxRetryAttempts);
				}
				BroadcastEvent(MoveTemp(RetryEv));
			}

			// Schedule retry via FTSTicker
			PendingRetryCancelToken = CancelToken;
			PendingRetryDepth = Depth;
			CurrentRetryAttempt++;

			TWeakPtr<FClaireonAnthropicClient> WeakSelfRetry = AsShared();
			RetryTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda(
					[WeakSelfRetry](float) -> bool
			{
				TSharedPtr<FClaireonAnthropicClient> Self = WeakSelfRetry.Pin();
				if (!Self.IsValid())
					return false;

				// Check cancel before retrying
				if (Self->PendingRetryCancelToken.IsValid() && *Self->PendingRetryCancelToken)
				{
					FREPLEvent Ev;
					Ev.Type = EREPLEventType::Cancelled;
					Ev.Text = TEXT("Stopped by user");
					Self->BroadcastEvent(MoveTemp(Ev));
					Self->FinalizeTurn();
					return false; // one-shot
				}

				Self->PostToAPI(Self->PendingRetryCancelToken, Self->PendingRetryDepth);
				return false; // one-shot
			}),
				Delay);

			return; // Do NOT call FinalizeTurn() — retry is pending
		}

		// Not retryable or retries exhausted — emit error
		{
			FString ErrorMsg;
			if (CurrentRetryAttempt > 0)
			{
				ErrorMsg = FString::Printf(TEXT("Failed after %d retries: %s"),
					CurrentRetryAttempt, *CategoryDesc);
				if (HttpCode > 0)
				{
					ErrorMsg += FString::Printf(TEXT(" (%d)"), HttpCode);
				}
			}
			else
			{
				if (HttpCode > 0)
				{
					ErrorMsg = FString::Printf(TEXT("%s (%d): %s"),
						*CategoryDesc, HttpCode, *ResponseStr.Left(256));
				}
				else
				{
					const FString FailureStr = LexToString(Request->GetFailureReason());
					ErrorMsg = FString::Printf(TEXT("%s (%s)"),
						*CategoryDesc, *FailureStr);
				}
			}

			// Structured error logging
			if (Logger.IsValid())
			{
				const FString VerboseBody = Settings->bVerboseNetworkLogging
					? ResponseStr
					: FString();
				Logger->LogError(
					CategoryDesc, ErrorMsg, HttpCode, VerboseBody,
					LexToString(Request->GetFailureReason()),
					CurrentRetryAttempt, 0.0, CurrentConversationId);
			}

			FREPLEvent Ev;
			Ev.Type = EREPLEventType::Error;
			Ev.Text = ErrorMsg;
			Ev.bIsError = true;
			BroadcastEvent(MoveTemp(Ev));
			FinalizeTurn();
			return;
		}
	}

	// Parse response JSON
	TSharedPtr<FJsonObject> RespJson;
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseStr);
		if (!FJsonSerializer::Deserialize(Reader, RespJson) || !RespJson.IsValid())
		{
			FREPLEvent Ev;
			Ev.Type = EREPLEventType::Error;
			Ev.Text = TEXT("Failed to parse API response JSON");
			Ev.bIsError = true;
			BroadcastEvent(MoveTemp(Ev));
			FinalizeTurn();
			return;
		}
	}

	{
		const UClaireonSettings* DebugSettings = UClaireonSettings::Get();
		if (DebugSettings->bLogAllToolCalls)
		{
			UE_LOG(LogClaireon, Log, TEXT("[REPLClient][DEBUG] === API RESPONSE ==="));
			int32 RespOffset = 0;
			while (RespOffset < ResponseStr.Len())
			{
				UE_LOG(LogClaireon, Log, TEXT("[REPLClient][DEBUG]   %s%s"),
					RespOffset > 0 ? TEXT("(cont) ") : TEXT(""),
					*ResponseStr.Mid(RespOffset, 4000));
				RespOffset += 4000;
			}
		}
	}

	FString StopReason;
	RespJson->TryGetStringField(TEXT("stop_reason"), StopReason);

	// Extract usage
	int32 InputTokens = 0, OutputTokens = 0;
	if (RespJson->HasField(TEXT("usage")))
	{
		TSharedPtr<FJsonObject> Usage = RespJson->GetObjectField(TEXT("usage"));
		if (Usage.IsValid())
		{
			Usage->TryGetNumberField(TEXT("input_tokens"), InputTokens);
			Usage->TryGetNumberField(TEXT("output_tokens"), OutputTokens);
		}
	}

	// Get content array
	const TArray<TSharedPtr<FJsonValue>>* ContentArray = nullptr;
	RespJson->TryGetArrayField(TEXT("content"), ContentArray);

	if (!ContentArray)
	{
		FREPLEvent Ev;
		Ev.Type = EREPLEventType::Error;
		Ev.Text = TEXT("API response missing content array");
		Ev.bIsError = true;
		BroadcastEvent(MoveTemp(Ev));
		FinalizeTurn();
		return;
	}

	// Log assistant message
	if (Logger)
	{
		Logger->LogAssistantMessage(ResponseStr.Left(4096), StopReason,
			InputTokens, OutputTokens, 0.0, CurrentConversationId);
	}

	// Append assistant message to history
	TSharedPtr<FJsonObject> AssistantMsg = MakeShared<FJsonObject>();
	AssistantMsg->SetStringField(TEXT("role"), TEXT("assistant"));
	AssistantMsg->SetArrayField(TEXT("content"), *ContentArray);
	ConversationMessages.Add(AssistantMsg);

	// Broadcast text content blocks immediately
	for (const TSharedPtr<FJsonValue>& ContentVal : *ContentArray)
	{
		if (!ContentVal.IsValid())
			continue;
		const TSharedPtr<FJsonObject> ContentObj = ContentVal->AsObject();
		if (!ContentObj.IsValid())
			continue;

		FString ContentType;
		ContentObj->TryGetStringField(TEXT("type"), ContentType);

		if (ContentType == TEXT("text"))
		{
			FString Text;
			ContentObj->TryGetStringField(TEXT("text"), Text);

			// Parse context markers out of text
			FString Handoff;
			bool bSuggestFresh = false;
			ParseContextMarkers(Text, Handoff, bSuggestFresh);

			if (!Text.IsEmpty())
			{
				FREPLEvent Ev;
				Ev.Type = EREPLEventType::AssistantText;
				Ev.Text = Text;
				BroadcastEvent(MoveTemp(Ev));
			}

			if (bSuggestFresh)
			{
				FREPLEvent Ev;
				Ev.Type = EREPLEventType::FreshContextSuggested;
				Ev.FreshContextHandoff = Handoff;
				BroadcastEvent(MoveTemp(Ev));
			}
		}
	}

	// Handle stop reasons
	if (StopReason == TEXT("end_turn") || StopReason == TEXT("stop_sequence"))
	{
		FREPLEvent Ev;
		Ev.Type = EREPLEventType::Finished;
		BroadcastEvent(MoveTemp(Ev));
		FinalizeTurn();
		return;
	}

	if (StopReason == TEXT("max_tokens"))
	{
		FREPLEvent Ev;
		Ev.Type = EREPLEventType::AssistantText;
		Ev.Text = TEXT("[Response truncated — max tokens reached]");
		BroadcastEvent(MoveTemp(Ev));
		FREPLEvent Ev2;
		Ev2.Type = EREPLEventType::Finished;
		BroadcastEvent(MoveTemp(Ev2));
		FinalizeTurn();
		return;
	}

	// Check depth limit
	const UClaireonSettings* Settings = UClaireonSettings::Get();
	if (Depth >= Settings->ToolUseDepthLimit)
	{
		// The last message is an assistant message with tool_use blocks.
		// Before injecting the "please summarize" text, emit synthetic tool_result
		// blocks for all pending tool_use IDs (required by the Anthropic API).
		if (ConversationMessages.Num() > 0)
		{
			const TSharedPtr<FJsonObject>& LastMsg = ConversationMessages.Last();
			FString LastRole;
			LastMsg->TryGetStringField(TEXT("role"), LastRole);
			if (LastRole == TEXT("assistant"))
			{
				TArray<TSharedPtr<FJsonValue>> SyntheticResults;
				const TArray<TSharedPtr<FJsonValue>>* Content;
				if (LastMsg->TryGetArrayField(TEXT("content"), Content) && Content != nullptr)
				{
					for (const TSharedPtr<FJsonValue>& Val : *Content)
					{
						TSharedPtr<FJsonObject> Obj = Val->AsObject();
						if (!Obj.IsValid())
							continue;
						FString Type;
						Obj->TryGetStringField(TEXT("type"), Type);
						if (Type != TEXT("tool_use"))
							continue;
						FString ToolUseId;
						Obj->TryGetStringField(TEXT("id"), ToolUseId);
						if (ToolUseId.IsEmpty())
							continue;

						TSharedPtr<FJsonObject> SynResult = MakeShared<FJsonObject>();
						SynResult->SetStringField(TEXT("type"), TEXT("tool_result"));
						SynResult->SetStringField(TEXT("tool_use_id"), ToolUseId);
						SynResult->SetStringField(TEXT("content"),
							TEXT("Skipped — tool use depth limit reached"));
						SynResult->SetBoolField(TEXT("is_error"), true);
						SyntheticResults.Add(MakeShared<FJsonValueObject>(SynResult));
					}
				}

				if (SyntheticResults.Num() > 0)
				{
					TSharedPtr<FJsonObject> SynMsg = MakeShared<FJsonObject>();
					SynMsg->SetStringField(TEXT("role"), TEXT("user"));
					SynMsg->SetArrayField(TEXT("content"), SyntheticResults);
					ConversationMessages.Add(SynMsg);
				}
			}
		}

		// Now inject the "please summarize" text message
		TSharedPtr<FJsonObject> LimitMsg = MakeShared<FJsonObject>();
		LimitMsg->SetStringField(TEXT("role"), TEXT("user"));
		TSharedPtr<FJsonObject> LimitContent = MakeShared<FJsonObject>();
		LimitContent->SetStringField(TEXT("type"), TEXT("text"));
		LimitContent->SetStringField(TEXT("text"),
			TEXT("Tool use limit reached. Please summarize what you found and respond without calling any more tools."));
		TArray<TSharedPtr<FJsonValue>> LimitArr;
		LimitArr.Add(MakeShared<FJsonValueObject>(LimitContent));
		LimitMsg->SetArrayField(TEXT("content"), LimitArr);
		ConversationMessages.Add(LimitMsg);
		PostToAPI(CancelToken, Depth + 1);
		return;
	}

	if (StopReason == TEXT("tool_use"))
	{
		// Execute tool_use blocks
		TArray<TSharedPtr<FJsonValue>> ToolResults;
		bool bContinue = ExecuteToolUses(*ContentArray, CancelToken, ToolResults);

		if (!bContinue)
		{
			// Cancelled during tool execution
			FREPLEvent Ev;
			Ev.Type = EREPLEventType::Cancelled;
			Ev.Text = TEXT("Stopped by user");
			BroadcastEvent(MoveTemp(Ev));
			FinalizeTurn();
			return;
		}

		// Append tool results as user message
		TSharedPtr<FJsonObject> ToolResultMsg = MakeShared<FJsonObject>();
		ToolResultMsg->SetStringField(TEXT("role"), TEXT("user"));
		ToolResultMsg->SetArrayField(TEXT("content"), ToolResults);
		ConversationMessages.Add(ToolResultMsg);

		// Check cancel before next loop iteration
		if (CancelToken.IsValid() && *CancelToken)
		{
			FREPLEvent Ev;
			Ev.Type = EREPLEventType::Cancelled;
			Ev.Text = TEXT("Stopped by user");
			BroadcastEvent(MoveTemp(Ev));
			FinalizeTurn();
			return;
		}

		PostToAPI(CancelToken, Depth + 1);
		return;
	}

	// Unknown stop reason — treat as finished
	FREPLEvent Ev;
	Ev.Type = EREPLEventType::Finished;
	BroadcastEvent(MoveTemp(Ev));
	FinalizeTurn();
}

bool FClaireonAnthropicClient::ExecuteToolUses(
	const TArray<TSharedPtr<FJsonValue>>& ContentArray,
	TSharedPtr<FThreadSafeBool> CancelToken,
	TArray<TSharedPtr<FJsonValue>>& OutToolResults)
{
	const UClaireonSettings* Settings = UClaireonSettings::Get();
	TArray<FString> CompletedTools;
	TArray<FString> SkippedTools;

	for (const TSharedPtr<FJsonValue>& ContentVal : ContentArray)
	{
		if (!ContentVal.IsValid())
			continue;
		TSharedPtr<FJsonObject> ContentObj = ContentVal->AsObject();
		if (!ContentObj.IsValid())
			continue;

		FString ContentType;
		ContentObj->TryGetStringField(TEXT("type"), ContentType);
		if (ContentType != TEXT("tool_use"))
			continue;

		FString ToolName, ToolUseId;
		ContentObj->TryGetStringField(TEXT("name"), ToolName);
		ContentObj->TryGetStringField(TEXT("id"), ToolUseId);
		TSharedPtr<FJsonObject> ToolInput = ContentObj->GetObjectField(TEXT("input"));

		// Check cancel before each tool
		if (CancelToken.IsValid() && *CancelToken)
		{
			SkippedTools.Add(ToolName);

			// Add cancelled tool_result to keep history well-formed
			TSharedPtr<FJsonObject> CancelledResult = MakeShared<FJsonObject>();
			CancelledResult->SetStringField(TEXT("type"), TEXT("tool_result"));
			CancelledResult->SetStringField(TEXT("tool_use_id"), ToolUseId);
			CancelledResult->SetStringField(TEXT("content"), TEXT("Cancelled by user"));
			CancelledResult->SetBoolField(TEXT("is_error"), true);
			OutToolResults.Add(MakeShared<FJsonValueObject>(CancelledResult));

			FREPLEvent Ev;
			Ev.Type = EREPLEventType::ToolCallCancelled;
			Ev.ToolName = ToolName;
			Ev.ToolUseId = ToolUseId;
			BroadcastEvent(MoveTemp(Ev));
			continue;
		}

		// Log tool call if debug enabled
		if (Settings->bLogAllToolCalls)
		{
			UE_LOG(LogClaireon, Log, TEXT("[REPLClient][DEBUG] === TOOL CALL: %s ==="), *ToolName);
			FString ArgsJson = SerializeJson(ToolInput);
			int32 ArgsOffset = 0;
			while (ArgsOffset < ArgsJson.Len())
			{
				UE_LOG(LogClaireon, Log, TEXT("[REPLClient][DEBUG]   args%s: %s"),
					ArgsOffset > 0 ? TEXT(" (cont)") : TEXT(""),
					*ArgsJson.Mid(ArgsOffset, 4000));
				ArgsOffset += 4000;
			}
		}

		// Broadcast tool starting
		{
			FString ArgsStr = SerializeJson(ToolInput);
			FREPLEvent Ev;
			Ev.Type = EREPLEventType::ToolCallStarted;
			Ev.ToolName = ToolName;
			Ev.ToolUseId = ToolUseId;
			Ev.ToolArgsJson = ArgsStr;
			BroadcastEvent(MoveTemp(Ev));
		}

		// Execute tool in-process
		double StartTime = FPlatformTime::Seconds();
		IClaireonTool::FToolResult ToolResult;
		bool bFoundTool = false;

		// Propagate the active conversation id to the bridge so downstream spill paths
		// (generic gate + python_execute internal gate) bucket spill files by
		// conv_NNN rather than "default".
		FClaireonBridge::SetCurrentConversationId(CurrentConversationId);

		FClaireonServer* CurrentServer = GetCurrentServer();
		if (CurrentServer)
		{
			const TMap<FString, TSharedPtr<IClaireonTool>>& Tools = CurrentServer->GetTools();
			const TSharedPtr<IClaireonTool>* FoundTool = Tools.Find(ToolName);
			if (FoundTool && FoundTool->IsValid())
			{
				bFoundTool = true;
				ToolResult = (*FoundTool)->Execute(ToolInput);

				// Route generic-tool results through the disk-spill gate.
				// python_execute routes its own stdout/uelog streams internally.
				if (!ToolResult.bIsError && ToolName != TEXT("python_execute"))
				{
					// Forward args so the spill manifest carries originating context.
					ToolResult = FClaireonOutputGate::RouteResult(
						MoveTemp(ToolResult),
						ToolName,
						ToolInput,
						CurrentConversationId,
						EClaireonSpillStreamSet::GenericData);
				}
			}
		}

		double DurationMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

		if (!bFoundTool)
		{
			ToolResult = IClaireonTool::MakeErrorResult(
				FString::Printf(TEXT("Tool not found: %s"), *ToolName));
		}

		// Build plain text for the REPL path from structured FToolResult fields.
		// No XML — the Anthropic API's tool_result already provides the envelope.
		FString ResultText;
		if (ToolResult.bIsError)
		{
			ResultText = ToolResult.ErrorMessage;
		}
		else
		{
			ResultText = ToolResult.Summary;
			if (ToolResult.Data.IsValid())
			{
				// Skip serializing Data for disk-spilled results — the summary and
				// per-stream manifest already carry the path + preview.  Dumping the
				// envelope here would include the manifest a second time.
				bool bIsSpilled = false;
				ToolResult.Data->TryGetBoolField(TEXT("__mcp_spilled__"), bIsSpilled);

				if (!bIsSpilled)
				{
					FString DataJson;
					auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&DataJson);
					FJsonSerializer::Serialize(ToolResult.Data.ToSharedRef(), Writer);
					Writer->Close();
					ResultText += TEXT("\n\n") + DataJson;
				}
			}
		}
		if (!ToolResult.Logs.IsEmpty())
		{
			ResultText += TEXT("\n\nLogs:\n") + ToolResult.Logs;
		}
		for (const FString& Warning : ToolResult.Warnings)
		{
			ResultText += TEXT("\n\nWarning: ") + Warning;
		}

		if (Settings->bLogAllToolCalls)
		{
			UE_LOG(LogClaireon, Log, TEXT("[REPLClient][DEBUG]   result (%s, %.0fms):"),
				ToolResult.bIsError ? TEXT("ERROR") : TEXT("OK"), DurationMs);
			int32 ResOffset = 0;
			while (ResOffset < ResultText.Len())
			{
				UE_LOG(LogClaireon, Log, TEXT("[REPLClient][DEBUG]   %s%s"),
					ResOffset > 0 ? TEXT("(cont) ") : TEXT(""),
					*ResultText.Mid(ResOffset, 4000));
				ResOffset += 4000;
			}
		}

		// Log tool result
		if (Logger)
		{
			Logger->LogToolResult(ToolName, ToolUseId, ResultText,
				DurationMs, ToolResult.bIsError, CurrentConversationId);
		}

		// Add diagnostics entry so the Diagnostics panel sees REPL tool calls
		if (CurrentServer)
		{
			FMCPDiagnosticsEntry DiagEntry;
			DiagEntry.Timestamp = FDateTime::Now();
			DiagEntry.Method = TEXT("tools/call (REPL)");
			DiagEntry.ToolName = ToolName;
			DiagEntry.DurationMs = DurationMs;
			DiagEntry.bIsError = ToolResult.bIsError;
			DiagEntry.RequestBody = SerializeJson(ToolInput).Left(2048);
			DiagEntry.ResponseBody = ResultText.Left(2048);
			CurrentServer->AddDiagnosticsEntry(MoveTemp(DiagEntry));
		}

		// Build tool_result content block — bridge between new FToolResult and Anthropic API
		TSharedPtr<FJsonObject> TextContent = MakeShared<FJsonObject>();
		TextContent->SetStringField(TEXT("type"), TEXT("text"));
		TextContent->SetStringField(TEXT("text"), ResultText);
		TArray<TSharedPtr<FJsonValue>> ToolResultContent;
		ToolResultContent.Add(MakeShared<FJsonValueObject>(TextContent));

		TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
		ResultObj->SetStringField(TEXT("type"), TEXT("tool_result"));
		ResultObj->SetStringField(TEXT("tool_use_id"), ToolUseId);
		ResultObj->SetArrayField(TEXT("content"), ToolResultContent);
		if (ToolResult.bIsError)
		{
			ResultObj->SetBoolField(TEXT("is_error"), true);
		}
		OutToolResults.Add(MakeShared<FJsonValueObject>(ResultObj));
		CompletedTools.Add(ToolName);

		// Broadcast tool completed
		{
			FREPLEvent Ev;
			Ev.Type = EREPLEventType::ToolCallCompleted;
			Ev.ToolName = ToolName;
			Ev.ToolUseId = ToolUseId;
			Ev.Text = ResultText;
			Ev.DurationMs = DurationMs;
			Ev.bIsError = ToolResult.bIsError;
			BroadcastEvent(MoveTemp(Ev));
		}
	}

	// Check if anything was cancelled
	return SkippedTools.Num() == 0 || CompletedTools.Num() > 0;
}

TArray<TSharedPtr<FJsonValue>> FClaireonAnthropicClient::BuildToolDefinitions() const
{
	TArray<TSharedPtr<FJsonValue>> ToolDefs;
	FClaireonServer* CurrentServer = GetCurrentServer();
	if (!CurrentServer)
	{
		UE_LOG(LogClaireon, Warning,
			TEXT("[REPLClient] BuildToolDefinitions: server not running — no tools will be sent to API"));
		return ToolDefs;
	}

	const TMap<FString, TSharedPtr<IClaireonTool>>& Tools = CurrentServer->GetTools();

	// Code Mode: only expose python_execute + tool_search.
	// All other tools are available via the claireon.* Python bridge inside python_execute.

	// 1. python_execute — with embedded type stubs for all tools
	{
		const TSharedPtr<IClaireonTool>* ExecuteTool = Tools.Find(TEXT("python_execute"));
		if (ExecuteTool && ExecuteTool->IsValid())
		{
			FString CategorySummary = FClaireonXmlFormatter::GenerateCategorySummary(Tools);
			FString Description = TEXT(
									  "Run Python code with access to the claireon.* namespace. "
									  "All MCP tools are callable as claireon.<name>(...). "
									  "The code runs in the Unreal Editor Python environment with the 'unreal' module available.\n\n")
				+ CategorySummary;

			TSharedPtr<FJsonObject> ToolDef = MakeShared<FJsonObject>();
			ToolDef->SetStringField(TEXT("name"), TEXT("python_execute"));
			ToolDef->SetStringField(TEXT("description"), Description);
			ToolDef->SetObjectField(TEXT("input_schema"), (*ExecuteTool)->GetInputSchema());
			ToolDefs.Add(MakeShared<FJsonValueObject>(ToolDef));
		}
	}

	// 2. tool_search — for dynamic discovery
	{
		const TSharedPtr<IClaireonTool>* SearchTool = Tools.Find(TEXT("tool_search"));
		if (SearchTool && SearchTool->IsValid())
		{
			TSharedPtr<FJsonObject> ToolDef = MakeShared<FJsonObject>();
			ToolDef->SetStringField(TEXT("name"), TEXT("tool_search"));
			ToolDef->SetStringField(TEXT("description"), (*SearchTool)->GetDescription());
			ToolDef->SetObjectField(TEXT("input_schema"), (*SearchTool)->GetInputSchema());
			ToolDefs.Add(MakeShared<FJsonValueObject>(ToolDef));
		}
	}

	return ToolDefs;
}

void FClaireonAnthropicClient::SanitizeConversationHistory()
{
	for (int32 i = 0; i < ConversationMessages.Num(); ++i)
	{
		const TSharedPtr<FJsonObject>& Msg = ConversationMessages[i];
		FString Role;
		Msg->TryGetStringField(TEXT("role"), Role);
		if (Role != TEXT("assistant"))
			continue;

		// Collect all tool_use IDs from this assistant message
		TArray<FString> ToolUseIds;
		const TArray<TSharedPtr<FJsonValue>>* Content;
		if (!Msg->TryGetArrayField(TEXT("content"), Content) || Content == nullptr)
			continue;
		for (const TSharedPtr<FJsonValue>& Val : *Content)
		{
			TSharedPtr<FJsonObject> Obj = Val->AsObject();
			if (!Obj.IsValid())
				continue;
			FString Type;
			Obj->TryGetStringField(TEXT("type"), Type);
			if (Type != TEXT("tool_use"))
				continue;
			FString Id;
			Obj->TryGetStringField(TEXT("id"), Id);
			if (!Id.IsEmpty())
				ToolUseIds.Add(Id);
		}
		if (ToolUseIds.IsEmpty())
			continue;

		// Find already-present tool_result IDs in the immediately following user message
		TSet<FString> CoveredIds;
		TSharedPtr<FJsonObject> FollowingUserMsg;
		if (i + 1 < ConversationMessages.Num())
		{
			FollowingUserMsg = ConversationMessages[i + 1];
			FString NextRole;
			FollowingUserMsg->TryGetStringField(TEXT("role"), NextRole);
			if (NextRole == TEXT("user"))
			{
				const TArray<TSharedPtr<FJsonValue>>* NextContent;
				if (FollowingUserMsg->TryGetArrayField(TEXT("content"), NextContent)
					&& NextContent != nullptr)
				{
					for (const TSharedPtr<FJsonValue>& Val : *NextContent)
					{
						TSharedPtr<FJsonObject> Obj = Val->AsObject();
						if (!Obj.IsValid())
							continue;
						FString Type;
						Obj->TryGetStringField(TEXT("type"), Type);
						if (Type == TEXT("tool_result"))
						{
							FString Id;
							Obj->TryGetStringField(TEXT("tool_use_id"), Id);
							if (!Id.IsEmpty())
								CoveredIds.Add(Id);
						}
					}
				}
			}
			else
			{
				FollowingUserMsg = nullptr; // not a user message; we'll insert one
			}
		}

		// Build synthetic tool_result blocks for any uncovered IDs
		TArray<TSharedPtr<FJsonValue>> MissingResults;
		for (const FString& Id : ToolUseIds)
		{
			if (CoveredIds.Contains(Id))
				continue;
			TSharedPtr<FJsonObject> SynResult = MakeShared<FJsonObject>();
			SynResult->SetStringField(TEXT("type"), TEXT("tool_result"));
			SynResult->SetStringField(TEXT("tool_use_id"), Id);
			SynResult->SetStringField(TEXT("content"),
				TEXT("Result unavailable — conversation history was repaired"));
			SynResult->SetBoolField(TEXT("is_error"), true);
			MissingResults.Add(MakeShared<FJsonValueObject>(SynResult));
		}
		if (MissingResults.IsEmpty())
			continue;

		UE_LOG(LogClaireon, Warning,
			TEXT("SanitizeConversationHistory: injecting %d synthetic tool_result(s) "
				 "at message index %d"),
			MissingResults.Num(), i + 1);

		if (FollowingUserMsg.IsValid())
		{
			// Prepend missing results to the existing user message's content array
			TArray<TSharedPtr<FJsonValue>> Existing;
			const TArray<TSharedPtr<FJsonValue>>* ExistingPtr;
			if (FollowingUserMsg->TryGetArrayField(TEXT("content"), ExistingPtr)
				&& ExistingPtr != nullptr)
			{
				Existing = *ExistingPtr;
			}
			TArray<TSharedPtr<FJsonValue>> Combined = MissingResults;
			Combined.Append(Existing);
			FollowingUserMsg->SetArrayField(TEXT("content"), Combined);
		}
		else
		{
			// No following user message — insert one
			TSharedPtr<FJsonObject> SynMsg = MakeShared<FJsonObject>();
			SynMsg->SetStringField(TEXT("role"), TEXT("user"));
			SynMsg->SetArrayField(TEXT("content"), MissingResults);
			ConversationMessages.Insert(SynMsg, i + 1);
			++i; // skip the newly inserted message on the next iteration
		}
	}
}

TSharedPtr<FJsonObject> FClaireonAnthropicClient::BuildRequestBody() const
{
	const UClaireonSettings* Settings = UClaireonSettings::Get();

	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("model"), Settings->ModelId);
	Body->SetNumberField(TEXT("max_tokens"), Settings->MaxTokens);
	Body->SetStringField(TEXT("system"), Settings->GetEffectiveSystemPrompt());

	// Messages
	TArray<TSharedPtr<FJsonValue>> MsgArray;
	for (const TSharedPtr<FJsonObject>& Msg : ConversationMessages)
	{
		MsgArray.Add(MakeShared<FJsonValueObject>(Msg));
	}
	Body->SetArrayField(TEXT("messages"), MsgArray);

	// Tools
	Body->SetArrayField(TEXT("tools"), BuildToolDefinitions());

	return Body;
}

void FClaireonAnthropicClient::ParseContextMarkers(FString& InOutText,
	FString& OutHandoff, bool& bOutSuggestFresh)
{
	bOutSuggestFresh = false;
	OutHandoff.Empty();

	// Strip <function_calls>...</function_calls> XML blocks.
	// Haiku sometimes echoes tool calls as old-format XML in text content blocks
	// alongside the proper structured tool_use blocks. Strip them so they don't
	// appear as raw markup in the conversation display.
	{
		const FString OpenTag = TEXT("<function_calls>");
		const FString CloseTag = TEXT("</function_calls>");
		while (true)
		{
			int32 Start = InOutText.Find(OpenTag, ESearchCase::IgnoreCase);
			if (Start == INDEX_NONE)
				break;
			int32 End = InOutText.Find(CloseTag, ESearchCase::IgnoreCase,
				ESearchDir::FromStart, Start);
			if (End == INDEX_NONE)
			{
				// Unclosed tag — strip everything from the open tag to end of string
				InOutText.LeftInline(Start);
				break;
			}
			End += CloseTag.Len();
			InOutText.RemoveAt(Start, End - Start);
			InOutText.TrimStartAndEndInline();
		}
	}

	// Extract [SUGGEST_FRESH_CONTEXT: ...] marker
	{
		const FString StartMarker = TEXT("[SUGGEST_FRESH_CONTEXT:");
		const FString EndMarker = TEXT("]");
		int32 Start = InOutText.Find(StartMarker);
		if (Start != INDEX_NONE)
		{
			int32 End = InOutText.Find(EndMarker, ESearchCase::IgnoreCase,
				ESearchDir::FromStart, Start + StartMarker.Len());
			if (End != INDEX_NONE)
			{
				bOutSuggestFresh = true;
				// Remove marker from displayed text
				int32 MarkerLen = End - Start + EndMarker.Len();
				InOutText.RemoveAt(Start, MarkerLen);
				InOutText.TrimStartAndEndInline();
			}
		}
	}

	// Extract [FRESH_CONTEXT_HANDOFF: ...] marker
	{
		const FString StartMarker = TEXT("[FRESH_CONTEXT_HANDOFF:");
		const FString EndMarker = TEXT("]");
		int32 Start = InOutText.Find(StartMarker);
		if (Start != INDEX_NONE)
		{
			int32 End = InOutText.Find(EndMarker, ESearchCase::IgnoreCase,
				ESearchDir::FromStart, Start + StartMarker.Len());
			if (End != INDEX_NONE)
			{
				int32 ContentStart = Start + StartMarker.Len();
				OutHandoff = InOutText.Mid(ContentStart, End - ContentStart).TrimStartAndEnd();
				int32 MarkerLen = End - Start + EndMarker.Len();
				InOutText.RemoveAt(Start, MarkerLen);
				InOutText.TrimStartAndEndInline();
			}
		}
	}
}

FString FClaireonAnthropicClient::SerializeJson(const TSharedPtr<FJsonObject>& Obj)
{
	if (!Obj.IsValid())
		return TEXT("{}");
	FString Out;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
	return Out;
}

void FClaireonAnthropicClient::BroadcastEvent(FREPLEvent&& Event)
{
	OnREPLEvent.Broadcast(Event);
}

void FClaireonAnthropicClient::FinalizeTurn()
{
	// Remove any pending retry/rate-limit tickers
	if (RetryTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(RetryTickerHandle);
		RetryTickerHandle.Reset();
	}
	if (RateLimitTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(RateLimitTickerHandle);
		RateLimitTickerHandle.Reset();
	}

	ActiveCancelToken.Reset();
	ActiveHttpRequest.Reset();
	bRequestInFlight = false;
	CurrentRetryAttempt = 0;
}

int32 FClaireonAnthropicClient::GetApproximateTokenCount() const
{
	int32 CharCount = 0;

	// Conversation messages
	for (const TSharedPtr<FJsonObject>& Msg : ConversationMessages)
	{
		CharCount += SerializeJson(Msg).Len();
	}

	// System prompt and tool definitions are sent with every request and
	// count against the context window. Include them in the estimate.
	const UClaireonSettings* Settings = UClaireonSettings::Get();
	CharCount += Settings->GetEffectiveSystemPrompt().Len();

	// Tool definitions are sent with every request and count against the context window.
	TArray<TSharedPtr<FJsonValue>> ToolDefs = BuildToolDefinitions();
	for (const TSharedPtr<FJsonValue>& ToolDef : ToolDefs)
	{
		TSharedPtr<FJsonObject> Obj = ToolDef->AsObject();
		if (Obj.IsValid())
		{
			CharCount += SerializeJson(Obj).Len();
		}
	}

	return CharCount / 4; // rough approximation: 4 chars per token
}

int32 FClaireonAnthropicClient::GetAPIToolCount() const
{
	return BuildToolDefinitions().Num();
}

EAnthropicErrorCategory FClaireonAnthropicClient::ClassifyHTTPResponse(
	bool bConnectedSuccessfully, int32 HttpStatusCode,
	EHttpRequestStatus::Type RequestStatus, EHttpFailureReason FailureReason)
{
	// Connection never established
	if (!bConnectedSuccessfully || RequestStatus == EHttpRequestStatus::Failed)
	{
		if (FailureReason == EHttpFailureReason::TimedOut)
		{
			return EAnthropicErrorCategory::Timeout;
		}
		return EAnthropicErrorCategory::Network;
	}

	// Successful connection — classify by HTTP status code
	switch (HttpStatusCode)
	{
		case 200:
			return EAnthropicErrorCategory::Success;
		case 429:
			return EAnthropicErrorCategory::RateLimit;
		case 529:
			return EAnthropicErrorCategory::Overloaded;
		case 500:
		case 502:
		case 503:
		case 504:
			return EAnthropicErrorCategory::ServerError;
		case 401:
		case 403:
			return EAnthropicErrorCategory::AuthError;
		case 400:
			return EAnthropicErrorCategory::ClientError;
		default:
			if (HttpStatusCode >= 500)
				return EAnthropicErrorCategory::ServerError;
			return EAnthropicErrorCategory::ClientError;
	}
}

bool FClaireonAnthropicClient::IsRetryable(EAnthropicErrorCategory Category)
{
	switch (Category)
	{
		case EAnthropicErrorCategory::Network:
		case EAnthropicErrorCategory::Timeout:
		case EAnthropicErrorCategory::RateLimit:
		case EAnthropicErrorCategory::Overloaded:
		case EAnthropicErrorCategory::ServerError:
			return true;
		default:
			return false;
	}
}

float FClaireonAnthropicClient::CalculateRetryDelay(EAnthropicErrorCategory Category,
	const FString& RetryAfterHeader, int32 Attempt,
	float InitialDelay, float MaxDelay)
{
	float Delay = 0.0f;

	// For 429, try Retry-After header first
	if (Category == EAnthropicErrorCategory::RateLimit && !RetryAfterHeader.IsEmpty())
	{
		Delay = FCString::Atof(*RetryAfterHeader);
		if (Delay > 0.0f)
		{
			return FMath::Min(Delay, MaxDelay);
		}
		// Fall through to exponential backoff if parsing failed
	}

	// Exponential backoff with jitter
	float Base = InitialDelay * FMath::Pow(2.0f, static_cast<float>(Attempt));
	float Jitter = FMath::FRandRange(0.0f, InitialDelay * 0.5f);
	Delay = FMath::Min(Base + Jitter, MaxDelay);
	return Delay;
}

void FClaireonAnthropicClient::Shutdown()
{
	if (RetryTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(RetryTickerHandle);
		RetryTickerHandle.Reset();
	}
	if (RateLimitTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(RateLimitTickerHandle);
		RateLimitTickerHandle.Reset();
	}
	if (ActiveCancelToken.IsValid())
	{
		*ActiveCancelToken = true;
	}
	if (ActiveHttpRequest.IsValid())
	{
		ActiveHttpRequest->CancelRequest();
		ActiveHttpRequest.Reset();
	}
	bRequestInFlight = false;
}
