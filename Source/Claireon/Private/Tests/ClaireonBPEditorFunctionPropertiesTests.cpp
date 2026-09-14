// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Test supported function properties, preflight refusals, retained-failure deltas, and reload persistence.
// Purity is checked on gateway pins; FunctionEntry retains its Then pin even for pure functions.
// Net-mode checks cover durable flags and the reported waiver, not runtime RPC routing.

#include "Misc/AutomationTest.h"

#if WITH_CLAIREON_TESTS && WITH_EDITOR

#include "Tests/ClaireonBPEditorFixtures.h"
#include "Tools/ClaireonBPFunctionRecipe.h"
#include "Tools/ClaireonBPMutationResult.h"
#include "Tools/ClaireonBPSnapshot.h"
#include "Tools/ClaireonBlueprintGraphTool_AddFunction.h"
#include "Tools/ClaireonBlueprintGraphTool_Create.h"
#include "Tools/ClaireonBlueprintGraphTool_SetFunctionProperties.h"
#include "Tools/IClaireonTool.h"

#include "ClaireonSessionManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_VariableGet.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "PackageTools.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/Script.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/TopLevelAssetPath.h"

namespace ClaireonBPSetFunctionPropsInternal
{
	// Prefix helpers to avoid unity-build collisions.

	/** The setter's transaction title for independent refusal checks. */
	const TCHAR* SFP_TransactionTitle() { return TEXT("[Claireon] Set Function Properties"); }

	FString SFP_Phase(EClaireonBPSetterPhase Phase)
	{
		return FString(ClaireonBPMutation::ToWireString(Phase));
	}

	FString SFP_State(EClaireonMutationState State)
	{
		return FString(ClaireonBPMutation::ToWireString(State));
	}

	// Envelope readers reject absent fields.

	bool SFP_ReadString(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Data,
		const TCHAR* Field, FString& Out)
	{
		if (!Data.IsValid() || !Data->TryGetStringField(Field, Out))
		{
			Test.AddError(FString::Printf(TEXT("the envelope omitted the string field '%s'."), Field));
			return false;
		}
		return true;
	}

	void SFP_ExpectString(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Data,
		const TCHAR* Field, const FString& Expected, const FString& What)
	{
		FString Observed;
		if (SFP_ReadString(Test, Data, Field, Observed))
		{
			Test.TestEqual(FString::Printf(TEXT("%s: %s"), *What, Field), Observed, Expected);
		}
	}

	void SFP_ExpectBool(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Data,
		const TCHAR* Field, bool bExpected, const FString& What)
	{
		bool bObserved = !bExpected;
		if (!Data.IsValid() || !Data->TryGetBoolField(Field, bObserved))
		{
			Test.AddError(FString::Printf(TEXT("%s: the envelope omitted the bool field '%s'."),
				*What, Field));
			return;
		}
		Test.TestTrue(FString::Printf(TEXT("%s: %s is %s"), *What, Field,
			bExpected ? TEXT("true") : TEXT("false")), bObserved == bExpected);
	}

	void SFP_ExpectAbsent(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Data,
		const TCHAR* Field, const FString& What, const TCHAR* Why)
	{
		if (Data.IsValid() && Data->HasField(Field))
		{
			Test.AddError(FString::Printf(TEXT("%s: '%s' is present and must not be: %s"),
				*What, Field, Why));
		}
	}

	/** Every refusal reason token the envelope reported, in order. */
	TArray<FString> SFP_RefusalReasons(const TSharedPtr<FJsonObject>& Data)
	{
		TArray<FString> Reasons;
		const TArray<TSharedPtr<FJsonValue>>* Findings = nullptr;
		if (!Data.IsValid() || !Data->TryGetArrayField(TEXT("refusal_findings"), Findings)
			|| Findings == nullptr)
		{
			return Reasons;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Findings)
		{
			const TSharedPtr<FJsonObject>* Finding = nullptr;
			if (Value.IsValid() && Value->TryGetObject(Finding) && Finding != nullptr)
			{
				FString Reason;
				(*Finding)->TryGetStringField(TEXT("reason"), Reason);
				Reasons.Add(Reason);
			}
		}
		return Reasons;
	}

	FString SFP_Join(const TArray<FString>& Values)
	{
		return Values.Num() == 0 ? FString(TEXT("none")) : FString::Join(Values, TEXT(", "));
	}

	// Observe transaction titles; undo purging makes queue-length changes insufficient.

	struct FFPTxnWindow
	{
		int32 StartLength = INDEX_NONE;
		int32 StartUndoCount = INDEX_NONE;
		bool bValid = false;
	};

	UTransBuffer* SFP_Buffer()
	{
		return IsValid(GEditor) ? Cast<UTransBuffer>(GEditor->Trans) : nullptr;
	}

	FFPTxnWindow SFP_OpenWindow(FAutomationTestBase& Test)
	{
		FFPTxnWindow Window;
		UTransBuffer* Buffer = SFP_Buffer();
		if (!IsValid(Buffer))
		{
			Test.AddError(TEXT("GEditor->Trans is not a UTransBuffer, so 'no transaction was "
				"opened' cannot be checked against the editor. This suite is EditorContext-only "
				"precisely so that cannot happen."));
			return Window;
		}
		Window.StartLength = Buffer->GetQueueLength();
		Window.StartUndoCount = Buffer->GetUndoCount();
		Window.bValid = true;
		return Window;
	}

	bool SFP_WindowHasSetterTransaction(const FFPTxnWindow& Window, bool& bOutMeasured)
	{
		bOutMeasured = false;
		UTransBuffer* Buffer = SFP_Buffer();
		if (!Window.bValid || !IsValid(Buffer))
		{
			return false;
		}
		const int32 Start = FMath::Max(0, Window.StartLength - FMath::Max(0, Window.StartUndoCount));
		const int32 Length = Buffer->GetQueueLength();
		if (Length < Start)
		{
			return false;
		}
		bOutMeasured = true;
		for (int32 Index = Start; Index < Length; ++Index)
		{
			const FTransaction* Transaction = Buffer->GetTransaction(Index);
			if (Transaction != nullptr
				&& Transaction->GetTitle().ToString().Equals(SFP_TransactionTitle()))
			{
				return true;
			}
		}
		return false;
	}

	/** Verify the refusal opened no transaction. */
	void SFP_ExpectNoTransactionOpened(FAutomationTestBase& Test, const FFPTxnWindow& Window,
		const FString& What)
	{
		bool bMeasured = false;
		const bool bPresent = SFP_WindowHasSetterTransaction(Window, bMeasured);
		if (!bMeasured)
		{
			Test.AddError(FString::Printf(
				TEXT("%s: the transaction window could not be read, so DEC-33's 'no transaction "
				     "was opened' could not be corroborated. Reported as a MEASUREMENT FAILURE "
				     "rather than as agreement."), *What));
			return;
		}
		Test.TestFalse(FString::Printf(
			TEXT("%s: no '%s' record reached the editor's transaction buffer -- the refusal runs "
			     "before the first mutating call, so quiescence holds BY CONSTRUCTION"),
			*What, SFP_TransactionTitle()), bPresent);
	}


	/** An unset TOptional omits the property; supplied false remains an explicit write. */
	struct FFPRequest
	{
		TOptional<bool> bPure;
		TOptional<bool> bConst;
		TOptional<bool> bStatic;
		TOptional<FString> Category;
		TOptional<FString> Tooltip;
		TOptional<FString> Access;
		TOptional<FString> NetCall;
	};

	IClaireonTool::FToolResult SFP_SetProperties(const FString& AssetPath,
		const FString& FunctionName, const FFPRequest& Request)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetStringField(TEXT("function_name"), FunctionName);
		Args->SetStringField(TEXT("response_mode"), TEXT("status"));
		if (Request.bPure.IsSet())     { Args->SetBoolField(TEXT("is_pure"), Request.bPure.GetValue()); }
		if (Request.bConst.IsSet())    { Args->SetBoolField(TEXT("is_const"), Request.bConst.GetValue()); }
		if (Request.bStatic.IsSet())   { Args->SetBoolField(TEXT("is_static"), Request.bStatic.GetValue()); }
		if (Request.Category.IsSet())  { Args->SetStringField(TEXT("category"), Request.Category.GetValue()); }
		if (Request.Tooltip.IsSet())   { Args->SetStringField(TEXT("tooltip"), Request.Tooltip.GetValue()); }
		if (Request.Access.IsSet())    { Args->SetStringField(TEXT("access_specifier"), Request.Access.GetValue()); }
		if (Request.NetCall.IsSet())   { Args->SetStringField(TEXT("is_network_call"), Request.NetCall.GetValue()); }

