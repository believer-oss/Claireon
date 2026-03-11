"""Chunking strategies for MCP output indexing."""
# Copyright (c) 2026 The Claireon Contributors
# SPDX-License-Identifier: MIT

import json
import re
from dataclasses import dataclass, field
from typing import Any, Optional


@dataclass
class ChunkData:
    text: str
    chunk_type: str  # "summary", "text", "json_array_item", etc.
    source_tool: str
    metadata: Optional[dict] = None


class Chunker:
    """Base interface for all chunkers."""
    def chunk(self, data: str, source_tool: str = "unknown") -> list:
        raise NotImplementedError


class SlidingWindowChunker(Chunker):
    """Default fallback chunker using sliding window with overlap."""

    def __init__(self, window_size: int = 2048, overlap: int = 256):
        self.window_size = window_size
        self.overlap = overlap

    def chunk(self, data: str, source_tool: str = "unknown") -> list:
        chunks = []

        # Generate summary chunk first
        preview_len = min(200, len(data))
        line_count = data.count('\n') + 1
        summary_text = (
            f"{len(data)} chars, {line_count} lines from {source_tool}. "
            f"Preview: {data[:preview_len]}..."
        )
        chunks.append(ChunkData(
            text=summary_text,
            chunk_type="summary",
            source_tool=source_tool,
            metadata={"total_chars": len(data), "total_lines": line_count}
        ))

        # Sliding window content chunks
        step = self.window_size - self.overlap
        if step <= 0:
            step = self.window_size

        pos = 0
        chunk_index = 0
        while pos < len(data):
            end = min(pos + self.window_size, len(data))
            window = data[pos:end]

            chunks.append(ChunkData(
                text=window,
                chunk_type="text",
                source_tool=source_tool,
                metadata={"chunk_index": chunk_index, "start_pos": pos, "end_pos": end}
            ))

            chunk_index += 1
            pos += step

            # Don't create a tiny trailing chunk — stop if remaining is smaller than overlap
            if pos < len(data) and (len(data) - pos) < self.overlap:
                break

        return chunks


class AssetEntryChunker(Chunker):
    """Chunker for find_assets / get_asset_references / validate_assets output.

    Expects either a JSON string representing a list of asset dicts, or a raw
    list.  Produces one chunk per asset entry plus a leading summary chunk with
    the total count and a breakdown by asset class.
    """

    def chunk(self, data, source_tool: str = "unknown") -> list:
        # Accept both a JSON string and a pre-parsed list/dict
        if isinstance(data, str):
            try:
                parsed = json.loads(data)
            except (ValueError, TypeError):
                # Fall back to SlidingWindowChunker for non-JSON input
                return SlidingWindowChunker().chunk(data, source_tool=source_tool)
        else:
            parsed = data

        # Unwrap common envelope shapes: {"assets": [...]} or top-level list
        if isinstance(parsed, dict):
            for key in ("assets", "results", "items"):
                if key in parsed and isinstance(parsed[key], list):
                    parsed = parsed[key]
                    break

        if not isinstance(parsed, list):
            # Not a list — fall back
            return SlidingWindowChunker().chunk(
                json.dumps(parsed) if not isinstance(parsed, str) else parsed,
                source_tool=source_tool,
            )

        assets = parsed
        chunks = []

        # Count by class for the summary
        class_counts: dict = {}
        for asset in assets:
            if isinstance(asset, dict):
                cls = asset.get("class") or asset.get("asset_class") or asset.get("AssetClass") or "Unknown"
            else:
                cls = "Unknown"
            class_counts[cls] = class_counts.get(cls, 0) + 1

        breakdown = ", ".join(f"{cls}={cnt}" for cls, cnt in sorted(class_counts.items()))
        summary_text = (
            f"{len(assets)} assets from {source_tool}. "
            f"Class breakdown: {breakdown}."
        )
        chunks.append(ChunkData(
            text=summary_text,
            chunk_type="summary",
            source_tool=source_tool,
            metadata={"total_assets": len(assets), "class_breakdown": class_counts},
        ))

        # One chunk per asset
        for i, asset in enumerate(assets):
            asset_text = json.dumps(asset, indent=2) if isinstance(asset, dict) else str(asset)
            asset_path = asset.get("asset_path") or asset.get("path") or asset.get("ObjectPath") or "" if isinstance(asset, dict) else ""
            asset_class = asset.get("class") or asset.get("asset_class") or asset.get("AssetClass") or "" if isinstance(asset, dict) else ""
            chunks.append(ChunkData(
                text=asset_text,
                chunk_type="asset",
                source_tool=source_tool,
                metadata={"asset_index": i, "asset_path": asset_path, "asset_class": asset_class},
            ))

        return chunks


