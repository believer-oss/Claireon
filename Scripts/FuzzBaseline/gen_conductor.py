# Build the Conductor control-flow and latent-action fixture.
# FUZZ records use stream MAIN and finish with DONE.

import claireon
import fuzzlib
from fuzzlib import Delta, ok, node_guid, switch_graph, compile_save

ASSET = "/Claireon/FuzzBaseline/BP_Fuzz_Conductor"
BPI = "/Claireon/FuzzBaseline/BPI_Fuzz"
BPI_CLASS = "/Claireon/FuzzBaseline/BPI_Fuzz.BPI_Fuzz_C"

S = {"sid": None, "guids": {}}

KSL = "KismetSystemLibrary"
KML = "KismetMathLibrary"
KTL = "KismetTextLibrary"
KSTR = "KismetStringLibrary"


def sid():
    if not S["sid"]:
        r = ok(claireon.bp_open(asset_path=ASSET), "open")
        S["sid"] = r["data"]["session_id"]
    return S["sid"]


def remember(name, result):
    g = node_guid(result)
    S["guids"][name] = g
    return g



def stage_create():
    r = ok(claireon.bp_create(parent_class="Actor", asset_path=ASSET), "create")
    S["sid"] = r["data"]["session_id"]
    ok(claireon.bp_add_component(session_id=S["sid"], component_name="ConductorRoot",
                                 component_class="SceneComponent"), "add ConductorRoot")
    ok(claireon.bp_add_component(session_id=S["sid"], component_name="OrbitMesh",
                                 component_class="StaticMeshComponent",
                                 parent_name="ConductorRoot"), "add OrbitMesh")
    compile_save(S["sid"], "stage_create")
    print("stage_create OK, sid=%s" % S["sid"])



def stage_variables():
    s = sid()

    def var(name, vtype=None, **kw):
        ok(claireon.bp_add_variable(session_id=s, variable_name=name,
                                    variable_type=vtype, **kw), "var %s" % name)

    var("FuzzName", "string", default_value="Conductor", category="Config",
        instance_editable=True, tooltip="Stream name used in FUZZ trace lines.")
    var("StartDelay", "float", default_value="2.0", category="Config",
        instance_editable=True, tooltip="Seconds after BeginPlay before the phase machine starts.")
    var("LoopCount", "int", default_value="3", category="Config",
        instance_editable=True, tooltip="Drives the PhaseD join-trap branch; must stay > 2.")
    var("UnusedTunable", "float", default_value="42.5", category="Config",
        instance_editable=True,
        tooltip="Deliberately unreferenced: unreferenced-variable lint bait. DO NOT DELETE.")
    # Soft object ref for the latent LoadAsset test
    ok(claireon.bp_add_variable(session_id=s, variable_name="CubeSoftRef",
        variable_type_spec={"base": "SoftObject", "subtype": "StaticMesh"},
        default_value="/Engine/BasicShapes/Cube.Cube", category="Config",
        instance_editable=True), "var CubeSoftRef")

    for name, vtype, dv in [
            ("FuzzSeq", "int", "0"), ("FuzzAcc", "int", "0"),
            ("PhaseIndex", "int", "0"), ("WhileCounter", "int", "0"),
            ("TimerFireCount", "int", "0"), ("LoopPulseCount", "int", "0")]:
        var(name, vtype, default_value=dv, category="Transient")
    var("ScratchVec", "/Script/CoreUObject.Vector", category="Transient")

    var("TimerHandleA", "/Script/Engine.TimerHandle", category="Transient|Handles")
    var("TimerHandleB", "/Script/Engine.TimerHandle", category="Transient|Handles")

    ok(claireon.bp_add_variable(session_id=s, variable_name="OnFuzzPulse",
        variable_type_spec={
            "base": "MulticastDelegate",
            "signature_function": "/Script/Claireon.ClaireonUObjectInspectMulticast__DelegateSignature"},
        category="Fuzz|Dispatchers"), "var OnFuzzPulse")

    # Reapply categories and flags after variable creation.
    plan = {
        "FuzzName": ("Config", None, None), "StartDelay": ("Config", None, None),
        "LoopCount": ("Config", None, None), "UnusedTunable": ("Config", None, None),
        "CubeSoftRef": ("Config", None, None),
        "FuzzSeq": ("Transient", ["Transient"], ["EditAnywhere"]),
        "FuzzAcc": ("Transient", ["Transient"], ["EditAnywhere"]),
        "PhaseIndex": ("Transient", ["Transient"], ["EditAnywhere"]),
        "WhileCounter": ("Transient", ["Transient"], ["EditAnywhere"]),
        "TimerFireCount": ("Transient", ["Transient"], ["EditAnywhere"]),
        "LoopPulseCount": ("Transient", ["Transient"], ["EditAnywhere"]),
        "ScratchVec": ("Transient", ["Transient"], ["EditAnywhere"]),
        "TimerHandleA": ("Transient|Handles", ["Transient"], ["EditAnywhere"]),
        "TimerHandleB": ("Transient|Handles", ["Transient"], ["EditAnywhere"]),
        "OnFuzzPulse": ("Fuzz|Dispatchers", None, ["EditAnywhere"]),
    }
    for name, (cat, flags, clear) in plan.items():
        kw = {"session_id": s, "variable_name": name, "category": cat}
        if flags: kw["flags"] = flags
        if clear: kw["clear_flags"] = clear
        ok(claireon.bp_set_variable_properties(**kw), "props %s" % name)

    compile_save(s, "stage_variables")
    print("stage_variables OK")


# Function and macro shells.

def stage_functions():
    s = sid()

    def fn(name, inputs=None, outputs=None, **kw):
        args = {"session_id": s, "function_name": name}
        if inputs: args["inputs"] = inputs
        if outputs: args["outputs"] = outputs
        args.update(kw)
        try:
            ok(claireon.bp_add_function(**args), "fn %s" % name)
        except RuntimeError as e:
            if "already exists" in str(e):
                print("  fn %s: already exists, skipped" % name)
            else:
                raise

    fn("FuzzLog", inputs=[{"name": "Tag", "type": "string"},
                          {"name": "Value", "type": "int"}],
       category="Fuzz|Trace",
       tooltip="Appends one FUZZ trace line and folds Value into the checksum accumulator.")
    fn("FuzzDone", category="Fuzz|Trace",
       tooltip="Prints the final FUZZ|...|DONE|<checksum> line.")
    fn("ComputeMagicNumber", inputs=[{"name": "Seed", "type": "int"}],
       outputs=[{"name": "Magic", "type": "int"}], is_pure=True,
       category="Fuzz|Math",
       tooltip="Pure with a local variable: Magic = Clamp(((Seed*3)+7)*2 - Seed, 0, 997).")
    fn("DescribeValue", inputs=[{"name": "V", "type": "int"}],
       outputs=[{"name": "Label", "type": "string"}], is_pure=True,
       category="Fuzz|Math", tooltip="Pure Select over V % 3.")
    fn("AccumulateArray", inputs=[{"name": "Items", "type": "Array<int>"}],
       outputs=[{"name": "Sum", "type": "int"}],
       category="Fuzz|Math", tooltip="ForEach macro + function-local accumulator.")
    fn("BuildScratch", inputs=[{"name": "BaseX", "type": "float"}],
       outputs=[{"name": "Out", "type": "/Script/CoreUObject.Vector"}], is_pure=True,
       category="Fuzz|Math", tooltip="Make/Break struct exercise; reads ScratchVec.Z.")
    fn("TailReport", inputs=[{"name": "Code", "type": "int"}],
       category="Fuzz|Flow",
       tooltip="Shared tail called from two exec paths. Logs Tail with Code*2+1.")
    fn("StartPhaseTimers", category="Fuzz|Flow",
       tooltip="Named timer at 0.9s + a 5s timer that is immediately cleared (must never fire).")
    fn("TimerTargetFunc", category="Fuzz|Flow",
       tooltip="Fired by SetTimerByFunctionName from StartPhaseTimers.")

    ok(claireon.bp_add_local_variable(session_id=s, function_name="ComputeMagicNumber",
        variable_name="LocalTmp", variable_type="int"), "local LocalTmp")
    ok(claireon.bp_add_local_variable(session_id=s, function_name="AccumulateArray",
        variable_name="RunningSum", variable_type="int"), "local RunningSum")

    # custom macro shell: data pins via bp_add_macro, exec pins via bp_add_pin
    # (the variable-type grammar has no 'exec'; tunnels accept exec user pins).
    ok(claireon.bp_add_macro(session_id=s, macro_name="ClampedInc",
        inputs=[{"name": "Value", "type": "int"}],
        outputs=[{"name": "Result", "type": "int"}]), "macro ClampedInc")
    tin = _find_node(s, "ClampedInc", title="Inputs")
    tout = _find_node(s, "ClampedInc", title="Outputs")
    ok(claireon.bp_add_pin(session_id=s, node_guid=tin, pin_name="In",
                           pin_type="exec", pin_direction="output"), "macro In pin")
    ok(claireon.bp_add_pin(session_id=s, node_guid=tout, pin_name="Out",
                           pin_type="exec", pin_direction="input"), "macro Out pin")

    compile_save(s, "stage_functions")
    print("stage_functions OK")


