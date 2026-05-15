"""Fuzzy tool catalog backed by the C++ nearest-string matcher.

This harness is a thin shim around two C++ bindings:
    claireon._tool_catalog_build(entries_json: str) -> None
    claireon._tool_catalog_nearest(query: str, max_results: int) -> str (JSON)

It keeps the _ABBREVIATIONS synonym table plus the _build_field_text /
_expand_query helpers so abbreviated queries (``bp``, ``dt``, ``gas``) still
match.  The consumer surface (``rebuild`` and ``search``) is unchanged -- see
ClaireonTool_SearchTools.cpp for the call sites.
"""
# Copyright (c) 2026 The Claireon Contributors
# SPDX-License-Identifier: MIT

import json
import logging

logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Synonym / abbreviation expansion table
#
# Keys are common abbreviations or alternate terms.  Values are space-
# separated expansions that get appended to the tool's searchable text
# when ANY of the value terms appear in the tool's name, description,
# or category.  The reverse mapping (key -> values) is used at query
# time to expand search terms.
# ---------------------------------------------------------------------------

# Forward map: abbreviation -> canonical expansions
_ABBREVIATIONS = {
    "bp":          "blueprint",
    "bt":          "behavior tree behaviortree",
    "st":          "state tree statetree",
    "dt":          "data table datatable",
    "pie":         "play in editor playtest runtime",
    "fx":          "effects niagara particles vfx visual",
    "vfx":         "effects niagara particles visual",
    "eqs":         "environment query system",
    "scs":         "simple construction script components",
    "ai":          "artificial intelligence behavior tree",
    "ui":          "widget user interface umg hud",
    "umg":         "widget user interface",
    "hud":         "widget user interface display",
    "csv":         "comma separated export import datatable",
    "perf":        "performance trace profiling",
    "prof":        "performance trace profiling",
    "hitch":       "performance trace frame spike",
    "compile":     "build compilation",
    "diff":        "compare comparison difference",
    "spawn":       "create instantiate enemy",
    "dmg":         "damage health combat",
    "anim":        "animation",
    "prop":        "property properties",
    "ref":         "reference dependency referencers",
    "cmd":         "command console commandlet",
    "log":         "logging output tail",
    "map":         "level world open",
    "lvl":         "level map world",
    "test":        "automation testing",
    "asset":       "content resource",
    "graph":       "blueprint node visual script",
    "node":        "blueprint graph",
    "pin":         "blueprint connection",
    "var":         "variable property",
    "func":        "function method",
    "comp":        "component",
    "actor":       "entity object level",
    "pawn":        "character player",
    "ability":     "gameplay gas",
    "gas":         "gameplay ability system",
    "redirect":    "redirector fixup",
    "resave":      "save serialize",
    "cook":        "package deploy",
    "screenshot":  "capture image viewport",
    "fly":         "flythrough camera",
    "cam":         "camera flythrough viewport",
    "camera":         "camera camera_asset camera_node rig",
    "cam_asset":      "camera asset rig camera_asset",
    "camera_node":    "camera_node node camera rig",
    "rig":            "camera rig camera_asset",
    "bb":          "blackboard behavior tree keys",
    "niag":        "niagara",
    "prefab":      "prefabrication level instance",
    "row":         "datatable entry record",
    "col":         "column field property datatable",
    "validate":    "check verify integrity",
    "inspect":     "read view examine structure",
    "edit":        "modify change update session",
    "search":      "find query lookup discover",
    "list":        "enumerate show available",
    "get":         "read fetch retrieve",
    "set":         "write update modify",
    "load":        "open read import",
    "save":        "write export persist",
    # Verb synonyms for discoverability (maps common actions to tool keywords)
    "open":        "open map level world load launch",
    "find":        "search discover lookup asset find_assets",
    "create":      "create new add spawn place",
    "delete":      "delete remove destroy",
    "run":         "run execute start launch",
    "stop":        "stop end close terminate kill",
    "screenshot":  "screenshot capture image snap viewport",
    "undo":        "undo transaction revert rollback",
    "redo":        "redo transaction reapply",
    "foliage":     "foliage paint vegetation instanced",
    "landscape":   "landscape terrain heightmap sculpt paint",
    "spline":      "spline path curve control point",
    "pcg":         "procedural content generation graph",
    "niagara":     "niagara vfx particles effect emitter",
    "statetree":   "state tree statetree hierarchical",
    "material":    "material shader expression parameter",
    "widgetbp":    "widget blueprint umg widgetbp user interface",
    "apply_spec":  "apply spec declarative batch idempotent",
    "sequence":       "sequence level_sequence sequencer cinematic keyframe track",
    "level_sequence": "sequence level_sequence sequencer cinematic keyframe track",
    "chooser":     "chooser table result row column context parameter fallback",
    "proxytable":  "proxy table inherit entry lookup",
    "proxyasset":  "proxy asset type result context parameter",
    "context":     "context parameter data input output struct class",
    "inherit":     "inherit parent proxy table chain",
    "fallback":    "fallback default result",
    # Audio cohort synonyms (decomposed audio_edit -> 42 per-op tools)
    "soundcue":    "sound cue node graph audio wave_player mixer",
    "metasound":   "metasound source builder audio document graph node",
    "soundclass":  "sound class hierarchy children audio mix volume pitch",
    "soundmix":    "sound mix adjuster envelope audio fade volume",
    "attenuation": "attenuation falloff audio spatial 3d distance",
    "concurrency": "concurrency limit voice stealing audio resolution",
}