class BlueprintNodeChunker(Chunker):
    """Chunker for get_blueprint_graph output.

    Expects a JSON string (or dict) with keys: nodes (list), connections (list),
    blueprint_name (str, optional), graph_name (str, optional).
    Produces one chunk per node plus a summary chunk.
    """

    def chunk(self, data, source_tool: str = "unknown") -> list:
        if isinstance(data, str):
            try:
                parsed = json.loads(data)
            except (ValueError, TypeError):
                return SlidingWindowChunker().chunk(data, source_tool=source_tool)
        else:
            parsed = data

        if not isinstance(parsed, dict):
            return SlidingWindowChunker().chunk(
                json.dumps(parsed) if not isinstance(parsed, str) else parsed,
                source_tool=source_tool,
            )

        nodes = parsed.get("nodes") or []
        connections = parsed.get("connections") or parsed.get("edges") or []
        blueprint_name = parsed.get("blueprint_name") or parsed.get("blueprint") or "Unknown Blueprint"
        graph_name = parsed.get("graph_name") or parsed.get("graph") or "Unknown Graph"

        chunks = []

        summary_text = (
            f"Blueprint '{blueprint_name}', graph '{graph_name}': "
            f"{len(nodes)} nodes, {len(connections)} connections."
        )
        chunks.append(ChunkData(
            text=summary_text,
            chunk_type="summary",
            source_tool=source_tool,
            metadata={
                "blueprint_name": blueprint_name,
                "graph_name": graph_name,
                "node_count": len(nodes),
                "connection_count": len(connections),
            },
        ))

        for node in nodes:
            if not isinstance(node, dict):
                node = {"raw": str(node)}
            node_id = node.get("node_id") or node.get("id") or node.get("NodeGuid") or ""
            node_class = node.get("class") or node.get("node_class") or node.get("NodeClass") or ""
            node_title = node.get("title") or node.get("NodeTitle") or node.get("name") or ""
            node_text = json.dumps(node, indent=2)
            chunks.append(ChunkData(
                text=node_text,
                chunk_type="blueprint_node",
                source_tool=source_tool,
                metadata={
                    "node_id": node_id,
                    "node_class": node_class,
                    "node_title": node_title,
                    "graph": graph_name,
                    "blueprint": blueprint_name,
                },
            ))

        return chunks