		ClaireonBlueprintGraphTool_SetFunctionProperties Tool;
		return Tool.Execute(Args);
	}

	/** Use UObject-based inheritance fixtures to avoid duplicate DefaultSceneRoot SCS nodes during reparenting. */
	UBlueprint* SFP_CreateBlueprint(const FString& AssetPath, const TCHAR* ParentClassName,
		FString& OutError)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetStringField(TEXT("parent_class"), ParentClassName);
		ClaireonBlueprintGraphTool_Create Tool;
		const IClaireonTool::FToolResult Result = Tool.Execute(Args);
		if (Result.bIsError)
		{
			OutError = Result.ErrorMessage;
			return nullptr;
		}
		UBlueprint* Blueprint = Cast<UBlueprint>(ClaireonBPEditorFixtures::Resolve(AssetPath));
		if (!IsValid(Blueprint))
		{
			OutError = TEXT("bp_create reported success but the Blueprint did not resolve");
		}
		return Blueprint;
	}

	/** One declared parameter for bp_add_function. */
	struct FFPParam
	{
		FString Name;
		FString Type;
	};

	/** Creation-time options, so a fixture starts in the state a case needs. */
	struct FFPCreate
	{
		TOptional<bool> bPure;
		TOptional<bool> bConst;
		TOptional<bool> bStatic;
		TOptional<FString> Access;
		TOptional<FString> NetCall;
		TOptional<FString> Category;
		TArray<FFPParam> Outputs;
	};

	bool SFP_AddFunction(FAutomationTestBase& Test, const FString& AssetPath,
		const FString& FunctionName, const FFPCreate& Create)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetStringField(TEXT("function_name"), FunctionName);
		Args->SetStringField(TEXT("response_mode"), TEXT("status"));
		if (Create.bPure.IsSet())   { Args->SetBoolField(TEXT("is_pure"), Create.bPure.GetValue()); }
		if (Create.bConst.IsSet())  { Args->SetBoolField(TEXT("is_const"), Create.bConst.GetValue()); }
		if (Create.bStatic.IsSet()) { Args->SetBoolField(TEXT("is_static"), Create.bStatic.GetValue()); }
		if (Create.Access.IsSet())  { Args->SetStringField(TEXT("access_specifier"), Create.Access.GetValue()); }
		if (Create.NetCall.IsSet()) { Args->SetStringField(TEXT("is_network_call"), Create.NetCall.GetValue()); }
		if (Create.Category.IsSet()){ Args->SetStringField(TEXT("category"), Create.Category.GetValue()); }
		if (Create.Outputs.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Outputs;
			for (const FFPParam& Param : Create.Outputs)
			{
				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("name"), Param.Name);
				Entry->SetStringField(TEXT("type"), Param.Type);
				Outputs.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Args->SetArrayField(TEXT("outputs"), Outputs);
		}

		ClaireonBlueprintGraphTool_AddFunction Tool;
		const IClaireonTool::FToolResult Result = Tool.Execute(Args);
		if (Result.bIsError)
		{
			Test.AddError(FString::Printf(TEXT("bp_add_function could not build fixture '%s': %s"),
				*FunctionName, *Result.ErrorMessage));
			return false;
		}
		return true;
	}


	UEdGraph* SFP_FindGraph(UBlueprint* Blueprint, const FString& GraphName)
	{
		if (!IsValid(Blueprint))
		{
			return nullptr;
		}
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			if (IsValid(Graph) && Graph->GetName().Equals(GraphName))
			{
				return Graph;
			}
		}
		return nullptr;
	}

	UK2Node_FunctionEntry* SFP_FindEntry(const UEdGraph* Graph)
	{
		if (!IsValid(Graph))
		{
			return nullptr;
		}
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node); IsValid(Entry))
			{
				return Entry;
			}
		}
		return nullptr;
	}

	int32 SFP_EntryFlags(UBlueprint* Blueprint, const FString& FunctionName)
	{
		UK2Node_FunctionEntry* Entry = SFP_FindEntry(SFP_FindGraph(Blueprint, FunctionName));
		return IsValid(Entry) ? Entry->GetExtraFlags() : 0;
	}

	/** Re-find the skeleton UFunction after each compile because compilation can replace it. */
	UFunction* SFP_SkeletonFunction(UBlueprint* Blueprint, const FString& FunctionName)
	{
		return (IsValid(Blueprint) && IsValid(Blueprint->SkeletonGeneratedClass))
			? Blueprint->SkeletonGeneratedClass->FindFunctionByName(FName(*FunctionName))
			: nullptr;
	}

	FString SFP_Hex(int32 Flags)
	{
		return FString::Printf(TEXT("0x%08X"), static_cast<uint32>(Flags));
	}

	/** Inspect orphaned exec pins as well as live pins. */
	TArray<FString> SFP_ExecPinNames(const UEdGraphNode* Node)
	{
		TArray<FString> Names;
		if (!IsValid(Node))
		{
			return Names;
		}
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin != nullptr && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				Names.Add(FString::Printf(TEXT("%s%s"), *Pin->PinName.ToString(),
					Pin->bOrphanedPin ? TEXT(" (orphaned)") : TEXT("")));
			}
		}
		Names.Sort();
		return Names;
	}

	/** The one loaded call site of a function inside this Blueprint, or null. */
	UK2Node_CallFunction* SFP_FindCallSite(UBlueprint* Blueprint, const FString& FunctionName)
	{
		if (!IsValid(Blueprint))
		{
			return nullptr;
		}
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		const FName Wanted(*FunctionName);
		for (UEdGraph* Graph : Graphs)
		{
			if (!IsValid(Graph))
			{
				continue;
			}
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
				if (IsValid(Call) && Call->FunctionReference.GetMemberName() == Wanted)
				{
					return Call;
				}
			}
		}
		return nullptr;
	}

	/** A self-context call node for one of this Blueprint's own functions. */
	UK2Node_CallFunction* SFP_AddSelfCall(UEdGraph& Graph, const FString& FunctionName,
		int32 X, int32 Y)
	{
		UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(&Graph);
		Graph.AddNode(Call, /*bFromUI=*/false, /*bSelectNewNode=*/false);
		Call->FunctionReference.SetSelfMember(FName(*FunctionName));
		Call->CreateNewGuid();
		Call->NodePosX = X;
		Call->NodePosY = Y;
		Call->PostPlacedNewNode();
		Call->AllocateDefaultPins();
		return Call;
	}


	/** Every (effect_class, target) pair the operation delta reported. */
	struct FFPDeltaEntry
	{
		FString EffectClass;
		FString Target;
	};

	bool SFP_ReadDelta(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Data,
		const FString& What, TArray<FFPDeltaEntry>& OutEntries, bool& bOutComparable)
	{
		bOutComparable = false;
		const TSharedPtr<FJsonObject>* Delta = nullptr;
		if (!Data.IsValid() || !Data->TryGetObjectField(TEXT("operation_delta"), Delta)
			|| Delta == nullptr)
		{
			Test.AddError(FString::Printf(
				TEXT("%s: the retained-failure envelope carries no operation_delta, so what "
				     "remains on the asset is unanswerable from the result."), *What));
			return false;
		}
		(*Delta)->TryGetBoolField(TEXT("comparable"), bOutComparable);

		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!(*Delta)->TryGetArrayField(TEXT("entries"), Entries) || Entries == nullptr)
		{
			return true;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Entries)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(Entry) || Entry == nullptr)
			{
				continue;
			}
			FFPDeltaEntry Parsed;
			(*Entry)->TryGetStringField(TEXT("effect_class"), Parsed.EffectClass);
			(*Entry)->TryGetStringField(TEXT("target"), Parsed.Target);
			OutEntries.Add(MoveTemp(Parsed));
		}
		return true;
	}

	/** Require the operation delta to cover the property target for this combination. */
	void SFP_ExpectDeltaCovers(FAutomationTestBase& Test, const TArray<FFPDeltaEntry>& Entries,
		const FString& Target, const FString& What)
	{
		bool bFound = false;
		TArray<FString> Seen;
		for (const FFPDeltaEntry& Entry : Entries)
		{
			if (Entry.EffectClass.Equals(TEXT("family_properties")))
			{
				Seen.Add(Entry.Target);
				bFound = bFound || Entry.Target.Equals(Target);
			}
		}
		Test.TestTrue(FString::Printf(
			TEXT("%s: the operation delta's family_properties class covers '%s' (it covers: %s)"),
			*What, *Target, *SFP_Join(Seen)), bFound);
	}

	// Separate creation and editor opening with a stack unwind to avoid reentrant loading.

	class FFP_FixtureCommand : public IAutomationLatentCommand
	{
	public:
		FFP_FixtureCommand(FAutomationTestBase* InTest, const FString& InAssetPath)
			: Test(InTest)
			, AssetPath(InAssetPath)
		{
		}

		virtual bool Update() override
		{
			switch (Phase)
			{
			case EPhase::Build:
			{
				FString BuildError;
				UBlueprint* Blueprint = CreateFixture(BuildError);
				if (!IsValid(Blueprint) || !BuildFixture(*Blueprint, BuildError))
				{
					Test->AddError(FString::Printf(TEXT("fixture setup failed at %s: %s"),
						*AssetPath, *BuildError));
					Phase = EPhase::Teardown;
					return false;
				}
				Phase = EPhase::Settle;
				return false;
			}

			case EPhase::Settle:
				if (++SettleUpdates < 3)
				{
					return false;
				}
				Phase = EPhase::Body;
				return false;

			case EPhase::Body:
			{
				// Resolve the fixture created in an earlier update without loading it again.
				UBlueprint* Blueprint = Cast<UBlueprint>(ClaireonBPEditorFixtures::Resolve(AssetPath));
				if (!IsValid(Blueprint))
				{
					Test->AddError(TEXT("the fixture is not resident, so the body cannot run "
						"without a load it is not allowed to perform here."));
					Phase = EPhase::Teardown;
					return false;
				}
				RunBody(*Blueprint);

				// Rebuild the open editor's widgets over whatever the body left, so a Slate
				// pump in a LATER test cannot paint a stale SGraphPanel.
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				Phase = EPhase::Teardown;
				return false;
			}

			case EPhase::Teardown:
			default:
				Cleanup();
				ClaireonBPEditorFixtures::Teardown(AssetPath);
				return true;
			}
		}

	protected:
		/** The fixture asset itself. An Actor unless a case needs otherwise. */
		virtual UBlueprint* CreateFixture(FString& OutError)
		{
			return ClaireonBPEditorFixtures::Create(AssetPath, OutError);
		}

		/** Add whatever functions and call sites the case needs. Runs with no editor open. */
		virtual bool BuildFixture(UBlueprint& Blueprint, FString& OutError) = 0;

		/** The measurement. Runs with the fixture resident and no create/load on the stack. */
		virtual void RunBody(UBlueprint& Blueprint) = 0;

		/** Extra assets a case created. Deleted before the fixture itself. */
		virtual void Cleanup() {}

		FAutomationTestBase* Test = nullptr;
		FString AssetPath;

	private:
		enum class EPhase : uint8
		{
			Build,
			Settle,
			Body,
			Teardown,
		};

		EPhase Phase = EPhase::Build;
		int32 SettleUpdates = 0;
	};
}

