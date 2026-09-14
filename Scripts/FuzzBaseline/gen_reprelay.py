# Build RepChild and RepRelay with SV and CL trace streams.
# The server drives timed actions; clients log replication notifications.

import claireon
import fuzzlib
import fuzztrace
from fuzzlib import Delta, ok, node_guid

CHILD = "/Claireon/FuzzBaseline/BP_Fuzz_RepChild"
RELAY = "/Claireon/FuzzBaseline/BP_Fuzz_RepRelay"
CHILD_CLASS = CHILD + ".BP_Fuzz_RepChild_C"
KSL = "KismetSystemLibrary"
KML = "KismetMathLibrary"
GS = "GameplayStatics"


def _find(asset, graph, cls=None, title=None):
    return fuzztrace.find_node(asset, graph, cls=cls, title=title)


def _recover_beginplay(s):
    import re
    try:
        return node_guid(ok(claireon.bp_add_node(session_id=s, node_type="EventOverride",
            function_name="ReceiveBeginPlay", position_x=0, position_y=0), "beginplay"))
    except RuntimeError as e:
        m = re.search(r"already exists \(node GUID: ([0-9A-Fa-f]+)\)", str(e))
        return m.group(1)


def _stream_head(d, bpg):
    """Add the FuzzStream = HasAuthority?'SV':'CL' head to delta `d`, wired from
    BeginPlay `bpg`. Returns the 'setStream' local id (exec continues from it)."""
    d.n("auth", "CallFunction", 300, 250, function_name="HasAuthority", function_class="Actor")
    d.n("svLit", "CallFunction", 300, 400, function_name="MakeLiteralString", function_class=KSL)
    d.pv("svLit", "Value", "SV")
    d.n("clLit", "CallFunction", 300, 520, function_name="MakeLiteralString", function_class=KSL)
    d.pv("clLit", "Value", "CL")
    d.n("selStream", "Select", 550, 350)
    d.c("auth", "ReturnValue", "selStream", "Index")
    d.c("clLit", "ReturnValue", "selStream", "Option 0")
    d.c("svLit", "ReturnValue", "selStream", "Option 1")
    d.n("setStream", "VariableSet", 800, 0, variable_name="FuzzStream")
    d.c("selStream", "ReturnValue", "setStream", "FuzzStream")
    d.c(bpg, "then", "setStream", "exec")
    return "setStream"



def build_child():
    r = ok(claireon.bp_create(parent_class="Actor", asset_path=CHILD), "create child")
    s = r["data"]["session_id"]
    ok(claireon.bp_set_property(session_id=s, property_name="bReplicates",
                               property_value="true"), "bReplicates")
    ok(claireon.bp_add_component(session_id=s, component_name="ChildRoot",
                                 component_class="SceneComponent"), "root")
    fuzztrace.add_trace_vars(s, CHILD, "SV", "RepChild")
    ok(claireon.bp_add_variable(session_id=s, variable_name="SpawnStamp",
        variable_type="int", default_value="0", category="Setup",
        instance_editable=True, replication="Replicated"), "SpawnStamp")
    ok(claireon.bp_set_variable_properties(session_id=s, variable_name="SpawnStamp",
        flags=["ExposeOnSpawn"], category="Setup"), "expose")
    fuzztrace.add_trace_functions(s, CHILD)
    claireon.bp_switch_graph(session_id=s, graph_name="EventGraph")
    bpg = _recover_beginplay(s)
    d = Delta(s)
    ss = _stream_head(d, bpg)
    d.n("log", "CallFunction", 1100, 0, function_name="FuzzLog")
    d.pv("log", "Tag", "ChildBegin")
    d.n("gss", "VariableGet", 950, 200, variable_name="SpawnStamp")
    d.c("gss", "SpawnStamp", "log", "Value")
    d.c(ss, "then", "log", "exec")
    d.n("done", "CallFunction", 1500, 0, function_name="FuzzDone")
    d.c("log", "then", "done", "exec")
    d.apply("child beginplay")
    ok(claireon.bp_compile(session_id=s), "compile child")
    ok(claireon.bp_save(session_id=s), "save child")
    claireon.bp_close(session_id=s, suppress_output=True)
    print("build_child OK")



R = {"sid": None, "g": {}}


def _rsid():
    if not R["sid"]:
        R["sid"] = claireon.bp_open(asset_path=RELAY)["data"]["session_id"]
    return R["sid"]


def _radd(name, **kw):
    import re
    if name in R["g"]:
        return R["g"][name]
    try:
        r = ok(claireon.bp_add_node(session_id=_rsid(), **kw), "add %s" % name)
    except RuntimeError as e:
        m = re.search(r"already exists \(node GUID: ([0-9A-Fa-f]+)\)", str(e))
        if m:
            R["g"][name] = m.group(1)
            return m.group(1)
        raise
    R["g"][name] = node_guid(r)
    return R["g"][name]