# Function bodies.

def _graph_nodes(s, graph_name):
    r = ok(claireon.bp_get_graph(asset_path=ASSET, graph_name=graph_name),
           "get_graph %s" % graph_name)
    graphs = r["data"].get("graphs") or []
    if not graphs:
        raise fuzzlib.FuzzError("graph %s not found" % graph_name)
    return graphs[0].get("nodes") or []


def _find_node(s, graph_name, cls=None, title=None):
    for n in _graph_nodes(s, graph_name):
        if cls and cls not in (n.get("node_class") or ""):
            continue
        if title and title != (n.get("node_title") or ""):
            continue
        return n["node_id"]
    raise fuzzlib.FuzzError("node cls=%s title=%s not found in %s: %s" %
                            (cls, title, graph_name,
                             [(x.get("node_title"), x.get("node_class"))
                              for x in _graph_nodes(s, graph_name)]))


def _entry_guid(s, graph_name):
    return _find_node(s, graph_name, cls="FunctionEntry")


def _result_guid(s, graph_name):
    return _find_node(s, graph_name, cls="FunctionResult")


def body_fuzzlog():
    s = sid()
    switch_graph(s, "FuzzLog")
    entry = _entry_guid(s, "FuzzLog")
    d = Delta(s, y=0, x=0)

    # Seq += 1
    d.n("getSeq", "VariableGet", 100, 250, variable_name="FuzzSeq")
    d.n("add1", "CallFunction", 300, 220, function_name="Add_IntInt", function_class=KML)
    d.pv("add1", "B", 1)
    d.n("setSeq", "VariableSet", 520, 0, variable_name="FuzzSeq")
    d.c("getSeq", "FuzzSeq", "add1", "A")
    d.c("add1", "ReturnValue", "setSeq", "FuzzSeq")
    d.c(entry, "then", "setSeq", "exec")

    # Acc = (Acc % 60000) * 31 + Value
    d.n("getAcc", "VariableGet", 520, 420, variable_name="FuzzAcc")
    d.n("mod", "CallFunction", 700, 390, function_name="Percent_IntInt", function_class=KML)
    d.pv("mod", "B", 60000)
    d.n("mul31", "CallFunction", 880, 360, function_name="Multiply_IntInt", function_class=KML)
    d.pv("mul31", "B", 31)
    d.n("getVal", "VariableGet", 880, 500, variable_name="Value", member_scope="FuzzLog")
    d.n("addVal", "CallFunction", 1060, 330, function_name="Add_IntInt", function_class=KML)
    d.n("setAcc", "VariableSet", 1240, 0, variable_name="FuzzAcc")
    d.c("getAcc", "FuzzAcc", "mod", "A")
    d.c("mod", "ReturnValue", "mul31", "A")
    d.c("mul31", "ReturnValue", "addVal", "A")
    d.c("getVal", "Value", "addVal", "B")
    d.c("addVal", "ReturnValue", "setAcc", "FuzzAcc")
    d.c("setSeq", "then", "setAcc", "exec")

    d.n("fmt", "FormatText", 1500, 300,
        format_text="FUZZ|{Nm}|MAIN|{Sq}|{Tg}|{Vl}")
    d.n("getName", "VariableGet", 1300, 500, variable_name="FuzzName")
    d.n("cvName", "CallFunction", 1460, 470, function_name="Conv_StringToText", function_class=KTL)
    d.n("getSeq2", "VariableGet", 1300, 620, variable_name="FuzzSeq")
    d.n("cvSeq", "CallFunction", 1460, 590, function_name="Conv_IntToText", function_class=KTL)
    d.n("getTag", "VariableGet", 1300, 740, variable_name="Tag", member_scope="FuzzLog")
    d.n("cvTag", "CallFunction", 1460, 710, function_name="Conv_StringToText", function_class=KTL)
    d.n("getVal2", "VariableGet", 1300, 860, variable_name="Value", member_scope="FuzzLog")
    d.n("cvVal", "CallFunction", 1460, 830, function_name="Conv_IntToText", function_class=KTL)
    d.c("getName", "FuzzName", "cvName", "InString")
    d.c("getSeq2", "FuzzSeq", "cvSeq", "Value")
    d.c("getTag", "Tag", "cvTag", "InString")
    d.c("getVal2", "Value", "cvVal", "Value")
    d.c("cvName", "ReturnValue", "fmt", "Nm")
    d.c("cvSeq", "ReturnValue", "fmt", "Sq")
    d.c("cvTag", "ReturnValue", "fmt", "Tg")
    d.c("cvVal", "ReturnValue", "fmt", "Vl")
    d.n("toStr", "CallFunction", 1780, 300, function_name="Conv_TextToString", function_class=KTL)
    d.c("fmt", "Result", "toStr", "InText")
    d.n("prn", "CallFunction", 2000, 0, function_name="PrintString", function_class=KSL)
    d.pv("prn", "bPrintToScreen", "false")
    d.pv("prn", "bPrintToLog", "true")
    d.pv("prn", "Duration", "0.0")
    d.c("toStr", "ReturnValue", "prn", "InString")
    d.c("setAcc", "then", "prn", "exec")
    d.apply("FuzzLog body")


def body_fuzzdone():
    s = sid()
    switch_graph(s, "FuzzDone")
    entry = _entry_guid(s, "FuzzDone")
    d = Delta(s)
    d.n("fmt", "FormatText", 600, 250, format_text="FUZZ|{Nm}|DONE|{Ac}")
    d.n("getName", "VariableGet", 400, 420, variable_name="FuzzName")
    d.n("cvName", "CallFunction", 560, 400, function_name="Conv_StringToText", function_class=KTL)
    d.n("getAcc", "VariableGet", 400, 540, variable_name="FuzzAcc")
    d.n("cvAcc", "CallFunction", 560, 520, function_name="Conv_IntToText", function_class=KTL)
    d.n("toStr", "CallFunction", 860, 250, function_name="Conv_TextToString", function_class=KTL)
    d.n("prn", "CallFunction", 1100, 0, function_name="PrintString", function_class=KSL)
    d.pv("prn", "bPrintToScreen", "false")
    d.pv("prn", "bPrintToLog", "true")
    d.pv("prn", "Duration", "0.0")
    d.c("getName", "FuzzName", "cvName", "InString")
    d.c("getAcc", "FuzzAcc", "cvAcc", "Value")
    d.c("cvName", "ReturnValue", "fmt", "Nm")
    d.c("cvAcc", "ReturnValue", "fmt", "Ac")
    d.c("fmt", "Result", "toStr", "InText")
    d.c("toStr", "ReturnValue", "prn", "InString")
    d.c(entry, "then", "prn", "exec")
    d.apply("FuzzDone body")


def body_magic():
    s = sid()
    switch_graph(s, "ComputeMagicNumber")
    entry = _entry_guid(s, "ComputeMagicNumber")
    result = _result_guid(s, "ComputeMagicNumber")
    d = Delta(s)
    # LocalTmp = Seed*3 + 7
    d.n("getSeed", "VariableGet", 200, 250, variable_name="Seed", member_scope="ComputeMagicNumber")
    d.n("mul3", "CallFunction", 380, 220, function_name="Multiply_IntInt", function_class=KML)
    d.pv("mul3", "B", 3)
    d.n("add7", "CallFunction", 560, 190, function_name="Add_IntInt", function_class=KML)
    d.pv("add7", "B", 7)
    d.n("setTmp", "VariableSet", 760, 0, variable_name="LocalTmp", member_scope="ComputeMagicNumber")
    d.c("getSeed", "Seed", "mul3", "A")
    d.c("mul3", "ReturnValue", "add7", "A")
    d.c("add7", "ReturnValue", "setTmp", "LocalTmp")
    d.c(entry, "then", "setTmp", "exec")
    # Magic = Clamp(LocalTmp*2 - Seed, 0, 997)
    d.n("getTmp", "VariableGet", 760, 300, variable_name="LocalTmp", member_scope="ComputeMagicNumber")
    d.n("mul2", "CallFunction", 940, 270, function_name="Multiply_IntInt", function_class=KML)
    d.pv("mul2", "B", 2)
    d.n("getSeed2", "VariableGet", 940, 410, variable_name="Seed", member_scope="ComputeMagicNumber")
    d.n("sub", "CallFunction", 1120, 240, function_name="Subtract_IntInt", function_class=KML)
    d.n("clamp", "CallFunction", 1300, 210, function_name="Clamp", function_class=KML)
    d.pv("clamp", "Min", 0)
    d.pv("clamp", "Max", 997)
    d.c("getTmp", "LocalTmp", "mul2", "A")
    d.c("mul2", "ReturnValue", "sub", "A")
    d.c("getSeed2", "Seed", "sub", "B")
    d.c("sub", "ReturnValue", "clamp", "Value")
    d.c("clamp", "ReturnValue", result, "Magic")
    d.c("setTmp", "then", result, "exec")
    d.apply("ComputeMagicNumber body")