// Run each supported combination with and without a forced readback mismatch.
// Both paths perform real writes and compilation; failed runs must report the affected property.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorFunctionPropertiesMatrix,
	"Claireon.BPEditor.FunctionProperties.SupportedCombinationsApplyAndCoverTheirSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPSetFunctionPropsInternal
{
	/** A supported request and its expected delta targets. */
	struct FFPSupportedCell
	{
		FString Label;
		FString Function;
		FFPCreate Create;
		FFPRequest Request;
		TArray<FString> DeltaTargets;
	};

	class FFP_MatrixCommand : public FFP_FixtureCommand
	{
	public:
		using FFP_FixtureCommand::FFP_FixtureCommand;

	protected:
		void BuildCells()
		{
			Cells.Reset();

			// 1. is_pure alone, on an exec-free body.
			{
				FFPSupportedCell Cell;
				Cell.Label = TEXT("is_pure alone");
				Cell.Function = TEXT("SFPMatrixPure");
				Cell.Create.Outputs.Add({TEXT("ReturnValue"), TEXT("bool")});
				Cell.Request.bPure = true;
				Cell.DeltaTargets = {TEXT("function_flags:"), TEXT("entry_extra_flags:")};
				Cells.Add(MoveTemp(Cell));
			}

			// Const applies only to non-static functions.
			{
				FFPSupportedCell Cell;
				Cell.Label = TEXT("is_const alone");
				Cell.Function = TEXT("SFPMatrixConst");
				Cell.Request.bConst = true;
				Cell.DeltaTargets = {TEXT("function_flags:"), TEXT("entry_extra_flags:")};
				Cells.Add(MoveTemp(Cell));
			}

			// 3. category alone -- metadata, asserted against the entry node's MetaData.
			{
				FFPSupportedCell Cell;
				Cell.Label = TEXT("category alone");
				Cell.Function = TEXT("SFPMatrixCategory");
				Cell.Request.Category = FString(TEXT("Stage019|Category"));
				Cell.DeltaTargets = {TEXT("category:")};
				Cells.Add(MoveTemp(Cell));
			}

			// 4. tooltip alone.
			{
				FFPSupportedCell Cell;
				Cell.Label = TEXT("tooltip alone");
				Cell.Function = TEXT("SFPMatrixTooltip");
				Cell.Request.Tooltip = FString(TEXT("Stage 019 tooltip."));
				Cell.DeltaTargets = {TEXT("tooltip:")};
				Cells.Add(MoveTemp(Cell));
			}

			// Access reduction carries an explicit safety limitation.
			{
				FFPSupportedCell Cell;
				Cell.Label = TEXT("access reduction Public -> Private");
				Cell.Function = TEXT("SFPMatrixAccess");
				Cell.Create.Access = FString(TEXT("Public"));
				Cell.Request.Access = FString(TEXT("Private"));
				Cell.DeltaTargets = {TEXT("function_flags:"), TEXT("entry_extra_flags:"),
					TEXT("access_specifier:")};
				Cells.Add(MoveTemp(Cell));
			}

			// 6. is_static on an ordinary Blueprint class, body preflight clear.
			{
				FFPSupportedCell Cell;
				Cell.Label = TEXT("is_static alone");
				Cell.Function = TEXT("SFPMatrixStatic");
				Cell.Request.bStatic = true;
				Cell.DeltaTargets = {TEXT("function_flags:"), TEXT("entry_extra_flags:")};
				Cells.Add(MoveTemp(Cell));
			}

			// 7. is_network_call None -> Server.
			{
				FFPSupportedCell Cell;
				Cell.Label = TEXT("is_network_call None -> Server");
				Cell.Function = TEXT("SFPMatrixNetOn");
				Cell.Request.NetCall = FString(TEXT("Server"));
				Cell.DeltaTargets = {TEXT("function_flags:"), TEXT("entry_extra_flags:"),
					TEXT("net_flags:")};
				Cells.Add(MoveTemp(Cell));
			}

			// Switching networking off must also clear FUNC_NetReliable.
			{
				FFPSupportedCell Cell;
				Cell.Label = TEXT("is_network_call Server -> None");
				Cell.Function = TEXT("SFPMatrixNetOff");
				Cell.Create.NetCall = FString(TEXT("Server"));
				Cell.Request.NetCall = FString(TEXT("None"));
				Cell.DeltaTargets = {TEXT("function_flags:"), TEXT("entry_extra_flags:"),
					TEXT("net_flags:")};
				Cells.Add(MoveTemp(Cell));
			}
		}

		virtual bool BuildFixture(UBlueprint& Blueprint, FString& OutError) override
		{
			BuildCells();
			for (const FFPSupportedCell& Cell : Cells)
			{
				// Use separate functions so armed and unarmed runs have independent snapshot windows.
				if (!SFP_AddFunction(*Test, AssetPath, Cell.Function, Cell.Create)
					|| !SFP_AddFunction(*Test, AssetPath, Cell.Function + TEXT("Armed"), Cell.Create))
				{
					OutError = FString::Printf(TEXT("could not build the '%s' cell."), *Cell.Label);
					return false;
				}
			}
			FKismetEditorUtilities::CompileBlueprint(&Blueprint);
			return true;
		}

		/** Verify the failed run's operation delta. */
		void RunArmed(const FFPSupportedCell& Cell)
		{
			const FString Armed = Cell.Function + TEXT("Armed");
			const FString What = FString::Printf(TEXT("DEC-32/%s"), *Cell.Label);

			IClaireonTool::FToolResult Result;
			{
				const ClaireonBPFaultInjection::FScopedFault Fault(
					SFP_Phase(EClaireonBPSetterPhase::CompileValidate));
				Result = SFP_SetProperties(AssetPath, Armed, Cell.Request);
			}

			if (!Result.Data.IsValid())
			{
				Test->AddError(FString::Printf(TEXT("%s: the armed call returned no structured "
					"data, so DEC-32 cannot be checked."), *What));
				return;
			}

			SFP_ExpectString(*Test, Result.Data, TEXT("mutation_state"),
				SFP_State(EClaireonMutationState::AppliedValidationFailed), What);
			SFP_ExpectBool(*Test, Result.Data, TEXT("mutation_retained"), true, What);
			SFP_ExpectString(*Test, Result.Data, TEXT("failed_phase"),
				SFP_Phase(EClaireonBPSetterPhase::CompileValidate), What);
			SFP_ExpectString(*Test, Result.Data, TEXT("engine_compile_status"),
				FString(TEXT("succeeded")), What);

			TArray<FFPDeltaEntry> Entries;
			bool bComparable = false;
			if (!SFP_ReadDelta(*Test, Result.Data, What, Entries, bComparable))
			{
				return;
			}
			Test->TestTrue(FString::Printf(
				TEXT("%s: the delta is COMPARABLE -- both snapshots were captured, so an empty "
				     "entry list would be a claim rather than an unread class"), *What),
				bComparable);

			const FString Suffix = Armed;
			for (const FString& Target : Cell.DeltaTargets)
			{
				SFP_ExpectDeltaCovers(*Test, Entries, Target + Suffix, What);
			}
		}

		/** The clean half, read back off the LIVE objects rather than off the payload. */
		void RunClean(UBlueprint& Blueprint, const FFPSupportedCell& Cell)
		{
			const FString What = FString::Printf(TEXT("supported/%s"), *Cell.Label);
			const int32 Before = SFP_EntryFlags(&Blueprint, Cell.Function);

			const IClaireonTool::FToolResult Result =
				SFP_SetProperties(AssetPath, Cell.Function, Cell.Request);

			if (Result.bIsError)
			{
				Test->AddError(FString::Printf(
					TEXT("%s: a SUPPORTED combination was rejected: %s"), *What, *Result.ErrorMessage));
				return;
			}
			if (!Result.Data.IsValid())
			{
				Test->AddError(FString::Printf(TEXT("%s: succeeded with no structured data."), *What));
				return;
			}

			SFP_ExpectString(*Test, Result.Data, TEXT("mutation_state"),
				SFP_State(EClaireonMutationState::AppliedClean), What);
			SFP_ExpectBool(*Test, Result.Data, TEXT("mutation_retained"), false, What);
			SFP_ExpectString(*Test, Result.Data, TEXT("last_completed_phase"),
				SFP_Phase(EClaireonBPSetterPhase::CompileValidate), What);
			SFP_ExpectString(*Test, Result.Data, TEXT("engine_compile_status"),
				FString(TEXT("succeeded")), What);

			SFP_ExpectAbsent(*Test, Result.Data, TEXT("operation_delta"), What,
				TEXT("the operation applied cleanly, so there is no failure to explain"));

			SFP_ExpectBool(*Test, Result.Data, TEXT("session_graph_preserved"), true, What);

			UK2Node_FunctionEntry* Entry = SFP_FindEntry(SFP_FindGraph(&Blueprint, Cell.Function));
			UFunction* Function = SFP_SkeletonFunction(&Blueprint, Cell.Function);
			if (!IsValid(Entry) || Function == nullptr)
			{
				Test->AddError(FString::Printf(
					TEXT("%s: the function's entry node or skeleton UFunction is missing after "
					     "the change, so nothing can be read back."), *What));
				return;
			}
			const int32 After = Entry->GetExtraFlags();
			const int32 Compiled = static_cast<int32>(Function->FunctionFlags);

			if (Cell.Request.bPure.IsSet())
			{
				Test->TestTrue(FString::Printf(TEXT("%s: FUNC_BlueprintPure on the entry node"), *What),
					(After & FUNC_BlueprintPure) != 0);
				Test->TestTrue(FString::Printf(TEXT("%s: FUNC_BlueprintPure on the skeleton UFunction"), *What),
					(Compiled & FUNC_BlueprintPure) != 0);
			}
			if (Cell.Request.bConst.IsSet())
			{
				Test->TestTrue(FString::Printf(TEXT("%s: FUNC_Const on the entry node"), *What),
					(After & FUNC_Const) != 0);
				Test->TestTrue(FString::Printf(TEXT("%s: FUNC_Const on the skeleton UFunction"), *What),
					(Compiled & FUNC_Const) != 0);
			}
			if (Cell.Request.bStatic.IsSet())
			{
				Test->TestTrue(FString::Printf(TEXT("%s: FUNC_Static on the entry node"), *What),
					(After & FUNC_Static) != 0);
				Test->TestTrue(FString::Printf(TEXT("%s: FUNC_Static on the skeleton UFunction"), *What),
					(Compiled & FUNC_Static) != 0);
			}
			if (Cell.Request.Access.IsSet())
			{
				Test->TestEqual(FString::Printf(
					TEXT("%s: the access bits on the entry node are exactly FUNC_Private "
					     "(before %s, after %s)"), *What, *SFP_Hex(Before), *SFP_Hex(After)),
					After & ClaireonBPFunctionRecipe::kAccessClearMask, static_cast<int32>(FUNC_Private));
				Test->TestEqual(FString::Printf(
					TEXT("%s: the access bits on the skeleton UFunction are exactly FUNC_Private"), *What),
					Compiled & ClaireonBPFunctionRecipe::kAccessClearMask, static_cast<int32>(FUNC_Private));

				FString Note;
				Test->TestTrue(FString::Printf(
					TEXT("%s: the result carries the access_reduction_note"), *What),
					SFP_ReadString(*Test, Result.Data, TEXT("access_reduction_note"), Note));
			}
			if (Cell.Request.Category.IsSet())
			{
				Test->TestEqual(FString::Printf(TEXT("%s: the entry node's MetaData.Category"), *What),
					Entry->MetaData.Category.ToString(), Cell.Request.Category.GetValue());
			}
			if (Cell.Request.Tooltip.IsSet())
			{
				Test->TestEqual(FString::Printf(TEXT("%s: the entry node's MetaData.ToolTip"), *What),
					Entry->MetaData.ToolTip.ToString(), Cell.Request.Tooltip.GetValue());
			}
			if (Cell.Request.NetCall.IsSet())
			{
				const bool bWantsNone = Cell.Request.NetCall.GetValue().Equals(TEXT("None"));
				const int32 ExpectedNet = bWantsNone
					? 0
					: (FUNC_Net | FUNC_NetServer | FUNC_NetReliable);
				Test->TestEqual(FString::Printf(
					TEXT("%s: the five managed net bits on the entry node"), *What),
					After & ClaireonBPFunctionRecipe::kNetModeClearMask, ExpectedNet);
				Test->TestEqual(FString::Printf(
					TEXT("%s: the five managed net bits on the skeleton UFunction"), *What),
					Compiled & ClaireonBPFunctionRecipe::kNetModeClearMask, ExpectedNet);
			}

			// Omitted properties must remain untouched.
			const TArray<TSharedPtr<FJsonValue>>* LeftAlone = nullptr;
			if (Result.Data->TryGetArrayField(TEXT("properties_left_alone"), LeftAlone)
				&& LeftAlone != nullptr)
			{
				Test->TestEqual(FString::Printf(
					TEXT("%s: six of the seven properties were omitted and left alone"), *What),
					LeftAlone->Num(), 6);
			}
			else
			{
				Test->AddError(FString::Printf(
					TEXT("%s: the result does not report which properties were left alone, so "
					     "'an omitted property is not reset' is unanswerable."), *What));
			}
		}

		virtual void RunBody(UBlueprint& Blueprint) override
		{
			Test->TestEqual(TEXT("the matrix drives every supported combination 080 names"),
				Cells.Num(), 8);

			for (const FFPSupportedCell& Cell : Cells)
			{
				RunArmed(Cell);
				RunClean(Blueprint, Cell);
			}
		}

	private:
		TArray<FFPSupportedCell> Cells;
	};
}

bool FClaireonBPEditorFunctionPropertiesMatrix::RunTest(const FString& /*Parameters*/)
{
	using namespace ClaireonBPSetFunctionPropsInternal;
	ADD_LATENT_AUTOMATION_COMMAND(FFP_MatrixCommand(
		this, ClaireonBPEditorFixtures::UniquePath(TEXT("SfpMatrix"))));
	return true;
}

