# fuzztrace.py -- reusable FUZZ trace scaffold shared by every fixture.
# Adds FuzzName/FuzzSeq/FuzzAcc member vars and FuzzLog(Tag,Value)/FuzzDone()
# functions to an open Blueprint session. FuzzLog appends one
#   FUZZ|<FuzzName>|<Stream>|<seq>|<tag>|<value>
# line and folds Value into a checksum accumulator; FuzzDone prints
#   FUZZ|<FuzzName>|DONE|<checksum>.

import claireon
from fuzzlib import Delta, ok

KSL = "KismetSystemLibrary"
KML = "KismetMathLibrary"
KTL = "KismetTextLibrary"


def graph_nodes(asset, graph_name):
    r = ok(claireon.bp_get_graph(asset_path=asset, graph_name=graph_name),
           "get_graph %s" % graph_name)
    gs = r["data"].get("graphs") or []
    return gs[0].get("nodes") or [] if gs else []


def find_node(asset, graph_name, cls=None, title=None):
    for n in graph_nodes(asset, graph_name):
        if cls and cls not in (n.get("node_class") or ""):
            continue
        if title and title != (n.get("node_title") or ""):
            continue
        return n["node_id"]
    raise RuntimeError("node cls=%s title=%s not in %s" % (cls, title, graph_name))


def add_trace_vars(sid, asset, stream_name, fuzz_name):
    """Create trace member variables. Call before add_trace_functions."""
    def var(name, vtype, dv=None, cat="Transient", ie=False):
        kw = {"session_id": sid, "variable_name": name, "variable_type": vtype,
              "category": cat}
        if dv is not None:
            kw["default_value"] = dv
        ok(claireon.bp_add_variable(**kw), "var %s" % name)

    var("FuzzName", "string", fuzz_name, cat="Config")
    var("FuzzStream", "string", stream_name, cat="Config")
    var("FuzzSeq", "int", "0")
    var("FuzzAcc", "int", "0")
    for name in ["FuzzName", "FuzzStream"]:
        ok(claireon.bp_set_variable_properties(session_id=sid, variable_name=name,
            category="Config", clear_flags=["EditAnywhere"]), "prop %s" % name)
    for name in ["FuzzSeq", "FuzzAcc"]:
        ok(claireon.bp_set_variable_properties(session_id=sid, variable_name=name,
            category="Transient", flags=["Transient"], clear_flags=["EditAnywhere"]),
           "prop %s" % name)


def add_trace_functions(sid, asset):
    """Create FuzzLog(Tag,Value) and FuzzDone() function graphs + bodies."""
    ok(claireon.bp_add_function(session_id=sid, function_name="FuzzLog",
        inputs=[{"name": "Tag", "type": "string"}, {"name": "Value", "type": "int"}],
        category="Fuzz|Trace"), "fn FuzzLog")
    ok(claireon.bp_add_function(session_id=sid, function_name="FuzzDone",
        category="Fuzz|Trace"), "fn FuzzDone")
    _body_fuzzlog(sid, asset)
    _body_fuzzdone(sid, asset)