def body_describe():
    s = sid()
    switch_graph(s, "DescribeValue")
    entry = _entry_guid(s, "DescribeValue")
    result = _result_guid(s, "DescribeValue")
    d = Delta(s)
    # Label = (V % 3 == 0) ? "Alpha" : "Beta"   via a Select node
    d.n("getV", "VariableGet", 200, 250, variable_name="V", member_scope="DescribeValue")
    d.n("mod", "CallFunction", 380, 220, function_name="Percent_IntInt", function_class=KML)
    d.pv("mod", "B", 3)
    d.n("eq0", "CallFunction", 560, 220, function_name="EqualEqual_IntInt", function_class=KML)
    d.pv("eq0", "B", 0)
    d.n("litA", "CallFunction", 560, 380, function_name="MakeLiteralString", function_class=KSL)
    d.pv("litA", "Value", "Beta")
    d.n("litB", "CallFunction", 560, 500, function_name="MakeLiteralString", function_class=KSL)
    d.pv("litB", "Value", "Alpha")
    d.n("sel", "Select", 800, 300)
    d.c("getV", "V", "mod", "A")
    d.c("mod", "ReturnValue", "eq0", "A")
    d.c("eq0", "ReturnValue", "sel", "Index")
    d.c("litA", "ReturnValue", "sel", "Option 0")
    d.c("litB", "ReturnValue", "sel", "Option 1")
    d.c("sel", "ReturnValue", result, "Label")
    d.c(entry, "then", result, "exec")
    d.apply("DescribeValue body")


def body_accumulate():
    s = sid()
    switch_graph(s, "AccumulateArray")
    entry = _entry_guid(s, "AccumulateArray")
    result = _result_guid(s, "AccumulateArray")
    d = Delta(s)
    d.n("getItems", "VariableGet", 300, 260, variable_name="Items", member_scope="AccumulateArray")
    d.n("each", "ForEachLoop", 520, 0)
    d.c(entry, "then", "each", "Exec")  # macro instances name exec-in 'Exec'
    d.c("getItems", "Items", "each", "Array")
    # body: RunningSum += element
    d.n("getSum", "VariableGet", 760, 320, variable_name="RunningSum", member_scope="AccumulateArray")
    d.n("add", "CallFunction", 960, 290, function_name="Add_IntInt", function_class=KML)
    d.n("setSum", "VariableSet", 1180, 120, variable_name="RunningSum", member_scope="AccumulateArray")
    d.c("each", "LoopBody", "setSum", "exec")
    d.c("getSum", "RunningSum", "add", "A")
    d.c("each", "Array Element", "add", "B")
    d.c("add", "ReturnValue", "setSum", "RunningSum")
    d.n("getSum2", "VariableGet", 1380, 260, variable_name="RunningSum", member_scope="AccumulateArray")
    d.c("each", "Completed", result, "exec")
    d.c("getSum2", "RunningSum", result, "Sum")
    d.apply("AccumulateArray body")


def body_buildscratch():
    s = sid()
    switch_graph(s, "BuildScratch")
    entry = _entry_guid(s, "BuildScratch")
    result = _result_guid(s, "BuildScratch")
    d = Delta(s)
    # Use IntPoint for generic struct-node coverage; Vector requires MakeVector/BreakVector.
    d.n("getBase", "VariableGet", 200, 260, variable_name="BaseX", member_scope="BuildScratch")
    d.n("mul10", "CallFunction", 400, 230, function_name="Multiply_DoubleDouble", function_class=KML)
    d.pv("mul10", "B", "10.0")
    d.n("trunc", "CallFunction", 580, 230, function_name="FTrunc", function_class=KML)
    d.n("mkIP", "MakeStruct", 760, 230, struct_type="/Script/CoreUObject.IntPoint")
    d.pv("mkIP", "Y", 20)
    d.n("brkIP", "BreakStruct", 940, 230, struct_type="/Script/CoreUObject.IntPoint")
    d.n("cvX", "CallFunction", 1120, 200, function_name="Conv_IntToDouble", function_class=KML)
    d.n("cvY", "CallFunction", 1120, 330, function_name="Conv_IntToDouble", function_class=KML)
    d.n("getScratch", "VariableGet", 200, 420, variable_name="ScratchVec")
    d.n("brkVec", "CallFunction", 400, 460, function_name="BreakVector", function_class=KML)
    d.n("halfZ", "CallFunction", 620, 430, function_name="Multiply_DoubleDouble", function_class=KML)
    d.pv("halfZ", "B", "0.5")
    d.n("mkVec", "CallFunction", 1320, 260, function_name="MakeVector", function_class=KML)
    d.c("getBase", "BaseX", "mul10", "A")
    d.c("mul10", "ReturnValue", "trunc", "A")
    d.c("trunc", "ReturnValue", "mkIP", "X")
    d.c("mkIP", "IntPoint", "brkIP", "IntPoint")
    d.c("brkIP", "X", "cvX", "InInt")
    d.c("brkIP", "Y", "cvY", "InInt")
    d.c("cvX", "ReturnValue", "mkVec", "X")
    d.c("cvY", "ReturnValue", "mkVec", "Y")
    d.c("getScratch", "ScratchVec", "brkVec", "InVec")
    d.c("brkVec", "Z", "halfZ", "A")
    d.c("halfZ", "ReturnValue", "mkVec", "Z")
    d.c("mkVec", "ReturnValue", result, "Out")
    d.c(entry, "then", result, "exec")
    d.apply("BuildScratch body")


def body_tail():
    s = sid()
    switch_graph(s, "TailReport")
    entry = _entry_guid(s, "TailReport")
    d = Delta(s)
    d.n("getCode", "VariableGet", 200, 260, variable_name="Code", member_scope="TailReport")
    d.n("mul2", "CallFunction", 400, 230, function_name="Multiply_IntInt", function_class=KML)
    d.pv("mul2", "B", 2)
    d.n("add1", "CallFunction", 580, 200, function_name="Add_IntInt", function_class=KML)
    d.pv("add1", "B", 1)
    d.n("log", "CallFunction", 800, 0, function_name="FuzzLog")
    d.pv("log", "Tag", "Tail")
    d.n("prn", "CallFunction", 1200, 0, function_name="PrintString", function_class=KSL)
    d.pv("prn", "InString", "conductor tail diagnostics (inline print bait)")
    d.pv("prn", "bPrintToScreen", "false")
    d.pv("prn", "bPrintToLog", "true")
    d.c("getCode", "Code", "mul2", "A")
    d.c("mul2", "ReturnValue", "add1", "A")
    d.c("add1", "ReturnValue", "log", "Value")
    d.c(entry, "then", "log", "exec")
    d.c("log", "then", "prn", "exec")
    d.apply("TailReport body")


def body_timers():
    s = sid()
    switch_graph(s, "StartPhaseTimers")
    entry = _entry_guid(s, "StartPhaseTimers")
    d = Delta(s)
    d.n("t1", "CallFunction", 400, 0, function_name="K2_SetTimer", function_class=KSL)
    d.pv("t1", "FunctionName", "TimerTargetFunc")
    d.pv("t1", "Time", "0.9")
    d.pv("t1", "bLooping", "false")
    d.n("setHB", "VariableSet", 800, 0, variable_name="TimerHandleB")
    d.c(entry, "then", "t1", "exec")
    d.c("t1", "ReturnValue", "setHB", "TimerHandleB")
    d.c("t1", "then", "setHB", "exec")
    # 5s timer stored in HandleA, then immediately cleared: must never fire.
    d.n("t2", "CallFunction", 1200, 0, function_name="K2_SetTimer", function_class=KSL)
    d.pv("t2", "FunctionName", "FuzzDone")
    d.pv("t2", "Time", "5.0")
    d.pv("t2", "bLooping", "false")
    d.n("setHA", "VariableSet", 1600, 0, variable_name="TimerHandleA")
    d.n("getHA", "VariableGet", 1800, 260, variable_name="TimerHandleA")
    d.n("clr", "CallFunction", 2000, 0, function_name="K2_ClearAndInvalidateTimerHandle", function_class=KSL)
    d.c("setHB", "then", "t2", "exec")
    d.c("t2", "ReturnValue", "setHA", "TimerHandleA")
    d.c("t2", "then", "setHA", "exec")
    d.c("setHA", "then", "clr", "exec")
    d.c("getHA", "TimerHandleA", "clr", "Handle")
    d.apply("StartPhaseTimers body")

    switch_graph(s, "TimerTargetFunc")
    entry2 = _entry_guid(s, "TimerTargetFunc")
    d2 = Delta(s)
    d2.n("log", "CallFunction", 400, 0, function_name="FuzzLog")
    d2.pv("log", "Tag", "NamedTimer")
    d2.pv("log", "Value", 71)
    d2.c(entry2, "then", "log", "exec")
    d2.apply("TimerTargetFunc body")