// Every refused combination must fail before a transaction or snapshot is opened.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorFunctionPropertiesRefusals,
	"Claireon.BPEditor.FunctionProperties.RefusedCombinationsRefuseBeforeAnyTransaction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPSetFunctionPropsInternal
{
	/** One refused cell: what to ask for, and the reason token and class it must return. */
	struct FFPRefusedCell
	{
		FString Label;
		FString Function;
		FFPRequest Request;
		FString Reason;
		FString Class;
	};

	class FFP_RefusalCommand : public FFP_FixtureCommand
	{
	public:
		using FFP_FixtureCommand::FFP_FixtureCommand;

		static const TCHAR* ConstFn()      { return TEXT("SFPRefConst"); }
		static const TCHAR* PureFn()       { return TEXT("SFPRefPure"); }
		static const TCHAR* StaticFn()     { return TEXT("SFPRefStatic"); }
		static const TCHAR* OutputsFn()    { return TEXT("SFPRefOutputs"); }
		static const TCHAR* SelfBodyFn()   { return TEXT("SFPRefSelfBody"); }
		static const TCHAR* ExecBodyFn()   { return TEXT("SFPRefExecBody"); }
		static const TCHAR* PlainFn()      { return TEXT("SFPRefPlain"); }
		static const TCHAR* WiredBodyFn()  { return TEXT("SFPRefWiredBody"); }
		static const TCHAR* MemberVar()    { return TEXT("SFPRefMember"); }

	protected:
		virtual bool BuildFixture(UBlueprint& Blueprint, FString& OutError) override
		{
			FFPCreate Plain;

			FFPCreate ConstCreate;
			ConstCreate.bConst = true;

			FFPCreate PureCreate;
			PureCreate.bPure = true;
			PureCreate.Outputs.Add({TEXT("ReturnValue"), TEXT("bool")});

			FFPCreate StaticCreate;
			StaticCreate.bStatic = true;

			FFPCreate OutputsCreate;
			OutputsCreate.Outputs.Add({TEXT("ReturnValue"), TEXT("int")});

			if (!SFP_AddFunction(*Test, AssetPath, PlainFn(), Plain)
				|| !SFP_AddFunction(*Test, AssetPath, ConstFn(), ConstCreate)
				|| !SFP_AddFunction(*Test, AssetPath, PureFn(), PureCreate)
				|| !SFP_AddFunction(*Test, AssetPath, StaticFn(), StaticCreate)
				|| !SFP_AddFunction(*Test, AssetPath, OutputsFn(), OutputsCreate)
				|| !SFP_AddFunction(*Test, AssetPath, SelfBodyFn(), Plain)
				|| !SFP_AddFunction(*Test, AssetPath, ExecBodyFn(), Plain))
			{
				OutError = TEXT("one of the refusal fixtures could not be created.");
				return false;
			}

			// A self-context member read must prevent making the function static.
			FEdGraphPinType BoolType;
			BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
			if (!FBlueprintEditorUtils::AddMemberVariable(&Blueprint, FName(MemberVar()), BoolType))
			{
				OutError = TEXT("the member variable the static preflight needs could not be added.");
				return false;
			}
			UEdGraph* SelfBody = SFP_FindGraph(&Blueprint, SelfBodyFn());
			if (!IsValid(SelfBody))
			{
				OutError = TEXT("the self-body fixture graph is missing.");
				return false;
			}
			{
				UK2Node_VariableGet* Get = NewObject<UK2Node_VariableGet>(SelfBody);
				SelfBody->AddNode(Get, /*bFromUI=*/false, /*bSelectNewNode=*/false);
				Get->VariableReference.SetSelfMember(FName(MemberVar()));
				Get->CreateNewGuid();
				Get->NodePosX = 400;
				Get->PostPlacedNewNode();
				Get->AllocateDefaultPins();
			}

			// Reading through an explicit object parameter is valid in a static function.
			if (!SFP_AddFunction(*Test, AssetPath, WiredBodyFn(), Plain))
			{
				OutError = TEXT("the wired-body fixture could not be created.");
				return false;
			}
			{
				UEdGraph* WiredBody = SFP_FindGraph(&Blueprint, WiredBodyFn());
				UK2Node_FunctionEntry* WiredEntry = SFP_FindEntry(WiredBody);
				if (!IsValid(WiredBody) || !IsValid(WiredEntry))
				{
					OutError = TEXT("the wired-body fixture graph is missing.");
					return false;
				}
				FEdGraphPinType ObjType;
				ObjType.PinCategory = UEdGraphSchema_K2::PC_Object;
				ObjType.PinSubCategoryObject = Blueprint.GeneratedClass
					? Blueprint.GeneratedClass.Get() : AActor::StaticClass();
				WiredEntry->CreateUserDefinedPin(FName(TEXT("Receiver")), ObjType, EGPD_Output);

				UK2Node_VariableGet* Get = NewObject<UK2Node_VariableGet>(WiredBody);
				WiredBody->AddNode(Get, /*bFromUI=*/false, /*bSelectNewNode=*/false);
				Get->VariableReference.SetSelfMember(FName(MemberVar()));
				Get->CreateNewGuid();
				Get->NodePosX = 400;
				Get->NodePosY = 200;
				Get->PostPlacedNewNode();
				Get->AllocateDefaultPins();

				UEdGraphPin* ReceiverOut = WiredEntry->FindPin(FName(TEXT("Receiver")), EGPD_Output);
				UEdGraphPin* TargetIn = Get->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input);
				if (ReceiverOut == nullptr || TargetIn == nullptr)
				{
					OutError = TEXT("the wired-body fixture could not be wired.");
					return false;
				}
				ReceiverOut->MakeLinkTo(TargetIn);
			}

			// An exec-connected body must not become pure and lose execution.
			UEdGraph* ExecBody = SFP_FindGraph(&Blueprint, ExecBodyFn());
			UK2Node_FunctionEntry* ExecEntry = SFP_FindEntry(ExecBody);
			UFunction* PrintString = UKismetSystemLibrary::StaticClass()->FindFunctionByName(
				TEXT("PrintString"));
			if (!IsValid(ExecBody) || !IsValid(ExecEntry) || PrintString == nullptr)
			{
				OutError = TEXT("the exec-body fixture could not be located.");
				return false;
			}
			{
				UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(ExecBody);
				ExecBody->AddNode(Call, /*bFromUI=*/false, /*bSelectNewNode=*/false);
				Call->SetFromFunction(PrintString);
				Call->CreateNewGuid();
				Call->NodePosX = 400;
				Call->PostPlacedNewNode();
				Call->AllocateDefaultPins();

				UEdGraphPin* Then = ExecEntry->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
				UEdGraphPin* Exec = Call->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
				if (Then == nullptr || Exec == nullptr)
				{
					OutError = TEXT("the exec-body fixture could not be wired.");
					return false;
				}
				Then->MakeLinkTo(Exec);
			}

			FKismetEditorUtilities::CompileBlueprint(&Blueprint);
			return true;
		}

		void BuildCells()
		{
			Cells.Reset();

			// --- Proven invalid.
			{
				// Validate merged state: is_static alone must reject an already-const function.
				FFPRefusedCell Cell;
				Cell.Label = TEXT("static on an already-const function (the DEC-23 shape)");
				Cell.Function = ConstFn();
				Cell.Request.bStatic = true;
				Cell.Reason = TEXT("static_plus_const");
				Cell.Class = TEXT("invalid");
				Cells.Add(MoveTemp(Cell));
			}
			{
				FFPRefusedCell Cell;
				Cell.Label = TEXT("static and const supplied together");
				Cell.Function = PlainFn();
				Cell.Request.bStatic = true;
				Cell.Request.bConst = true;
				Cell.Reason = TEXT("static_plus_const");
				Cell.Class = TEXT("invalid");
				Cells.Add(MoveTemp(Cell));
			}
			{
				FFPRefusedCell Cell;
				Cell.Label = TEXT("pure with a connected exec body");
				Cell.Function = ExecBodyFn();
				Cell.Request.bPure = true;
				Cell.Reason = TEXT("pure_with_exec_body");
				Cell.Class = TEXT("invalid");
				Cells.Add(MoveTemp(Cell));
			}
			{
				FFPRefusedCell Cell;
				Cell.Label = TEXT("no property supplied");
				Cell.Function = PlainFn();
				Cell.Reason = TEXT("no_property_supplied");
				Cell.Class = TEXT("invalid");
				Cells.Add(MoveTemp(Cell));
			}

			// --- Unverified: refused because this gate did not establish them.
			{
				FFPRefusedCell Cell;
				Cell.Label = TEXT("pure + RPC, supplied together");
				Cell.Function = PlainFn();
				Cell.Request.bPure = true;
				Cell.Request.NetCall = FString(TEXT("Server"));
				Cell.Reason = TEXT("pure_plus_rpc");
				Cell.Class = TEXT("unverified");
				Cells.Add(MoveTemp(Cell));
			}
			{
				FFPRefusedCell Cell;
				Cell.Label = TEXT("static + RPC, supplied together");
				Cell.Function = PlainFn();
				Cell.Request.bStatic = true;
				Cell.Request.NetCall = FString(TEXT("Client"));
				Cell.Reason = TEXT("static_plus_rpc");
				Cell.Class = TEXT("unverified");
				Cells.Add(MoveTemp(Cell));
			}
			{
				// Only networking is supplied; the existing static flag still participates in validation.
				FFPRefusedCell Cell;
				Cell.Label = TEXT("RPC alone on an already-static function");
				Cell.Function = StaticFn();
				Cell.Request.NetCall = FString(TEXT("Server"));
				Cell.Reason = TEXT("static_plus_rpc");
				Cell.Class = TEXT("unverified");
				Cells.Add(MoveTemp(Cell));
			}
			{
				FFPRefusedCell Cell;
				Cell.Label = TEXT("RPC on a function with declared outputs");
				Cell.Function = OutputsFn();
				Cell.Request.NetCall = FString(TEXT("NetMulticast"));
				Cell.Reason = TEXT("rpc_with_outputs");
				Cell.Class = TEXT("unverified");
				Cells.Add(MoveTemp(Cell));
			}
			{
				FFPRefusedCell Cell;
				Cell.Label = TEXT("static on a body that reads a member variable");
				Cell.Function = SelfBodyFn();
				Cell.Request.bStatic = true;
				Cell.Reason = TEXT("static_body_uses_self");
				Cell.Class = TEXT("unverified");
				Cells.Add(MoveTemp(Cell));
			}
			{
				FFPRefusedCell Cell;
				Cell.Label = TEXT("a function this Blueprint does not own");
				Cell.Function = TEXT("SFPNoSuchFunction");
				Cell.Request.bConst = true;
				Cell.Reason = TEXT("function_not_owned");
				Cell.Class = TEXT("unverified");
				Cells.Add(MoveTemp(Cell));
			}
		}

		virtual void RunBody(UBlueprint& Blueprint) override
		{
			BuildCells();
			Test->TestEqual(TEXT("every refused cell that needs only one asset is driven here"),
				Cells.Num(), 10);

			for (const FFPRefusedCell& Cell : Cells)
			{
				const FString What = FString::Printf(TEXT("refused/%s"), *Cell.Label);

				const int32 FlagsBefore = SFP_EntryFlags(&Blueprint, Cell.Function);
				UK2Node_FunctionEntry* EntryBefore =
					SFP_FindEntry(SFP_FindGraph(&Blueprint, Cell.Function));
				const FString CategoryBefore = IsValid(EntryBefore)
					? EntryBefore->MetaData.Category.ToString() : FString();

				const FFPTxnWindow Window = SFP_OpenWindow(*Test);
				const IClaireonTool::FToolResult Result =
					SFP_SetProperties(AssetPath, Cell.Function, Cell.Request);

				if (!Result.bIsError)
				{
					Test->AddError(FString::Printf(
						TEXT("%s: the setter ACCEPTED a combination 080 refuses."), *What));
					continue;
				}
				if (!Result.Data.IsValid())
				{
					Test->AddError(FString::Printf(
						TEXT("%s: the refusal carried no structured data, so a caller cannot tell "
						     "whether routing flags changed."), *What));
					continue;
				}

				SFP_ExpectString(*Test, Result.Data, TEXT("mutation_state"),
					SFP_State(EClaireonMutationState::Refused), What);
				SFP_ExpectBool(*Test, Result.Data, TEXT("mutation_retained"), false, What);
				SFP_ExpectString(*Test, Result.Data, TEXT("failed_phase"),
					SFP_Phase(EClaireonBPSetterPhase::MergedStateValidation), What);
				SFP_ExpectString(*Test, Result.Data, TEXT("last_completed_phase"),
					FString(kClaireonBPPhaseNone), What);
				SFP_ExpectBool(*Test, Result.Data, TEXT("undo_record_available"), false, What);
				SFP_ExpectString(*Test, Result.Data, TEXT("quiescence_proof"),
					FString(TEXT("by_construction")), What);
				SFP_ExpectString(*Test, Result.Data, TEXT("refusal_class"), Cell.Class, What);

				SFP_ExpectAbsent(*Test, Result.Data, TEXT("operation_delta"), What,
					TEXT("a refusal runs before the operation-start snapshot, so it has no "
					     "snapshot pair to diff"));

				const TArray<FString> Reasons = SFP_RefusalReasons(Result.Data);
				Test->TestTrue(FString::Printf(
					TEXT("%s: the refusal names '%s' (it named: %s)"),
					*What, *Cell.Reason, *SFP_Join(Reasons)), Reasons.Contains(Cell.Reason));

				SFP_ExpectNoTransactionOpened(*Test, Window, What);

				Test->TestEqual(FString::Printf(
					TEXT("%s: the entry node's extra flags did not move"), *What),
					SFP_EntryFlags(&Blueprint, Cell.Function), FlagsBefore);
				UK2Node_FunctionEntry* EntryAfter =
					SFP_FindEntry(SFP_FindGraph(&Blueprint, Cell.Function));
				if (IsValid(EntryAfter))
				{
					Test->TestEqual(FString::Printf(
						TEXT("%s: the entry node's category did not move either -- a refusal that "
						     "wrote metadata before validating would be a silent partial apply"),
						*What), EntryAfter->MetaData.Category.ToString(), CategoryBefore);
				}
			}

			// An explicit object receiver must remain valid when making the function static.
			{
				FFPRequest Request;
				Request.bStatic = true;
				const IClaireonTool::FToolResult Result =
					SFP_SetProperties(AssetPath, WiredBodyFn(), Request);
				Test->TestFalse(FString::Printf(
					TEXT("allowed/member access through a wired receiver converts to static (%s)"),
					*Result.ErrorMessage), Result.bIsError);
				if (Result.bIsError && Result.Data.IsValid())
				{
					const TArray<FString> Reasons = SFP_RefusalReasons(Result.Data);
					Test->TestFalse(FString::Printf(
						TEXT("the refusal wrongly names static_body_uses_self (named: %s)"),
						*SFP_Join(Reasons)),
						Reasons.Contains(FString(TEXT("static_body_uses_self"))));
				}
			}
		}

	private:
		TArray<FFPRefusedCell> Cells;
	};
}

