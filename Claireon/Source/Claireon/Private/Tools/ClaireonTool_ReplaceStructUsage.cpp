// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_ReplaceStructUsage.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder
#include "Tools/ClaireonAnimGraphHelpers.h"
#include "ClaireonStructReflection.h"
#include "ClaireonNameResolver.h"
#include "ClaireonPathResolver.h"

#include "Animation/AnimBlueprint.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_SetFieldsInStruct.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_CallFunction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "UObject/SavePackage.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

using FToolResult = IClaireonTool::FToolResult;

namespace
{
	// One pre-reconstruction pin connection: the node GUID + pin name form a stable
	// identity we can resolve again after AllocateDefaultPins blows the pin set away.
	struct FPinLinkSnapshot
	{
		FGuid OtherNodeGuid;
		FName OtherPinName;
		EEdGraphPinDirection OtherDirection;
	};

	struct FMakeBreakSnapshot
	{
		TMap<FName, TArray<FPinLinkSnapshot>> PinLinksByName;
	};

	FMakeBreakSnapshot SnapshotNodeLinks(UEdGraphNode* Node)
	{
		FMakeBreakSnapshot Snapshot;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin) continue;
			TArray<FPinLinkSnapshot> Links;
			for (UEdGraphPin* Linked : Pin->LinkedTo)
			{
				if (!Linked || !Linked->GetOwningNode()) continue;
				FPinLinkSnapshot L;
				L.OtherNodeGuid = Linked->GetOwningNode()->NodeGuid;
				L.OtherPinName = Linked->PinName;
				L.OtherDirection = Linked->Direction.GetValue();
				Links.Add(L);
			}
			if (Links.Num() > 0)
			{
				Snapshot.PinLinksByName.Add(Pin->PinName, MoveTemp(Links));
			}
		}
		return Snapshot;
	}

	UEdGraphPin* FindPinByGuidAndName(UEdGraph* Graph, const FGuid& NodeGuid, FName PinName, EEdGraphPinDirection Direction)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node || Node->NodeGuid != NodeGuid) continue;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin && Pin->PinName == PinName && Pin->Direction.GetValue() == Direction)
					return Pin;
			}
		}
		return nullptr;
	}

	// Resolve a dotted field_map target like "BlendIn.BlendTime": find the parent pin,
	// split it via the K2 schema if not already split, then hand back the sub-pin whose
	// name matches the sub-path. Returns null if either step fails (caller emits a drop
	// warning like any other unresolved pin).
	UEdGraphPin* FindOrSplitSubPin(UEdGraphNode* Node, const FString& ParentName, const FString& SubName, FString& OutNote)
	{
		UEdGraphPin* Parent = nullptr;
		for (UEdGraphPin* P : Node->Pins)
		{
			if (P && P->PinName.ToString().Equals(ParentName, ESearchCase::IgnoreCase))
			{
				Parent = P;
				break;
			}
		}
		if (!Parent) return nullptr;

		if (Parent->SubPins.Num() == 0)
		{
			const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
			if (!K2Schema) return nullptr;
			K2Schema->SplitPin(Parent, /*bNotify=*/false);
		}

		// K2 schema names sub-pins as Parent_SubName (underscore-joined).
		const FString ExpectedCompound = ParentName + TEXT("_") + SubName;
		for (UEdGraphPin* Sub : Parent->SubPins)
		{
			if (!Sub) continue;
			const FString SubPinName = Sub->PinName.ToString();
			if (SubPinName.Equals(ExpectedCompound, ESearchCase::IgnoreCase) ||
				SubPinName.EndsWith(SubName, ESearchCase::IgnoreCase))
			{
				OutNote = FString::Printf(TEXT("split-pin routed '%s' → '%s.%s'"),
					*ParentName, *ParentName, *SubName);
				return Sub;
			}
		}
		return nullptr;
	}

	// Resolve a snapshotted pin name against the new pin set on a reconstructed node.
	// Order: explicit field_map > exact name > case-insensitive > fuzzy via
	// ResolvePropertyName against the new struct (which already handles spaces vs PascalCase
	// and the "b"-prefix bool convention).
	UEdGraphPin* MapAndFindPin(
		UEdGraphNode* Node,
		FName OldName,
		UScriptStruct* NewStruct,
		const TMap<FString, FString>& FieldMap,
		FString& OutNote)
	{
		// 1. Explicit field_map. Values may be dotted paths ("BlendIn.BlendTime") to
		// target a sub-pin of a nested struct — the tool will split-pin the parent
		// and hand back the sub-pin for linking.
		const FString OldNameStr = OldName.ToString();
		if (const FString* Mapped = FieldMap.Find(OldNameStr))
		{
			int32 DotIdx = INDEX_NONE;
			if (Mapped->FindChar(TEXT('.'), DotIdx))
			{
				const FString ParentName = Mapped->Left(DotIdx);
				const FString SubName = Mapped->Mid(DotIdx + 1);
				if (UEdGraphPin* Sub = FindOrSplitSubPin(Node, ParentName, SubName, OutNote))
				{
					return Sub;
				}
				// Fall through to try the simple match below — a bad dotted path shouldn't
				// mask a legitimate flat match if the caller typo'd the parent name.
			}
			for (UEdGraphPin* P : Node->Pins)
			{
				if (P && P->PinName.ToString().Equals(*Mapped, ESearchCase::IgnoreCase))
				{
					OutNote = FString::Printf(TEXT("mapped '%s' → '%s'"), *OldNameStr, **Mapped);
					return P;
				}
			}
		}

		// 2. Exact / case-insensitive match on the node's current pins
		for (UEdGraphPin* P : Node->Pins)
		{
			if (P && P->PinName == OldName) return P;
		}
		for (UEdGraphPin* P : Node->Pins)
		{
			if (P && P->PinName.ToString().Equals(OldNameStr, ESearchCase::IgnoreCase))
			{
				OutNote = FString::Printf(TEXT("case-insensitive match '%s' → '%s'"), *OldNameStr, *P->PinName.ToString());
				return P;
			}
		}

		// 3. Fuzzy via the struct's property resolver (handles "Blend In Time" ↔ "BlendInTime", b-prefix, etc.)
		if (NewStruct)
		{
			ClaireonNameResolver::FNameResolveResult R;
			FProperty* Resolved = ClaireonNameResolver::ResolvePropertyName(NewStruct, OldNameStr, R);
			if (Resolved)
			{
				const FString ResolvedAuthored = ClaireonStructReflection::GetFriendlyPropertyName(Resolved);
				const FString ResolvedRaw = Resolved->GetName();
				for (UEdGraphPin* P : Node->Pins)
				{
					const FString PinName = P ? P->PinName.ToString() : FString();
					if (PinName.Equals(ResolvedAuthored, ESearchCase::IgnoreCase) ||
						PinName.Equals(ResolvedRaw, ESearchCase::IgnoreCase))
					{
						OutNote = FString::Printf(TEXT("fuzzy '%s' → '%s'%s"),
							*OldNameStr, *P->PinName.ToString(),
							R.ResolutionNote.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *R.ResolutionNote));
						return P;
					}
				}
			}
		}

		return nullptr;
	}

	// Gather every editable graph the replacement must visit on a UBlueprint (covers
	// UAnimBlueprint too, via its inherited UbergraphPages/FunctionGraphs/MacroGraphs,
	// plus the anim-graph-specific nested subgraph walk when applicable).
	TArray<UEdGraph*> CollectEditableGraphs(UBlueprint* BP)
	{
		TArray<UEdGraph*> Graphs;
		if (!BP) return Graphs;

		auto AddIfNew = [&](UEdGraph* G)
		{
			if (G) Graphs.AddUnique(G);
		};

		for (UEdGraph* G : BP->UbergraphPages) AddIfNew(G);
		for (UEdGraph* G : BP->FunctionGraphs) AddIfNew(G);
		for (UEdGraph* G : BP->MacroGraphs)    AddIfNew(G);
		for (UEdGraph* G : BP->IntermediateGeneratedGraphs) AddIfNew(G);

		if (UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(BP))
		{
			for (const ClaireonAnimGraphHelpers::FAnimGraphInfo& Info : ClaireonAnimGraphHelpers::CollectAllGraphs(AnimBP))
			{
				AddIfNew(Info.Graph);
			}
		}

		return Graphs;
	}

	void SavePackageIfDirty(UBlueprint* BP, TArray<FString>& OutWarnings)
	{
		if (!BP || !BP->GetOutermost()) return;
		UPackage* Pkg = BP->GetOutermost();
		if (!Pkg->IsDirty()) return;

		FString PackageFilename = FPackageName::LongPackageNameToFilename(
			Pkg->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		if (!UPackage::SavePackage(Pkg, nullptr, *PackageFilename, Args))
		{
			OutWarnings.Add(FString::Printf(TEXT("Failed to save package '%s'"), *Pkg->GetName()));
		}
	}
}

