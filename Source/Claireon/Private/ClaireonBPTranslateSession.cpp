// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonBPTranslateSession.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/SecureHash.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

TMap<FString, TSharedPtr<FClaireonBPTranslateSession>> FClaireonBPTranslateSession::SessionCache;

bool FClaireonBPTranslateSession::SaveToFile(const FString& FilePath) const
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("session_id"), SessionId);
	Root->SetStringField(TEXT("created"), Created);
	Root->SetStringField(TEXT("target_module"), TargetModule);
	Root->SetStringField(TEXT("target_directory"), TargetDirectory);
	Root->SetStringField(TEXT("ini_section"), IniSection);
	Root->SetStringField(TEXT("status"), Status);

	// Serialize blueprints
	TSharedPtr<FJsonObject> BPsObj = MakeShared<FJsonObject>();
	for (const auto& BPPair : Blueprints)
	{
		TSharedPtr<FJsonObject> BPObj = MakeShared<FJsonObject>();
		BPObj->SetStringField(TEXT("generated_header"), BPPair.Value.GeneratedHeader);
		BPObj->SetStringField(TEXT("generated_source"), BPPair.Value.GeneratedSource);
		BPObj->SetStringField(TEXT("header_hash"), BPPair.Value.HeaderHash);
		BPObj->SetStringField(TEXT("source_hash"), BPPair.Value.SourceHash);
		BPObj->SetNumberField(TEXT("total_nodes"), BPPair.Value.TotalNodes);
		BPObj->SetNumberField(TEXT("implemented_nodes"), BPPair.Value.ImplementedNodes);
		BPObj->SetNumberField(TEXT("skipped_nodes"), BPPair.Value.SkippedNodes);

		// Serialize nodes
		TSharedPtr<FJsonObject> NodesObj = MakeShared<FJsonObject>();
		for (const auto& NodePair : BPPair.Value.Nodes)
		{
			TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
			NodeObj->SetStringField(TEXT("status"), NodePair.Value.Status);
			NodeObj->SetStringField(TEXT("type"), NodePair.Value.Type);
			NodeObj->SetStringField(TEXT("name"), NodePair.Value.Name);
			NodesObj->SetObjectField(NodePair.Key, NodeObj);
		}
		BPObj->SetObjectField(TEXT("nodes"), NodesObj);

		BPsObj->SetObjectField(BPPair.Key, BPObj);
	}
	Root->SetObjectField(TEXT("blueprints"), BPsObj);

	// Serialize cross references
	TArray<TSharedPtr<FJsonValue>> XRefsArray;
	for (const FClaireonBPTranslateCrossRef& XRef : CrossReferences)
	{
		TSharedPtr<FJsonObject> XRefObj = MakeShared<FJsonObject>();
		XRefObj->SetStringField(TEXT("from_bp"), XRef.FromBP);
		XRefObj->SetStringField(TEXT("from_node"), XRef.FromNode);
		XRefObj->SetStringField(TEXT("to_bp"), XRef.ToBP);
		XRefObj->SetStringField(TEXT("to_class"), XRef.ToClass);
		XRefsArray.Add(MakeShared<FJsonValueObject>(XRefObj));
	}
	Root->SetArrayField(TEXT("cross_references"), XRefsArray);

	// Serialize to string
	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

	return FFileHelper::SaveStringToFile(OutputString, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

FClaireonBPTranslateSession FClaireonBPTranslateSession::LoadFromFile(const FString& FilePath)
{
	FClaireonBPTranslateSession Session;

	FString JsonContent;
	if (!FFileHelper::LoadFileToString(JsonContent, *FilePath))
	{
		return Session;
	}

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonContent);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return Session;
	}

	Session.SessionId = Root->GetStringField(TEXT("session_id"));
	Session.Created = Root->GetStringField(TEXT("created"));
	Session.TargetModule = Root->GetStringField(TEXT("target_module"));
	Session.TargetDirectory = Root->GetStringField(TEXT("target_directory"));
	Session.IniSection = Root->GetStringField(TEXT("ini_section"));
	Session.Status = Root->GetStringField(TEXT("status"));
	Session.SessionFilePath = FilePath;

	// Deserialize blueprints
	const TSharedPtr<FJsonObject>* BPsObjPtr = nullptr;
	if (Root->TryGetObjectField(TEXT("blueprints"), BPsObjPtr) && BPsObjPtr)
	{
		for (const auto& BPPair : (*BPsObjPtr)->Values)
		{
			const TSharedPtr<FJsonObject>& BPObj = BPPair.Value->AsObject();
			if (!BPObj.IsValid()) continue;

			FClaireonBPTranslateBlueprintState BPState;
			BPState.GeneratedHeader = BPObj->GetStringField(TEXT("generated_header"));
			BPState.GeneratedSource = BPObj->GetStringField(TEXT("generated_source"));
			BPState.HeaderHash = BPObj->GetStringField(TEXT("header_hash"));
			BPState.SourceHash = BPObj->GetStringField(TEXT("source_hash"));
			BPState.TotalNodes = static_cast<int32>(BPObj->GetNumberField(TEXT("total_nodes")));
			BPState.ImplementedNodes = static_cast<int32>(BPObj->GetNumberField(TEXT("implemented_nodes")));
			BPState.SkippedNodes = static_cast<int32>(BPObj->GetNumberField(TEXT("skipped_nodes")));

			const TSharedPtr<FJsonObject>* NodesObjPtr = nullptr;
			if (BPObj->TryGetObjectField(TEXT("nodes"), NodesObjPtr) && NodesObjPtr)
			{
				for (const auto& NodePair : (*NodesObjPtr)->Values)
				{
					const TSharedPtr<FJsonObject>& NodeObj = NodePair.Value->AsObject();
					if (!NodeObj.IsValid()) continue;

					FClaireonBPTranslateNodeStatus NodeStatus;
					NodeStatus.Status = NodeObj->GetStringField(TEXT("status"));
					NodeStatus.Type = NodeObj->GetStringField(TEXT("type"));
					NodeStatus.Name = NodeObj->GetStringField(TEXT("name"));
					BPState.Nodes.Add(NodePair.Key, NodeStatus);
				}
			}

			Session.Blueprints.Add(BPPair.Key, BPState);
		}
	}

	// Deserialize cross references
	const TArray<TSharedPtr<FJsonValue>>* XRefsPtr = nullptr;
	if (Root->TryGetArrayField(TEXT("cross_references"), XRefsPtr) && XRefsPtr)
	{
		for (const TSharedPtr<FJsonValue>& XRefVal : *XRefsPtr)
		{
			const TSharedPtr<FJsonObject>& XRefObj = XRefVal->AsObject();
			if (!XRefObj.IsValid()) continue;

			FClaireonBPTranslateCrossRef XRef;
			XRef.FromBP = XRefObj->GetStringField(TEXT("from_bp"));
			XRef.FromNode = XRefObj->GetStringField(TEXT("from_node"));
			XRef.ToBP = XRefObj->GetStringField(TEXT("to_bp"));
			XRef.ToClass = XRefObj->GetStringField(TEXT("to_class"));
			Session.CrossReferences.Add(XRef);
		}
	}

	return Session;
}