bool FClaireonBPEditorFunctionPropertiesRefusals::RunTest(const FString& /*Parameters*/)
{
	using namespace ClaireonBPSetFunctionPropsInternal;
	ADD_LATENT_AUTOMATION_COMMAND(FFP_RefusalCommand(
		this, ClaireonBPEditorFixtures::UniquePath(TEXT("SfpRefuse"))));
	return true;
}

// Shared refusal assertions for interface and inherited graphs.
namespace ClaireonBPSetFunctionPropsInternal
{
	void SFP_ExpectRefusedAs(FAutomationTestBase& Test, const FString& AssetPath,
		const FString& Function, const TCHAR* ExpectedReason, const FString& What)
	{
		const FFPTxnWindow Window = SFP_OpenWindow(Test);
		FFPRequest Request;
		Request.bConst = true;
		const IClaireonTool::FToolResult Result = SFP_SetProperties(AssetPath, Function, Request);

		if (!Result.bIsError || !Result.Data.IsValid())
		{
			Test.AddError(FString::Printf(
				TEXT("%s: the setter ACCEPTED a property change on a graph 080 records as "
				     "unverified."), *What));
			return;
		}

		SFP_ExpectString(Test, Result.Data, TEXT("mutation_state"),
			SFP_State(EClaireonMutationState::Refused), What);
		SFP_ExpectBool(Test, Result.Data, TEXT("mutation_retained"), false, What);
		SFP_ExpectString(Test, Result.Data, TEXT("failed_phase"),
			SFP_Phase(EClaireonBPSetterPhase::MergedStateValidation), What);
		SFP_ExpectString(Test, Result.Data, TEXT("last_completed_phase"),
			FString(kClaireonBPPhaseNone), What);
		SFP_ExpectBool(Test, Result.Data, TEXT("undo_record_available"), false, What);
		SFP_ExpectString(Test, Result.Data, TEXT("refusal_class"),
			FString(TEXT("unverified")), What);
		SFP_ExpectString(Test, Result.Data, TEXT("quiescence_proof"),
			FString(TEXT("by_construction")), What);
		SFP_ExpectAbsent(Test, Result.Data, TEXT("operation_delta"), What,
			TEXT("a refusal runs before the operation-start snapshot"));

		const TArray<FString> Reasons = SFP_RefusalReasons(Result.Data);
		Test.TestTrue(FString::Printf(
			TEXT("%s: the refusal names '%s' rather than the generic 'function_not_owned' "
			     "(it named: %s)"), *What, ExpectedReason, *SFP_Join(Reasons)),
			Reasons.Contains(FString(ExpectedReason)));

		SFP_ExpectNoTransactionOpened(Test, Window, What);
	}
}

// Use a return-valued interface function so implementation creates a graph rather than an event.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorFunctionPropertiesInterface,
	"Claireon.BPEditor.FunctionProperties.InterfaceImplementationsRefuse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPSetFunctionPropsInternal
{
	class FFP_InterfaceCommand : public FFP_FixtureCommand
	{
	public:
		FFP_InterfaceCommand(FAutomationTestBase* InTest, const FString& InAssetPath,
			const FString& InInterfacePath)
			: FFP_FixtureCommand(InTest, InAssetPath)
			, InterfacePath(InInterfacePath)
		{
		}

		static const TCHAR* InterfaceFunction() { return TEXT("SFPIfaceQuery"); }

	protected:
		virtual bool BuildFixture(UBlueprint& Blueprint, FString& OutError) override
		{
			{
				TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
				Args->SetStringField(TEXT("asset_path"), InterfacePath);
				Args->SetStringField(TEXT("blueprint_type"), TEXT("Interface"));
				ClaireonBlueprintGraphTool_Create Tool;
				const IClaireonTool::FToolResult Result = Tool.Execute(Args);
				if (Result.bIsError)
				{
					OutError = FString::Printf(
						TEXT("the interface Blueprint could not be created: %s"), *Result.ErrorMessage);
					return false;
				}
			}

			FFPCreate Query;
			Query.Outputs.Add({TEXT("ReturnValue"), TEXT("bool")});
			if (!SFP_AddFunction(*Test, InterfacePath, InterfaceFunction(), Query))
			{
				OutError = TEXT("the interface function could not be declared.");
				return false;
			}

			UBlueprint* Interface =
				Cast<UBlueprint>(ClaireonBPEditorFixtures::Resolve(InterfacePath));
			if (!IsValid(Interface))
			{
				OutError = TEXT("the interface Blueprint did not resolve after creation.");
				return false;
			}
			FKismetEditorUtilities::CompileBlueprint(Interface);
			if (!IsValid(Interface->GeneratedClass))
			{
				OutError = TEXT("the interface Blueprint produced no generated class.");
				return false;
			}

			// Release the interface session before testing its implementer and unloading fixtures.
			FClaireonSessionManager::Get().ReleaseByAssetPath(InterfacePath);

			if (!FBlueprintEditorUtils::ImplementNewInterface(&Blueprint,
					Interface->GeneratedClass->GetClassPathName()))
			{
				OutError = TEXT("the fixture could not implement the interface.");
				return false;
			}
			FKismetEditorUtilities::CompileBlueprint(&Blueprint);
			return true;
		}

		virtual void RunBody(UBlueprint& Blueprint) override
		{
			bool bInInterfaceGraphs = false;
			for (const FBPInterfaceDescription& Description : Blueprint.ImplementedInterfaces)
			{
				for (const UEdGraph* Graph : Description.Graphs)
				{
					bInInterfaceGraphs = bInInterfaceGraphs
						|| (IsValid(Graph) && Graph->GetName().Equals(InterfaceFunction()));
				}
			}
			if (!bInInterfaceGraphs)
			{
				Test->AddError(TEXT("the implemented interface produced no graph named "
					"SFPIfaceQuery, so the interface refusal cannot be driven and would pass "
					"vacuously against function_not_owned instead."));
				return;
			}

			SFP_ExpectRefusedAs(*Test, AssetPath, InterfaceFunction(), TEXT("interface_function"),
				TEXT("refused/interface implementation"));
		}

		virtual void Cleanup() override
		{
			ClaireonBPEditorFixtures::Teardown(InterfacePath);
		}

	private:
		FString InterfacePath;
	};
}

bool FClaireonBPEditorFunctionPropertiesInterface::RunTest(const FString& /*Parameters*/)
{
	using namespace ClaireonBPSetFunctionPropsInternal;
	ADD_LATENT_AUTOMATION_COMMAND(FFP_InterfaceCommand(
		this,
		ClaireonBPEditorFixtures::UniquePath(TEXT("SfpIfaceImpl")),
		ClaireonBPEditorFixtures::UniquePath(TEXT("SfpIface"))));
	return true;
}

// Build a real inherited override with the parent class as signature source.
// Use UObject-based Blueprints to avoid DefaultSceneRoot collisions.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorFunctionPropertiesInherited,
	"Claireon.BPEditor.FunctionProperties.InheritedOverridesRefuse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPSetFunctionPropsInternal
{
	class FFP_InheritedCommand : public FFP_FixtureCommand
	{
	public:
		FFP_InheritedCommand(FAutomationTestBase* InTest, const FString& InAssetPath,
			const FString& InParentPath)
			: FFP_FixtureCommand(InTest, InAssetPath)
			, ParentPath(InParentPath)
		{
		}

		static const TCHAR* InheritedFunction() { return TEXT("SFPInheritedQuery"); }

	protected:
		virtual UBlueprint* CreateFixture(FString& OutError) override
		{
			return SFP_CreateBlueprint(AssetPath, TEXT("Object"), OutError);
		}

		virtual bool BuildFixture(UBlueprint& Blueprint, FString& OutError) override
		{
			UBlueprint* Parent = SFP_CreateBlueprint(ParentPath, TEXT("Object"), OutError);
			if (!IsValid(Parent))
			{
				return false;
			}

			// A return value requires a function override graph rather than an event.
			FFPCreate Query;
			Query.Outputs.Add({TEXT("ReturnValue"), TEXT("bool")});
			if (!SFP_AddFunction(*Test, ParentPath, InheritedFunction(), Query))
			{
				OutError = TEXT("the parent's function could not be declared.");
				return false;
			}
			FKismetEditorUtilities::CompileBlueprint(Parent);

			UClass* ParentClass = Parent->GeneratedClass;
			if (!IsValid(ParentClass)
				|| ParentClass->FindFunctionByName(FName(InheritedFunction())) == nullptr)
			{
				OutError = TEXT("the parent Blueprint's generated class does not carry the "
					"function the child is meant to override, so the refusal could not fire.");
				return false;
			}
			FClaireonSessionManager::Get().ReleaseByAssetPath(ParentPath);

			// Recompile after reparenting before creating the override graph.
			Blueprint.ParentClass = ParentClass;
			FBlueprintEditorUtils::RefreshAllNodes(&Blueprint);
			FKismetEditorUtilities::CompileBlueprint(&Blueprint);

			UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(&Blueprint,
				FName(InheritedFunction()), UEdGraph::StaticClass(),
				UEdGraphSchema_K2::StaticClass());
			if (!IsValid(Graph))
			{
				OutError = TEXT("the override graph could not be created.");
				return false;
			}
			FBlueprintEditorUtils::AddFunctionGraph<UClass>(&Blueprint, Graph,
				/*bIsUserCreated=*/false, ParentClass);
			FKismetEditorUtilities::CompileBlueprint(&Blueprint);
			return true;
		}

		virtual void RunBody(UBlueprint& Blueprint) override
		{
			// Verify ownership and inherited signature so the refusal cannot pass for the wrong reason.
			UEdGraph* Override = nullptr;
			for (UEdGraph* Graph : Blueprint.FunctionGraphs)
			{
				if (IsValid(Graph) && Graph->GetName().Equals(InheritedFunction()))
				{
					Override = Graph;
				}
			}
			UK2Node_FunctionEntry* Entry = SFP_FindEntry(Override);
			const bool bResolves = IsValid(Entry)
				&& Entry->FunctionReference.ResolveMember<UFunction>(
					Entry->GetBlueprintClassFromNode()) != nullptr;
			if (!IsValid(Override) || !bResolves)
			{
				Test->AddError(FString::Printf(
					TEXT("the override graph is %s and its entry %s a signature UFunction, so the "
					     "inherited refusal cannot be driven and would pass vacuously."),
					IsValid(Override) ? TEXT("present in FunctionGraphs") : TEXT("ABSENT"),
					bResolves ? TEXT("resolves") : TEXT("does NOT resolve")));
				return;
			}

			SFP_ExpectRefusedAs(*Test, AssetPath, InheritedFunction(), TEXT("inherited_function"),
				TEXT("refused/inherited override"));
		}

		virtual void Cleanup() override
		{
			ClaireonBPEditorFixtures::Teardown(ParentPath);
		}

	private:
		FString ParentPath;
	};
}