FString ClaireonTool_ReplaceStructUsage::GetOperation() const { return TEXT("replace_struct_usage"); }

FString ClaireonTool_ReplaceStructUsage::GetDescription() const
{
	return TEXT("Retarget every reference to a struct type across a Blueprint or Animation Blueprint in one pass. "
		"Updates UK2Node_MakeStruct / UK2Node_BreakStruct (reconstructing pins and preserving connections by name / "
		"fuzzy match / field_map), user-defined pins on function entry/result nodes, local variables on function "
		"entries, and any loose struct-typed pin elsewhere. Member variables typed by the old struct should be "
		"retyped first via blueprint_set_variable_type — this tool warns (but does not retype) for safety. "
		"Supports dry_run for a plan-before-apply report.");
}

TSharedPtr<FJsonObject> ClaireonTool_ReplaceStructUsage::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the Blueprint or Animation Blueprint to rewrite"), true);
	S.AddString(TEXT("from_struct_path"), TEXT("Struct to replace (native /Script/... or BP /Game/... asset path)"), true);
	S.AddString(TEXT("to_struct_path"),   TEXT("Replacement struct (native /Script/... or BP /Game/... asset path)"), true);
	S.AddObject(TEXT("field_map"), TEXT("Optional { old_field_name: new_field_name } overrides that win over fuzzy matching when reconnecting Make/Break/SetFields pins. Values may be dotted paths (\"BlendInTime\": \"BlendIn.BlendTime\") to target a sub-pin of a nested struct — the tool will split-pin the parent and link into the sub-pin."));
	S.AddBoolean(TEXT("dry_run"),  TEXT("If true (default false), report would-change counts without mutating the Blueprint"));
	S.AddBoolean(TEXT("compile"),  TEXT("If true (default false), compile the Blueprint after applying changes"));
	S.AddBoolean(TEXT("save"),     TEXT("If true (default false), save the Blueprint package after applying changes"));
	S.AddBoolean(TEXT("reconcile"), TEXT("If true, operate on Make/Break/SetFields nodes already typed as to_struct instead of the remaining from_struct ones. Use this to re-apply a corrected field_map after an initial migration dropped connections that a proper map would have caught; the tool re-snapshots, reconstructs, and remaps with the new field_map. Safe to re-run."));
	S.AddBoolean(TEXT("confirm_dropped_fields"), TEXT("Safety rail. If dropped_fields is non-empty and this is not true, the tool refuses to mutate and returns the drop list instead — so callers have to explicitly acknowledge they plan to handle or accept each dropped field before committing. Ignored during dry_run."));
	return S.Build();
}

