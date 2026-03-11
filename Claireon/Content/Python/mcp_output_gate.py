"""Output Gate: routes execute responses based on size thresholds."""
# Copyright (c) 2026 The Claireon Contributors
# SPDX-License-Identifier: MIT

import json
import uuid
from dataclasses import dataclass, field
from typing import Any, Optional, Union


@dataclass
class DirectResult:
    content: str
    stream_type: str  # "result" or "logs"


@dataclass
class IndexedResult:
    summary: str
    excerpts: list  # top N excerpt dicts
    index_id: str
    chunk_count: int
    stream_type: str


class OutputGate:
    def __init__(self, threshold_bytes: int = 8192, default_top_results: int = 2):
        self.threshold_bytes = threshold_bytes
        self.default_top_results = default_top_results

    def route(
        self,
        data: Any,
        stream_type: str = "result",
        source_tool: str = "execute",
    ) -> Union[DirectResult, IndexedResult]:
        """Route data based on size threshold.

        Returns DirectResult when data is below the byte threshold, or
        IndexedResult when data exceeds the threshold (chunks are stored in
        the IndexEngine so the caller can issue follow-up index_search queries).
        """
        if data is None:
            return DirectResult(content="", stream_type=stream_type)

        if isinstance(data, str):
            serialized = data
        else:
            serialized = json.dumps(data, indent=2)

        size = len(serialized.encode('utf-8'))

        if size < self.threshold_bytes:
            return DirectResult(content=serialized, stream_type=stream_type)

        # Large result — chunk it and store in the index engine
        # Deferred imports to avoid circular dependencies at module load time
        from mcp_index_engine import get_engine
        from mcp_chunkers import get_registry

        engine = get_engine()

        # Select the appropriate chunker via the registry.
        # For log streams the caller passes source_tool="__logs__" as a sentinel.
        registry = get_registry()
        chunker = registry.get(source_tool)

        # For domain-aware chunkers (AssetEntryChunker, BlueprintNodeChunker)
        # pass the original un-serialized data when available, otherwise pass
        # the serialized string so the chunker can JSON-parse it internally.
        if stream_type == "logs" and source_tool != "__logs__":
            # Ensure log data routes through LogBlockChunker
            chunker = registry.get("__logs__")

        chunks = chunker.chunk(serialized, source_tool=source_tool)

        index_id = uuid.uuid4().hex[:8]
        engine.create_index(index_id, chunks)

        # Use the source_tool name as the representative query so we get
        # the most on-topic excerpts back for the summary
        top = engine.search(index_id, source_tool, max_results=self.default_top_results)

        # Extract the summary text from the chunker's dedicated summary chunk.
        # The summary chunk is always inserted first by every chunker implementation,
        # so prefer the first chunk with chunk_type=="summary".  Fall back to a
        # generic description when no summary chunk is present.
        summary_chunks = [c for c in chunks if c.chunk_type == "summary"]
        if summary_chunks:
            summary_text = summary_chunks[0].text
        else:
            content_chunks = [c for c in chunks if c.chunk_type != "summary"]
            summary_text = (
                f"{len(chunks)} chunks indexed from {source_tool} "
                f"({len(content_chunks)} content chunk(s))."
            )

        return IndexedResult(
            summary=summary_text,
            excerpts=top,
            index_id=index_id,
            chunk_count=len(chunks),
            stream_type=stream_type,
        )


# Module-level singleton
_gate = None


def get_gate(threshold_bytes: int = 8192) -> OutputGate:
    global _gate
    if _gate is None:
        _gate = OutputGate(threshold_bytes=threshold_bytes)
    return _gate