def relay_create():
    r = ok(claireon.bp_create(parent_class="Actor", asset_path=RELAY), "create relay")
    R["sid"] = r["data"]["session_id"]
    s = R["sid"]
    ok(claireon.bp_set_property(session_id=s, property_name="bReplicates",
                               property_value="true"), "bReplicates")
    ok(claireon.bp_add_component(session_id=s, component_name="RelayRoot",
                                 component_class="SceneComponent"), "root")
    fuzztrace.add_trace_vars(s, RELAY, "SV", "RepRelay")
    ok(claireon.bp_add_variable(session_id=s, variable_name="RepCounter", variable_type="int",
        default_value="0", category="Replicated", replication="RepNotify",
        rep_notify_func="OnRep_RepCounter"), "RepCounter")
    ok(claireon.bp_add_variable(session_id=s, variable_name="RepFlags", variable_type="int",
        default_value="0", category="Replicated", replication="RepNotify",
        rep_notify_func="OnRep_RepFlags"), "RepFlags")
    ok(claireon.bp_add_variable(session_id=s, variable_name="RepList", variable_type="int",
        container_type="array", category="Replicated", replication="Replicated"), "RepList")
    ok(claireon.bp_add_variable(session_id=s, variable_name="PlainRep", variable_type="int",
        default_value="0", category="Replicated", replication="Replicated"), "PlainRep")
    ok(claireon.bp_add_variable(session_id=s, variable_name="ChildRef", variable_type="Actor",
        category="Transient|Handles"), "ChildRef")
    for name in ["RepCounter", "RepFlags", "RepList", "PlainRep"]:
        ok(claireon.bp_set_variable_properties(session_id=s, variable_name=name,
            category="Replicated", clear_flags=["EditAnywhere"]), "cat %s" % name)
    ok(claireon.bp_set_variable_properties(session_id=s, variable_name="ChildRef",
        category="Transient|Handles", flags=["Transient"], clear_flags=["EditAnywhere"]),
       "cat ChildRef")
    fuzztrace.add_trace_functions(s, RELAY)
    ok(claireon.bp_compile(session_id=s), "compile")
    ok(claireon.bp_save(session_id=s), "save")
    print("relay_create OK")


def relay_events():
    """RPC + OnRep custom events (typed nodes bp_add_node cannot batch)."""
    s = _rsid()
    claireon.bp_switch_graph(session_id=s, graph_name="EventGraph")
    _radd("beginplay", node_type="EventOverride", function_name="ReceiveBeginPlay",
          position_x=0, position_y=0)
    _radd("serverSeq", node_type="CustomEvent", event_name="RunServerSeq",
          position_x=0, position_y=1200)
    _radd("serverReport", node_type="CustomEvent", event_name="ServerReport",
          user_defined_pins=[{"name": "Code", "type": "int"}],
          position_x=0, position_y=5200)
    _radd("multiPing", node_type="CustomEvent", event_name="MulticastPing",
          user_defined_pins=[{"name": "Code", "type": "int"}],
          position_x=0, position_y=5800)
    _radd("multiFlash", node_type="CustomEvent", event_name="MulticastFlash",
          user_defined_pins=[{"name": "Code", "type": "int"}],
          position_x=0, position_y=6400)
    ok(claireon.bp_compile(session_id=s), "compile")
    ok(claireon.bp_save(session_id=s), "save")
    print("relay_events OK; guids:", list(R["g"].keys()))
    import json
    json.dump(R["g"], open(fuzzlib.guids_path("relay"), "w"), indent=1)


def relay_rpc_flags():
    """Set the RPC replication mode on the three custom events. A custom event's
    replication is its K2Node_CustomEvent.FunctionFlags bitmask, NOT settable via
    bp_set_function_properties (which owns only function graphs). OR the net bits
    into the clean custom-event base."""
    s = _rsid()
    G = R["g"]
    BASE = 201457664  # BlueprintCallable|BlueprintEvent|Public|Net (fresh custom event)
    FUNC_Net = 0x40
    FUNC_NetReliable = 0x80
    FUNC_NetMulticast = 0x4000
    FUNC_NetServer = 0x200000
    plan = {
        "serverReport": BASE | FUNC_Net | FUNC_NetServer | FUNC_NetReliable,
        "multiPing": BASE | FUNC_Net | FUNC_NetMulticast | FUNC_NetReliable,
        "multiFlash": BASE | FUNC_Net | FUNC_NetMulticast,  # unreliable
    }
    for k, v in plan.items():
        ok(claireon.bp_set_node_property(session_id=s, node_guid=G[k],
            property_name="FunctionFlags", property_value=str(v)), "flags %s" % k)
    ok(claireon.bp_compile(session_id=s), "compile")
    ok(claireon.bp_save(session_id=s), "save")
    print("relay_rpc_flags OK")


