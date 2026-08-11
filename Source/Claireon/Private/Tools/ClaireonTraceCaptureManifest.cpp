// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonTraceCaptureManifest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GenericPlatform/GenericPlatformMisc.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Channel.h"
#include "TraceServices/Model/Diagnostics.h"
#include "TraceServices/Model/TimingProfiler.h"

namespace ClaireonTraceCaptureManifestInternal
{

// File-local discriminator prefix (CaptureManifest_) per module convention.

/**
 * Named-events state is deliberately a tri-state, not a bool.
 *
 * Absence of -statnamedevents from the command line does NOT prove the flag was
 * off: GCycleStatsShouldEmitNamedEvents can be set programmatically at runtime,
 * which is exactly what pie_trace_start now does. Reporting "disabled" in that
 * case would be a confident wrong answer about whether the capture is complete,
 * which is the same class of defect the manifest exists to remove.
 */
const TCHAR* CaptureManifest_NamedEventsState(const TraceServices::FSessionInfo& SessionInfo,
	bool bSessionInfoAvailable)
{
	if (!bSessionInfoAvailable)
	{
		return TEXT("unknown");
	}
	if (SessionInfo.CommandLine.Contains(TEXT("-statnamedevents"), ESearchCase::IgnoreCase))
	{
		return TEXT("enabled");
	}
	// Could still have been forced on in-process; see the comment above.
	return TEXT("unknown");
}

} // namespace ClaireonTraceCaptureManifestInternal

namespace ClaireonTraceCaptureManifest
{

TSharedPtr<FJsonObject> Build(const TraceServices::IAnalysisSession& Session)
{
	using namespace ClaireonTraceCaptureManifestInternal;

	TSharedPtr<FJsonObject> Manifest = MakeShared<FJsonObject>();

	// ---- Channels ---------------------------------------------------------
	// Which channels the capture was taken with is the first thing that
	// explains a sparse scope population.
	if (const TraceServices::IChannelProvider* ChannelProvider = TraceServices::ReadChannelProvider(Session))
	{
		TArray<TSharedPtr<FJsonValue>> ChannelsArray;
		const TArray<TraceServices::FChannelEntry>& Channels = ChannelProvider->GetChannels();
		ChannelsArray.Reserve(Channels.Num());
		for (const TraceServices::FChannelEntry& Channel : Channels)
		{
			TSharedPtr<FJsonObject> ChannelObj = MakeShared<FJsonObject>();
			ChannelObj->SetStringField(TEXT("name"), Channel.Name);
			ChannelObj->SetBoolField(TEXT("enabled"), Channel.bIsEnabled);
			ChannelsArray.Add(MakeShared<FJsonValueObject>(ChannelObj));
		}
		Manifest->SetArrayField(TEXT("channels"), ChannelsArray);
		Manifest->SetNumberField(TEXT("channel_count"), ChannelsArray.Num());
	}
	else
	{
		Manifest->SetField(TEXT("channels"), MakeShared<FJsonValueNull>());
		Manifest->SetStringField(TEXT("channels_unavailable_reason"),
			TEXT("Channel provider not present in this trace"));
	}

	// ---- Session info -----------------------------------------------------
	// ConfigurationType and TargetType explain sparse scopes independently of
	// channels: a Shipping or Server capture simply has fewer instrumented
	// scopes compiled in.
	bool bSessionInfoAvailable = false;
	if (const TraceServices::IDiagnosticsProvider* DiagnosticsProvider = TraceServices::ReadDiagnosticsProvider(Session))
	{
		bSessionInfoAvailable = DiagnosticsProvider->IsSessionInfoAvailable();
		if (bSessionInfoAvailable)
		{
			const TraceServices::FSessionInfo& SessionInfo = DiagnosticsProvider->GetSessionInfo();

			TSharedPtr<FJsonObject> InfoObj = MakeShared<FJsonObject>();
			InfoObj->SetStringField(TEXT("platform"), SessionInfo.Platform);
			InfoObj->SetStringField(TEXT("app_name"), SessionInfo.AppName);
			InfoObj->SetStringField(TEXT("project_name"), SessionInfo.ProjectName);
			InfoObj->SetStringField(TEXT("branch"), SessionInfo.Branch);
			InfoObj->SetStringField(TEXT("build_version"), SessionInfo.BuildVersion);
			InfoObj->SetNumberField(TEXT("changelist"), static_cast<double>(SessionInfo.Changelist));
			InfoObj->SetStringField(TEXT("configuration_type"), LexToString(SessionInfo.ConfigurationType));
			InfoObj->SetStringField(TEXT("target_type"), LexToString(SessionInfo.TargetType));
			InfoObj->SetStringField(TEXT("command_line"), SessionInfo.CommandLine);
			Manifest->SetObjectField(TEXT("session_info"), InfoObj);

			Manifest->SetStringField(TEXT("stat_named_events"),
				CaptureManifest_NamedEventsState(SessionInfo, true));
		}
	}

	if (!bSessionInfoAvailable)
	{
		Manifest->SetField(TEXT("session_info"), MakeShared<FJsonValueNull>());
		Manifest->SetStringField(TEXT("stat_named_events"), TEXT("unknown"));
	}

	// ---- GPU presence -----------------------------------------------------
	// Event counts, never GetGpuTimelineIndex: that call hands back a fixed slot
	// index and unconditionally returns true, so it cannot answer whether the
	// capture actually contains GPU work.
	uint64 GpuEventCount = 0;
	bool bGpuPresent = false;
	if (const TraceServices::ITimingProfilerProvider* TimingProvider = TraceServices::ReadTimingProfilerProvider(Session))
	{
		uint32 GpuTimelineIndex = 0;
		if (TimingProvider->GetGpuTimelineIndex(GpuTimelineIndex))
		{
			TimingProvider->ReadTimeline(GpuTimelineIndex,
				[&GpuEventCount](const TraceServices::ITimingProfilerProvider::Timeline& Timeline)
			{
				GpuEventCount = Timeline.GetEventCount();
			});
		}
		bGpuPresent = GpuEventCount > 0;
		Manifest->SetNumberField(TEXT("timeline_count"), static_cast<double>(TimingProvider->GetTimelineCount()));
	}

	Manifest->SetBoolField(TEXT("gpu_timelines_present"), bGpuPresent);
	Manifest->SetNumberField(TEXT("gpu_event_count"), static_cast<double>(GpuEventCount));

	return Manifest;
}

} // namespace ClaireonTraceCaptureManifest