def _body_fuzzlog(sid, asset):
    claireon.bp_switch_graph(session_id=sid, graph_name="FuzzLog")
    entry = find_node(asset, "FuzzLog", cls="FunctionEntry")
    d = Delta(sid)
    d.n("getSeq", "VariableGet", 100, 250, variable_name="FuzzSeq")
    d.n("add1", "CallFunction", 300, 220, function_name="Add_IntInt", function_class=KML)
    d.pv("add1", "B", 1)
    d.n("setSeq", "VariableSet", 520, 0, variable_name="FuzzSeq")
    d.c("getSeq", "FuzzSeq", "add1", "A")
    d.c("add1", "ReturnValue", "setSeq", "FuzzSeq")
    d.c(entry, "then", "setSeq", "exec")
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
    d.n("fmt", "FormatText", 1500, 300, format_text="FUZZ|{Nm}|{St}|{Sq}|{Tg}|{Vl}")
    d.n("gN", "VariableGet", 1300, 480, variable_name="FuzzName")
    d.n("cN", "CallFunction", 1460, 460, function_name="Conv_StringToText", function_class=KTL)
    d.n("gS", "VariableGet", 1300, 560, variable_name="FuzzStream")
    d.n("cS", "CallFunction", 1460, 540, function_name="Conv_StringToText", function_class=KTL)
    d.n("gQ", "VariableGet", 1300, 640, variable_name="FuzzSeq")
    d.n("cQ", "CallFunction", 1460, 620, function_name="Conv_IntToText", function_class=KTL)
    d.n("gT", "VariableGet", 1300, 720, variable_name="Tag", member_scope="FuzzLog")
    d.n("cT", "CallFunction", 1460, 700, function_name="Conv_StringToText", function_class=KTL)
    d.n("gV", "VariableGet", 1300, 800, variable_name="Value", member_scope="FuzzLog")
    d.n("cV", "CallFunction", 1460, 780, function_name="Conv_IntToText", function_class=KTL)
    for src, sp, conv, cp, fp in [("gN", "FuzzName", "cN", "InString", "Nm"),
                                  ("gS", "FuzzStream", "cS", "InString", "St"),
                                  ("gQ", "FuzzSeq", "cQ", "Value", "Sq"),
                                  ("gT", "Tag", "cT", "InString", "Tg"),
                                  ("gV", "Value", "cV", "Value", "Vl")]:
        d.c(src, sp, conv, cp)
        d.c(conv, "ReturnValue", "fmt", fp)
    d.n("toStr", "CallFunction", 1780, 300, function_name="Conv_TextToString", function_class=KTL)
    d.c("fmt", "Result", "toStr", "InText")
    d.n("prn", "CallFunction", 2000, 0, function_name="PrintString", function_class=KSL)
    d.pv("prn", "bPrintToScreen", "false")
    d.pv("prn", "bPrintToLog", "true")
    d.pv("prn", "Duration", "0.0")
    d.c("toStr", "ReturnValue", "prn", "InString")
    d.c("setAcc", "then", "prn", "exec")
    d.apply("FuzzLog body")


def _body_fuzzdone(sid, asset):
    claireon.bp_switch_graph(session_id=sid, graph_name="FuzzDone")
    entry = find_node(asset, "FuzzDone", cls="FunctionEntry")
    d = Delta(sid)
    d.n("fmt", "FormatText", 600, 250, format_text="FUZZ|{Nm}|DONE|{Ac}")
    d.n("gN", "VariableGet", 400, 420, variable_name="FuzzName")
    d.n("cN", "CallFunction", 560, 400, function_name="Conv_StringToText", function_class=KTL)
    d.n("gA", "VariableGet", 400, 540, variable_name="FuzzAcc")
    d.n("cA", "CallFunction", 560, 520, function_name="Conv_IntToText", function_class=KTL)
    d.c("gN", "FuzzName", "cN", "InString")
    d.c("gA", "FuzzAcc", "cA", "Value")
    d.c("cN", "ReturnValue", "fmt", "Nm")
    d.c("cA", "ReturnValue", "fmt", "Ac")
    d.n("toStr", "CallFunction", 860, 250, function_name="Conv_TextToString", function_class=KTL)
    d.n("prn", "CallFunction", 1100, 0, function_name="PrintString", function_class=KSL)
    d.pv("prn", "bPrintToScreen", "false")
    d.pv("prn", "bPrintToLog", "true")
    d.pv("prn", "Duration", "0.0")
    d.c("fmt", "Result", "toStr", "InText")
    d.c("toStr", "ReturnValue", "prn", "InString")
    d.c(entry, "then", "prn", "exec")
    d.apply("FuzzDone body")