def load_relay_guids():
    import json, os
    p = fuzzlib.guids_path("relay")
    if os.path.exists(p):
        R["g"].update(json.load(open(p)))
    return R["g"]


def _simple_log_event(s, graph, event_guid, tag, code_pin="Code"):
    """Wire an event's `then` to FuzzLog(tag, <event.code_pin>), reading the code
    directly from the event node's output pin."""
    claireon.bp_switch_graph(session_id=s, graph_name=graph)
    d = Delta(s)
    d.n("log", "CallFunction", 400, 0, function_name="FuzzLog")
    d.pv("log", "Tag", tag)
    if code_pin:
        d.c(event_guid, code_pin, "log", "Value")
    d.c(event_guid, "then", "log", "exec")
    d.apply("%s body" % tag)


def relay_handlers():
    """OnRep handler graphs + RPC event bodies (all simple logs)."""
    s = _rsid()
    G = load_relay_guids()
    for graph, tag, var in [("OnRep_RepCounter", "RepNotifyCounter", "RepCounter"),
                            ("OnRep_RepFlags", "RepNotifyFlags", "RepFlags")]:
        claireon.bp_switch_graph(session_id=s, graph_name=graph)
        entry = _find(RELAY, graph, cls="FunctionEntry")
        d = Delta(s)
        d.n("log", "CallFunction", 400, 0, function_name="FuzzLog")
        d.pv("log", "Tag", tag)
        d.n("gv", "VariableGet", 250, 250, variable_name=var)
        d.c("gv", var, "log", "Value")
        d.c(entry, "then", "log", "exec")
        d.apply("%s body" % graph)
    # RPC event bodies (read the event's own Code pin)
    _simple_log_event(s, "EventGraph", G["serverReport"], "ServerRPC")
    _simple_log_event(s, "EventGraph", G["multiPing"], "Multicast")
    _simple_log_event(s, "EventGraph", G["multiFlash"], "Flash")
    ok(claireon.bp_compile(session_id=s), "compile")
    ok(claireon.bp_save(session_id=s), "save")
    print("relay_handlers OK")


def relay_beginplay():
    """BeginPlay: stream head, log Begin(authority), SwitchHasAuthority -> server
    branch delays then RunServerSeq."""
    s = _rsid()
    G = load_relay_guids()
    claireon.bp_switch_graph(session_id=s, graph_name="EventGraph")
    bpg = G["beginplay"]
    d = Delta(s)
    ss = _stream_head(d, bpg)
    d.n("gauth", "CallFunction", 1000, 250, function_name="HasAuthority", function_class="Actor")
    d.n("authInt", "CallFunction", 1150, 250, function_name="Conv_BoolToInt", function_class=KML)
    d.c("gauth", "ReturnValue", "authInt", "InBool")
    d.n("logB", "CallFunction", 1300, 0, function_name="FuzzLog")
    d.pv("logB", "Tag", "Begin")
    d.c("authInt", "ReturnValue", "logB", "Value")
    d.c(ss, "then", "logB", "exec")
    d.n("sw", "SwitchHasAuthority", 1700, 0)
    d.c("logB", "then", "sw", "execute")
    d.n("delay", "CallFunction", 2100, 0, function_name="Delay", function_class=KSL)
    d.pv("delay", "Duration", "2.0")
    d.c("sw", "Authority", "delay", "exec")
    # RunServerSeq is a custom event (entry point); invoke it with a call node.
    d.n("callSeq", "CallFunction", 2500, 0, function_name="RunServerSeq")
    d.c("delay", "then", "callSeq", "exec")
    d.apply("relay beginplay")
    ok(claireon.bp_compile(session_id=s), "compile")
    ok(claireon.bp_save(session_id=s), "save")
    print("relay_beginplay OK")


