# Build ComponentChorus: delegates, component events, interfaces, and latent motion.
# Stream=CHORUS; moving the sphere ends the overlap before DONE.

import claireon
import fuzzlib
import fuzztrace
from fuzzlib import Delta, ok, node_guid

ASSET = "/Claireon/FuzzBaseline/BP_Fuzz_ComponentChorus"
BEACON = "/Claireon/FuzzBaseline/AC_Fuzz_Beacon"
BEACON_CLASS = "/Claireon/FuzzBaseline/AC_Fuzz_Beacon.AC_Fuzz_Beacon_C"
BPI_CLASS = "/Claireon/FuzzBaseline/BPI_Fuzz.BPI_Fuzz_C"
TARGET = "/Claireon/FuzzBaseline/BP_Fuzz_ChorusTarget.BP_Fuzz_ChorusTarget_C"
KSL = "KismetSystemLibrary"
KML = "KismetMathLibrary"

S = {"sid": None, "guids": {}}


def sid():
    if not S["sid"]:
        S["sid"] = claireon.bp_open(asset_path=ASSET)["data"]["session_id"]
    return S["sid"]


def gn(graph, cls=None, title=None):
    return fuzztrace.find_node(ASSET, graph, cls=cls, title=title)


def _add(name, **kw):
    if name in S["guids"]:
        return S["guids"][name]
    import re
    try:
        r = ok(claireon.bp_add_node(session_id=sid(), **kw), "add %s" % name)
    except RuntimeError as e:
        m = re.search(r"already exists \(node GUID: ([0-9A-Fa-f]+)\)", str(e))
        if m:
            S["guids"][name] = m.group(1)
            return m.group(1)
        raise
    g = node_guid(r)
    S["guids"][name] = g
    return g


def stage_create():
    r = ok(claireon.bp_create(parent_class="Actor", asset_path=ASSET), "create")
    S["sid"] = r["data"]["session_id"]
    s = S["sid"]
    ok(claireon.bp_add_component(session_id=s, component_name="ChorusRoot",
                                 component_class="SceneComponent"), "root")
    ok(claireon.bp_add_component(session_id=s, component_name="SphereMover",
                                 component_class="SphereComponent",
                                 parent_name="ChorusRoot"), "sphere")
    ok(claireon.bp_add_component(session_id=s, component_name="Beacon",
                                 component_class=BEACON_CLASS), "beacon")
    fuzztrace.add_trace_vars(s, ASSET, "CHORUS", "ComponentChorus")
    ok(claireon.bp_add_variable(session_id=s, variable_name="OnChorusEcho",
        variable_type_spec={"base": "MulticastDelegate",
            "signature_function": "/Script/Claireon.ClaireonUObjectInspectMulticast__DelegateSignature"},
        category="Fuzz|Dispatchers"), "OnChorusEcho")
    ok(claireon.bp_add_variable(session_id=s, variable_name="SpawnedTarget",
        variable_type="Actor", category="Transient|Handles"), "SpawnedTarget")
    ok(claireon.bp_add_variable(session_id=s, variable_name="OverlapCount",
        variable_type="int", default_value="0", category="Transient"), "OverlapCount")
    ok(claireon.bp_add_variable(session_id=s, variable_name="EchoScratch",
        variable_type="int", default_value="0", category="Transient"), "EchoScratch")
    ok(claireon.bp_add_variable(session_id=s, variable_name="UnusedDecoy",
        variable_type="/Script/Engine.Actor", category="Config", instance_editable=True,
        tooltip="Unreferenced object-typed var: unreferenced-variable + asset_references bait."),
       "UnusedDecoy")
    for name, cat, fl in [("SpawnedTarget", "Transient|Handles", ["Transient"]),
                          ("OverlapCount", "Transient", ["Transient"]),
                          ("EchoScratch", "Transient", ["Transient"]),
                          ("OnChorusEcho", "Fuzz|Dispatchers", None)]:
        kw = {"session_id": s, "variable_name": name, "category": cat,
              "clear_flags": ["EditAnywhere"]}
        if fl:
            kw["flags"] = fl
        ok(claireon.bp_set_variable_properties(**kw), "prop %s" % name)
    fuzztrace.add_trace_functions(s, ASSET)
    fuzztrace.compile_save = None
    ok(claireon.bp_compile(session_id=s), "compile")
    ok(claireon.bp_save(session_id=s), "save")
    print("stage_create OK")