bool FClaireonBPEditorFunctionPropertiesInherited::RunTest(const FString& /*Parameters*/)
{
	using namespace ClaireonBPSetFunctionPropsInternal;
	ADD_LATENT_AUTOMATION_COMMAND(FFP_InheritedCommand(
		this,
		ClaireonBPEditorFixtures::UniquePath(TEXT("SfpChild")),
		ClaireonBPEditorFixtures::UniquePath(TEXT("SfpParent"))));
	return true;
}

// Purity must remove gateway exec pins while the entry retains Then.
// A const-only change is the control: its gateway pins remain unchanged.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorFunctionPropertiesGateway,
	"Claireon.BPEditor.FunctionProperties.PurityRefreshesTheGatewayCallNode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPSetFunctionPropsInternal
{
	class FFP_GatewayCommand : public FFP_FixtureCommand
	{
	public:
		using FFP_FixtureCommand::FFP_FixtureCommand;

		static const TCHAR* PurifiedFn() { return TEXT("SFPGatewayPurified"); }
		static const TCHAR* ConstFn()    { return TEXT("SFPGatewayConst"); }
		static const TCHAR* HostEvent()  { return TEXT("SFPGatewayHost"); }

	protected:
		virtual bool BuildFixture(UBlueprint& Blueprint, FString& OutError) override
		{
			FFPCreate WithReturn;
			WithReturn.Outputs.Add({TEXT("ReturnValue"), TEXT("bool")});
			if (!SFP_AddFunction(*Test, AssetPath, PurifiedFn(), WithReturn)
				|| !SFP_AddFunction(*Test, AssetPath, ConstFn(), WithReturn))
			{
				OutError = TEXT("the gateway fixtures could not be created.");
				return false;
			}
			FKismetEditorUtilities::CompileBlueprint(&Blueprint);

			// Use loaded, non-transient call sites so the engine reconstruction pass includes them.
			UEdGraph* Uber = ClaireonBPEditorFixtures::FirstUbergraph(&Blueprint);
			if (!IsValid(Uber))
			{
				OutError = TEXT("the fixture has no ubergraph to host the call sites.");
				return false;
			}

			UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Uber);
			Uber->AddNode(Event, /*bFromUI=*/false, /*bSelectNewNode=*/false);
			Event->CreateNewGuid();
			Event->CustomFunctionName = FName(HostEvent());
			Event->AllocateDefaultPins();

			UK2Node_CallFunction* PurifiedCall = SFP_AddSelfCall(*Uber, PurifiedFn(), 400, 0);
			UK2Node_CallFunction* ConstCall = SFP_AddSelfCall(*Uber, ConstFn(), 400, 300);

			UEdGraphPin* Then = Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
			UEdGraphPin* PurifiedExec = PurifiedCall->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
			UEdGraphPin* ConstExec = ConstCall->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
			UEdGraphPin* PurifiedThen = PurifiedCall->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
			if (Then == nullptr || PurifiedExec == nullptr || ConstExec == nullptr
				|| PurifiedThen == nullptr)
			{
				OutError = TEXT("the gateway call sites could not be wired: an exec pin is missing.");
				return false;
			}
			Then->MakeLinkTo(PurifiedExec);
			PurifiedThen->MakeLinkTo(ConstExec);

			PurifiedGuid = PurifiedCall->NodeGuid;
			ConstGuid = ConstCall->NodeGuid;
			FKismetEditorUtilities::CompileBlueprint(&Blueprint);
			return true;
		}

		UEdGraphNode* FindByGuid(UBlueprint& Blueprint, const FGuid& Guid)
		{
			TArray<UEdGraph*> Graphs;
			Blueprint.GetAllGraphs(Graphs);
			for (UEdGraph* Graph : Graphs)
			{
				if (!IsValid(Graph))
				{
					continue;
				}
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (IsValid(Node) && Node->NodeGuid == Guid)
					{
						return Node;
					}
				}
			}
			return nullptr;
		}

		virtual void RunBody(UBlueprint& Blueprint) override
		{
			UEdGraphNode* Gateway = FindByGuid(Blueprint, PurifiedGuid);
			if (!IsValid(Gateway))
			{
				Test->AddError(TEXT("the gateway call node is missing before the change."));
				return;
			}
			const TArray<FString> GatewayBefore = SFP_ExecPinNames(Gateway);
			Test->TestTrue(FString::Printf(
				TEXT("premise: the impure gateway carries exec pins before the change (%s)"),
				*SFP_Join(GatewayBefore)), GatewayBefore.Num() > 0);

			UEdGraphNode* ConstGateway = FindByGuid(Blueprint, ConstGuid);
			const TArray<FString> ConstBefore = SFP_ExecPinNames(ConstGateway);
			{
				FFPRequest Request;
				Request.bConst = true;
				const IClaireonTool::FToolResult Result =
					SFP_SetProperties(AssetPath, ConstFn(), Request);
				if (Result.bIsError)
				{
					Test->AddError(FString::Printf(
						TEXT("the const-only change was rejected: %s"), *Result.ErrorMessage));
				}
				else
				{
					ConstGateway = FindByGuid(Blueprint, ConstGuid);
					const TArray<FString> ConstAfter = SFP_ExecPinNames(ConstGateway);
					Test->TestEqual(FString::Printf(
						TEXT("a const-only change reconstructs NEITHER the entry node nor the call "
						     "sites, so the gateway's exec pins do not move (before %s, after %s)"),
						*SFP_Join(ConstBefore), *SFP_Join(ConstAfter)),
						SFP_Join(ConstAfter), SFP_Join(ConstBefore));

					if (Result.Data.IsValid())
					{
						FString LastPhase;
						SFP_ReadString(*Test, Result.Data, TEXT("last_completed_phase"), LastPhase);
						Test->TestEqual(
							TEXT("the const-only change reports setter_compile_validate as its last "
							     "completed phase"),
							LastPhase, SFP_Phase(EClaireonBPSetterPhase::CompileValidate));
					}
				}
			}

			{
				FFPRequest Request;
				Request.bPure = true;
				const IClaireonTool::FToolResult Result =
					SFP_SetProperties(AssetPath, PurifiedFn(), Request);
				if (Result.bIsError)
				{
					Test->AddError(FString::Printf(
						TEXT("making an exec-free function pure was rejected: %s"),
						*Result.ErrorMessage));
					return;
				}
				// The final compile phase cannot establish whether call-site refresh occurred; inspect pins.
				if (Result.Data.IsValid())
				{
					SFP_ExpectString(*Test, Result.Data, TEXT("last_completed_phase"),
						SFP_Phase(EClaireonBPSetterPhase::CompileValidate),
						TEXT("purity change"));
				}
			}

			Gateway = FindByGuid(Blueprint, PurifiedGuid);
			if (!IsValid(Gateway))
			{
				Test->AddError(TEXT("the gateway call node did not survive the purity change."));
				return;
			}
			const TArray<FString> GatewayAfter = SFP_ExecPinNames(Gateway);
			Test->TestEqual(FString::Printf(
				TEXT("THE GATEWAY CALL NODE carries NO exec pins after the refresh -- orphans "
				     "included, because an orphaned exec pin is the semantic loss a "
				     "presence-only check reports as success (before %s, after %s)"),
				*SFP_Join(GatewayBefore), *SFP_Join(GatewayAfter)), GatewayAfter.Num(), 0);

			UK2Node_FunctionEntry* Entry = SFP_FindEntry(SFP_FindGraph(&Blueprint, PurifiedFn()));
			if (!IsValid(Entry))
			{
				Test->AddError(TEXT("the purified function's entry node is missing."));
				return;
			}
			Test->TestTrue(
				TEXT("the ENTRY node still carries its Then pin: AllocateDefaultPins creates it "
				     "UNCONDITIONALLY (K2Node_FunctionEntry.cpp:396), pure or not, so asserting "
				     "its absence would fail on a correctly repaired function"),
				Entry->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output) != nullptr);

			Test->TestTrue(TEXT("the entry node's FUNC_BlueprintPure bit is set"),
				(Entry->GetExtraFlags() & FUNC_BlueprintPure) != 0);
			UFunction* Function = SFP_SkeletonFunction(&Blueprint, PurifiedFn());
			Test->TestTrue(TEXT("the skeleton UFunction's FUNC_BlueprintPure bit is set"),
				Function != nullptr && (Function->FunctionFlags & FUNC_BlueprintPure) != 0);
		}

	private:
		FGuid PurifiedGuid;
		FGuid ConstGuid;
	};
}

bool FClaireonBPEditorFunctionPropertiesGateway::RunTest(const FString& /*Parameters*/)
{
	using namespace ClaireonBPSetFunctionPropsInternal;
	ADD_LATENT_AUTOMATION_COMMAND(FFP_GatewayCommand(
		this, ClaireonBPEditorFixtures::UniquePath(TEXT("SfpGateway"))));
	return true;
}

