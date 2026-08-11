// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonBlueprintNodeTypeRegistry.h"

namespace ClaireonBlueprintNodeTypes
{
	static TArray<FNodeTypeInfo> BuildRegistry()
	{
		TArray<FNodeTypeInfo> R;

		auto Add = [&R](const TCHAR* Alias, ENodeTypeKind Kind,
			TArray<FString> Required, TArray<FString> Optional, const TCHAR* Description)
		{
			FNodeTypeInfo Info;
			Info.Alias = Alias;
			Info.Kind = Kind;
			Info.RequiredParams = MoveTemp(Required);
			Info.OptionalParams = MoveTemp(Optional);
			Info.Description = Description;
			R.Add(MoveTemp(Info));
		};

		const TArray<FString> Pos = {TEXT("position")};

		// ---- Factory-handled ----
		Add(TEXT("CallFunction"), ENodeTypeKind::Factory,
			{TEXT("function_name")}, {TEXT("function_class"), TEXT("node_class"), TEXT("position")},
			TEXT("Call a UFUNCTION. function_class names the owner; omit it for functions on this Blueprint. Latent task factories are promoted to their dedicated node class automatically."));
		Add(TEXT("CallArrayFunction"), ENodeTypeKind::Factory,
			{TEXT("function_name")}, {TEXT("function_class"), TEXT("position")},
			TEXT("Call an array-utility UFUNCTION with wildcard array pins."));
		Add(TEXT("AsyncAction"), ENodeTypeKind::Factory,
			{TEXT("function_name"), TEXT("function_class")}, Pos,
			TEXT("UBlueprintAsyncActionBase proxy node. GameplayTask factories belong on LatentGameplayTaskCall / LatentAbilityCall instead."));
		Add(TEXT("CallParentFunction"), ENodeTypeKind::Factory,
			{TEXT("function_name")}, Pos,
			TEXT("Parent: <fn> call, for running the parent implementation an override would otherwise shadow."));
		Add(TEXT("LatentGameplayTaskCall"), ENodeTypeKind::Factory,
			{TEXT("function_name"), TEXT("function_class")}, Pos,
			TEXT("Latent UGameplayTask node. Rewritten to Generic + class_name='K2Node_LatentGameplayTaskCall'."));
		Add(TEXT("LatentAbilityCall"), ENodeTypeKind::Factory,
			{TEXT("function_name"), TEXT("function_class")}, Pos,
			TEXT("Latent UAbilityTask node. Rewritten to Generic + class_name='K2Node_LatentAbilityCall'; the resolved factory type decides which of the two classes is used."));
		Add(TEXT("VariableGet"), ENodeTypeKind::Factory,
			{TEXT("variable_name")}, Pos, TEXT("Read a Blueprint variable."));
		Add(TEXT("VariableSet"), ENodeTypeKind::Factory,
			{TEXT("variable_name")}, Pos, TEXT("Write a Blueprint variable."));
		Add(TEXT("Branch"), ENodeTypeKind::Factory, {}, Pos, TEXT("Conditional exec branch (true/false)."));
		Add(TEXT("Sequence"), ENodeTypeKind::Factory, {}, {TEXT("num_extra_pins"), TEXT("position")},
			TEXT("Run several exec outputs in order. num_extra_pins grows then_N beyond the default two."));
		Add(TEXT("ExecutionSequence"), ENodeTypeKind::Factory, {}, {TEXT("num_extra_pins"), TEXT("position")},
			TEXT("Alias of Sequence."));
		Add(TEXT("Cast"), ENodeTypeKind::Factory, {TEXT("target_type")}, Pos,
			TEXT("Dynamic cast to target_type."));
		Add(TEXT("SpawnActor"), ENodeTypeKind::Factory, {}, {TEXT("class_name"), TEXT("position")},
			TEXT("SpawnActorFromClass. Setting the Class pin grows ExposeOnSpawn pins in the same call."));
		Add(TEXT("CustomEvent"), ENodeTypeKind::Factory, {TEXT("event_name")}, Pos,
			TEXT("Custom event entry point."));
		Add(TEXT("Knot"), ENodeTypeKind::Factory, {}, Pos, TEXT("Reroute node."));
		Add(TEXT("Comment"), ENodeTypeKind::Factory, {}, {TEXT("comment_text"), TEXT("position")},
			TEXT("Comment box."));
		Add(TEXT("Select"), ENodeTypeKind::Factory, {}, Pos, TEXT("Select between wildcard inputs by index or enum."));
		Add(TEXT("MakeArray"), ENodeTypeKind::Factory, {}, Pos, TEXT("Build an array literal from element pins."));
		Add(TEXT("MakeSet"), ENodeTypeKind::Factory, {}, Pos, TEXT("Build a set literal."));
		Add(TEXT("MakeMap"), ENodeTypeKind::Factory, {}, Pos, TEXT("Build a map literal from key/value pin pairs."));
		Add(TEXT("GetArrayItem"), ENodeTypeKind::Factory, {}, Pos, TEXT("Index into an array."));
		Add(TEXT("MakeStruct"), ENodeTypeKind::Factory, {TEXT("struct_type")}, Pos, TEXT("Build a struct from member pins."));
		Add(TEXT("BreakStruct"), ENodeTypeKind::Factory, {TEXT("struct_type")}, Pos, TEXT("Split a struct into member pins."));
		Add(TEXT("SetFieldsInStruct"), ENodeTypeKind::Factory, {TEXT("struct_type")}, Pos,
			TEXT("Set selected struct members, passing the rest through."));
		Add(TEXT("SwitchInteger"), ENodeTypeKind::Factory, {}, {TEXT("num_extra_pins"), TEXT("position")},
			TEXT("Switch on an integer."));
		Add(TEXT("SwitchString"), ENodeTypeKind::Factory, {}, {TEXT("num_extra_pins"), TEXT("position")},
			TEXT("Switch on a string."));
		Add(TEXT("SwitchName"), ENodeTypeKind::Factory, {}, {TEXT("num_extra_pins"), TEXT("position")},
			TEXT("Switch on a name."));
		Add(TEXT("SwitchEnum"), ENodeTypeKind::Factory, {TEXT("enum_type")}, Pos, TEXT("Switch on an enum."));
		Add(TEXT("ForEachElementInEnum"), ENodeTypeKind::Factory, {TEXT("enum_type")}, Pos,
			TEXT("Iterate every value of an enum."));
		Add(TEXT("DoOnceMultiInput"), ENodeTypeKind::Factory, {}, {TEXT("num_extra_pins"), TEXT("position")},
			TEXT("DoOnce with several independent inputs."));
		Add(TEXT("MultiGate"), ENodeTypeKind::Factory, {}, {TEXT("num_extra_pins"), TEXT("position")},
			TEXT("Fire exec outputs one at a time."));
		Add(TEXT("FormatText"), ENodeTypeKind::Factory, {}, Pos, TEXT("Format Text with named argument pins."));
		Add(TEXT("SwitchGameplayTag"), ENodeTypeKind::Factory, {}, {TEXT("num_extra_pins"), TEXT("position")},
			TEXT("Switch on a gameplay tag."));
		Add(TEXT("Composite"), ENodeTypeKind::Factory, {}, Pos, TEXT("Collapsed sub-graph container."));
		Add(TEXT("CollapsedGraph"), ENodeTypeKind::Factory, {}, Pos, TEXT("Alias of Composite."));
		Add(TEXT("Macro"), ENodeTypeKind::Factory, {TEXT("macro_name")}, {TEXT("macro_library"), TEXT("position")},
			TEXT("Instantiate an existing macro. macro_library defaults to the engine StandardMacros library."));
		Add(TEXT("MacroInstance"), ENodeTypeKind::Factory, {TEXT("macro_name")}, {TEXT("macro_library"), TEXT("position")},
			TEXT("Alias of Macro. Create new macros with bp_add_macro."));
		Add(TEXT("SwitchHasAuthority"), ENodeTypeKind::Factory, {}, Pos, TEXT("Authority/remote exec split."));

		// ---- Macro shorthands (rewritten to a StandardMacros MacroInstance) ----
		Add(TEXT("ForEachLoop"), ENodeTypeKind::Shorthand, {}, Pos, TEXT("StandardMacros ForEachLoop."));
		Add(TEXT("ForEachLoopWithBreak"), ENodeTypeKind::Shorthand, {}, Pos, TEXT("StandardMacros ForEachLoopWithBreak."));
		Add(TEXT("ForLoop"), ENodeTypeKind::Shorthand, {}, Pos, TEXT("StandardMacros ForLoop."));
		Add(TEXT("ForLoopWithBreak"), ENodeTypeKind::Shorthand, {}, Pos, TEXT("StandardMacros ForLoopWithBreak."));
		Add(TEXT("WhileLoop"), ENodeTypeKind::Shorthand, {}, Pos, TEXT("StandardMacros WhileLoop."));
		Add(TEXT("DoOnce"), ENodeTypeKind::Shorthand, {}, Pos, TEXT("StandardMacros DoOnce."));
		Add(TEXT("DoN"), ENodeTypeKind::Shorthand, {}, Pos, TEXT("StandardMacros DoN."));
		Add(TEXT("FlipFlop"), ENodeTypeKind::Shorthand, {}, Pos, TEXT("StandardMacros FlipFlop."));
		Add(TEXT("Gate"), ENodeTypeKind::Shorthand, {}, Pos, TEXT("StandardMacros Gate."));
		Add(TEXT("IsValid"), ENodeTypeKind::Shorthand, {}, Pos, TEXT("StandardMacros IsValid."));
		// "StandardMacroBranch" is deliberately NOT registered as a creatable shorthand.
		// It advertised a "StandardMacros Branch macro, as distinct from the native Branch
		// node", but no such macro exists: StandardMacros contains no Branch graph, and
		// bp_add_node's macro chain never handled the name, so every attempt to create one
		// failed. Branch in Blueprints is the native K2Node_IfThenElse, registered
		// separately. The name is still meaningful on the READ side --
		// ClaireonBPMacroHandler maps a macro instance called StandardMacroBranch to
		// if/else when decompiling an existing graph -- which is why it is not deleted
		// outright, only removed from the creatable surface.

		// ---- Inline-handled (dedicated branches in bp_add_node) ----
		Add(TEXT("AddComponent"), ENodeTypeKind::Inline, {TEXT("component_class")},
			{TEXT("component_name"), TEXT("parent_name"), TEXT("position")},
			TEXT("Add a component to the Blueprint's construction script."));
		Add(TEXT("Timeline"), ENodeTypeKind::Inline, {TEXT("timeline_name")}, Pos, TEXT("Timeline node and its template."));
		Add(TEXT("Tunnel"), ENodeTypeKind::Inline, {}, Pos,
			TEXT("FIND-ONLY: locates an existing macro graph's entry/exit tunnel. Create macros with bp_add_macro."));
		Add(TEXT("FunctionEntry"), ENodeTypeKind::Inline, {}, Pos, TEXT("FIND-ONLY: a function graph's entry node."));
		Add(TEXT("FunctionResult"), ENodeTypeKind::Inline, {}, Pos, TEXT("A function graph's result node."));
		Add(TEXT("EventOverride"), ENodeTypeKind::Inline, {TEXT("function_name")}, Pos,
			TEXT("Override an inherited event. bp_add_function_override is the richer route."));
		Add(TEXT("ComponentBoundEvent"), ENodeTypeKind::Inline,
			{TEXT("component_name"), TEXT("delegate_name")}, Pos,
			TEXT("Event bound to a component's delegate property."));
		Add(TEXT("AddDelegate"), ENodeTypeKind::Inline, {TEXT("delegate_name")}, Pos, TEXT("Bind to a multicast delegate."));
		Add(TEXT("RemoveDelegate"), ENodeTypeKind::Inline, {TEXT("delegate_name")}, Pos, TEXT("Unbind from a multicast delegate."));
		Add(TEXT("ClearDelegate"), ENodeTypeKind::Inline, {TEXT("delegate_name")}, Pos, TEXT("Clear all bindings."));
		Add(TEXT("CallDelegate"), ENodeTypeKind::Inline, {TEXT("delegate_name")}, Pos, TEXT("Broadcast a multicast delegate."));
		Add(TEXT("CreateDelegate"), ENodeTypeKind::Inline, {}, Pos, TEXT("Make a delegate value from a function."));
		Add(TEXT("AssignDelegate"), ENodeTypeKind::Inline, {TEXT("delegate_name")}, Pos,
			TEXT("Bind a delegate and create its companion custom event in one step."));

		// ---- Escape hatch ----
		Add(TEXT("Generic"), ENodeTypeKind::Generic, {TEXT("class_name")},
			{TEXT("node_properties"), TEXT("function_name"), TEXT("function_class"), TEXT("position")},
			TEXT("Construct any UEdGraphNode subclass by name. node_properties writes reflected fields before pin allocation."));

		return R;
	}

	const TArray<FNodeTypeInfo>& GetRegistry()
	{
		static const TArray<FNodeTypeInfo> Registry = BuildRegistry();
		return Registry;
	}

	bool IsFactoryHandled(const FString& NodeType)
	{
		for (const FNodeTypeInfo& Info : GetRegistry())
		{
			// Shorthand entries reach the factory too, but only after the macro
			// rewrite has replaced node_type with MacroInstance.
			if (Info.Kind != ENodeTypeKind::Inline && NodeType.Equals(Info.Alias, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	}

	bool IsMacroShorthand(const FString& NodeType)
	{
		for (const FNodeTypeInfo& Info : GetRegistry())
		{
			if (Info.Kind == ENodeTypeKind::Shorthand && NodeType.Equals(Info.Alias, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	}

	const TArray<FString>& GetMacroShorthandNames()
	{
		static const TArray<FString> Names = []()
		{
			TArray<FString> Out;
			for (const FNodeTypeInfo& Info : GetRegistry())
			{
				if (Info.Kind == ENodeTypeKind::Shorthand)
				{
					Out.Add(Info.Alias);
				}
			}
			return Out;
		}();
		return Names;
	}
}