def stage_functions():
    s = sid()
    ok(claireon.bp_add_function(session_id=s, function_name="ComputeEcho",
        inputs=[{"name": "Seed", "type": "int"}], outputs=[{"name": "Out", "type": "int"}],
        is_pure=True, category="Fuzz|Math"), "fn ComputeEcho")
    claireon.bp_switch_graph(session_id=s, graph_name="ComputeEcho")
    entry = gn("ComputeEcho", cls="FunctionEntry")
    res = gn("ComputeEcho", cls="FunctionResult")
    d = Delta(s)
    d.n("g1", "VariableGet", 200, 250, variable_name="Seed", member_scope="ComputeEcho")
    d.n("g2", "VariableGet", 200, 380, variable_name="Seed", member_scope="ComputeEcho")
    d.n("mul", "CallFunction", 420, 250, function_name="Multiply_IntInt", function_class=KML)
    d.n("sub", "CallFunction", 620, 230, function_name="Subtract_IntInt", function_class=KML)
    d.pv("sub", "B", 1)
    d.c("g1", "Seed", "mul", "A")
    d.c("g2", "Seed", "mul", "B")
    d.c("mul", "ReturnValue", "sub", "A")
    d.c("sub", "ReturnValue", res, "Out")
    d.c(entry, "then", res, "exec")
    d.apply("ComputeEcho body")
    try:
        claireon.bp_add_interface(session_id=s, interface_class=BPI_CLASS)
    except RuntimeError as e:
        print("  add_interface:", str(e)[:80])
    ok(claireon.bp_compile(session_id=s), "compile")
    ok(claireon.bp_save(session_id=s), "save")
    print("stage_functions OK")


def stage_events():
    s = sid()
    claireon.bp_switch_graph(session_id=s, graph_name="EventGraph")
    _add("beginplay", node_type="EventOverride", function_name="ReceiveBeginPlay",
         position_x=0, position_y=0)
    _add("ovBegin", node_type="ComponentBoundEvent", component_name="SphereMover",
         delegate_name="OnComponentBeginOverlap", position_x=0, position_y=1400)
    _add("ovEnd", node_type="ComponentBoundEvent", component_name="SphereMover",
         delegate_name="OnComponentEndOverlap", position_x=0, position_y=2100)
    _add("ovHit", node_type="ComponentBoundEvent", component_name="SphereMover",
         delegate_name="OnComponentHit", position_x=0, position_y=2700)
    _add("beaconPulse", node_type="ComponentBoundEvent", component_name="Beacon",
         delegate_name="OnBeaconPulse", position_x=0, position_y=3300)
    _add("choreo", node_type="CustomEvent", event_name="Choreograph",
         position_x=0, position_y=3900)
    _add("handleEcho", node_type="CustomEvent", event_name="HandleChorusEcho",
         user_defined_pins=[{"name": "Value", "type": "int"}],
         position_x=0, position_y=5200)
    try:
        _add("nudge", node_type="EventOverride", function_name="FuzzNudge",
             position_x=0, position_y=5800)
    except RuntimeError as e:
        print("  nudge:", str(e)[:100])
    _add("addEcho", node_type="AddDelegate", delegate_name="OnChorusEcho",
         position_x=1800, position_y=4000)
    _add("addEcho2", node_type="AddDelegate", delegate_name="OnChorusEcho",
         position_x=5200, position_y=4000)
    _add("remEcho", node_type="RemoveDelegate", delegate_name="OnChorusEcho",
         position_x=4400, position_y=4000)
    _add("clrEcho", node_type="ClearDelegate", delegate_name="OnChorusEcho",
         position_x=6400, position_y=4000)
    for i, x in [(1, 2600), (2, 4800), (3, 5600)]:
        _add("callEcho%d" % i, node_type="CallDelegate", delegate_name="OnChorusEcho",
             position_x=x, position_y=3900)
    # Create delegates before the delta; it cannot create them.
    _add("cdBind", node_type="CreateDelegate", function_name="HandleChorusEcho",
         position_x=1500, position_y=4300)
    _add("cdUnbind", node_type="CreateDelegate", function_name="HandleChorusEcho",
         position_x=4200, position_y=4300)
    _add("cdRebind", node_type="CreateDelegate", function_name="HandleChorusEcho",
         position_x=5000, position_y=4300)
    ok(claireon.bp_compile(session_id=s), "compile")
    ok(claireon.bp_save(session_id=s), "save")
    print("stage_events OK; guids:", list(S["guids"].keys()))
    import json
    json.dump(S["guids"], open(fuzzlib.guids_path("chorus"), "w"), indent=1)