// Cycle network modes and inspect both entry flags and the re-found skeleton function.
// None must clear reliability too; these tests do not establish multiplayer behavior.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorFunctionPropertiesNetCycle,
	"Claireon.BPEditor.FunctionProperties.NetCycleClearsReliabilityOnBothSites",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPSetFunctionPropsInternal
{
	/** One leg of the cycle, and the exact five-bit word both sites must carry after it. */
	struct FFPNetLeg
	{
		const TCHAR* Mode;
		int32 ExpectedNetBits;
		bool bReliable;
	};

	class FFP_NetCycleCommand : public FFP_FixtureCommand
	{
	public:
		using FFP_FixtureCommand::FFP_FixtureCommand;

		static const TCHAR* NetFn() { return TEXT("SFPNetCycle"); }

	protected:
		virtual bool BuildFixture(UBlueprint& Blueprint, FString& OutError) override
		{
			FFPCreate Create;
			if (!SFP_AddFunction(*Test, AssetPath, NetFn(), Create))
			{
				OutError = TEXT("the net-cycle fixture could not be created.");
				return false;
			}
			FKismetEditorUtilities::CompileBlueprint(&Blueprint);
			return true;
		}

		virtual void RunBody(UBlueprint& Blueprint) override
		{
			const int32 Mask = ClaireonBPFunctionRecipe::kNetModeClearMask;

			Test->TestEqual(TEXT("premise: the fixture starts with no net bits set"),
				SFP_EntryFlags(&Blueprint, NetFn()) & Mask, 0);

			const FFPNetLeg Legs[] = {
				{TEXT("Server"),       FUNC_Net | FUNC_NetServer    | FUNC_NetReliable, true},
				{TEXT("Client"),       FUNC_Net | FUNC_NetClient    | FUNC_NetReliable, true},
				{TEXT("NetMulticast"), FUNC_Net | FUNC_NetMulticast | FUNC_NetReliable, true},
				{TEXT("None"),         0,                                               false},
			};

			for (const FFPNetLeg& Leg : Legs)
			{
				const FString What = FString::Printf(TEXT("net cycle -> %s"), Leg.Mode);

				FFPRequest Request;
				Request.NetCall = FString(Leg.Mode);
				const IClaireonTool::FToolResult Result =
					SFP_SetProperties(AssetPath, NetFn(), Request);

				if (Result.bIsError || !Result.Data.IsValid())
				{
					Test->AddError(FString::Printf(TEXT("%s: the leg failed: %s"),
						*What, *Result.ErrorMessage));
					return;
				}
				SFP_ExpectString(*Test, Result.Data, TEXT("mutation_state"),
					SFP_State(EClaireonMutationState::AppliedClean), What);

				const int32 EntryBits = SFP_EntryFlags(&Blueprint, NetFn()) & Mask;
				Test->TestEqual(FString::Printf(
					TEXT("%s: the entry node's five managed net bits are exactly %s"),
					*What, *SFP_Hex(Leg.ExpectedNetBits)), EntryBits, Leg.ExpectedNetBits);

				UFunction* Function = SFP_SkeletonFunction(&Blueprint, NetFn());
				if (Function == nullptr)
				{
					Test->AddError(FString::Printf(
						TEXT("%s: the skeleton UFunction could not be re-found after the compile, "
						     "so the second durable site is unobserved."), *What));
					return;
				}
				const int32 FunctionBits = static_cast<int32>(Function->FunctionFlags) & Mask;
				Test->TestEqual(FString::Printf(
					TEXT("%s: the generated function's five managed net bits are exactly %s"),
					*What, *SFP_Hex(Leg.ExpectedNetBits)), FunctionBits, Leg.ExpectedNetBits);

				Test->TestTrue(FString::Printf(
					TEXT("%s: FUNC_NetReliable on the entry node is %s -- the engine's own "
					     "SetNetFlags omits this bit from its clear mask "
					     "(BlueprintDetailsCustomization.cpp:5094), so a non-networked function "
					     "left marked reliable is the exact defect this mask prevents"),
					*What, Leg.bReliable ? TEXT("SET") : TEXT("CLEAR")),
					((EntryBits & FUNC_NetReliable) != 0) == Leg.bReliable);
				Test->TestTrue(FString::Printf(
					TEXT("%s: FUNC_NetReliable on the generated function is %s"),
					*What, Leg.bReliable ? TEXT("SET") : TEXT("CLEAR")),
					((FunctionBits & FUNC_NetReliable) != 0) == Leg.bReliable);

				const int32 ModeBits = FUNC_NetServer | FUNC_NetClient | FUNC_NetMulticast;
				const int32 SetModeBits = EntryBits & ModeBits;
				Test->TestTrue(FString::Printf(
					TEXT("%s: at most ONE mutually exclusive mode bit is set (%s)"),
					*What, *SFP_Hex(SetModeBits)),
					SetModeBits == 0 || FMath::CountBits(static_cast<uint64>(SetModeBits)) == 1);

				const TSharedPtr<FJsonObject>* Net = nullptr;
				if (!Result.Data->TryGetObjectField(TEXT("network_call"), Net) || Net == nullptr)
				{
					Test->AddError(FString::Printf(
						TEXT("%s: the result carries no network_call block, so a caller must "
						     "decode a flag word to see an authority change."), *What));
					continue;
				}
				SFP_ExpectString(*Test, *Net, TEXT("mode_after"), FString(Leg.Mode), What);
				SFP_ExpectBool(*Test, *Net, TEXT("net_reliable_after_entry"), Leg.bReliable, What);
				SFP_ExpectBool(*Test, *Net, TEXT("net_reliable_after_skeleton"), Leg.bReliable, What);
				SFP_ExpectString(*Test, *Net, TEXT("entry_net_flags_after"),
					SFP_Hex(Leg.ExpectedNetBits), What);
				SFP_ExpectString(*Test, *Net, TEXT("skeleton_net_flags_after"),
					SFP_Hex(Leg.ExpectedNetBits), What);
				SFP_ExpectString(*Test, *Net, TEXT("clear_mask"), SFP_Hex(Mask), What);

				FString NotVerified;
				if (SFP_ReadString(*Test, Result.Data, TEXT("network_behaviour_not_verified"),
						NotVerified))
				{
					Test->TestTrue(FString::Printf(
						TEXT("%s: the result states that runtime RPC behaviour across a real "
						     "client/server boundary was NOT verified -- nothing in this suite "
						     "runs a client or a server"), *What),
						NotVerified.Contains(TEXT("NOT VERIFIED")));
				}
				FString Compatibility;
				SFP_ReadString(*Test, Result.Data, TEXT("network_compatibility_warning"),
					Compatibility);
			}

			// Check the full flag word to catch changes outside the managed mask.
			const int32 Final = SFP_EntryFlags(&Blueprint, NetFn());
			Test->TestEqual(TEXT("after the cycle the entry node carries none of the five net bits"),
				Final & Mask, 0);
			Test->TestTrue(
				TEXT("and the function is still callable -- the cycle cleared net bits, not the "
				     "whole flag word"),
				(Final & FUNC_BlueprintCallable) != 0);
		}
	};
}

bool FClaireonBPEditorFunctionPropertiesNetCycle::RunTest(const FString& /*Parameters*/)
{
	using namespace ClaireonBPSetFunctionPropsInternal;
	ADD_LATENT_AUTOMATION_COMMAND(FFP_NetCycleCommand(
		this, ClaireonBPEditorFixtures::UniquePath(TEXT("SfpNetCycle"))));
	return true;
}

// Save, unload, and reload to verify flags survive PostLoad.
// Static+const is refused because PostLoad clears const. Release sessions before unloading.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorFunctionPropertiesReload,
	"Claireon.BPEditor.FunctionProperties.AppliedPropertiesSurviveSaveAndReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPSetFunctionPropsInternal
{
	/** What the pre-reload half wrote, and the post-reload half must find again. */
	struct FFPReloadExpectation
	{
		FString Function;
		int32 EntryFlags = 0;
		FString Category;
		FString Tooltip;
	};

	class FFP_ReloadCommand : public IAutomationLatentCommand
	{
	public:
		FFP_ReloadCommand(FAutomationTestBase* InTest, const FString& InAssetPath)
			: Test(InTest)
			, AssetPath(InAssetPath)
		{
		}

		static const TCHAR* MetaFn()   { return TEXT("SFPTripMeta"); }
		static const TCHAR* ConstFn()  { return TEXT("SFPTripConst"); }
		static const TCHAR* StaticFn() { return TEXT("SFPTripStatic"); }
		static const TCHAR* NetFn()    { return TEXT("SFPTripNet"); }
		static const TCHAR* PureFn()   { return TEXT("SFPTripPure"); }

		virtual bool Update() override
		{
			switch (Phase)
			{
			case EPhase::Build:
			{
				FString BuildError;
				UBlueprint* Blueprint = ClaireonBPEditorFixtures::Create(AssetPath, BuildError);
				if (!IsValid(Blueprint) || !Build(*Blueprint, BuildError))
				{
					Test->AddError(FString::Printf(TEXT("fixture setup failed at %s: %s"),
						*AssetPath, *BuildError));
					Phase = EPhase::Teardown;
					return false;
				}
				Phase = EPhase::Settle;
				return false;
			}

			case EPhase::Settle:
				if (++SettleUpdates < 3)
				{
					return false;
				}
				Phase = EPhase::Apply;
				return false;

			case EPhase::Apply:
			{
				UBlueprint* Blueprint = Cast<UBlueprint>(ClaireonBPEditorFixtures::Resolve(AssetPath));
				if (!IsValid(Blueprint) || !ApplyAndSave(*Blueprint))
				{
					Phase = EPhase::Teardown;
					return false;
				}
				Phase = EPhase::Unload;
				return false;
			}

			case EPhase::Unload:
				if (!Unload())
				{
					Phase = EPhase::Teardown;
					return false;
				}
				Phase = EPhase::Verify;
				return false;

			case EPhase::Verify:
				Verify();
				Phase = EPhase::Teardown;
				return false;

			case EPhase::Teardown:
			default:
				ClaireonBPEditorFixtures::Teardown(AssetPath);
				return true;
			}
		}

	private:
		enum class EPhase : uint8
		{
			Build,
			Settle,
			Apply,
			Unload,
			Verify,
			Teardown,
		};

		bool Build(UBlueprint& Blueprint, FString& OutError)
		{
			FFPCreate Plain;
			FFPCreate WithReturn;
			WithReturn.Outputs.Add({TEXT("ReturnValue"), TEXT("bool")});

			if (!SFP_AddFunction(*Test, AssetPath, MetaFn(), Plain)
				|| !SFP_AddFunction(*Test, AssetPath, ConstFn(), Plain)
				|| !SFP_AddFunction(*Test, AssetPath, StaticFn(), Plain)
				|| !SFP_AddFunction(*Test, AssetPath, NetFn(), Plain)
				|| !SFP_AddFunction(*Test, AssetPath, PureFn(), WithReturn))
			{
				OutError = TEXT("the round-trip fixtures could not be created.");
				return false;
			}
			FKismetEditorUtilities::CompileBlueprint(&Blueprint);
			return true;
		}

		bool ApplyOne(const FString& Function, const FFPRequest& Request)
		{
			const IClaireonTool::FToolResult Result =
				SFP_SetProperties(AssetPath, Function, Request);
			if (Result.bIsError)
			{
				Test->AddError(FString::Printf(
					TEXT("round trip: the change on '%s' was rejected: %s"),
					*Function, *Result.ErrorMessage));
				return false;
			}
			return true;
		}

		bool ApplyAndSave(UBlueprint& Blueprint)
		{
			{
				FFPRequest Request;
				Request.Category = FString(TEXT("Stage019|Trip"));
				Request.Tooltip = FString(TEXT("Survives a reload."));
				Request.Access = FString(TEXT("Protected"));
				if (!ApplyOne(MetaFn(), Request)) { return false; }
			}
			{
				FFPRequest Request;
				Request.bConst = true;
				if (!ApplyOne(ConstFn(), Request)) { return false; }
			}
			{
				FFPRequest Request;
				Request.bStatic = true;
				if (!ApplyOne(StaticFn(), Request)) { return false; }
			}
			{
				FFPRequest Request;
				Request.NetCall = FString(TEXT("NetMulticast"));
				if (!ApplyOne(NetFn(), Request)) { return false; }
			}
			{
				FFPRequest Request;
				Request.bPure = true;
				if (!ApplyOne(PureFn(), Request)) { return false; }
			}

			Expectations.Reset();
			for (const TCHAR* Function : {MetaFn(), ConstFn(), StaticFn(), NetFn(), PureFn()})
			{
				UK2Node_FunctionEntry* Entry = SFP_FindEntry(SFP_FindGraph(&Blueprint, Function));
				if (!IsValid(Entry))
				{
					Test->AddError(FString::Printf(
						TEXT("round trip: '%s' has no entry node before the save."), Function));
					return false;
				}
				FFPReloadExpectation Expectation;
				Expectation.Function = Function;
				Expectation.EntryFlags = Entry->GetExtraFlags();
				Expectation.Category = Entry->MetaData.Category.ToString();
				Expectation.Tooltip = Entry->MetaData.ToolTip.ToString();
				Expectations.Add(MoveTemp(Expectation));
			}

			// Save before unloading so the disk copy contains the change.
			if (!ClaireonBPEditorFixtures::Save(&Blueprint))
			{
				Test->AddError(TEXT("round trip: the fixture package could not be saved, so there "
					"is nothing on disk to reload."));
				return false;
			}
			return true;
		}

		/** Drop the in-memory copy so the next read genuinely comes off disk. */
		bool Unload()
		{
			FClaireonSessionManager::Get().ReleaseByAssetPath(AssetPath);

			UPackage* Package = FindPackage(nullptr, *AssetPath);
			if (!IsValid(Package))
			{
				Test->AddError(TEXT("round trip: the fixture package is not resident before the "
					"unload, which means the changes ran somewhere this test cannot see."));
				return false;
			}

			TArray<UPackage*> ToUnload;
			ToUnload.Add(Package);
			UPackageTools::FUnloadPackageParams UnloadParams(ToUnload);
			UnloadParams.bUnloadDirtyPackages = true;
			UPackageTools::UnloadPackages(UnloadParams);
			CollectGarbage(RF_NoFlags);

			// Require the package to be absent before reload.
			const bool bGone = FindPackage(nullptr, *AssetPath) == nullptr;
			Test->TestTrue(FString::Printf(
				TEXT("round trip: the package is genuinely unloaded before anything reloads it (%s)"),
				UnloadParams.OutErrorMessage.IsEmpty()
					? TEXT("no error reported") : *UnloadParams.OutErrorMessage.ToString()),
				bGone);
			return bGone;
		}

		void Verify()
		{
			const FString ObjectPath = AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
			UBlueprint* Reloaded = LoadObject<UBlueprint>(nullptr, *ObjectPath);
			if (!IsValid(Reloaded))
			{
				Test->AddError(TEXT("round trip: the fixture did not reload from disk."));
				return;
			}

			for (const FFPReloadExpectation& Expectation : Expectations)
			{
				const FString What = FString::Printf(TEXT("round trip/%s"), *Expectation.Function);
				UK2Node_FunctionEntry* Entry =
					SFP_FindEntry(SFP_FindGraph(Reloaded, Expectation.Function));
				if (!IsValid(Entry))
				{
					Test->AddError(FString::Printf(
						TEXT("%s: the function did not survive the reload."), *What));
					continue;
				}

				// Compare the whole flag word to catch unrequested rewrites.
				Test->TestEqual(FString::Printf(
					TEXT("%s: the entry node's extra flags are identical after a save, an unload "
					     "and a reload from disk (%s)"), *What, *SFP_Hex(Expectation.EntryFlags)),
					Entry->GetExtraFlags(), Expectation.EntryFlags);
				Test->TestEqual(FString::Printf(TEXT("%s: MetaData.Category survived"), *What),
					Entry->MetaData.Category.ToString(), Expectation.Category);
				Test->TestEqual(FString::Printf(TEXT("%s: MetaData.ToolTip survived"), *What),
					Entry->MetaData.ToolTip.ToString(), Expectation.Tooltip);
			}

			FKismetEditorUtilities::CompileBlueprint(Reloaded);
			for (const FFPReloadExpectation& Expectation : Expectations)
			{
				UFunction* Function = SFP_SkeletonFunction(Reloaded, Expectation.Function);
				if (Function == nullptr)
				{
					Test->AddError(FString::Printf(
						TEXT("round trip/%s: the reloaded Blueprint's skeleton has no such function."),
						*Expectation.Function));
					continue;
				}
				const int32 Managed = FUNC_BlueprintPure | FUNC_Const | FUNC_Static
					| ClaireonBPFunctionRecipe::kAccessClearMask
					| ClaireonBPFunctionRecipe::kNetModeClearMask;
				Test->TestEqual(FString::Printf(
					TEXT("round trip/%s: the reloaded generated function carries the same managed "
					     "bits as the reloaded entry node"), *Expectation.Function),
					static_cast<int32>(Function->FunctionFlags) & Managed,
					Expectation.EntryFlags & Managed);
			}

			// Verify Static survives without Const, the pairing PostLoad would otherwise repair.
			for (const FFPReloadExpectation& Expectation : Expectations)
			{
				if (!Expectation.Function.Equals(StaticFn()))
				{
					continue;
				}
				Test->TestTrue(TEXT("round trip: the static function came back STATIC"),
					(Expectation.EntryFlags & FUNC_Static) != 0);
				UK2Node_FunctionEntry* Entry =
					SFP_FindEntry(SFP_FindGraph(Reloaded, Expectation.Function));
				Test->TestTrue(
					TEXT("round trip: and NOT const -- the pair UFunction::PostLoad "
					     "(Class.cpp:7256-7261) silently repairs is the pair the setter refuses"),
					IsValid(Entry) && (Entry->GetExtraFlags() & FUNC_Const) == 0);
			}
		}

		FAutomationTestBase* Test = nullptr;
		FString AssetPath;
		EPhase Phase = EPhase::Build;
		int32 SettleUpdates = 0;
		TArray<FFPReloadExpectation> Expectations;
	};
}