FToolResult ClaireonTool_ReplaceStructUsage::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath, FromPath, ToPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		return MakeErrorResult(TEXT("Missing required field: asset_path"));
	if (!Arguments->TryGetStringField(TEXT("from_struct_path"), FromPath) || FromPath.IsEmpty())
		return MakeErrorResult(TEXT("Missing required field: from_struct_path"));
	if (!Arguments->TryGetStringField(TEXT("to_struct_path"), ToPath) || ToPath.IsEmpty())
		return MakeErrorResult(TEXT("Missing required field: to_struct_path"));

	bool bDryRun = false, bCompile = false, bSave = false;
	bool bReconcile = false, bConfirmDroppedFields = false;
	Arguments->TryGetBoolField(TEXT("dry_run"), bDryRun);
	Arguments->TryGetBoolField(TEXT("compile"), bCompile);
	Arguments->TryGetBoolField(TEXT("save"), bSave);
	Arguments->TryGetBoolField(TEXT("reconcile"), bReconcile);
	Arguments->TryGetBoolField(TEXT("confirm_dropped_fields"), bConfirmDroppedFields);

	TMap<FString, FString> FieldMap;
	const TSharedPtr<FJsonObject>* FieldMapObj = nullptr;
	if (Arguments->TryGetObjectField(TEXT("field_map"), FieldMapObj) && FieldMapObj && (*FieldMapObj).IsValid())
	{
		for (const auto& Pair : (*FieldMapObj)->Values)
		{
			FString V;
			if (Pair.Value.IsValid() && Pair.Value->TryGetString(V))
			{
				FieldMap.Add(Pair.Key, V);
			}
		}
	}

	// Resolve the Blueprint. Use FSoftObjectPath::TryLoad so freshly-created assets
	// that the registry hasn't seen yet still work.
	auto BPResolve = ClaireonPathResolver::Resolve(AssetPath);
	if (!BPResolve.bSuccess)
		return MakeErrorResult(BPResolve.Error);
	UObject* Loaded = FSoftObjectPath(BPResolve.ResolvedPath.Path).TryLoad();
	UBlueprint* BP = Cast<UBlueprint>(Loaded);
	if (!BP)
	{
		return MakeErrorResult(FString::Printf(TEXT("Asset at '%s' is not a Blueprint"), *BPResolve.ResolvedPath.Path));
	}

	FString StructErr;
	UScriptStruct* FromStruct = ClaireonStructReflection::ResolveStructPath(FromPath, StructErr);
	if (!FromStruct) return MakeErrorResult(FString::Printf(TEXT("from_struct_path: %s"), *StructErr));
	UScriptStruct* ToStruct = ClaireonStructReflection::ResolveStructPath(ToPath, StructErr);
	if (!ToStruct)   return MakeErrorResult(FString::Printf(TEXT("to_struct_path: %s"), *StructErr));
	if (FromStruct == ToStruct)
	{
		return MakeErrorResult(TEXT("from_struct and to_struct resolve to the same type — nothing to do"));
	}

	TArray<FString> Warnings;
	TArray<FString> DroppedFields;
	int32 MakeNodesRetargeted = 0;
	int32 BreakNodesRetargeted = 0;
	int32 SetFieldsRetargeted = 0;
	int32 FunctionParamsRetargeted = 0;
	int32 LocalVarsRetargeted = 0;
	int32 LoosePinsRetargeted = 0;
	int32 VariablesFlagged = 0;
	int32 DroppedConnections = 0;

	TSet<UEdGraphNode*> NodesNeedingReconstruction;

	// Pre-flight 1: warn about member variables still typed as FromStruct so the caller
	// is nudged to run set_variable_type first. We don't auto-retype here because that
	// would hide the workflow step and surprise callers who only wanted the usage swept.
	for (const FBPVariableDescription& Var : BP->NewVariables)
	{
		if (Var.VarType.PinCategory == UEdGraphSchema_K2::PC_Struct &&
			Var.VarType.PinSubCategoryObject == FromStruct)
		{
			++VariablesFlagged;
			Warnings.Add(FString::Printf(
				TEXT("Variable '%s' is still typed as the old struct — run set_variable_type before this tool so downstream VariableGet/Set pins update cleanly."),
				*Var.VarName.ToString()));
		}
	}

	// Pre-flight 2: identify FromStruct fields with no match in ToStruct (via the same
	// field_map + ResolvePropertyName ladder the runtime pin-mapper will use). Surfacing
	// these up-front tells the caller which fields will lose their connections silently
	// without having to grep compile-error messages afterwards.
	for (TFieldIterator<FProperty> It(FromStruct); It; ++It)
	{
		FProperty* FromProp = *It;
		if (!FromProp) continue;
		const FString FromName = FromProp->GetName();
		const FString FromAuthored = ClaireonStructReflection::GetFriendlyPropertyName(FromProp);

		// Explicit field_map wins (keyed by either raw name or authored display name).
		if (FieldMap.Contains(FromName) || FieldMap.Contains(FromAuthored)) continue;

		// Same resolver the runtime will use — if it finds something, this field survives.
		ClaireonNameResolver::FNameResolveResult Probe;
		if (ClaireonNameResolver::ResolvePropertyName(ToStruct, FromName, Probe)) continue;
		if (ClaireonNameResolver::ResolvePropertyName(ToStruct, FromAuthored, Probe)) continue;

		DroppedFields.Add(FromAuthored.IsEmpty() ? FromName : FromAuthored);
		Warnings.Add(FString::Printf(
			TEXT("Field '%s' from '%s' has no equivalent on '%s' — any pin using it will drop its connections. Supply field_map={\"%s\": \"<new_field>\"} if a rename covers it, or plan to rewire manually."),
			*(FromAuthored.IsEmpty() ? FromName : FromAuthored), *FromStruct->GetName(), *ToStruct->GetName(),
			*(FromAuthored.IsEmpty() ? FromName : FromAuthored)));
	}

	// Safety rail: if any fields have no equivalent and the caller hasn't explicitly
	// confirmed they plan to handle them, refuse to mutate. Dry-run is always allowed
	// through so iterative planning (dry_run → amend field_map → dry_run again) works.
	if (!bDryRun && DroppedFields.Num() > 0 && !bConfirmDroppedFields)
	{
		TSharedPtr<FJsonObject> GateData = MakeShared<FJsonObject>();
		GateData->SetBoolField(TEXT("dry_run"), false);
		GateData->SetBoolField(TEXT("reconcile"), bReconcile);
		GateData->SetStringField(TEXT("asset_path"), BP->GetPathName());
		GateData->SetStringField(TEXT("from_struct"), FromStruct->GetPathName());
		GateData->SetStringField(TEXT("to_struct"), ToStruct->GetPathName());
		TArray<TSharedPtr<FJsonValue>> GateDroppedJson;
		for (const FString& F : DroppedFields) GateDroppedJson.Add(MakeShared<FJsonValueString>(F));
		GateData->SetArrayField(TEXT("dropped_fields"), GateDroppedJson);
		TArray<TSharedPtr<FJsonValue>> GateWarningJson;
		for (const FString& W : Warnings) GateWarningJson.Add(MakeShared<FJsonValueString>(W));
		GateData->SetArrayField(TEXT("warnings"), GateWarningJson);

		const FString GateMsg = FString::Printf(
			TEXT("Refusing to mutate: %d field(s) from '%s' have no equivalent on '%s' (%s). "
			     "Inspect dropped_fields, plan your handling (field_map rename, nested path, or manual rewire), "
			     "then rerun with confirm_dropped_fields=true. Pass dry_run=true to iterate without gating."),
			DroppedFields.Num(), *FromStruct->GetName(), *ToStruct->GetName(),
			*FString::Join(DroppedFields, TEXT(", ")));

		FToolResult GateResult = MakeErrorResult(GateMsg);
		GateResult.Data = GateData;
		GateResult.Warnings = Warnings;
		return GateResult;
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Replace Struct Usage")));
	BP->Modify();

	const TArray<UEdGraph*> Graphs = CollectEditableGraphs(BP);

	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph) continue;
		Graph->Modify();

		// Snapshot for iteration stability — we'll mutate nodes' pins inside the loop.
		TArray<UEdGraphNode*> NodesSnapshot = Graph->Nodes;

		for (UEdGraphNode* Node : NodesSnapshot)
		{
			if (!Node) continue;

			// ----- 1. Make/Break struct nodes: retarget + reconstruct with pin preservation -----
			UK2Node_MakeStruct* MakeNode = Cast<UK2Node_MakeStruct>(Node);
			UK2Node_BreakStruct* BreakNode = Cast<UK2Node_BreakStruct>(Node);
			UK2Node_SetFieldsInStruct* SetFieldsNode = Cast<UK2Node_SetFieldsInStruct>(Node);
			if (MakeNode || BreakNode || SetFieldsNode)
			{
				// All three node types carry a StructType property and allocate pins derived
				// from it — they share the same snapshot → swap → reconstruct → remap flow.
				UScriptStruct* CurrentStruct = MakeNode
					? MakeNode->StructType.Get()
					: (BreakNode ? BreakNode->StructType.Get() : SetFieldsNode->StructType.Get());

				// In normal mode, match nodes still typed as the old struct. In reconcile mode,
				// match nodes already migrated to the new struct — lets the caller re-snapshot
				// and re-apply an amended field_map without reverting the initial migration.
				UScriptStruct* const ExpectedStruct = bReconcile ? ToStruct : FromStruct;
				if (CurrentStruct != ExpectedStruct) continue;

				const TCHAR* NodeKind = MakeNode ? TEXT("MakeStruct")
					: (BreakNode ? TEXT("BreakStruct") : TEXT("SetFieldsInStruct"));

				// Dry-run: just count. Leave the node alone.
				if (bDryRun)
				{
					if (MakeNode) ++MakeNodesRetargeted;
					else if (BreakNode) ++BreakNodesRetargeted;
					else ++SetFieldsRetargeted;
					continue;
				}

				const FMakeBreakSnapshot Snapshot = SnapshotNodeLinks(Node);
				Node->Modify();

				if (MakeNode) MakeNode->StructType = ToStruct;
				else if (BreakNode) BreakNode->StructType = ToStruct;
				else SetFieldsNode->StructType = ToStruct;

				// Clear ShowPinForProperties before reconstruction. UK2Node_StructOperation
				// (the common base for Make/Break/SetFields) caches a per-property pin-visibility
				// list keyed by property FName from the OLD struct. ReconstructNode replays that
				// list to allocate pins — if we don't clear it, the new node gets pins named
				// after properties on the old struct (e.g. "Start Time", "Looping") and emits
				// "pin X no longer exists" compile errors despite StructType pointing at the new
				// struct. Clearing forces the node to rebuild ShowPinForProperties from the
				// current StructType during AllocateDefaultPins.
				if (MakeNode) MakeNode->ShowPinForProperties.Reset();
				else if (BreakNode) BreakNode->ShowPinForProperties.Reset();
				else SetFieldsNode->ShowPinForProperties.Reset();

				Node->ReconstructNode();

				// Reattach links. Iterate the old pin names; each one finds its counterpart on
				// the new pin set via MapAndFindPin, then hooks up to the other node's pin.
				for (const auto& Pair : Snapshot.PinLinksByName)
				{
					FString MapNote;
					UEdGraphPin* NewPin = MapAndFindPin(Node, Pair.Key, ToStruct, FieldMap, MapNote);
					if (!NewPin)
					{
						DroppedConnections += Pair.Value.Num();
						Warnings.Add(FString::Printf(
							TEXT("%s at %s/%s: pin '%s' has no counterpart on '%s' — %d connection(s) dropped"),
							NodeKind,
							*Graph->GetName(), *Node->NodeGuid.ToString(),
							*Pair.Key.ToString(), *ToStruct->GetName(),
							Pair.Value.Num()));
						continue;
					}
					if (!MapNote.IsEmpty())
					{
						Warnings.Add(FString::Printf(
							TEXT("%s at %s/%s: %s"),
							NodeKind,
							*Graph->GetName(), *Node->NodeGuid.ToString(), *MapNote));
					}

					for (const FPinLinkSnapshot& Link : Pair.Value)
					{
						UEdGraphPin* OtherPin = FindPinByGuidAndName(Graph, Link.OtherNodeGuid, Link.OtherPinName, Link.OtherDirection);
						if (OtherPin)
						{
							NewPin->MakeLinkTo(OtherPin);
						}
						else
						{
							Warnings.Add(FString::Printf(
								TEXT("%s at %s/%s: could not re-find other side (%s.%s) when reattaching pin '%s'"),
								NodeKind,
								*Graph->GetName(), *Node->NodeGuid.ToString(),
								*Link.OtherNodeGuid.ToString(), *Link.OtherPinName.ToString(),
								*NewPin->PinName.ToString()));
						}
					}
				}

				// ReconstructNode builds new pins alongside any old pins that had LinkedTo
				// entries — UE keeps them as orphaned pins to avoid data loss. Since we've
				// just re-attached via snapshot → MapAndFindPin to the new pin set, the old
				// ones are redundant and produce "pin X no longer exists" compile errors.
				// Sweep them now. A pin is "old" if it's marked as an orphan OR if its name
				// doesn't match any expected pin on the new struct (post-reconstruction).
				TArray<UEdGraphPin*> PinsToRemove;
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin && Pin->bOrphanedPin)
					{
						// Break its connections so removal is clean on both sides.
						Pin->BreakAllPinLinks();
						PinsToRemove.Add(Pin);
					}
				}
				for (UEdGraphPin* Pin : PinsToRemove)
				{
					Node->Pins.Remove(Pin);
					Node->DestroyPin(Pin);
				}

				if (MakeNode) ++MakeNodesRetargeted;
				else if (BreakNode) ++BreakNodesRetargeted;
				else ++SetFieldsRetargeted;
				continue;
			}

			// ----- 2. Function entry / result user-defined pins + local variables -----
			UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node);
			UK2Node_FunctionResult* ResultNode = Cast<UK2Node_FunctionResult>(Node);
			if (EntryNode || ResultNode)
			{
				// User-defined pin specs live on both entry and result; retarget any whose type is the old struct.
				TArray<TSharedPtr<FUserPinInfo>>& UserPins = EntryNode ? EntryNode->UserDefinedPins : ResultNode->UserDefinedPins;
				bool bAnyChanged = false;
				for (const TSharedPtr<FUserPinInfo>& UP : UserPins)
				{
					if (!UP.IsValid()) continue;
					if (UP->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct &&
						UP->PinType.PinSubCategoryObject == FromStruct)
					{
						if (bDryRun) { ++FunctionParamsRetargeted; continue; }
						UP->PinType.PinSubCategoryObject = ToStruct;
						++FunctionParamsRetargeted;
						bAnyChanged = true;
					}
				}

				// Local variables (entry-only)
				if (EntryNode)
				{
					for (FBPVariableDescription& Local : EntryNode->LocalVariables)
					{
						if (Local.VarType.PinCategory == UEdGraphSchema_K2::PC_Struct &&
							Local.VarType.PinSubCategoryObject == FromStruct)
						{
							if (bDryRun) { ++LocalVarsRetargeted; continue; }
							Local.VarType.PinSubCategoryObject = ToStruct;
							++LocalVarsRetargeted;
							bAnyChanged = true;
						}
					}
				}

				if (bAnyChanged && !bDryRun)
				{
					Node->Modify();
					NodesNeedingReconstruction.Add(Node);
				}
				continue;
			}

			// ----- 3. Loose struct-typed pins on any other node -----
			// Skip K2Node_CallFunction: its pins mirror the target function signature, which
			// will either be handled by a separate pass (retargeting that function's entry/result)
			// or is defined outside the current Blueprint entirely.
			if (Cast<UK2Node_CallFunction>(Node)) continue;

			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin) continue;
				if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct &&
					Pin->PinType.PinSubCategoryObject == FromStruct)
				{
					if (bDryRun) { ++LoosePinsRetargeted; continue; }
					Pin->PinType.PinSubCategoryObject = ToStruct;
					++LoosePinsRetargeted;
					NodesNeedingReconstruction.Add(Node);
				}
			}
		}
	}

	if (!bDryRun)
	{
		for (UEdGraphNode* Node : NodesNeedingReconstruction)
		{
			if (Node) Node->ReconstructNode();
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		FBlueprintEditorUtils::RefreshAllNodes(BP);

		if (bCompile)
		{
			FKismetEditorUtilities::CompileBlueprint(BP);
		}
		if (bSave)
		{
			SavePackageIfDirty(BP, Warnings);
		}
	}
	else
	{
		// Dry-run shouldn't commit the Modify() calls we made for graph iteration stability.
		Transaction.Cancel();
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("dry_run"), bDryRun);
	Data->SetStringField(TEXT("asset_path"), BP->GetPathName());
	Data->SetStringField(TEXT("from_struct"), FromStruct->GetPathName());
	Data->SetStringField(TEXT("to_struct"), ToStruct->GetPathName());
	Data->SetNumberField(TEXT("make_struct_retargeted"), MakeNodesRetargeted);
	Data->SetNumberField(TEXT("break_struct_retargeted"), BreakNodesRetargeted);
	Data->SetNumberField(TEXT("set_fields_retargeted"), SetFieldsRetargeted);
	Data->SetNumberField(TEXT("function_params_retargeted"), FunctionParamsRetargeted);
	Data->SetNumberField(TEXT("local_variables_retargeted"), LocalVarsRetargeted);
	Data->SetNumberField(TEXT("loose_pins_retargeted"), LoosePinsRetargeted);
	Data->SetNumberField(TEXT("variables_still_typed_as_from"), VariablesFlagged);
	Data->SetNumberField(TEXT("dropped_connections"), DroppedConnections);

	TArray<TSharedPtr<FJsonValue>> DroppedFieldsJson;
	for (const FString& F : DroppedFields) DroppedFieldsJson.Add(MakeShared<FJsonValueString>(F));
	Data->SetArrayField(TEXT("dropped_fields"), DroppedFieldsJson);

	TArray<TSharedPtr<FJsonValue>> WarningJson;
	for (const FString& W : Warnings) WarningJson.Add(MakeShared<FJsonValueString>(W));
	Data->SetArrayField(TEXT("warnings"), WarningJson);

	const int32 Total = MakeNodesRetargeted + BreakNodesRetargeted + SetFieldsRetargeted
		+ FunctionParamsRetargeted + LocalVarsRetargeted + LoosePinsRetargeted;
	const FString Summary = FString::Printf(
		TEXT("%s: %d edit(s) on '%s' — %d Make, %d Break, %d SetFields, %d func params, %d locals, %d loose pins%s"),
		bDryRun ? TEXT("dry_run") : TEXT("applied"),
		Total, *BP->GetName(),
		MakeNodesRetargeted, BreakNodesRetargeted, SetFieldsRetargeted,
		FunctionParamsRetargeted, LocalVarsRetargeted, LoosePinsRetargeted,
		VariablesFlagged > 0 ? *FString::Printf(TEXT(" (%d variable(s) still typed as source — run set_variable_type)"), VariablesFlagged) : TEXT(""));

	FToolResult Result = MakeSuccessResult(Data, Summary);
	Result.Warnings = Warnings;
	return Result;
}