def load_guids():
    import json as _json, os
    p = fuzzlib.guids_path("chorus")
    if os.path.exists(p):
        S["guids"].update(_json.load(open(p)))
    return S["guids"]


def _eg():
    s = sid()
    claireon.bp_switch_graph(session_id=s, graph_name="EventGraph")
    return Delta(s)


def _log(d, nid, x, y, tag, value=None):
    d.n(nid, "CallFunction", x, y, function_name="FuzzLog")
    d.pv(nid, "Tag", tag)
    if value is not None:
        d.pv(nid, "Value", int(value))
    return nid


def isl_beginplay():
    G = load_guids()
    d = _eg()
    d.n("cmt", "Comment", 300, -200,
        comment_text="ISLAND 1: BeginPlay. Delay 2s, then a 3-lane Sequence: bind the self "
                     "dispatcher (AddDelegate+CreateDelegate to HandleChorusEcho), spawn the "
                     "overlap target on top of SphereMover (synchronous begin-overlap), and "
                     "kick Choreograph.")
    _log(d, "logBP", 400, 0, "BeginPlay", 1)
    d.c(G["beginplay"], "then", "logBP", "exec")
    d.n("delay", "CallFunction", 800, 0, function_name="Delay", function_class=KSL)
    d.pv("delay", "Duration", "2.0")
    d.c("logBP", "then", "delay", "exec")
    d.n("seq", "Sequence", 1200, 0, num_extra_pins=1)
    d.c("delay", "then", "seq", "exec")
    d.c("seq", "then_0", G["addEcho"], "execute")
    d.c(G["cdBind"], "OutputDelegate", G["addEcho"], "Delegate")
    # lane 1: spawn overlap target at sphere world location
    d.n("getSphere", "VariableGet", 1500, 600, variable_name="SphereMover")
    d.n("getLoc", "CallFunction", 1720, 600, function_name="K2_GetComponentLocation",
        function_class="SceneComponent")
    d.c("getSphere", "SphereMover", "getLoc", "self")
    d.n("mkxf", "CallFunction", 1960, 560, function_name="MakeTransform", function_class=KML)
    d.c("getLoc", "ReturnValue", "mkxf", "Location")
    d.n("spawn", "SpawnActor", 2200, 250, actor_class=TARGET)
    d.c("seq", "then_1", "spawn", "exec")
    d.c("mkxf", "ReturnValue", "spawn", "SpawnTransform")
    d.n("setTgt", "VariableSet", 2600, 250, variable_name="SpawnedTarget")
    d.c("spawn", "ReturnValue", "setTgt", "SpawnedTarget")
    d.c("spawn", "then", "setTgt", "exec")
    d.n("callCh", "CallFunction", 1500, 900, function_name="Choreograph")
    d.c("seq", "then_2", "callCh", "exec")
    d.apply("isl_beginplay")