void FClaireonBPTranslateSession::UpdateNodeStatus(const FString& BlueprintPath, const FString& NodeGuid, const FString& NewStatus)
{
	FClaireonBPTranslateBlueprintState* BPState = Blueprints.Find(BlueprintPath);
	if (!BPState)
	{
		return;
	}

	FClaireonBPTranslateNodeStatus* NodeStatus = BPState->Nodes.Find(NodeGuid);
	if (!NodeStatus)
	{
		return;
	}

	FString OldStatus = NodeStatus->Status;
	NodeStatus->Status = NewStatus;

	// Update counts
	if (OldStatus == TEXT("pending"))
	{
		// Was pending, now changing
	}
	else if (OldStatus == TEXT("implemented"))
	{
		BPState->ImplementedNodes--;
	}
	else if (OldStatus == TEXT("skipped"))
	{
		BPState->SkippedNodes--;
	}

	if (NewStatus == TEXT("implemented"))
	{
		BPState->ImplementedNodes++;
	}
	else if (NewStatus == TEXT("skipped"))
	{
		BPState->SkippedNodes++;
	}
}

TSharedPtr<FJsonObject> FClaireonBPTranslateSession::GetCompletionStats() const
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("session_id"), SessionId);
	Result->SetStringField(TEXT("status"), Status);

	TSharedPtr<FJsonObject> BPsObj = MakeShared<FJsonObject>();
	int32 OverallTotal = 0;
	int32 OverallImplemented = 0;
	int32 OverallSkipped = 0;
	int32 OverallPending = 0;

	for (const auto& BPPair : Blueprints)
	{
		const FClaireonBPTranslateBlueprintState& BPState = BPPair.Value;
		int32 Pending = BPState.TotalNodes - BPState.ImplementedNodes - BPState.SkippedNodes;

		TSharedPtr<FJsonObject> BPObj = MakeShared<FJsonObject>();
		BPObj->SetNumberField(TEXT("total_nodes"), BPState.TotalNodes);
		BPObj->SetNumberField(TEXT("implemented"), BPState.ImplementedNodes);
		BPObj->SetNumberField(TEXT("skipped"), BPState.SkippedNodes);
		BPObj->SetNumberField(TEXT("pending"), Pending);
		double CompletionPct = BPState.TotalNodes > 0
			? (static_cast<double>(BPState.ImplementedNodes + BPState.SkippedNodes) / BPState.TotalNodes * 100.0)
			: 100.0;
		BPObj->SetNumberField(TEXT("completion_pct"), CompletionPct);

		// List pending nodes
		TArray<TSharedPtr<FJsonValue>> PendingNodes;
		for (const auto& NodePair : BPState.Nodes)
		{
			if (NodePair.Value.Status == TEXT("pending"))
			{
				TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
				NodeObj->SetStringField(TEXT("guid"), NodePair.Key);
				NodeObj->SetStringField(TEXT("type"), NodePair.Value.Type);
				NodeObj->SetStringField(TEXT("name"), NodePair.Value.Name);
				PendingNodes.Add(MakeShared<FJsonValueObject>(NodeObj));
			}
		}
		BPObj->SetArrayField(TEXT("pending_nodes"), PendingNodes);

		BPsObj->SetObjectField(BPPair.Key, BPObj);

		OverallTotal += BPState.TotalNodes;
		OverallImplemented += BPState.ImplementedNodes;
		OverallSkipped += BPState.SkippedNodes;
		OverallPending += Pending;
	}

	Result->SetObjectField(TEXT("blueprints"), BPsObj);

	TSharedPtr<FJsonObject> OverallObj = MakeShared<FJsonObject>();
	OverallObj->SetNumberField(TEXT("total_nodes"), OverallTotal);
	OverallObj->SetNumberField(TEXT("implemented"), OverallImplemented);
	OverallObj->SetNumberField(TEXT("skipped"), OverallSkipped);
	OverallObj->SetNumberField(TEXT("pending"), OverallPending);
	double OverallPct = OverallTotal > 0
		? (static_cast<double>(OverallImplemented + OverallSkipped) / OverallTotal * 100.0)
		: 100.0;
	OverallObj->SetNumberField(TEXT("completion_pct"), OverallPct);
	Result->SetObjectField(TEXT("overall"), OverallObj);

	return Result;
}

