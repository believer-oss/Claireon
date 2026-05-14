// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_ProxyTableInspect.h"
#include "Tools/ClaireonProxyTableHelpers.h"
#include "Tools/ClaireonAnimEditToolBase.h"
#include "ProxyTable.h"
#include "ProxyAsset.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	// Augment a serialized entry with provenance info from inheritance flattening.
	void AddProvenance(TSharedPtr<FJsonObject> Entry, const UProxyTable* SourceTable, int32 OriginalIndex, int32 Depth)
	{
		TSharedPtr<FJsonObject> Prov = MakeShared<FJsonObject>();
		Prov->SetStringField(TEXT("table_path"), SourceTable->GetPathName());
		Prov->SetStringField(TEXT("table_name"), SourceTable->GetName());
		Prov->SetNumberField(TEXT("original_index"), OriginalIndex);
		Prov->SetNumberField(TEXT("depth"), Depth);
		Entry->SetObjectField(TEXT("_resolved_from"), Prov);
	}

	bool EntryMatchesFind(const FProxyEntry& Entry, const FString& Needle)
	{
#if WITH_EDITORONLY_DATA
		if (Entry.Proxy)
		{
			if (Entry.Proxy->GetName().Contains(Needle)) { return true; }
			if (Entry.Proxy->GetPathName().Contains(Needle)) { return true; }
		}
#endif
		return false;
	}
}

FString ClaireonTool_ProxyTableInspect::GetName() const { return TEXT("claireon.proxytable_inspect"); }

FString ClaireonTool_ProxyTableInspect::GetDescription() const
{
	return TEXT("Inspect a ProxyTable. Returns local entries (proxy ref + value + output struct), "
		"the inheritance chain, and optionally the resolved entry set across that chain. "
		"include_inherited='flat' walks parents and merges entries with provenance ('_resolved_from'). "
		"include_inherited='chain' returns the per-level entries in order. "
		"find_proxy='<name-or-path-substring>' filters to a single matching entry (composes with include_inherited).");
}

TSharedPtr<FJsonObject> ClaireonTool_ProxyTableInspect::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the ProxyTable asset"), true);
	S.AddEnum(TEXT("detail_level"), TEXT("Verbosity: 'summary' or 'full' (default)"),
		{TEXT("summary"), TEXT("full")});
	S.AddEnum(TEXT("include_inherited"),
		TEXT("Inheritance handling: 'none' (default; local entries only), 'flat' (resolved set with provenance), 'chain' (per-level entries)"),
		{TEXT("none"), TEXT("flat"), TEXT("chain")});
	S.AddString(TEXT("find_proxy"), TEXT("Substring of proxy name or path; filter to matching entries only."));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ProxyTableInspect::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	FString Error;
	UProxyTable* ProxyTable = ClaireonProxyTableHelpers::LoadProxyTableAsset(AssetPath, Error);
	if (!ProxyTable)
	{
		return MakeErrorResult(Error);
	}

	FString DetailLevel = TEXT("full");
	Arguments->TryGetStringField(TEXT("detail_level"), DetailLevel);

	FString IncludeInherited = TEXT("none");
	Arguments->TryGetStringField(TEXT("include_inherited"), IncludeInherited);
	const bool bWantFlat = IncludeInherited == TEXT("flat");
	const bool bWantChain = IncludeInherited == TEXT("chain");

	FString FindProxy;
	Arguments->TryGetStringField(TEXT("find_proxy"), FindProxy);
	const bool bHasFind = !FindProxy.IsEmpty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), ProxyTable->GetPathName());
	Data->SetStringField(TEXT("asset_name"), ProxyTable->GetName());