def body_macro():
    s = sid()
    switch_graph(s, "ClampedInc")
    tin = _find_node(s, "ClampedInc", title="Inputs")
    tout = _find_node(s, "ClampedInc", title="Outputs")
    d = Delta(s)
    d.n("add1", "CallFunction", 400, 200, function_name="Add_IntInt", function_class=KML)
    d.pv("add1", "B", 1)
    d.n("clamp", "CallFunction", 620, 200, function_name="Clamp", function_class=KML)
    d.pv("clamp", "Min", 0)
    d.pv("clamp", "Max", 100)
    d.c(tin, "Value", "add1", "A")
    d.c("add1", "ReturnValue", "clamp", "Value")
    d.c("clamp", "ReturnValue", tout, "Result")
    d.c(tin, "In", tout, "Out")
    d.apply("ClampedInc body")


def stage_bodies():
    body_fuzzlog()
    body_fuzzdone()
    body_magic()
    body_describe()
    body_accumulate()
    body_buildscratch()
    body_tail()
    body_timers()
    body_macro()
    compile_save(sid(), "stage_bodies")
    print("stage_bodies OK")


# Interface, timeline, delegate, and switch nodes.

def _add(name, **kw):
    """bp_add_node into the active graph; remember guid under name."""
    s = sid()
    if name in S["guids"]:
        print("  %s: guid already known, skipped" % name)
        return S["guids"][name]
    try:
        r = ok(claireon.bp_add_node(session_id=s, **kw), "add %s" % name)
    except RuntimeError as e:
        import re as _re
        m = _re.search(r"already exists \(node GUID: ([0-9A-Fa-f]+)\)", str(e))
        if m:
            S["guids"][name] = m.group(1)
            print("  %s: reused existing node %s" % (name, m.group(1)))
            return m.group(1)
        raise
    return remember(name, r)


def stage_interface():
    s = sid()
    ok(claireon.bp_add_interface(session_id=s, interface_class=BPI_CLASS), "add_interface")
    # FuzzQuery (has output) -> function graph; FuzzNudge (no output) -> event
    try:
        ok(claireon.bp_add_function_override(session_id=s, function_name="FuzzQuery",
                                             interface_class=BPI_CLASS), "override FuzzQuery")
    except RuntimeError as e:
        print("  FuzzQuery override: %s" % str(e)[:160])
    try:
        ok(claireon.bp_add_function_override(session_id=s, function_name="FuzzNudge",
                                             interface_class=BPI_CLASS), "override FuzzNudge")
    except RuntimeError as e:
        print("  FuzzNudge override: %s" % str(e)[:160])
    compile_save(s, "stage_interface")
    print("stage_interface OK")


def body_fuzzquery():
    """Interface implementation: Result = Q*Q + 1."""
    s = sid()
    switch_graph(s, "FuzzQuery")
    entry = _entry_guid(s, "FuzzQuery")
    result = _result_guid(s, "FuzzQuery")
    d = Delta(s)
    # interface graphs are not FunctionGraphs, so member_scope cannot name them;
    # bare variable_name falls back to the session graph's local/param scope.
    d.n("getQ", "VariableGet", 200, 260, variable_name="Q")
    d.n("getQ2", "VariableGet", 200, 400, variable_name="Q")
    d.n("mul", "CallFunction", 420, 230, function_name="Multiply_IntInt", function_class=KML)
    d.n("add1", "CallFunction", 620, 200, function_name="Add_IntInt", function_class=KML)
    d.pv("add1", "B", 1)
    d.c("getQ", "Q", "mul", "A")
    d.c("getQ2", "Q", "mul", "B")
    d.c("mul", "ReturnValue", "add1", "A")
    d.c("add1", "ReturnValue", result, "Result")
    d.c(entry, "then", result, "exec")
    d.apply("FuzzQuery body")


def stage_events():
    """Create all event-graph nodes that need bp_add_node's typed paths."""
    s = sid()
    switch_graph(s, "EventGraph")

    _add("ovBeginPlay", node_type="EventOverride", function_name="ReceiveBeginPlay",
         position_x=0, position_y=0)
    _add("evNudge", node_type="EventOverride", function_name="FuzzNudge",
         position_x=0, position_y=9200)

    ev_ys = {"PhaseA": 900, "PhaseB": 2600, "PhaseC": 4200, "PhaseD": 5000,
             "PhaseE": 6200, "PhaseF": 7600, "RetrigPing": 3600,
             "TimerEventTick": 6700, "ResumeTick": 7300}
    for name, y in ev_ys.items():
        _add("ev%s" % name, node_type="CustomEvent", event_name=name,
             position_x=0, position_y=y)
    _add("evHandle", node_type="CustomEvent", event_name="HandleFuzzPulse",
         user_defined_pins=[{"name": "Value", "type": "int"}],
         position_x=0, position_y=8600)

    _add("tlmain", node_type="Timeline", timeline_name="TL_Main",
         autoplay=False, loop=False, length=3.0,
         float_tracks=[{"track_name": "Alpha", "interpolation": "linear",
                        "keys": [{"time": 0.0, "value": 0.0},
                                 {"time": 3.0, "value": 1.0}]}],
         vector_tracks=[{"track_name": "Orbit", "interpolation": "linear",
                         "keys": [{"time": 0.0, "x": 0, "y": 0, "z": 0},
                                  {"time": 3.0, "x": 0, "y": 0, "z": 60}]}],
         event_tracks=[{"track_name": "Pulse",
                        "keys": [{"time": 0.5}, {"time": 1.5}, {"time": 2.5}]}],
         position_x=300, position_y=1800)
    _add("tlloop", node_type="Timeline", timeline_name="TL_Loop",
         autoplay=False, loop=True, length=0.5,
         event_tracks=[{"track_name": "Tick", "keys": [{"time": 0.25}]}],
         position_x=300, position_y=4500)

    _add("bind1", node_type="AddDelegate", delegate_name="OnFuzzPulse",
         position_x=1800, position_y=300)
    _add("bind2", node_type="AddDelegate", delegate_name="OnFuzzPulse",
         position_x=5200, position_y=5400)
    _add("remd", node_type="RemoveDelegate", delegate_name="OnFuzzPulse",
         position_x=4400, position_y=5400)
    for i, x in [(1, 4000), (2, 4800), (3, 5600)]:
        _add("cd%d" % i, node_type="CallDelegate", delegate_name="OnFuzzPulse",
             position_x=x, position_y=5300)

    # Switches that need case pins added up front
    _add("swInt", node_type="SwitchInteger", position_x=1200, position_y=2600)
    for case in ["1", "2"]:
        ok(claireon.bp_add_pin(session_id=s, node_guid=S["guids"]["swInt"],
                               pin_case=case), "swInt case %s" % case)
    _add("swStr", node_type="SwitchString", position_x=2000, position_y=2600)
    for case in ["Alpha", "Beta"]:
        ok(claireon.bp_add_pin(session_id=s, node_guid=S["guids"]["swStr"],
                               pin_case=case), "swStr case %s" % case)
    _add("swName", node_type="SwitchName", position_x=2800, position_y=2600)
    for case in ["Rho", "Sigma"]:
        ok(claireon.bp_add_pin(session_id=s, node_guid=S["guids"]["swName"],
                               pin_case=case), "swName case %s" % case)
    _add("swTag", node_type="SwitchGameplayTag",
         tags=["Claireon.FuzzTest.Alpha", "Claireon.FuzzTest.Beta"],
         position_x=3600, position_y=2600)
    _add("swTick", node_type="SwitchInteger", position_x=1600, position_y=6700)
    for case in ["1", "2", "3"]:
        ok(claireon.bp_add_pin(session_id=s, node_guid=S["guids"]["swTick"],
                               pin_case=case), "swTick case %s" % case)

    _add("asyncLoad", node_type="AsyncAction",
         function_class="AsyncActionHandleSaveGame",
         function_name="AsyncLoadGameFromSlot",
         position_x=2400, position_y=7600)
    _add("loadAsset", node_type="Generic", class_name="K2Node_LoadAsset",
         position_x=1400, position_y=7600)

    compile_save(s, "stage_events")
    print("stage_events OK; guids: %d" % len(S["guids"]))
    print(S["guids"])


