# fuzzlib.py -- shared helpers for authoring the Claireon fuzz-baseline Blueprints.
# Runs INSIDE the Unreal Editor's embedded Python (claireon module available).
#
# These fixtures are deliberately feature-dense Blueprints with deterministic,
# log-observable runtime behavior. See README.md in this directory.

import claireon
import json
import os
import sys


class FuzzError(RuntimeError):
    pass


def script_dir():
    """The directory holding these scripts. Uses __file__ when the module was
    imported normally, and otherwise the sys.path entry the caller added in order
    to import it."""
    if "__file__" in globals():
        return os.path.dirname(os.path.abspath(__file__))
    for entry in sys.path:
        if entry and os.path.isfile(os.path.join(entry, "fuzzlib.py")):
            return os.path.abspath(entry)
    raise RuntimeError(
        "cannot locate the FuzzBaseline script directory; add it to sys.path "
        "before importing fuzzlib")


def guids_path(fixture):
    """Script-relative path of a fixture's authored-node GUID sidecar."""
    return os.path.join(script_dir(), "%s_guids.json" % fixture)


def ok(result, what=""):
    """Validate a claireon tool result; raise with context on failure."""
    if result is None:
        raise FuzzError("%s: tool returned None" % what)
    if isinstance(result, dict):
        err = result.get("error")
        if err:
            raise FuzzError("%s: %s" % (what, err))
        for w in result.get("warnings") or []:
            print("  [warn] %s: %s" % (what, w))
    return result


def node_guid(result):
    """Extract the created node GUID from a bp_add_node result."""
    d = result.get("data") or {}
    for key in ("node_guid", "guid", "node_id"):
        if d.get(key):
            return d[key]
    # some results nest under 'node'
    n = d.get("node") or {}
    for key in ("node_guid", "guid", "node_id"):
        if n.get(key):
            return n[key]
    # bp_add_node reports the new node only in the summary text
    import re
    m = re.search(r"\[GUID: ([0-9A-Fa-f]{32})\]", result.get("summary") or "")
    if m:
        return m.group(1)
    raise FuzzError("no node guid in result: %s" % json.dumps(d)[:400])


class Delta:
    """Accumulates a bp_apply_delta batch (nodes/pin_defaults/connections)
    with simple rail-based auto-layout.

    Exec chains ride a horizontal rail (x step per link); data providers are
    placed up/left of their consumer. Islands are stacked by the caller via
    the y origin. bp_format runs at the end of authoring anyway; this layout
    just keeps nodes un-stacked so intermediate saves are inspectable."""

    XSTEP = 420
    YSTEP = 130

    def __init__(self, session_id, y=0, x=0):
        self.session_id = session_id
        self.nodes = []
        self.pin_defaults = []
        self.connections = []
        self.x = x
        self.y = y
        self._data_slot = {}

    # ---- node placement ----
    def n(self, nid, node_type, x=None, y=None, **kw):
        """Place a node at an explicit position."""
        d = {"id": nid, "node_type": node_type,
             "position": {"x": self.x if x is None else x,
                          "y": self.y if y is None else y}}
        d.update({k: v for k, v in kw.items() if v is not None})
        self.nodes.append(d)
        return nid

    def chain(self, nid, node_type, **kw):
        """Place the next node on the exec rail and advance the cursor."""
        self.x += self.XSTEP
        return self.n(nid, node_type, x=self.x, y=self.y, **kw)

    def data(self, nid, node_type, consumer_id, **kw):
        """Place a data provider below-left of a consumer node."""
        cx, cy = self._pos_of(consumer_id)
        slot = self._data_slot.get(consumer_id, 0)
        self._data_slot[consumer_id] = slot + 1
        return self.n(nid, node_type, x=cx - 340, y=cy + 180 + slot * self.YSTEP, **kw)

    def _pos_of(self, nid):
        for d in self.nodes:
            if d["id"] == nid:
                return d["position"]["x"], d["position"]["y"]
        return self.x, self.y

    # ---- values and wires ----
    def pv(self, node, pin, value):
        self.pin_defaults.append({"node": node, "pin": pin, "value": str(value)})

    def c(self, frm, from_pin, to, to_pin):
        # bp_apply_delta pin resolution is exact: the exec-in pin is 'execute'.
        if to_pin == "exec":
            to_pin = "execute"
        self.connections.append(
            {"from": frm, "from_pin": from_pin, "to": to, "to_pin": to_pin})

    def cc(self, frm, from_pin, to, to_pin):
        """Chain-connect: wire and remember 'to' as the new rail tail."""
        self.c(frm, from_pin, to, to_pin)

    # ---- common composite patterns ----
    _uniq = [0]

    def _u(self, prefix):
        self._uniq[0] += 1
        return "%s_%d" % (prefix, self._uniq[0])

    def call_self(self, nid, function_name, **kw):
        """CallFunction on self (member function / custom event of this BP)."""
        return self.chain(nid, "CallFunction", function_name=function_name, **kw)

    def fuzz_log(self, prev, prev_pin, tag, value_literal=None):
        """FuzzLog(tag, literal value) call appended to the exec rail.
        Returns the call node id (exec continues from its 'then')."""
        nid = self._u("log")
        self.call_self(nid, "FuzzLog")
        self.pv(nid, "Tag", tag)
        if value_literal is not None:
            self.pv(nid, "Value", int(value_literal))
        if prev is not None:
            self.c(prev, prev_pin, nid, "exec")
        return nid

    def apply(self, what="delta", prefer_local_gets=True):
        kwargs = {"session_id": self.session_id,
                  "prefer_local_gets": prefer_local_gets}
        if self.nodes:
            kwargs["nodes"] = self.nodes
        if self.pin_defaults:
            kwargs["pin_defaults"] = self.pin_defaults
        if self.connections:
            kwargs["connections"] = self.connections
        r = claireon.bp_apply_delta(**kwargs)
        ok(r, what)
        self.id_map = (r.get("data") or {}).get("id_map") or {}
        print("  applied %s: %d nodes, %d defaults, %d wires" %
              (what, len(self.nodes), len(self.pin_defaults), len(self.connections)))
        return r

    def guid(self, local_id):
        """Resolve a spec-local node id to its created GUID (after apply)."""
        return getattr(self, "id_map", {}).get(local_id)


def switch_graph(session_id, graph_name):
    return ok(claireon.bp_switch_graph(session_id=session_id, graph_name=graph_name),
              "switch_graph(%s)" % graph_name)


def compile_save(session_id, what=""):
    r = ok(claireon.bp_compile(session_id=session_id), "compile %s" % what)
    ok(claireon.bp_save(session_id=session_id), "save %s" % what)
    return r
