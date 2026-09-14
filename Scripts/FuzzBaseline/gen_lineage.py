# Build Base, Child, and Grandchild fixtures for parent calls, virtual dispatch,
# construction scripts, and inherited configuration.

import claireon
import fuzzlib
import fuzztrace
from fuzzlib import Delta, ok, node_guid

BASE = "/Claireon/FuzzBaseline/BP_Fuzz_LineageBase"
CHILD = "/Claireon/FuzzBaseline/BP_Fuzz_LineageChild"
GRAND = "/Claireon/FuzzBaseline/BP_Fuzz_LineageGrandchild"
KSL = "KismetSystemLibrary"
KML = "KismetMathLibrary"

S = {}


def _open(asset):
    return claireon.bp_open(asset_path=asset)["data"]["session_id"]


def _find(asset, graph, cls=None, title=None):
    return fuzztrace.find_node(asset, graph, cls=cls, title=title)



def build_base():
    r = ok(claireon.bp_create(parent_class="Actor", asset_path=BASE), "create base")
    s = r["data"]["session_id"]
    ok(claireon.bp_add_component(session_id=s, component_name="BaseRoot",
                                 component_class="SceneComponent"), "root")
    fuzztrace.add_trace_vars(s, BASE, "LINEAGE", "LineageBase")
    ok(claireon.bp_add_variable(session_id=s, variable_name="BaseTuning",
        variable_type="int", default_value="10", category="Config",
        instance_editable=True,
        tooltip="Per-instance tuning; construction script derives ConstructedValue from it."),
       "BaseTuning")
    ok(claireon.bp_add_variable(session_id=s, variable_name="ConstructedValue",
        variable_type="int", default_value="0", category="Setup"), "ConstructedValue")
    ok(claireon.bp_add_variable(session_id=s, variable_name="OnLineageSignal",
        variable_type_spec={"base": "MulticastDelegate",
            "signature_function": "/Script/Claireon.ClaireonUObjectInspectMulticast__DelegateSignature"},
        category="Fuzz|Dispatchers"), "OnLineageSignal")
    # construction-authored persistent default: NOT Transient (lint-exempt pattern)
    ok(claireon.bp_set_variable_properties(session_id=s, variable_name="ConstructedValue",
        category="Setup", clear_flags=["EditAnywhere"]), "prop CV")
    ok(claireon.bp_set_variable_properties(session_id=s, variable_name="OnLineageSignal",
        category="Fuzz|Dispatchers", clear_flags=["EditAnywhere"]), "prop sig")
    fuzztrace.add_trace_functions(s, BASE)

    # RunTier(): overridable, each tier calls super. Base logs Tier=1.
    ok(claireon.bp_add_function(session_id=s, function_name="RunTier",
        category="Fuzz|Lineage",
        tooltip="Virtual: each subclass overrides and Calls Parent first."), "fn RunTier")
    # ComputeTier(int)->int: virtual, base returns Seed + BaseTuning.
    ok(claireon.bp_add_function(session_id=s, function_name="ComputeTier",
        inputs=[{"name": "Seed", "type": "int"}], outputs=[{"name": "Out", "type": "int"}],
        category="Fuzz|Lineage"), "fn ComputeTier")
    ok(claireon.bp_add_function(session_id=s, function_name="HandleSignal",
        inputs=[{"name": "Value", "type": "int"}], category="Fuzz|Lineage"), "fn HandleSignal")

    _base_computetier(s)
    _base_runtier(s)
    _base_handlesignal(s)
    _base_construction(s)
    _base_beginplay(s)
    ok(claireon.bp_compile(session_id=s), "compile base")
    ok(claireon.bp_save(session_id=s), "save base")
    claireon.bp_close(session_id=s, suppress_output=True)
    print("build_base OK")


def _base_computetier(s):
    claireon.bp_switch_graph(session_id=s, graph_name="ComputeTier")
    entry = _find(BASE, "ComputeTier", cls="FunctionEntry")
    res = _find(BASE, "ComputeTier", cls="FunctionResult")
    d = Delta(s)
    d.n("gs", "VariableGet", 200, 250, variable_name="Seed", member_scope="ComputeTier")
    d.n("gt", "VariableGet", 200, 380, variable_name="BaseTuning")
    d.n("add", "CallFunction", 420, 240, function_name="Add_IntInt", function_class=KML)
    d.c("gs", "Seed", "add", "A")
    d.c("gt", "BaseTuning", "add", "B")
    d.c("add", "ReturnValue", res, "Out")
    d.c(entry, "then", res, "exec")
    d.apply("base ComputeTier")