# Event-graph islands.

def load_guids():
    import json as _json, os
    p = fuzzlib.guids_path("conductor")
    if os.path.exists(p):
        S["guids"].update(_json.load(open(p)))
    return S["guids"]


def _log(d, nid, x, y, tag, value=None):
    d.n(nid, "CallFunction", x, y, function_name="FuzzLog")
    d.pv(nid, "Tag", tag)
    if value is not None:
        d.pv(nid, "Value", int(value))
    return nid


def _eg_delta():
    s = sid()
    switch_graph(s, "EventGraph")
    return Delta(s)


def isl_beginplay():
    G = load_guids()
    d = _eg_delta()
    d.n("cmt", "Comment", 300, -200,
        comment_text="ISLAND 1: BeginPlay. Delays StartDelay seconds, then fans out via a "
                     "4-lane Sequence: phase kickoff, diagnostics lane, timer setup, dispatcher bind.")
    _log(d, "logBP", 400, 0, "BeginPlay", 1)
    d.c(G["ovBeginPlay"], "then", "logBP", "exec")
    d.n("delayS", "CallFunction", 800, 0, function_name="Delay", function_class=KSL)
    d.n("getSD", "VariableGet", 650, 200, variable_name="StartDelay")
    d.c("getSD", "StartDelay", "delayS", "Duration")
    d.c("logBP", "then", "delayS", "exec")
    d.n("seqM", "Sequence", 1200, 0, num_extra_pins=2)
    d.c("delayS", "then", "seqM", "exec")
    _log(d, "logPS", 1600, 0, "PhaseStart", 2)
    d.c("seqM", "then_0", "logPS", "exec")
    d.n("setPh", "VariableSet", 2000, 0, variable_name="PhaseIndex")
    d.pv("setPh", "PhaseIndex", 1)
    d.c("logPS", "then", "setPh", "exec")
    d.n("callA", "CallFunction", 2400, 0, function_name="PhaseA")
    d.c("setPh", "then", "callA", "exec")
    d.n("prnD", "CallFunction", 1600, 300, function_name="PrintString", function_class=KSL)
    d.pv("prnD", "InString", "conductor entry diagnostics lane")
    d.pv("prnD", "bPrintToScreen", "false")
    d.pv("prnD", "bPrintToLog", "true")
    d.c("seqM", "then_1", "prnD", "exec")
    d.n("callST", "CallFunction", 1600, 500, function_name="StartPhaseTimers")
    d.c("seqM", "then_2", "callST", "exec")
    d.c("seqM", "then_3", G["bind1"], "execute")
    d.c(G["evHandle"], "OutputDelegate", G["bind1"], "Delegate")
    d.apply("isl_beginplay")


def isl_phaseA():
    G = load_guids()
    d = _eg_delta()
    d.n("cmt", "Comment", 300, 750,
        comment_text="ISLAND 2: PhaseA macro torture. ForLoop x5 drives a looping MultiGate "
                     "into a FlipFlop into a DoOnce (3-way and 2-way exec joins, all exclusive). "
                     "Then ForEach over a literal array, a WhileLoop using the local ClampedInc "
                     "macro, and a reroute chain into TL_Main.Play.")
    _log(d, "logA", 400, 900, "PhaseA", 10)
    d.c(G["evPhaseA"], "then", "logA", "exec")
    d.n("forl", "ForLoop", 800, 900)
    d.pv("forl", "FirstIndex", 0)
    d.pv("forl", "LastIndex", 4)
    d.c("logA", "then", "forl", "execute")
    d.n("mg", "MultiGate", 1200, 900, num_extra_pins=1)
    d.pv("mg", "Loop", "true")
    d.c("forl", "LoopBody", "mg", "execute")
    _log(d, "logMG0", 1600, 850, "MG0", 11)
    _log(d, "logMG1", 1600, 1000, "MG1", 12)
    _log(d, "logMG2", 1600, 1150, "MG2", 13)
    d.c("mg", "Out 0", "logMG0", "exec")
    d.c("mg", "Out 1", "logMG1", "exec")
    d.c("mg", "Out 2", "logMG2", "exec")
    d.n("flip", "FlipFlop", 2000, 950)
    d.c("logMG0", "then", "flip", "None")
    d.c("logMG1", "then", "flip", "None")
    d.c("logMG2", "then", "flip", "None")
    _log(d, "logFlipA", 2400, 900, "FlipA", 14)
    _log(d, "logFlipB", 2400, 1050, "FlipB", 15)
    d.c("flip", "A", "logFlipA", "exec")
    d.c("flip", "B", "logFlipB", "exec")
    d.n("once", "DoOnce", 2800, 950)
    d.c("logFlipA", "then", "once", "execute")
    d.c("logFlipB", "then", "once", "execute")
    _log(d, "logOnce", 3200, 950, "Once", 16)
    d.c("once", "Completed", "logOnce", "exec")
    for nid, x, v in [("lit7", 700, 7), ("lit11", 700, 11), ("lit13", 700, 13)]:
        d.n(nid, "CallFunction", x, 1350 + (v * 4), function_name="MakeLiteralInt",
            function_class=KSL)
        d.pv(nid, "Value", v)
    d.n("mkbag", "MakeArray", 1000, 1400, num_extra_pins=2)
    d.c("lit7", "ReturnValue", "mkbag", "[0]")
    d.c("lit11", "ReturnValue", "mkbag", "[1]")
    d.c("lit13", "ReturnValue", "mkbag", "[2]")
    d.n("each", "ForEachLoop", 1400, 1300)
    d.c("forl", "Completed", "each", "Exec")
    d.c("mkbag", "Array", "each", "Array")
    d.n("magic", "CallFunction", 1700, 1500, function_name="ComputeMagicNumber")
    d.c("each", "Array Element", "magic", "Seed")
    _log(d, "logBag", 2000, 1300, "Bag")
    d.c("magic", "Magic", "logBag", "Value")
    d.c("each", "LoopBody", "logBag", "exec")
    d.n("wh", "WhileLoop", 2400, 1300)
    d.c("each", "Completed", "wh", "execute")
    d.n("getWC", "VariableGet", 2200, 1550, variable_name="WhileCounter")
    d.n("less", "CallFunction", 2400, 1520, function_name="Less_IntInt", function_class=KML)
    d.pv("less", "B", 3)
    d.c("getWC", "WhileCounter", "less", "A")
    d.c("less", "ReturnValue", "wh", "Condition")
    d.n("inc", "MacroInstance", 2800, 1300, macro_name="ClampedInc", macro_library=ASSET)
    d.c("wh", "LoopBody", "inc", "In")
    d.n("getWC2", "VariableGet", 2650, 1500, variable_name="WhileCounter")
    d.c("getWC2", "WhileCounter", "inc", "Value")
    d.n("setWC", "VariableSet", 3200, 1300, variable_name="WhileCounter")
    d.c("inc", "Result", "setWC", "WhileCounter")
    d.c("inc", "Out", "setWC", "exec")
    _log(d, "logWhile", 3600, 1300, "While")
    d.c("setWC", "Output_Get", "logWhile", "Value")
    d.c("setWC", "then", "logWhile", "exec")
    # reroute chain into the timeline (deliberate long-jog knots)
    d.n("knot1", "Knot", 3000, 1700)
    d.n("knot2", "Knot", 1800, 1750)
    d.c("wh", "Completed", "knot1", "InputPin")
    d.c("knot1", "OutputPin", "knot2", "InputPin")
    d.c("knot2", "OutputPin", G["tlmain"], "Play")
    d.apply("isl_phaseA")


def isl_tlmain():
    G = load_guids()
    d = _eg_delta()
    d.n("cmt", "Comment", 300, 1650,
        comment_text="ISLAND 3: TL_Main timeline. Update drives OrbitMesh through a validated "
                     "get (no logging on Update: frame-rate dependent). The Pulse event track "
                     "fires exactly 3 times; Finished chains into PhaseB.")
    d.n("vget", "VariableGet", 700, 1800, variable_name="OrbitMesh", validated=True)
    d.c(G["tlmain"], "Update", "vget", "exec")
    d.n("setrel", "CallFunction", 1100, 1800, function_name="K2_SetRelativeLocation",
        function_class="SceneComponent")
    d.c("vget", "then", "setrel", "exec")
    d.c("vget", "OrbitMesh", "setrel", "self")
    d.c(G["tlmain"], "Orbit", "setrel", "NewLocation")
    _log(d, "logPulse", 1100, 2100, "Pulse", 21)
    d.c(G["tlmain"], "Pulse", "logPulse", "exec")
    _log(d, "logTLDone", 1100, 2300, "TLDone", 22)
    d.c(G["tlmain"], "Finished", "logTLDone", "exec")
    d.n("callB", "CallFunction", 1500, 2300, function_name="PhaseB")
    d.c("logTLDone", "then", "callB", "exec")
    d.apply("isl_tlmain")