def isl_overlaps():
    G = load_guids()
    d = _eg()
    d.n("cmt", "Comment", 300, 1250,
        comment_text="ISLAND 2: SphereMover component-bound events. Begin-overlap increments "
                     "OverlapCount and logs it (validated get on OtherActor); End-overlap and "
                     "Hit log on their own rails. Beacon dispatcher bound event logs its payload.")
    d.n("getOC", "VariableGet", 400, 1650, variable_name="OverlapCount")
    d.n("inc", "CallFunction", 600, 1620, function_name="Add_IntInt", function_class=KML)
    d.pv("inc", "B", 1)
    d.c("getOC", "OverlapCount", "inc", "A")
    d.n("setOC", "VariableSet", 820, 1400, variable_name="OverlapCount")
    d.c("inc", "ReturnValue", "setOC", "OverlapCount")
    d.c(G["ovBegin"], "then", "setOC", "exec")
    _log(d, "logOv", 1200, 1400, "Overlap")
    d.c("setOC", "Output_Get", "logOv", "Value")
    d.c("setOC", "then", "logOv", "exec")
    _log(d, "logEnd", 400, 2100, "EndOverlap", 0)
    d.c(G["ovEnd"], "then", "logEnd", "exec")
    _log(d, "logHit", 400, 2700, "Hit", 0)
    d.c(G["ovHit"], "then", "logHit", "exec")
    _log(d, "logBeacon", 400, 3300, "BeaconPulse")
    d.c(G["beaconPulse"], "Value", "logBeacon", "Value")
    d.c(G["beaconPulse"], "then", "logBeacon", "exec")
    d.apply("isl_overlaps")


def isl_choreo():
    G = load_guids()
    d = _eg()
    d.n("cmt", "Comment", 300, 3750,
        comment_text="ISLAND 3: Choreograph. Broadcast self dispatcher (5), unbind, broadcast "
                     "(6, dropped), rebind via a second CreateDelegate, broadcast (7); call the "
                     "beacon interface FuzzQuery; make the beacon Pulse; MoveComponentTo drives "
                     "the sphere out of the target (end-overlap), then FuzzDone.")
    d.pin_defaults.append({"node": G["callEcho1"], "pin": "Value", "value": "5"})
    d.c(G["choreo"], "then", G["callEcho1"], "execute")
    # unbind, broadcast 6 (dropped), rebind, broadcast 7
    d.pin_defaults.append({"node": G["callEcho2"], "pin": "Value", "value": "6"})
    d.pin_defaults.append({"node": G["callEcho3"], "pin": "Value", "value": "7"})
    d.c(G["callEcho1"], "then", G["remEcho"], "execute")
    d.c(G["cdUnbind"], "OutputDelegate", G["remEcho"], "Delegate")
    d.c(G["remEcho"], "then", G["callEcho2"], "execute")
    d.c(G["callEcho2"], "then", G["addEcho2"], "execute")
    d.c(G["cdRebind"], "OutputDelegate", G["addEcho2"], "Delegate")
    d.c(G["addEcho2"], "then", G["callEcho3"], "execute")
    d.n("getB", "VariableGet", 6000, 4300, variable_name="Beacon")
    d.n("echoSeed", "CallFunction", 6000, 4450, function_name="ComputeEcho")
    d.pv("echoSeed", "Seed", 3)
    d.n("ifq", "CallFunction", 6200, 3900, function_name="FuzzQuery",
        function_class=BPI_CLASS)
    d.c(G["callEcho3"], "then", "ifq", "exec")
    d.c("getB", "Beacon", "ifq", "self")
    d.c("echoSeed", "Out", "ifq", "Q")
    _log(d, "logIfq", 6600, 3900, "IfaceQ")
    d.c("ifq", "Result", "logIfq", "Value")
    d.c("ifq", "then", "logIfq", "exec")
    # beacon Pulse(9) -> fires beaconPulse bound event
    d.n("getB2", "VariableGet", 7000, 4300, variable_name="Beacon")
    d.n("pulse", "CallFunction", 7000, 3900, function_name="Pulse",
        function_class=BEACON_CLASS)
    d.pv("pulse", "Amount", 9)
    d.c("getB2", "Beacon", "pulse", "self")
    d.c("logIfq", "then", "pulse", "exec")
    d.n("selfN", "Generic", 7400, 4300, class_name="K2Node_Self")
    d.n("nud", "CallFunction", 7400, 3900, function_name="FuzzNudge",
        function_class=BPI_CLASS)
    d.pv("nud", "Code", 4)
    d.c("selfN", "self", "nud", "self")
    d.c("pulse", "then", "nud", "exec")
    # MoveComponentTo sphere away
    d.n("getSphere", "VariableGet", 7800, 4300, variable_name="SphereMover")
    d.n("mvTo", "CallFunction", 8000, 3900, function_name="MoveComponentTo",
        function_class=KSL)
    d.pv("mvTo", "OverTime", "0.4")
    d.n("mkVec", "CallFunction", 7800, 4500, function_name="MakeVector", function_class=KML)
    d.pv("mkVec", "X", "5000.0")
    d.c("getSphere", "SphereMover", "mvTo", "Component")
    d.c("mkVec", "ReturnValue", "mvTo", "TargetRelativeLocation")
    d.c("nud", "then", "mvTo", "Move")
    _log(d, "logMove", 8500, 3900, "MoveDone", 0)
    d.c("mvTo", "then", "logMove", "exec")
    d.n("done", "CallFunction", 8900, 3900, function_name="FuzzDone")
    d.c("logMove", "then", "done", "exec")
    d.apply("isl_choreo")