def relay_server_seq():
    """RunServerSeq (server-only): drive the replicated state + RPCs in a strictly
    ordered timed sequence so the SV trace is deterministic."""
    s = _rsid()
    G = load_relay_guids()
    claireon.bp_switch_graph(session_id=s, graph_name="EventGraph")
    d = Delta(s)
    # step 1: RepCounter = 5  (client OnRep_RepCounter logs 5)
    d.n("setC", "VariableSet", 400, 1200, variable_name="RepCounter")
    d.pv("setC", "RepCounter", 5)
    d.c(G["serverSeq"], "then", "setC", "exec")
    d.n("d1", "CallFunction", 700, 1200, function_name="Delay", function_class=KSL)
    d.pv("d1", "Duration", "0.6")
    d.c("setC", "then", "d1", "exec")
    # step 2: MulticastPing(11) -> SV + CL log Multicast 11.
    # RPC events are invoked via a call node (Code is an input there), not by
    # wiring into the event definition node.
    d.n("callPing", "CallFunction", 900, 1200, function_name="MulticastPing")
    d.pv("callPing", "Code", 11)
    d.c("d1", "then", "callPing", "exec")
    d.n("d2", "CallFunction", 1100, 1200, function_name="Delay", function_class=KSL)
    d.pv("d2", "Duration", "0.6")
    d.c("callPing", "then", "d2", "exec")
    # step 3: RepFlags = 7  (client OnRep_RepFlags logs 7)
    d.n("setF", "VariableSet", 1500, 1200, variable_name="RepFlags")
    d.pv("setF", "RepFlags", 7)
    d.c("d2", "then", "setF", "exec")
    d.n("d3", "CallFunction", 1800, 1200, function_name="Delay", function_class=KSL)
    d.pv("d3", "Duration", "0.6")
    d.c("setF", "then", "d3", "exec")
    # step 4: PlainRep=3, RepList add 3, MulticastFlash(22)
    d.n("setP", "VariableSet", 2200, 1200, variable_name="PlainRep")
    d.pv("setP", "PlainRep", 3)
    d.c("d3", "then", "setP", "exec")
    d.n("addL", "CallArrayFunction", 2500, 1400, function_name="Array_Add",
        function_class="KismetArrayLibrary")
    d.n("litItem", "CallFunction", 2350, 1600, function_name="MakeLiteralInt", function_class=KSL)
    d.pv("litItem", "Value", 3)
    d.c("litItem", "ReturnValue", "addL", "NewItem")  # wildcard pin: wire, don't default
    d.n("gL", "VariableGet", 2350, 1450, variable_name="RepList")
    d.c("gL", "RepList", "addL", "TargetArray")
    d.c("setP", "then", "addL", "exec")
    d.n("callFlash", "CallFunction", 2900, 1200, function_name="MulticastFlash")
    d.pv("callFlash", "Code", 22)
    d.c("addL", "then", "callFlash", "exec")
    d.n("d4", "CallFunction", 3100, 1200, function_name="Delay", function_class=KSL)
    d.pv("d4", "Duration", "0.6")
    d.c("callFlash", "then", "d4", "exec")
    # step 5: ServerReport(33) -- server-invoked server RPC (SV only)
    d.n("callReport", "CallFunction", 3300, 1200, function_name="ServerReport")
    d.pv("callReport", "Code", 33)
    d.c("d4", "then", "callReport", "exec")
    d.n("d5", "CallFunction", 3500, 1200, function_name="Delay", function_class=KSL)
    d.pv("d5", "Duration", "0.6")
    d.c("callReport", "then", "d5", "exec")
    d.apply("relay server seq part1")
    d5_guid = d.guid("d5")  # cross-delta reference needs the resolved GUID
    # step 6: spawn RepChild with SpawnStamp=99, then FuzzDone
    d2 = Delta(s)
    d2.n("spawn", "SpawnActor", 3900, 1200, actor_class=CHILD_CLASS)
    d2.pv("spawn", "SpawnStamp", 99)
    d2.n("xf", "CallFunction", 3750, 1450, function_name="MakeTransform", function_class=KML)
    d2.c("xf", "ReturnValue", "spawn", "SpawnTransform")
    d2.c(d5_guid, "then", "spawn", "exec")
    d2.n("setCR", "VariableSet", 4300, 1200, variable_name="ChildRef")
    d2.c("spawn", "ReturnValue", "setCR", "ChildRef")
    d2.c("spawn", "then", "setCR", "exec")
    d2.n("d6", "CallFunction", 4600, 1200, function_name="Delay", function_class=KSL)
    d2.pv("d6", "Duration", "0.6")
    d2.c("setCR", "then", "d6", "exec")
    d2.n("done", "CallFunction", 5000, 1200, function_name="FuzzDone")
    d2.c("d6", "then", "done", "exec")
    d2.apply("relay server seq part2")
    ok(claireon.bp_compile(session_id=s), "compile")
    ok(claireon.bp_save(session_id=s), "save")
    print("relay_server_seq OK")


def build_relay():
    """Full RepRelay build in the ONLY order that compiles: create + events first,
    then handlers/beginplay/server-seq while the RPC custom events are still PLAIN
    (a CallFunction cannot bind a net-flagged custom event by name), and only then
    apply the net FunctionFlags. bp_create accepts only /Game/ paths, so build at
    /Game and rename to /Claireon afterwards (see README 'Regenerating')."""
    relay_create()
    relay_events()
    load_relay_guids()
    relay_handlers()
    relay_beginplay()
    relay_server_seq()
    relay_rpc_flags()
    print("build_relay OK -- now rename %s to the /Claireon mount" % RELAY)