def isl_phaseB():
    G = load_guids()
    d = _eg_delta()
    d.n("cmt", "Comment", 300, 2450,
        comment_text="ISLAND 4: PhaseB switch gauntlet (int/string/name/tag/enum), a dead-cast "
                     "and a failing cast (exclusive join), then the retriggerable-delay trap: "
                     "two exec paths enter one RetriggerableDelay; a timer re-triggers it at "
                     "+0.2s so it completes once at +0.8s.")
    _log(d, "logB", 400, 2600, "PhaseB", 30)
    d.c(G["evPhaseB"], "then", "logB", "exec")
    d.c("logB", "then", G["swInt"], "execute")
    d.n("magic2", "CallFunction", 900, 2850, function_name="ComputeMagicNumber")
    d.pv("magic2", "Seed", 2)
    d.n("mod3", "CallFunction", 1080, 2820, function_name="Percent_IntInt", function_class=KML)
    d.pv("mod3", "B", 3)
    d.c("magic2", "Magic", "mod3", "A")
    d.c("mod3", "ReturnValue", G["swInt"], "Selection")
    _log(d, "logSwInt", 1500, 2600, "SwInt", 23)
    d.c(G["swInt"], "0", "logSwInt", "exec")
    d.c("logSwInt", "then", G["swStr"], "execute")
    d.n("desc", "CallFunction", 1800, 2850, function_name="DescribeValue")
    d.c("magic2", "Magic", "desc", "V")
    d.c("desc", "Label", G["swStr"], "Selection")
    _log(d, "logSwStr", 2300, 2600, "SwStr", 24)
    d.c(G["swStr"], "Case_0", "logSwStr", "exec")
    d.c("logSwStr", "then", G["swName"], "execute")
    d.n("litRho", "CallFunction", 2600, 2850, function_name="MakeLiteralName", function_class=KSL)
    d.pv("litRho", "Value", "Rho")
    d.c("litRho", "ReturnValue", G["swName"], "Selection")
    _log(d, "logSwName", 3100, 2600, "SwName", 26)
    d.c(G["swName"], "Case_0", "logSwName", "exec")
    d.c("logSwName", "then", G["swTag"], "execute")
    d.n("mkTag", "CallFunction", 3400, 2850, function_name="MakeLiteralGameplayTag",
        function_class="BlueprintGameplayTagLibrary")
    d.pv("mkTag", "Value", '(TagName="Claireon.FuzzTest.Alpha")')
    d.c("mkTag", "ReturnValue", G["swTag"], "Selection")
    _log(d, "logSwTag", 3900, 2600, "SwTag", 25)
    d.c(G["swTag"], "Claireon.FuzzTest.Alpha", "logSwTag", "exec")
    # Default-pin safety: SwitchString/Name case values via pin_case do not always
    # match at runtime, so route Default into the same case log (exclusive join).
    # This keeps the phase flow deterministic regardless of case-string matching.
    d.c(G["swStr"], "Default", "logSwStr", "exec")
    d.c(G["swName"], "Default", "logSwName", "exec")
    d.c(G["swTag"], "Default", "logSwTag", "exec")
    d.n("swEnum", "SwitchEnum", 4300, 2600, enum_type="ENetRole")
    d.c("logSwTag", "then", "swEnum", "execute")
    d.n("getRole", "CallFunction", 4100, 2850, function_name="GetLocalRole", function_class="Actor")
    d.c("getRole", "ReturnValue", "swEnum", "Selection")
    _log(d, "logSwEnum", 4700, 2600, "SwEnum", 27)
    d.c("swEnum", "ROLE_Authority", "logSwEnum", "exec")
    d.n("selfA", "Generic", 4900, 2850, class_name="K2Node_Self")
    d.n("castOK", "Cast", 5100, 2600, target_class="Actor")
    d.c("logSwEnum", "then", "castOK", "execute")
    d.c("selfA", "self", "castOK", "Object")
    _log(d, "logCastOK", 5500, 2600, "CastOK", 34)
    d.c("castOK", "then", "logCastOK", "exec")
    d.n("selfB", "Generic", 5700, 2850, class_name="K2Node_Self")
    d.n("castFail", "Cast", 5900, 2600, target_class="Pawn")
    d.c("logCastOK", "then", "castFail", "execute")
    d.c("selfB", "self", "castFail", "Object")
    _log(d, "logCastFail", 6300, 2700, "CastFail", 33)
    d.c("castFail", "CastFailed", "logCastFail", "exec")
    # both cast outcomes converge on the scratch write (exclusive join)
    d.n("setSV", "VariableSet", 6700, 2600, variable_name="ScratchVec")
    d.c("castFail", "then", "setSV", "exec")
    d.c("logCastFail", "then", "setSV", "exec")
    d.n("bscall", "CallFunction", 6500, 2850, function_name="BuildScratch")
    d.pv("bscall", "BaseX", "2.5")
    d.c("bscall", "Out", "setSV", "ScratchVec")
    # retrigger trap
    d.n("stPing", "CallFunction", 7100, 2600, function_name="K2_SetTimerDelegate",
        function_class=KSL)
    d.pv("stPing", "Time", "0.2")
    d.pv("stPing", "bLooping", "false")
    d.c(G["evRetrigPing"], "OutputDelegate", "stPing", "Delegate")
    d.c("setSV", "then", "stPing", "exec")
    d.n("retrig", "CallFunction", 7500, 2600, function_name="RetriggerableDelay",
        function_class=KSL)
    d.pv("retrig", "Duration", "0.6")
    d.c("stPing", "then", "retrig", "exec")
    _log(d, "logPing", 400, 3600, "Ping", 32)
    d.c(G["evRetrigPing"], "then", "logPing", "exec")
    d.c("logPing", "then", "retrig", "exec")  # deliberate join INTO a latent node
    _log(d, "logRetrig", 7900, 2600, "Retrig", 31)
    d.c("retrig", "then", "logRetrig", "exec")
    d.n("callC", "CallFunction", 8300, 2600, function_name="PhaseC")
    d.c("logRetrig", "then", "callC", "exec")
    d.apply("isl_phaseB")


def isl_phaseC():
    G = load_guids()
    d = _eg_delta()
    d.n("cmt", "Comment", 300, 4050,
        comment_text="ISLAND 5: PhaseC gated loop. TL_Loop's Tick event track pulses through a "
                     "Gate into a Do N; a counter branch closes the gate, stops the timeline, "
                     "and advances to PhaseD after exactly 3 pulses.")
    _log(d, "logC", 400, 4200, "PhaseC", 40)
    d.c(G["evPhaseC"], "then", "logC", "exec")
    d.n("seqC", "Sequence", 800, 4200)
    d.c("logC", "then", "seqC", "exec")
    d.n("gate", "Gate", 1200, 4400)
    d.pv("gate", "bStartClosed", "true")
    d.c("seqC", "then_0", "gate", "Open")
    d.c("seqC", "then_1", G["tlloop"], "Play")
    d.c(G["tlloop"], "Tick", "gate", "Enter")
    d.n("don", "MacroInstance", 1600, 4400, macro_name="Do N")
    d.pv("don", "n", 3)
    d.c("gate", "Exit", "don", "Enter")
    d.n("getLP", "VariableGet", 1700, 4650, variable_name="LoopPulseCount")
    d.n("addLP", "CallFunction", 1900, 4620, function_name="Add_IntInt", function_class=KML)
    d.pv("addLP", "B", 1)
    d.c("getLP", "LoopPulseCount", "addLP", "A")
    d.n("setLP", "VariableSet", 2100, 4400, variable_name="LoopPulseCount")
    d.c("addLP", "ReturnValue", "setLP", "LoopPulseCount")
    d.c("don", "Exit", "setLP", "exec")
    _log(d, "logLP", 2500, 4400, "LoopPulse")
    d.c("setLP", "Output_Get", "logLP", "Value")
    d.c("setLP", "then", "logLP", "exec")
    d.n("ge3", "CallFunction", 2700, 4650, function_name="GreaterEqual_IntInt", function_class=KML)
    d.pv("ge3", "B", 3)
    d.c("setLP", "Output_Get", "ge3", "A")
    d.n("brLP", "Branch", 2900, 4400)
    d.c("logLP", "then", "brLP", "exec")
    d.c("ge3", "ReturnValue", "brLP", "Condition")
    d.n("seqC2", "Sequence", 3300, 4400, num_extra_pins=1)
    d.c("brLP", "then", "seqC2", "exec")
    d.c("seqC2", "then_0", "gate", "Close")
    d.c("seqC2", "then_1", G["tlloop"], "Stop")
    d.n("callD", "CallFunction", 3700, 4500, function_name="PhaseD")
    d.c("seqC2", "then_2", "callD", "exec")
    d.apply("isl_phaseC")