# Build reverse map: canonical term -> set of abbreviations that expand to it
_REVERSE_MAP: dict = {}
for _abbr, _expansions in _ABBREVIATIONS.items():
    for _term in _expansions.split():
        _REVERSE_MAP.setdefault(_term, set()).add(_abbr)


# ---------------------------------------------------------------------------
# Catalog state
# ---------------------------------------------------------------------------

_catalog_version: int = 0  # bumped on each rebuild
_catalog_tool_count: int = 0


def _abbreviations_for(text: str) -> list:
    """Return abbreviation expansion strings matching tokens in ``text``.

        - Reverse-map: when the field text contains a canonical term
          (``blueprint``), append the abbreviations that expand to it
          (``bp``) so a query for ``bp`` still hits this tool.
        - Forward-map: when the field text contains an abbreviation
          (``gas``), append the canonical expansion (``gameplay ability
          system``).
    """
    out = []
    lower = text.lower()
    added = set()
    for term, abbrevs in _REVERSE_MAP.items():
        if term in lower:
            for abbr in abbrevs:
                if abbr not in added:
                    out.append(abbr)
                    added.add(abbr)
    for token in lower.split():
        if token in _ABBREVIATIONS and token not in added:
            out.append(_ABBREVIATIONS[token])
            added.add(token)
    return out


def _build_field_text(field_value: str) -> str:
    """Build a per-field searchable string with abbreviation expansions appended.

    The matcher tokenises this string on whitespace + common punctuation and
    stamps the appropriate EFieldMask bit on each posting.
    """
    if not field_value:
        return ""
    # Tokenise dots/underscores out so multi-part names ("edit_blueprint_graph")
    # contribute their components as separate index tokens.
    tokens_view = field_value.replace(".", " ").replace("_", " ")
    parts = [field_value, tokens_view]
    parts.extend(_abbreviations_for(tokens_view))
    return " ".join(parts)


def _expand_query(query: str) -> str:
    """Expand query tokens with synonym/abbreviation mappings.

    The expanded string is whitespace-joined because the C++ matcher does
    its own tokenisation on whitespace + punctuation; it has no FTS5-style
    boolean grammar to honour.  Example::

        "bp asset load" -> "bp blueprint asset content resource load open read import"
    """
    tokens = query.lower().split()
    expanded_parts = []

    for token in tokens:
        expanded_parts.append(token)
        if token in _ABBREVIATIONS:
            expanded_parts.extend(_ABBREVIATIONS[token].split())
        abbrevs = _REVERSE_MAP.get(token)
        if abbrevs:
            expanded_parts.extend(abbrevs)

    return " ".join(expanded_parts)