def _base_runtier(s):
    claireon.bp_switch_graph(session_id=s, graph_name="RunTier")
    entry = _find(BASE, "RunTier", cls="FunctionEntry")
    d = Delta(s)
    d.n("ct", "CallFunction", 400, 0, function_name="ComputeTier")
    d.pv("ct", "Seed", 1)
    d.n("log", "CallFunction", 700, 0, function_name="FuzzLog")
    d.pv("log", "Tag", "Tier1")
    d.c(entry, "then", "ct", "exec")   # ComputeTier is impure: keep it on the exec rail
    d.c("ct", "then", "log", "exec")
    d.c("ct", "Out", "log", "Value")
    d.apply("base RunTier")


def _base_handlesignal(s):
    claireon.bp_switch_graph(session_id=s, graph_name="HandleSignal")
    entry = _find(BASE, "HandleSignal", cls="FunctionEntry")
    d = Delta(s)
    d.n("log", "CallFunction", 400, 0, function_name="FuzzLog")
    d.pv("log", "Tag", "Signal")
    d.n("gv", "VariableGet", 250, 250, variable_name="Value", member_scope="HandleSignal")
    d.c("gv", "Value", "log", "Value")
    d.c(entry, "then", "log", "exec")
    d.apply("base HandleSignal")


def _base_construction(s):
    claireon.bp_switch_graph(session_id=s, graph_name="UserConstructionScript")
    entry = _find(BASE, "UserConstructionScript", cls="FunctionEntry")
    d = Delta(s)
    d.n("gt", "VariableGet", 300, 250, variable_name="BaseTuning")
    d.n("mul", "CallFunction", 500, 220, function_name="Multiply_IntInt", function_class=KML)
    d.pv("mul", "B", 2)
    d.n("set", "VariableSet", 720, 0, variable_name="ConstructedValue")
    d.c("gt", "BaseTuning", "mul", "A")
    d.c("mul", "ReturnValue", "set", "ConstructedValue")
    d.c(entry, "then", "set", "exec")
    d.apply("base construction")


def _base_beginplay(s):
    claireon.bp_switch_graph(session_id=s, graph_name="EventGraph")
    # Pre-create the nodes apply_delta cannot make (event override + delegate nodes).
    import re
    try:
        bpg = node_guid(ok(claireon.bp_add_node(session_id=s, node_type="EventOverride",
            function_name="ReceiveBeginPlay", position_x=0, position_y=0), "beginplay"))
    except RuntimeError as e:
        m = re.search(r"already exists \(node GUID: ([0-9A-Fa-f]+)\)", str(e))
        bpg = m.group(1)
    cdg = node_guid(ok(claireon.bp_add_node(session_id=s, node_type="CreateDelegate",
        function_name="HandleSignal", position_x=600, position_y=350), "cd"))
    addg = node_guid(ok(claireon.bp_add_node(session_id=s, node_type="AddDelegate",
        delegate_name="OnLineageSignal", position_x=1300, position_y=0), "add"))
    callg = node_guid(ok(claireon.bp_add_node(session_id=s, node_type="CallDelegate",
        delegate_name="OnLineageSignal", position_x=2300, position_y=0), "callsig"))
    d = Delta(s)
    d.n("delay", "CallFunction", 300, 0, function_name="Delay", function_class=KSL)
    d.pv("delay", "Duration", "2.0")
    d.c(bpg, "then", "delay", "exec")
    _log(d, "logBP", 700, -200, "BeginPlay", 1)
    d.c("delay", "then", "logBP", "exec")
    d.c("logBP", "then", addg, "execute")
    d.c(cdg, "OutputDelegate", addg, "Delegate")
    _log(d, "logCV", 1700, -200, "Constructed")
    d.n("gcv", "VariableGet", 1550, 50, variable_name="ConstructedValue")
    d.c("gcv", "ConstructedValue", "logCV", "Value")
    d.c(addg, "then", "logCV", "exec")
    d.pv2 = None
    d.pin_defaults.append({"node": callg, "pin": "Value", "value": "3"})
    d.c("logCV", "then", callg, "execute")
    d.n("rt", "CallFunction", 2700, -200, function_name="RunTier")
    d.n("done", "CallFunction", 3000, -200, function_name="FuzzDone")
    d.c(callg, "then", "rt", "exec")
    d.c("rt", "then", "done", "exec")
    d.apply("base beginplay")
    # CreateDelegate resolves only when SelectedFunctionName is set via the
    # property-change path AFTER OutputDelegate is wired.
    ok(claireon.bp_set_node_property(session_id=s, node_guid=cdg,
        property_name="SelectedFunctionName", property_value="HandleSignal"), "bind cd")