def isl_handlers():
    G = load_guids()
    d = _eg()
    d.n("cmt", "Comment", 300, 5050,
        comment_text="ISLAND 4: dispatcher + interface handlers. HandleChorusEcho logs the "
                     "echo payload (expected 5 then 7; the 6 broadcast lands while unbound). "
                     "FuzzNudge logs Code*10.")
    _log(d, "logEcho", 400, 5200, "Echo")
    d.c(G["handleEcho"], "Value", "logEcho", "Value")
    d.c(G["handleEcho"], "then", "logEcho", "exec")
    d.n("mulN", "CallFunction", 400, 6050, function_name="Multiply_IntInt", function_class=KML)
    d.pv("mulN", "B", 10)
    d.c(G["nudge"], "Code", "mulN", "A")
    _log(d, "logNudge", 700, 5800, "Nudge")
    d.c("mulN", "ReturnValue", "logNudge", "Value")
    d.c(G["nudge"], "then", "logNudge", "exec")
    d.apply("isl_handlers")


def stage_islands():
    isl_beginplay()
    isl_overlaps()
    isl_choreo()
    isl_handlers()
    s = sid()
    ok(claireon.bp_compile(session_id=s), "compile")
    ok(claireon.bp_save(session_id=s), "save")
    print("stage_islands OK")


def stage_finalize():
    """CreateDelegate binds resolve only when SelectedFunctionName is set via the
    property-change path AFTER the OutputDelegate pin is wired (bp_add_node's raw
    assignment does not fire the notify that resolves the function guid)."""
    s = sid()
    G = load_guids()
    for k in ["cdBind", "cdUnbind", "cdRebind"]:
        ok(claireon.bp_set_node_property(session_id=s, node_guid=G[k],
            property_name="SelectedFunctionName", property_value="HandleChorusEcho"),
           "bind %s" % k)
    ok(claireon.bp_compile(session_id=s), "compile")
    ok(claireon.bp_save(session_id=s), "save")
    print("stage_finalize OK")