class LogBlockChunker(Chunker):
    """Chunker for raw Unreal Engine log text.

    Groups consecutive lines by UE log category/severity pattern,
    e.g. ``LogTemp: Warning: message``.  Falls back to blank-line grouping
    when lines do not match the UE log pattern.
    """

    # Matches: LogCategory: [Warning:|Error:|Display:|Verbose:]? message
    _UE_LOG_RE = re.compile(
        r'^(?P<category>Log\w+):\s*(?P<severity>Warning:|Error:|Display:|Verbose:|Fatal:)?\s*(?P<message>.*)',
        re.IGNORECASE,
    )

    def _parse_line(self, line: str):
        """Return (category, severity, message) or (None, None, line)."""
        m = self._UE_LOG_RE.match(line)
        if m:
            cat = m.group("category")
            sev = (m.group("severity") or "Info").rstrip(":").capitalize()
            msg = m.group("message")
            return cat, sev, msg
        return None, None, line

    def chunk(self, data: str, source_tool: str = "unknown") -> list:
        if not isinstance(data, str):
            data = str(data)

        lines = data.splitlines()
        chunks = []

        # Count severity totals for summary
        severity_counts: dict = {}

        # Group lines
        groups = []  # list of (category, severity, [lines], start_line, end_line)
        current_group = None

        for i, line in enumerate(lines):
            cat, sev, msg = self._parse_line(line)

            if cat is not None:
                key = (cat, sev)
                if current_group and current_group[0] == key:
                    current_group[2].append(line)
                    current_group[4] = i
                else:
                    if current_group:
                        groups.append(current_group)
                    current_group = [key, cat, [line], i, i, sev]
            else:
                # Non-UE log line — group with blank-line boundaries
                if current_group and current_group[0] is None:
                    if line.strip() == "":
                        # Blank line ends current group
                        groups.append(current_group)
                        current_group = None
                    else:
                        current_group[2].append(line)
                        current_group[4] = i
                else:
                    if current_group:
                        groups.append(current_group)
                    if line.strip() != "":
                        current_group = [None, "General", [line], i, i, "Info"]
                    else:
                        current_group = None

        if current_group:
            groups.append(current_group)

        # Build chunk per group
        for group in groups:
            key, cat_name, group_lines, start_line, end_line = group[0], group[1], group[2], group[3], group[4]
            severity = group[5] if len(group) > 5 else "Info"
            group_text = "\n".join(group_lines)
            severity_counts[severity] = severity_counts.get(severity, 0) + len(group_lines)
            chunks.append(ChunkData(
                text=group_text,
                chunk_type="log",
                source_tool=source_tool,
                metadata={
                    "source_category": cat_name,
                    "severity": severity,
                    "line_range": [start_line, end_line],
                },
            ))

        # Summary chunk at the front
        sev_breakdown = ", ".join(f"{s}={c}" for s, c in sorted(severity_counts.items()))
        summary_text = (
            f"{len(lines)} log lines from {source_tool}. "
            f"Severity breakdown: {sev_breakdown}. "
            f"{len(chunks)} groups."
        )
        chunks.insert(0, ChunkData(
            text=summary_text,
            chunk_type="summary",
            source_tool=source_tool,
            metadata={"total_lines": len(lines), "severity_breakdown": severity_counts, "group_count": len(chunks)},
        ))

        return chunks


class ChunkerRegistry:
    """Maps tool names to the appropriate chunker instance.

    Default mappings:
      - find_assets, get_asset_references, validate_assets -> AssetEntryChunker
      - get_blueprint_graph -> BlueprintNodeChunker
      - __logs__ -> LogBlockChunker  (sentinel used by output gate for log streams)
      - everything else -> SlidingWindowChunker
    """

    def __init__(self):
        self._registry: dict = {}
        self._sliding = SlidingWindowChunker()

        # Register built-in defaults
        _asset_chunker = AssetEntryChunker()
        for name in ("find_assets", "get_asset_references", "validate_assets"):
            self._registry[name] = _asset_chunker

        self._registry["get_blueprint_graph"] = BlueprintNodeChunker()
        self._registry["__logs__"] = LogBlockChunker()

    def register(self, tool_name: str, chunker: Chunker) -> None:
        """Register a custom chunker for the given tool name."""
        self._registry[tool_name] = chunker

    def get(self, tool_name: str) -> Chunker:
        """Return the chunker for the given tool_name, or SlidingWindowChunker."""
        return self._registry.get(tool_name, self._sliding)


# Module-level singleton registry
_registry = None


def get_registry() -> ChunkerRegistry:
    global _registry
    if _registry is None:
        _registry = ChunkerRegistry()
    return _registry