def _log(d, nid, x, y, tag, value=None):
    d.n(nid, "CallFunction", x, y, function_name="FuzzLog")
    d.pv(nid, "Tag", tag)
    if value is not None:
        d.pv(nid, "Value", int(value))
    return nid


def _find_by_tag(s, tag):
    """Find a FuzzLog call node whose Tag pin default is `tag`."""
    r = claireon.bp_get_graph(asset_path=BASE, graph_name="EventGraph",
                              node_detail_level="full", include_pin_defaults=True)
    for g in r["data"]["graphs"]:
        for n in g["nodes"]:
            if n.get("node_title") in ("FuzzLog", "Fuzz Log"):
                for p in n.get("pins", []):
                    if p.get("pin_name") == "Tag" and p.get("default_value") == tag:
                        return n["node_id"]
    raise RuntimeError("no FuzzLog with tag %s" % tag)


# Child overrides call the parent; ChildOnlyStep has no parent equivalent.

def _override(s, asset, fn, interface=False):
    import re
    try:
        r = claireon.bp_add_function_override(session_id=s, function_name=fn)
        return r
    except RuntimeError as e:
        if "already" in str(e):
            return None
        raise


def build_child():
    r = ok(claireon.bp_create(parent_class=BASE + ".BP_Fuzz_LineageBase_C",
                              asset_path=CHILD), "create child")
    s = r["data"]["session_id"]
    # override ComputeTier: Out = Parent(Seed) + 100
    ok(claireon.bp_add_function_override(session_id=s, function_name="ComputeTier"),
       "ov ComputeTier")
    claireon.bp_switch_graph(session_id=s, graph_name="ComputeTier")
    entry = _find(CHILD, "ComputeTier", cls="FunctionEntry")
    res = _find(CHILD, "ComputeTier", cls="FunctionResult")
    parent = node_guid(ok(claireon.bp_add_node(session_id=s, node_type="CallParentFunction",
        function_name="ComputeTier", position_x=350, position_y=0), "callparent"))
    d = Delta(s)
    d.n("gs", "VariableGet", 200, 250, variable_name="Seed", member_scope="ComputeTier")
    d.c("gs", "Seed", parent, "Seed")
    d.c(entry, "then", parent, "exec")
    d.n("add", "CallFunction", 700, 200, function_name="Add_IntInt", function_class=KML)
    d.pv("add", "B", 100)
    d.c(parent, "Out", "add", "A")
    d.c("add", "ReturnValue", res, "Out")
    d.c(parent, "then", res, "exec")
    d.apply("child ComputeTier")
    # override RunTier: Call Parent, then log Tier2 with ComputeTier(2)
    ok(claireon.bp_add_function_override(session_id=s, function_name="RunTier", force_function_graph=True),
       "ov RunTier")
    claireon.bp_switch_graph(session_id=s, graph_name="RunTier")
    entry2 = _find(CHILD, "RunTier", cls="FunctionEntry")
    parent2 = node_guid(ok(claireon.bp_add_node(session_id=s, node_type="CallParentFunction",
        function_name="RunTier", position_x=350, position_y=0), "callparent2"))
    d2 = Delta(s)
    d2.c(entry2, "then", parent2, "exec")
    d2.n("ct", "CallFunction", 700, 0, function_name="ComputeTier")
    d2.pv("ct", "Seed", 2)
    d2.c(parent2, "then", "ct", "exec")
    d2.n("log", "CallFunction", 1000, 0, function_name="FuzzLog")
    d2.pv("log", "Tag", "Tier2")
    d2.c("ct", "then", "log", "exec")
    d2.c("ct", "Out", "log", "Value")
    d2.apply("child RunTier")
    ok(claireon.bp_add_function(session_id=s, function_name="ChildOnlyStep",
        category="Fuzz|Lineage",
        tooltip="Child-only helper (no parent equivalent); a legit non-shadowing member."),
       "fn ChildOnlyStep")
    claireon.bp_switch_graph(session_id=s, graph_name="ChildOnlyStep")
    e3 = _find(CHILD, "ChildOnlyStep", cls="FunctionEntry")
    d3 = Delta(s)
    d3.n("log", "CallFunction", 400, 0, function_name="FuzzLog")
    d3.pv("log", "Tag", "ChildStep")
    d3.pv("log", "Value", 222)
    d3.c(e3, "then", "log", "exec")
    d3.apply("child ChildOnlyStep")
    ok(claireon.bp_compile(session_id=s), "compile child")
    ok(claireon.bp_save(session_id=s), "save child")
    claireon.bp_close(session_id=s, suppress_output=True)
    print("build_child OK")