bool FClaireonBPEditorFunctionPropertiesReload::RunTest(const FString& /*Parameters*/)
{
	using namespace ClaireonBPSetFunctionPropsInternal;
	ADD_LATENT_AUTOMATION_COMMAND(FFP_ReloadCommand(
		this, ClaireonBPEditorFixtures::UniquePath(TEXT("SfpRoundTrip"))));
	return true;
}

// Cover both compiler failure and a forced readback mismatch after successful compilation.
// Both must retain and report the mutation, with distinct engine_compile_status values.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorFunctionPropertiesRetention,
	"Claireon.BPEditor.FunctionProperties.ValidationFailureRetainsOnBothCompileStatuses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPSetFunctionPropsInternal
{
	class FFP_RetentionCommand : public FFP_FixtureCommand
	{
	public:
		using FFP_FixtureCommand::FFP_FixtureCommand;

		static const TCHAR* CleanFn()  { return TEXT("SFPRetainClean"); }
		static const TCHAR* BrokenFn() { return TEXT("SFPRetainBroken"); }

	protected:
		virtual bool BuildFixture(UBlueprint& Blueprint, FString& OutError) override
		{
			FFPCreate Plain;
			if (!SFP_AddFunction(*Test, AssetPath, CleanFn(), Plain)
				|| !SFP_AddFunction(*Test, AssetPath, BrokenFn(), Plain))
			{
				OutError = TEXT("the retention fixtures could not be created.");
				return false;
			}
			FKismetEditorUtilities::CompileBlueprint(&Blueprint);
			return true;
		}

		/** An exec output carrying TWO links, which the compiler rejects outright. */
		bool BreakTheCompile(UBlueprint& Blueprint)
		{
			UEdGraph* Uber = ClaireonBPEditorFixtures::FirstUbergraph(&Blueprint);
			UFunction* PrintString = UKismetSystemLibrary::StaticClass()->FindFunctionByName(
				TEXT("PrintString"));
			if (!IsValid(Uber) || PrintString == nullptr)
			{
				Test->AddError(TEXT("the uncompilable island could not be built."));
				return false;
			}

			UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Uber);
			Uber->AddNode(Event, /*bFromUI=*/false, /*bSelectNewNode=*/false);
			Event->CreateNewGuid();
			Event->CustomFunctionName = TEXT("SFPRetainBrokenEntry");
			Event->AllocateDefaultPins();

			UK2Node_CallFunction* Calls[2] = {nullptr, nullptr};
			for (int32 Index = 0; Index < 2; ++Index)
			{
				Calls[Index] = NewObject<UK2Node_CallFunction>(Uber);
				Uber->AddNode(Calls[Index], /*bFromUI=*/false, /*bSelectNewNode=*/false);
				Calls[Index]->SetFromFunction(PrintString);
				Calls[Index]->CreateNewGuid();
				Calls[Index]->NodePosX = 400;
				Calls[Index]->NodePosY = Index * 200;
				Calls[Index]->PostPlacedNewNode();
				Calls[Index]->AllocateDefaultPins();
			}

			UEdGraphPin* Then = Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
			UEdGraphPin* FirstExec = Calls[0]->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
			UEdGraphPin* SecondExec = Calls[1]->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
			if (Then == nullptr || FirstExec == nullptr || SecondExec == nullptr)
			{
				Test->AddError(TEXT("the uncompilable island could not be wired."));
				return false;
			}
			Then->MakeLinkTo(FirstExec);
			Then->MakeLinkTo(SecondExec);
			if (Then->LinkedTo.Num() != 2)
			{
				Test->AddError(FString::Printf(
					TEXT("the uncompilable island's exec output holds %d link(s), not 2, so the "
					     "compile would SUCCEED and the leg would pass vacuously."),
					Then->LinkedTo.Num()));
				return false;
			}
			return true;
		}

		/** The shared half of both legs: retained, with a delta that proves durable effect. */
		void ExpectRetained(const IClaireonTool::FToolResult& Result, const FString& What,
			const TCHAR* ExpectedCompileStatus)
		{
			if (!Result.bIsError)
			{
				Test->AddError(FString::Printf(
					TEXT("%s: a validation failure was reported as a SUCCESS, which is DEC-31's "
					     "'failure is never encoded inside a successful transport result'."),
					*What));
				return;
			}
			if (!Result.Data.IsValid())
			{
				Test->AddError(FString::Printf(TEXT("%s: no structured data."), *What));
				return;
			}

			SFP_ExpectString(*Test, Result.Data, TEXT("mutation_state"),
				SFP_State(EClaireonMutationState::AppliedValidationFailed), What);
			SFP_ExpectBool(*Test, Result.Data, TEXT("mutation_retained"), true, What);
			SFP_ExpectString(*Test, Result.Data, TEXT("failed_phase"),
				SFP_Phase(EClaireonBPSetterPhase::CompileValidate), What);
			SFP_ExpectString(*Test, Result.Data, TEXT("engine_compile_status"),
				FString(ExpectedCompileStatus), What);
			SFP_ExpectBool(*Test, Result.Data, TEXT("undo_record_available"), true, What);

			TArray<FFPDeltaEntry> Entries;
			bool bComparable = false;
			if (!SFP_ReadDelta(*Test, Result.Data, What, Entries, bComparable))
			{
				return;
			}
			Test->TestTrue(FString::Printf(TEXT("%s: the delta is comparable"), *What), bComparable);
			Test->TestTrue(FString::Printf(
				TEXT("%s: the delta carries at least one entry, so 'the mutation is retained' is "
				     "an OBSERVATION rather than a claim"), *What), Entries.Num() > 0);

			// Retained failures with undo records must offer recovery.
			Test->TestTrue(FString::Printf(TEXT("%s: the failure carries a recovery hint"), *What),
				Result.Hints.Num() > 0);
		}

		virtual void RunBody(UBlueprint& Blueprint) override
		{
			// ---- LEG ONE: compile SUCCEEDED, validation failed.
			{
				const FString What = TEXT("retention/compile succeeded");
				const int32 Before = SFP_EntryFlags(&Blueprint, CleanFn());

				IClaireonTool::FToolResult Result;
				{
					const ClaireonBPFaultInjection::FScopedFault Fault(
						SFP_Phase(EClaireonBPSetterPhase::CompileValidate));
					FFPRequest Request;
					Request.bConst = true;
					Result = SFP_SetProperties(AssetPath, CleanFn(), Request);
				}
				ExpectRetained(Result, What, TEXT("succeeded"));

				const int32 After = SFP_EntryFlags(&Blueprint, CleanFn());
				Test->TestTrue(FString::Printf(
					TEXT("%s: the const bit is RETAINED on the asset after the failure "
					     "(before %s, after %s)"), *What, *SFP_Hex(Before), *SFP_Hex(After)),
					(After & FUNC_Const) != 0);
				Test->TestTrue(FString::Printf(
					TEXT("%s: the Blueprint is NOT in BS_Error -- the compile really did succeed, "
					     "so this leg is not a disguised copy of the other one"), *What),
					Blueprint.Status != BS_Error);

				Test->TestTrue(TEXT("the fault seam disarmed on scope exit"),
					ClaireonBPFaultInjection::GetArmedPhase().IsEmpty());
			}

			// ---- LEG TWO: compile FAILED. No seam; the graph really does not compile.
			{
				const FString What = TEXT("retention/compile failed");
				if (!BreakTheCompile(Blueprint))
				{
					return;
				}

				FFPRequest Request;
				Request.Category = FString(TEXT("Stage019|Retained"));
				const IClaireonTool::FToolResult Result =
					SFP_SetProperties(AssetPath, BrokenFn(), Request);

				Test->TestEqual(FString::Printf(
					TEXT("%s: the Blueprint really is in BS_Error after the tool's compile"), *What),
					static_cast<int32>(Blueprint.Status), static_cast<int32>(BS_Error));

				ExpectRetained(Result, What, TEXT("failed"));

				UK2Node_FunctionEntry* Entry = SFP_FindEntry(SFP_FindGraph(&Blueprint, BrokenFn()));
				Test->TestTrue(FString::Printf(
					TEXT("%s: the category is RETAINED on the asset after the failed compile"), *What),
					IsValid(Entry)
						&& Entry->MetaData.Category.ToString().Equals(TEXT("Stage019|Retained")));
			}
		}
	};
}

bool FClaireonBPEditorFunctionPropertiesRetention::RunTest(const FString& /*Parameters*/)
{
	using namespace ClaireonBPSetFunctionPropsInternal;

	// Register expected compiler errors before the first compile; structural changes can emit them repeatedly.
	AddExpectedMessagePlain(TEXT("cannot have more than one connection"),
		EAutomationExpectedMessageFlags::Contains, /*Occurrences=*/0);

	ADD_LATENT_AUTOMATION_COMMAND(FFP_RetentionCommand(
		this, ClaireonBPEditorFixtures::UniquePath(TEXT("SfpRetain"))));
	return true;
}

#endif // WITH_CLAIREON_TESTS && WITH_EDITOR
