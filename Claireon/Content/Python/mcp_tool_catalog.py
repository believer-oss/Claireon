"""Fuzzy tool catalog backed by the IndexEngine (FTS5 + semantic search)."""
# Copyright (c) 2026 The Claireon Contributors
# SPDX-License-Identifier: MIT

import json
import logging
from dataclasses import dataclass
from typing import Optional

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
}

# Build reverse map: canonical term -> set of abbreviations that expand to it
_REVERSE_MAP: dict = {}
for _abbr, _expansions in _ABBREVIATIONS.items():
    for _term in _expansions.split():
        _REVERSE_MAP.setdefault(_term, set()).add(_abbr)


@dataclass
class _ToolChunk:
    """Lightweight chunk for the tool catalog (matches IndexEngine expectations)."""
    text: str
    chunk_type: str = "tool"
    source_tool: str = "tool_catalog"
    metadata: Optional[dict] = None


# ---------------------------------------------------------------------------
# Catalog state
# ---------------------------------------------------------------------------

_CATALOG_INDEX_ID = "__tool_catalog__"
_catalog_version: int = 0  # bumped on each rebuild


def _enrich_text(name: str, description: str, category: str) -> str:
    """Build a rich searchable document for a single tool.

    Includes the tool name (with dots replaced by spaces), description,
    category, and all matching synonym expansions so that abbreviated
    queries like "bp" or "dt" can match the right tools.
    """
    # Tokenize the name: "edit_blueprint_graph" -> "edit blueprint graph"
    name_tokens = name.replace(".", " ").replace("_", " ")

    parts = [name, name_tokens, category, description]

    # Collect all words from the tool's text
    all_text_lower = " ".join(parts).lower()

    # Add abbreviation synonyms: if tool text contains "blueprint",
    # also add "bp" so searching "bp" finds it via FTS5
    added = set()
    for term, abbrevs in _REVERSE_MAP.items():
        if term in all_text_lower:
            for abbr in abbrevs:
                if abbr not in added:
                    parts.append(abbr)
                    added.add(abbr)

    # Also add forward expansions for any abbreviations already in the name
    for token in all_text_lower.split():
        if token in _ABBREVIATIONS and token not in added:
            parts.append(_ABBREVIATIONS[token])
            added.add(token)

    return " ".join(parts)


def rebuild(tools_json: str) -> dict:
    """Rebuild the tool catalog index from a JSON array of tool metadata.

    Expected format: [{"name": "...", "description": "...", "category": "..."}, ...]

    Returns dict with index creation stats.
    """
    global _catalog_version

    from mcp_index_engine import get_engine

    engine = get_engine()

    # Clear the old catalog index
    engine.clear(index_id=_CATALOG_INDEX_ID)

    tools = json.loads(tools_json)

    chunks = []
    for tool in tools:
        name = tool.get("name", "")
        description = tool.get("description", "")
        category = tool.get("category", "uncategorized")

        enriched = _enrich_text(name, description, category)
        chunks.append(_ToolChunk(
            text=enriched,
            chunk_type="tool",
            source_tool="tool_catalog",
            metadata={"tool_name": name, "category": category},
        ))

    result = engine.create_index(_CATALOG_INDEX_ID, chunks)
    _catalog_version += 1
    result["catalog_version"] = _catalog_version
    result["tools_indexed"] = len(tools)

    logger.info(
        "Tool catalog rebuilt: %d tools indexed (v%d), %d chunks inserted, %d reused.",
        len(tools), _catalog_version, result.get("chunks_inserted", 0), result.get("chunks_reused", 0),
    )

    return result


def _expand_query(query: str) -> str:
    """Expand query tokens with synonym/abbreviation mappings.

    "bp asset load" -> "bp OR blueprint asset load OR open OR read OR import"

    Uses FTS5 OR syntax so any expanded term can match.
    """
    tokens = query.lower().split()
    expanded_parts = []

    for token in tokens:
        if token in _ABBREVIATIONS:
            # Add both the abbreviation and its expansions with OR
            expansions = _ABBREVIATIONS[token].split()
            or_group = [token] + expansions
            # FTS5 OR groups: (term1 OR term2 OR term3)
            expanded_parts.append("(" + " OR ".join(or_group) + ")")
        else:
            # Check if this token has reverse mappings (abbreviations)
            abbrevs = _REVERSE_MAP.get(token, set())
            if abbrevs:
                or_group = [token] + list(abbrevs)
                expanded_parts.append("(" + " OR ".join(or_group) + ")")
            else:
                expanded_parts.append(token)

    return " ".join(expanded_parts)


def search(query: str, max_results: int = 20, method: str = "hybrid") -> list:
    """Search the tool catalog with fuzzy matching.

    Returns a list of dicts: [{"tool_name": "...", "category": "...", "score": ...}, ...]
    sorted by relevance (best first).
    """
    from mcp_index_engine import get_engine

    engine = get_engine()

    # Check if catalog exists
    info = engine.get_index_info(_CATALOG_INDEX_ID)
    if "error" in info:
        return []

    # For keyword/hybrid search, expand the query with synonyms
    expanded = _expand_query(query)

    # Use the IndexEngine's search with the expanded query
    # For semantic search, pass the original query (embeddings work better
    # with natural language than with FTS5 syntax)
    if method == "semantic":
        hits = engine.search(_CATALOG_INDEX_ID, query, max_results=max_results, method="semantic")
    elif method == "keyword":
        hits = engine.search(_CATALOG_INDEX_ID, expanded, max_results=max_results, method="keyword")
    else:
        # Hybrid: run keyword with expanded query, semantic with original
        kw_hits = engine._keyword_search(_CATALOG_INDEX_ID, expanded, max_results)
        sem_hits = engine._semantic_search(_CATALOG_INDEX_ID, query, max_results)

        if sem_hits:
            from mcp_index_engine import _rrf_fuse
            hits = _rrf_fuse(kw_hits, sem_hits)[:max_results]
        else:
            hits = kw_hits

    # Extract tool metadata from results
    results = []
    seen = set()
    for hit in hits:
        meta = hit.get("metadata")
        if meta and isinstance(meta, dict):
            tool_name = meta.get("tool_name", "")
            if tool_name and tool_name not in seen:
                seen.add(tool_name)
                results.append({
                    "tool_name": tool_name,
                    "category": meta.get("category", ""),
                    "score": hit.get("rrf_score", hit.get("score", 0)),
                })

    return results