def isl_phaseD():
    G = load_guids()
    d = _eg_delta()
    d.n("cmt", "Comment", 300, 4850,
        comment_text="ISLAND 6: PhaseD containers and strings (array/map/set literals, "
                     "CallArrayFunction get/length, SelectInt), the branch-join trap whose "
                     "true path is latent (Delay 0.3), the shared TailReport tail called from "
                     "two sites, and the dispatcher bind/unbind/rebind sequence.")
    _log(d, "logD", 400, 5000, "PhaseD", 44)
    d.c(G["evPhaseD"], "then", "logD", "exec")
    for nid, y, v in [("lit7b", 5250, 7), ("lit11b", 5350, 11), ("lit13b", 5450, 13)]:
        d.n(nid, "CallFunction", 500, y, function_name="MakeLiteralInt", function_class=KSL)
        d.pv(nid, "Value", v)
    d.n("mkArr2", "MakeArray", 800, 5250, num_extra_pins=2)
    d.c("lit7b", "ReturnValue", "mkArr2", "[0]")
    d.c("lit11b", "ReturnValue", "mkArr2", "[1]")
    d.c("lit13b", "ReturnValue", "mkArr2", "[2]")
    d.n("accum", "CallFunction", 1100, 5000, function_name="AccumulateArray")
    d.c("logD", "then", "accum", "exec")
    d.c("mkArr2", "Array", "accum", "Items")
    _log(d, "logSum", 1500, 5000, "Sum")
    d.c("accum", "Sum", "logSum", "Value")
    d.c("accum", "then", "logSum", "exec")
    d.n("litKA", "CallFunction", 1500, 5300, function_name="MakeLiteralString", function_class=KSL)
    d.pv("litKA", "Value", "alpha")
    d.n("litV3", "CallFunction", 1500, 5420, function_name="MakeLiteralInt", function_class=KSL)
    d.pv("litV3", "Value", 3)
    d.n("litKB", "CallFunction", 1500, 5540, function_name="MakeLiteralString", function_class=KSL)
    d.pv("litKB", "Value", "beta")
    d.n("litV5", "CallFunction", 1500, 5660, function_name="MakeLiteralInt", function_class=KSL)
    d.pv("litV5", "Value", 5)
    d.n("mkMap", "MakeMap", 1800, 5300, num_extra_pins=1)
    d.c("litKA", "ReturnValue", "mkMap", "Key 0")
    d.c("litV3", "ReturnValue", "mkMap", "Value 0")
    d.c("litKB", "ReturnValue", "mkMap", "Key 1")
    d.c("litV5", "ReturnValue", "mkMap", "Value 1")
    d.n("litKB2", "CallFunction", 1800, 5550, function_name="MakeLiteralString", function_class=KSL)
    d.pv("litKB2", "Value", "beta")
    d.n("mapFind", "CallFunction", 2100, 5300, function_name="Map_Find",
        function_class="BlueprintMapLibrary")
    d.c("mkMap", "Map", "mapFind", "TargetMap")
    d.c("litKB2", "ReturnValue", "mapFind", "Key")
    _log(d, "logMap", 2100, 5000, "MapFind")
    d.c("mapFind", "Value", "logMap", "Value")
    d.c("logSum", "then", "logMap", "exec")
    d.n("litNR", "CallFunction", 2500, 5300, function_name="MakeLiteralName", function_class=KSL)
    d.pv("litNR", "Value", "Red")
    d.n("litNG", "CallFunction", 2500, 5420, function_name="MakeLiteralName", function_class=KSL)
    d.pv("litNG", "Value", "Green")
    d.n("mkSet", "MakeSet", 2800, 5300, num_extra_pins=1)
    d.c("litNR", "ReturnValue", "mkSet", "[0]")
    d.c("litNG", "ReturnValue", "mkSet", "[1]")
    d.n("litNR2", "CallFunction", 2800, 5550, function_name="MakeLiteralName", function_class=KSL)
    d.pv("litNR2", "Value", "Red")
    d.n("setHas", "CallFunction", 3100, 5300, function_name="Set_Contains",
        function_class="BlueprintSetLibrary")
    d.c("mkSet", "Set", "setHas", "TargetSet")
    d.c("litNR2", "ReturnValue", "setHas", "ItemToFind")
    d.n("selHas", "CallFunction", 3300, 5250, function_name="SelectInt", function_class=KML)
    d.pv("selHas", "A", 1)
    d.pv("selHas", "B", 0)
    d.c("setHas", "ReturnValue", "selHas", "bPickA")
    _log(d, "logSetHas", 2600, 5000, "SetHas")
    d.c("selHas", "ReturnValue", "logSetHas", "Value")
    d.c("logMap", "then", "logSetHas", "exec")
    d.n("arrLen", "CallArrayFunction", 3500, 5450, function_name="Array_Length",
        function_class="KismetArrayLibrary")
    d.c("mkArr2", "Array", "arrLen", "TargetArray")
    _log(d, "logArrLen", 3100, 5000, "ArrLen")
    d.c("arrLen", "ReturnValue", "logArrLen", "Value")
    d.c("logSetHas", "then", "logArrLen", "exec")
    d.n("arrGet", "CallArrayFunction", 3700, 5300, function_name="Array_Get",
        function_class="KismetArrayLibrary")
    d.pv("arrGet", "Index", 1)
    d.c("mkArr2", "Array", "arrGet", "TargetArray")
    _log(d, "logArrGet", 3600, 5000, "ArrGet")
    d.c("arrGet", "Item", "logArrGet", "Value")
    d.c("logArrLen", "then", "logArrGet", "exec")
    d.n("strLen", "CallFunction", 4000, 5300, function_name="Len", function_class=KSTR)
    d.pv("strLen", "S", "FuzzBaseline")
    _log(d, "logStrLen", 4100, 5000, "StrLen")
    d.c("strLen", "ReturnValue", "logStrLen", "Value")
    d.c("logArrGet", "then", "logStrLen", "exec")
    d.n("getSV", "VariableGet", 4300, 5300, variable_name="ScratchVec")
    d.n("brkSV", "CallFunction", 4500, 5280, function_name="BreakVector", function_class=KML)
    d.c("getSV", "ScratchVec", "brkSV", "InVec")
    d.n("truncX", "CallFunction", 4700, 5260, function_name="FTrunc", function_class=KML)
    d.c("brkSV", "X", "truncX", "A")
    _log(d, "logScratch", 4600, 5000, "Scratch")
    d.c("truncX", "ReturnValue", "logScratch", "Value")
    d.c("logStrLen", "then", "logScratch", "exec")
    d.n("getLC", "VariableGet", 4900, 5300, variable_name="LoopCount")
    d.n("gt2", "CallFunction", 5100, 5280, function_name="Greater_IntInt", function_class=KML)
    d.pv("gt2", "B", 2)
    d.c("getLC", "LoopCount", "gt2", "A")
    d.n("brJoin", "Branch", 5100, 5000)
    d.c("logScratch", "then", "brJoin", "exec")
    d.c("gt2", "ReturnValue", "brJoin", "Condition")
    d.n("delayJ", "CallFunction", 5500, 5000, function_name="Delay", function_class=KSL)
    d.pv("delayJ", "Duration", "0.3")
    d.c("brJoin", "then", "delayJ", "exec")
    _log(d, "logJoined", 5900, 5050, "Joined", 45)
    d.c("delayJ", "then", "logJoined", "exec")
    d.c("brJoin", "else", "logJoined", "exec")
    d.n("tail1", "CallFunction", 6300, 5050, function_name="TailReport")
    d.pv("tail1", "Code", 1)
    d.c("logJoined", "then", "tail1", "exec")
    d.c("tail1", "then", G["cd1"], "execute")
    d.pin_defaults.append({"node": G["cd1"], "pin": "Value", "value": "41"})
    d.c(G["cd1"], "then", G["remd"], "execute")
    d.c(G["evHandle"], "OutputDelegate", G["remd"], "Delegate")
    d.pin_defaults.append({"node": G["cd2"], "pin": "Value", "value": "42"})
    d.c(G["remd"], "then", G["cd2"], "execute")
    d.c(G["cd2"], "then", G["bind2"], "execute")
    d.c(G["evHandle"], "OutputDelegate", G["bind2"], "Delegate")
    d.pin_defaults.append({"node": G["cd3"], "pin": "Value", "value": "43"})
    d.c(G["bind2"], "then", G["cd3"], "execute")
    d.n("tail2", "CallFunction", 8300, 5050, function_name="TailReport")
    d.pv("tail2", "Code", 2)
    d.c(G["cd3"], "then", "tail2", "exec")
    d.n("callE", "CallFunction", 8700, 5050, function_name="PhaseE")
    d.c("tail2", "then", "callE", "exec")
    d.apply("isl_phaseD")