#if WITH_EDITORONLY_DATA
	const int32 LocalEntryCount = ProxyTable->Entries.Num();
	Data->SetNumberField(TEXT("entry_count"), LocalEntryCount);

	// Direct parents (always emitted for orientation; the walk below is opt-in).
	if (ProxyTable->InheritEntriesFrom.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> InheritArray;
		for (const auto& Parent : ProxyTable->InheritEntriesFrom)
		{
			if (!Parent) { continue; }
			TSharedPtr<FJsonObject> ParentObj = MakeShared<FJsonObject>();
			ParentObj->SetStringField(TEXT("name"), Parent->GetName());
			ParentObj->SetStringField(TEXT("path"), Parent->GetPathName());
			InheritArray.Add(MakeShared<FJsonValueObject>(ParentObj));
		}
		Data->SetArrayField(TEXT("inherits_from"), InheritArray);
	}

	if (DetailLevel != TEXT("summary"))
	{
		// Local entries (always emitted; the inheritance walk overlays/extends, doesn't replace).
		TArray<TSharedPtr<FJsonValue>> EntriesArray;
		for (int32 i = 0; i < LocalEntryCount; ++i)
		{
			const FProxyEntry& Entry = ProxyTable->Entries[i];
			if (bHasFind && !EntryMatchesFind(Entry, FindProxy)) { continue; }
			TSharedPtr<FJsonObject> EntryObj = ClaireonProxyTableHelpers::SerializeProxyEntry(Entry, i);
			EntriesArray.Add(MakeShared<FJsonValueObject>(EntryObj));
		}
		Data->SetArrayField(TEXT("entries"), EntriesArray);

		if (bWantFlat || bWantChain)
		{
			// BFS over InheritEntriesFrom with cycle guard.
			struct FWalkLevel
			{
				UProxyTable* Table;
				int32 Depth;
			};
			TArray<FWalkLevel> Queue;
			TSet<FString> Visited;
			Visited.Add(ProxyTable->GetPathName()); // self handled above

			for (const auto& Parent : ProxyTable->InheritEntriesFrom)
			{
				if (Parent) { Queue.Add({ Parent, 1 }); }
			}

			TArray<TSharedPtr<FJsonValue>> ResolvedEntries; // for "flat"
			TArray<TSharedPtr<FJsonValue>> ChainLevels;     // for "chain"
			TSet<FString> ResolvedKeys; // proxy path => already taken (child wins).
			// Seed with local entries' proxy paths so children block parents.
			for (const FProxyEntry& E : ProxyTable->Entries)
			{
				if (E.Proxy)
				{
					ResolvedKeys.Add(E.Proxy->GetPathName());
					if (bWantFlat)
					{
						const int32 Idx = ResolvedEntries.Num();
						TSharedPtr<FJsonObject> EObj = ClaireonProxyTableHelpers::SerializeProxyEntry(E, Idx);
						AddProvenance(EObj, ProxyTable, Idx, 0);
						if (!bHasFind || EntryMatchesFind(E, FindProxy))
						{
							ResolvedEntries.Add(MakeShared<FJsonValueObject>(EObj));
						}
					}
				}
			}

			int32 Head = 0;
			while (Head < Queue.Num())
			{
				FWalkLevel L = Queue[Head++];
				if (!L.Table) { continue; }
				const FString TPath = L.Table->GetPathName();
				if (Visited.Contains(TPath)) { continue; }
				Visited.Add(TPath);

				if (bWantChain)
				{
					TSharedPtr<FJsonObject> Level = MakeShared<FJsonObject>();
					Level->SetStringField(TEXT("table_path"), TPath);
					Level->SetStringField(TEXT("table_name"), L.Table->GetName());
					Level->SetNumberField(TEXT("depth"), L.Depth);

					TArray<TSharedPtr<FJsonValue>> LevelEntries;
					for (int32 i = 0; i < L.Table->Entries.Num(); ++i)
					{
						const FProxyEntry& E = L.Table->Entries[i];
						if (bHasFind && !EntryMatchesFind(E, FindProxy)) { continue; }
						LevelEntries.Add(MakeShared<FJsonValueObject>(ClaireonProxyTableHelpers::SerializeProxyEntry(E, i)));
					}
					Level->SetArrayField(TEXT("entries"), LevelEntries);
					ChainLevels.Add(MakeShared<FJsonValueObject>(Level));
				}

				if (bWantFlat)
				{
					for (int32 i = 0; i < L.Table->Entries.Num(); ++i)
					{
						const FProxyEntry& E = L.Table->Entries[i];
						if (!E.Proxy) { continue; }
						const FString PPath = E.Proxy->GetPathName();
						if (ResolvedKeys.Contains(PPath)) { continue; } // child already won
						ResolvedKeys.Add(PPath);
						if (bHasFind && !EntryMatchesFind(E, FindProxy)) { continue; }
						const int32 Idx = ResolvedEntries.Num();
						TSharedPtr<FJsonObject> EObj = ClaireonProxyTableHelpers::SerializeProxyEntry(E, Idx);
						AddProvenance(EObj, L.Table, i, L.Depth);
						ResolvedEntries.Add(MakeShared<FJsonValueObject>(EObj));
					}
				}

				for (const auto& GrandParent : L.Table->InheritEntriesFrom)
				{
					if (GrandParent && !Visited.Contains(GrandParent->GetPathName()))
					{
						Queue.Add({ GrandParent, L.Depth + 1 });
					}
				}
			}

			if (bWantFlat)
			{
				Data->SetArrayField(TEXT("resolved_entries"), ResolvedEntries);
				Data->SetNumberField(TEXT("resolved_count"), ResolvedEntries.Num());
			}
			if (bWantChain)
			{
				Data->SetArrayField(TEXT("chain"), ChainLevels);
			}
		}
	}
#else
	Data->SetNumberField(TEXT("entry_count"), ProxyTable->Keys.Num());
#endif

	const FString Summary = FString::Printf(TEXT("ProxyTable '%s': %d local entries (inherit=%s%s)"),
		*ProxyTable->GetName(),
#if WITH_EDITORONLY_DATA
		LocalEntryCount,
#else
		ProxyTable->Keys.Num(),
#endif
		*IncludeInherited,
		bHasFind ? *FString::Printf(TEXT(", find='%s'"), *FindProxy) : TEXT(""));

	return MakeSuccessResult(Data, Summary);
}