FString FClaireonBPTranslateSession::ComputeFileHash(const FString& FilePath)
{
	TArray<uint8> FileBytes;
	if (!FFileHelper::LoadFileToArray(FileBytes, *FilePath))
	{
		return FString();
	}

	FSHAHash Hash;
	FSHA1 Sha1;
	Sha1.Update(FileBytes.GetData(), FileBytes.Num());
	Sha1.Final();
	Sha1.GetHash(Hash.Hash);

	return TEXT("sha1:") + Hash.ToString().ToLower();
}

TSharedPtr<FClaireonBPTranslateSession> FClaireonBPTranslateSession::ResolveSession(
	const FString& SessionId, const FString& SessionFile, FString& OutError)
{
	// Step 1: Check in-memory cache
	if (TSharedPtr<FClaireonBPTranslateSession>* Cached = SessionCache.Find(SessionId))
	{
		return *Cached;
	}

	// Step 2: Load from explicit file
	if (!SessionFile.IsEmpty())
	{
		FString AbsPath = SessionFile;
		AbsPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), AbsPath);

		if (FPaths::FileExists(AbsPath))
		{
			TSharedPtr<FClaireonBPTranslateSession> Session = MakeShared<FClaireonBPTranslateSession>(LoadFromFile(AbsPath));
			if (!Session->SessionId.IsEmpty())
			{
				Session->SessionFilePath = AbsPath;
				CacheSession(Session);
				return Session;
			}
		}
		OutError = FString::Printf(TEXT("Session file not found or invalid: %s"), *SessionFile);
		return nullptr;
	}

	// Step 3: Scan Saved/BPTranslator/ directory for matching session files
	FString ScanDir = FPaths::Combine(FPaths::ProjectDir(), TEXT("Saved"), TEXT("BPTranslator"));
	TArray<FString> FoundFiles;

	// Also scan project root and common directories
	TArray<FString> ScanDirs;
	ScanDirs.Add(ScanDir);
	ScanDirs.Add(FPaths::ProjectDir());

	for (const FString& Dir : ScanDirs)
	{
		IFileManager::Get().FindFilesRecursive(FoundFiles, *Dir, TEXT("*.json"), true, false, false);
	}

	FString TargetFilename = FString::Printf(TEXT(".bp_translate_session_%s.json"), *SessionId);

	for (const FString& FilePath : FoundFiles)
	{
		if (FilePath.Contains(TargetFilename))
		{
			TSharedPtr<FClaireonBPTranslateSession> Session = MakeShared<FClaireonBPTranslateSession>(LoadFromFile(FilePath));
			if (!Session->SessionId.IsEmpty() && Session->SessionId == SessionId)
			{
				Session->SessionFilePath = FilePath;
				CacheSession(Session);
				return Session;
			}
		}
	}

	OutError = FString::Printf(
		TEXT("Session not found: %s. Provide session_file path or re-run scaffold."), *SessionId);
	return nullptr;
}

void FClaireonBPTranslateSession::CacheSession(TSharedPtr<FClaireonBPTranslateSession> Session)
{
	if (Session.IsValid() && !Session->SessionId.IsEmpty())
	{
		SessionCache.Add(Session->SessionId, Session);
	}
}