def build_grand():
    r = ok(claireon.bp_create(parent_class=CHILD + ".BP_Fuzz_LineageChild_C",
                              asset_path=GRAND), "create grand")
    s = r["data"]["session_id"]
    # override ComputeTier: Out = Parent(Seed) + 1000
    ok(claireon.bp_add_function_override(session_id=s, function_name="ComputeTier"),
       "ov ComputeTier")
    claireon.bp_switch_graph(session_id=s, graph_name="ComputeTier")
    entry = _find(GRAND, "ComputeTier", cls="FunctionEntry")
    res = _find(GRAND, "ComputeTier", cls="FunctionResult")
    parent = node_guid(ok(claireon.bp_add_node(session_id=s, node_type="CallParentFunction",
        function_name="ComputeTier", position_x=350, position_y=0), "callparent"))
    d = Delta(s)
    d.n("gs", "VariableGet", 200, 250, variable_name="Seed", member_scope="ComputeTier")
    d.c("gs", "Seed", parent, "Seed")
    d.c(entry, "then", parent, "exec")
    d.n("add", "CallFunction", 700, 200, function_name="Add_IntInt", function_class=KML)
    d.pv("add", "B", 1000)
    d.c(parent, "Out", "add", "A")
    d.c("add", "ReturnValue", res, "Out")
    d.c(parent, "then", res, "exec")
    d.apply("grand ComputeTier")
    # override RunTier: Call Parent, then log Tier3 with ComputeTier(3), then ChildOnlyStep
    ok(claireon.bp_add_function_override(session_id=s, function_name="RunTier", force_function_graph=True),
       "ov RunTier")
    claireon.bp_switch_graph(session_id=s, graph_name="RunTier")
    entry2 = _find(GRAND, "RunTier", cls="FunctionEntry")
    parent2 = node_guid(ok(claireon.bp_add_node(session_id=s, node_type="CallParentFunction",
        function_name="RunTier", position_x=350, position_y=0), "callparent2"))
    d2 = Delta(s)
    d2.c(entry2, "then", parent2, "exec")
    d2.n("ct", "CallFunction", 700, 0, function_name="ComputeTier")
    d2.pv("ct", "Seed", 3)
    d2.c(parent2, "then", "ct", "exec")
    d2.n("log", "CallFunction", 1000, 0, function_name="FuzzLog")
    d2.pv("log", "Tag", "Tier3")
    d2.c("ct", "then", "log", "exec")
    d2.c("ct", "Out", "log", "Value")
    d2.n("cs", "CallFunction", 1300, 0, function_name="ChildOnlyStep")
    d2.c("log", "then", "cs", "exec")
    d2.apply("grand RunTier")
    ge = ok(claireon.bp_add_node(session_id=s, node_type="CustomEvent",
        event_name="GrandchildBellRing", position_x=0, position_y=800), "customevent")
    geg = node_guid(ge)
    d3 = Delta(s)
    d3.n("log", "CallFunction", 400, 800, function_name="FuzzLog")
    d3.pv("log", "Tag", "Bell")
    d3.pv("log", "Value", 333)
    d3.c(geg, "then", "log", "exec")
    d3.apply("grand bell event")
    ok(claireon.bp_compile(session_id=s), "compile grand")
    ok(claireon.bp_save(session_id=s), "save grand")
    claireon.bp_close(session_id=s, suppress_output=True)
    print("build_grand OK")