def _get_binding(name: str):
    """Return the named C++ PyCFunction binding (registered onto the ``unreal`` module).

    FClaireonBridge::RegisterBridgeFunctions installs ``_tool_catalog_build``
    and ``_tool_catalog_nearest`` directly on the ``unreal`` module dict.
    The ``claireon._tool_catalog_*`` aliases in the REPL prefix are a
    convenience for user code; this harness goes straight to the source
    so it works from any Python execution frame (RebuildCatalog /
    FuzzySearch both run as one-shot PythonCommand blocks with no
    ``claireon`` global).
    """
    try:
        import unreal  # type: ignore
    except ImportError:
        return None
    return getattr(unreal, name, None)


def rebuild(tools_json: str) -> dict:
    """Rebuild the tool catalog from a JSON array of tool metadata.

    Expected format: ``[{"name": "...", "description": "...", "category": "...",
    "operation": "...", "keywords": [...]}, ...]``.

    Returns a dict with index creation stats.
    """
    global _catalog_version, _catalog_tool_count

    build_fn = _get_binding("_tool_catalog_build")
    if build_fn is None:
        # Binding not registered; surface a non-fatal result so the C++
        # caller sees ``tools_indexed`` = 0 and falls back to substring search.
        logger.warning("Tool catalog rebuild: unreal._tool_catalog_build binding not available")
        return {"tools_indexed": 0, "catalog_version": _catalog_version}

    tools = json.loads(tools_json)

    entries = []
    for tool in tools:
        name = tool.get("name", "")
        description = tool.get("description", "")
        category = tool.get("category", "uncategorized")
        operation = tool.get("operation", "")
        keywords = tool.get("keywords", []) or []
        keywords_joined = " ".join(str(k) for k in keywords)

        entries.append({
            "name": name,
            "description": description,
            "category": category,
            "name_text": _build_field_text(name),
            "category_text": _build_field_text(category),
            "keywords_text": _build_field_text(keywords_joined),
            "operation_text": _build_field_text(operation),
            "description_text": _build_field_text(description),
        })

    build_fn(json.dumps(entries))

    _catalog_version += 1
    _catalog_tool_count = len(tools)

    result = {
        "tools_indexed": len(tools),
        "catalog_version": _catalog_version,
    }

    logger.info(
        "Tool catalog rebuilt: %d tools indexed (v%d).",
        len(tools), _catalog_version,
    )

    return result


def search(query: str, max_results: int = 20, method: str = "hybrid") -> list:
    """Search the tool catalog with fuzzy matching.

    Returns a list of dicts: ``[{"tool_name": "...", "category": "...", "score": ...}, ...]``
    sorted by relevance (best first).

    The ``method`` parameter is accepted for backward compatibility with the
    old hybrid matcher but is ignored: the new C++ matcher always runs
    the same BM25-lite nearest-string algorithm.
    """
    del method  # no longer a knob -- single-algorithm matcher

    nearest_fn = _get_binding("_tool_catalog_nearest")
    if nearest_fn is None:
        return []

    if _catalog_tool_count == 0:
        # No rebuild has happened yet; matcher would return empty anyway.
        return []

    expanded = _expand_query(query)
    raw = nearest_fn(expanded, int(max_results))

    try:
        hits = json.loads(raw) if raw else []
    except (TypeError, ValueError):
        hits = []

    results = []
    seen = set()
    for hit in hits:
        if not isinstance(hit, dict):
            continue
        tool_name = hit.get("name", "")
        if not tool_name or tool_name in seen:
            continue
        seen.add(tool_name)
        results.append({
            "tool_name": tool_name,
            "category": hit.get("category", ""),
            "score": hit.get("score", 0.0),
            # Surface distinct query-token-hit count so the C++ caller can flag
            # pathological fuzzy responses and emit a per-tool
            # `query_tokens_matched` diagnostic field.
            "tokens_matched": int(hit.get("tokens_matched", 0)),
        })

    return results