def isl_phaseE():
    G = load_guids()
    d = _eg_delta()
    d.n("cmt", "Comment", 300, 6050,
        comment_text="ISLAND 7: PhaseE timer choreography. A 0.3s looping timer-by-event "
                     "fires TimerEventTick; fire 2 pauses the handle and schedules ResumeTick "
                     "(+0.5s) to unpause; fire 3 clears the handle and advances to PhaseF. "
                     "Expected fires at 0.3, 0.6, resume 1.1, 1.4.")
    _log(d, "logE", 400, 6200, "PhaseE", 50)
    d.c(G["evPhaseE"], "then", "logE", "exec")
    d.n("stTick", "CallFunction", 800, 6200, function_name="K2_SetTimerDelegate",
        function_class=KSL)
    d.pv("stTick", "Time", "0.3")
    d.pv("stTick", "bLooping", "true")
    d.c(G["evTimerEventTick"], "OutputDelegate", "stTick", "Delegate")
    d.c("logE", "then", "stTick", "exec")
    d.n("setHA2", "VariableSet", 1200, 6200, variable_name="TimerHandleA")
    d.c("stTick", "ReturnValue", "setHA2", "TimerHandleA")
    d.c("stTick", "then", "setHA2", "exec")
    d.n("getTF", "VariableGet", 300, 6950, variable_name="TimerFireCount")
    d.n("addTF", "CallFunction", 500, 6920, function_name="Add_IntInt", function_class=KML)
    d.pv("addTF", "B", 1)
    d.c("getTF", "TimerFireCount", "addTF", "A")
    d.n("setTF", "VariableSet", 700, 6700, variable_name="TimerFireCount")
    d.c("addTF", "ReturnValue", "setTF", "TimerFireCount")
    d.c(G["evTimerEventTick"], "then", "setTF", "exec")
    _log(d, "logTick", 1100, 6700, "Tick")
    d.c("setTF", "Output_Get", "logTick", "Value")
    d.c("setTF", "then", "logTick", "exec")
    d.c("logTick", "then", G["swTick"], "execute")
    d.c("setTF", "Output_Get", G["swTick"], "Selection")
    d.n("pause", "CallFunction", 2000, 6700, function_name="K2_PauseTimerHandle",
        function_class=KSL)
    d.n("getHA", "VariableGet", 1850, 6950, variable_name="TimerHandleA")
    d.c(G["swTick"], "2", "pause", "exec")
    d.c("getHA", "TimerHandleA", "pause", "Handle")
    d.n("stResume", "CallFunction", 2400, 6700, function_name="K2_SetTimerDelegate",
        function_class=KSL)
    d.pv("stResume", "Time", "0.5")
    d.pv("stResume", "bLooping", "false")
    d.c(G["evResumeTick"], "OutputDelegate", "stResume", "Delegate")
    d.c("pause", "then", "stResume", "exec")
    d.n("clear", "CallFunction", 2000, 7000, function_name="K2_ClearAndInvalidateTimerHandle",
        function_class=KSL)
    d.n("getHA2", "VariableGet", 1850, 7250, variable_name="TimerHandleA")
    d.c(G["swTick"], "3", "clear", "exec")
    d.c("getHA2", "TimerHandleA", "clear", "Handle")
    d.n("callF", "CallFunction", 2400, 7000, function_name="PhaseF")
    d.c("clear", "then", "callF", "exec")
    _log(d, "logResume", 400, 7300, "Resume", 55)
    d.c(G["evResumeTick"], "then", "logResume", "exec")
    d.n("unpause", "CallFunction", 800, 7300, function_name="K2_UnPauseTimerHandle",
        function_class=KSL)
    d.n("getHA3", "VariableGet", 650, 7550, variable_name="TimerHandleA")
    d.c("getHA3", "TimerHandleA", "unpause", "Handle")
    d.c("logResume", "then", "unpause", "exec")
    d.apply("isl_phaseE")


def isl_phaseF():
    G = load_guids()
    d = _eg_delta()
    d.n("cmt", "Comment", 300, 7450,
        comment_text="ISLAND 8: PhaseF finale. A no-op Sequence (lint bait), interface calls "
                     "through BPI_Fuzz (function + event), a latent soft-object LoadAsset, an "
                     "async load from a nonexistent save slot (deterministic failure), then "
                     "the DONE checksum line.")
    _log(d, "logF", 400, 7600, "PhaseF", 60)
    d.c(G["evPhaseF"], "then", "logF", "exec")
    d.n("seqNoop", "Sequence", 700, 7600)
    d.c("logF", "then", "seqNoop", "exec")
    d.n("selfC", "Generic", 850, 7850, class_name="K2Node_Self")
    d.n("ifq", "CallFunction", 1000, 7600, function_name="FuzzQuery", function_class=BPI_CLASS)
    d.pv("ifq", "Q", 6)
    d.c("seqNoop", "then_0", "ifq", "exec")
    d.c("selfC", "self", "ifq", "self")
    _log(d, "logIfq", 1400, 7600, "IfaceQ")
    d.c("ifq", "Result", "logIfq", "Value")
    d.c("ifq", "then", "logIfq", "exec")
    d.n("selfD", "Generic", 1550, 7850, class_name="K2Node_Self")
    d.n("nud", "CallFunction", 1700, 7600, function_name="FuzzNudge", function_class=BPI_CLASS)
    d.pv("nud", "Code", 9)
    d.c("selfD", "self", "nud", "self")
    d.c("logIfq", "then", "nud", "exec")
    d.n("getCube", "VariableGet", 1850, 7850, variable_name="CubeSoftRef")
    d.c("nud", "then", G["loadAsset"], "execute")
    d.c("getCube", "CubeSoftRef", G["loadAsset"], "Asset")
    d.n("objName", "CallFunction", 2300, 7850, function_name="GetObjectName",
        function_class="KismetSystemLibrary")
    d.c(G["loadAsset"], "Object", "objName", "Object")
    d.n("len4", "CallFunction", 2500, 7830, function_name="Len", function_class=KSTR)
    d.c("objName", "ReturnValue", "len4", "S")
    _log(d, "logLoaded", 2700, 7600, "Loaded")
    d.c("len4", "ReturnValue", "logLoaded", "Value")
    d.c(G["loadAsset"], "Completed", "logLoaded", "exec")
    d.pin_defaults.append({"node": G["asyncLoad"], "pin": "SlotName", "value": "FuzzNoSuchSlot"})
    d.pin_defaults.append({"node": G["asyncLoad"], "pin": "UserIndex", "value": "0"})
    d.c("logLoaded", "then", G["asyncLoad"], "execute")
    d.n("isv", "CallFunction", 3100, 7850, function_name="IsValid", function_class=KSL)
    d.c(G["asyncLoad"], "SaveGame", "isv", "Object")
    d.n("brValid", "Branch", 3300, 7600)
    d.c(G["asyncLoad"], "Completed", "brValid", "exec")
    d.c("isv", "ReturnValue", "brValid", "Condition")
    _log(d, "logLS1", 3700, 7550, "LoadSlot", 1)
    _log(d, "logLS0", 3700, 7700, "LoadSlot", 0)
    d.c("brValid", "then", "logLS1", "exec")
    d.c("brValid", "else", "logLS0", "exec")
    d.n("done", "CallFunction", 4100, 7600, function_name="FuzzDone")
    d.c("logLS1", "then", "done", "exec")
    d.c("logLS0", "then", "done", "exec")
    d.n("mulN", "CallFunction", 400, 9450, function_name="Multiply_IntInt", function_class=KML)
    d.pv("mulN", "B", 3)
    d.c(G["evNudge"], "Code", "mulN", "A")
    _log(d, "logNudge", 700, 9200, "Nudge")
    d.c("mulN", "ReturnValue", "logNudge", "Value")
    d.c(G["evNudge"], "then", "logNudge", "exec")
    d.apply("isl_phaseF")


def isl_handler():
    G = load_guids()
    d = _eg_delta()
    d.n("cmt", "Comment", 300, 8450,
        comment_text="ISLAND 9: HandleFuzzPulse callback. Bound/unbound/rebound to the "
                     "OnFuzzPulse dispatcher by islands 1 and 6; logs the delegate payload "
                     "(expected 41 then 43; the 42 broadcast lands while unbound).")
    _log(d, "logHandled", 400, 8600, "Handled")
    d.c(G["evHandle"], "Value", "logHandled", "Value")
    d.c(G["evHandle"], "then", "logHandled", "exec")
    d.apply("isl_handler")


def stage_islands():
    isl_beginplay()
    isl_phaseA()
    isl_tlmain()
    isl_phaseB()
    isl_phaseC()
    isl_phaseD()
    isl_phaseE()
    isl_phaseF()
    isl_handler()
    compile_save(sid(), "stage_islands")
    print("stage_islands OK")
