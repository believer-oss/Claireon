"""Thin shim around the C++ tool catalog matcher.

Both abbreviation/synonym expansion and field tokenisation now live in C++
(``ClaireonToolCatalogAbbreviations.h`` + ``ClaireonToolCatalogMatcher.cpp``).
This module just forwards raw fields to ``claireon._tool_catalog_build`` and
raw queries to ``claireon._tool_catalog_nearest``; nothing here is in the
search path beyond format conversion.
"""
# Copyright (c) 2026 The Claireon Contributors
# SPDX-License-Identifier: MIT

import json
import logging

logger = logging.getLogger(__name__)

_catalog_version: int = 0  # bumped on each rebuild
_catalog_tool_count: int = 0


def _get_binding(name: str):
    """Return the named C++ PyCFunction binding (registered onto the ``unreal`` module).

    FClaireonBridge::RegisterBridgeFunctions installs ``_tool_catalog_build``
    and ``_tool_catalog_nearest`` directly on the ``unreal`` module dict.
    The ``claireon._tool_catalog_*`` aliases in the REPL prefix are a
    convenience for user code; this harness goes straight to the source
    so it works from any Python execution frame.
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

    The C++ matcher owns tokenisation, field-weighting, and abbreviation
    expansion; this function only forwards the raw payload.
    """
    global _catalog_version, _catalog_tool_count

    build_fn = _get_binding("_tool_catalog_build")
    if build_fn is None:
        logger.warning("Tool catalog rebuild: unreal._tool_catalog_build binding not available")
        return {"tools_indexed": 0, "catalog_version": _catalog_version}

    tools = json.loads(tools_json)

    # Forward raw fields as-is. The C++ binding reads name/description/
    # category/operation as strings and keywords as a JSON array.
    build_fn(json.dumps(tools))

    _catalog_version += 1
    _catalog_tool_count = len(tools)

    logger.info(
        "Tool catalog rebuilt: %d tools indexed (v%d).",
        len(tools), _catalog_version,
    )

    return {"tools_indexed": len(tools), "catalog_version": _catalog_version}


def search(query: str, max_results: int = 20, method: str = "hybrid") -> list:
    """Search the tool catalog with fuzzy matching.

    Returns a list of dicts: ``[{"tool_name": "...", "category": "...",
    "score": ..., "tokens_matched": ...}, ...]`` sorted by relevance.

    The ``method`` parameter is accepted for backward compatibility with the
    old hybrid matcher but is ignored: the C++ matcher always runs the same
    BM25-lite weighted nearest-string algorithm with built-in abbreviation
    expansion (forward + reverse maps via ClaireonToolCatalogAbbreviations.h).
    """
    del method  # no longer a knob -- single-algorithm matcher

    nearest_fn = _get_binding("_tool_catalog_nearest")
    if nearest_fn is None:
        return []

    if _catalog_tool_count == 0:
        return []

    raw = nearest_fn(query, int(max_results))

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
