"""In-memory SQLite FTS5 index engine for MCP output indexing."""
# Copyright (c) 2026 The Claireon Contributors
# SPDX-License-Identifier: MIT

import os
import re
import sqlite3
import hashlib
import time
import json
import logging
from typing import Optional

logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Optional dependency guards
# ---------------------------------------------------------------------------
try:
    import model2vec  # noqa: F401  — checked later in _get_model()
    _MODEL2VEC_AVAILABLE = True
except ImportError:
    _MODEL2VEC_AVAILABLE = False


def _find_vec0_dll() -> Optional[str]:
    """Return an absolute path to vec0.dll/vec0.so, or None if not found.

    Search order:
      1. SQLITE_VEC_PATH environment variable (explicit override)
      2. Alongside this Python file (plugin's Content/Python/)
      3. The plugin root (two directories up from Content/Python/)
      4. Common system locations (PATH entries)
    """
    lib_name = "vec0.dll" if os.name == "nt" else "vec0.so"

    candidates: list = []

    # 1. Environment variable override
    env_path = os.environ.get("SQLITE_VEC_PATH")
    if env_path:
        candidates.append(env_path)

    # 2. Next to this file (Content/Python/)
    this_dir = os.path.dirname(os.path.abspath(__file__))
    candidates.append(os.path.join(this_dir, lib_name))

    # 3. Plugin root (Content/Python/ -> Content/ -> plugin root)
    plugin_root = os.path.dirname(os.path.dirname(this_dir))
    candidates.append(os.path.join(plugin_root, lib_name))
    candidates.append(os.path.join(plugin_root, "Binaries", "Win64", lib_name))
    candidates.append(os.path.join(plugin_root, "Binaries", "ThirdParty", "sqlite-vec", "Win64", lib_name))
    candidates.append(os.path.join(plugin_root, "ThirdParty", lib_name))

    # 4. PATH entries
    path_dirs = os.environ.get("PATH", "").split(os.pathsep)
    for d in path_dirs:
        candidates.append(os.path.join(d, lib_name))

    for path in candidates:
        if path and os.path.isfile(path):
            return path

    return None


# ---------------------------------------------------------------------------
# RRF helpers
# ---------------------------------------------------------------------------

def _rrf_fuse(keyword_results: list, semantic_results: list, k: int = 60) -> list:
    """Reciprocal Rank Fusion over two ranked result lists.

    Each result dict must contain at least 'chunk_id'.  Returns a merged list
    sorted by descending RRF score, with an added 'rrf_score' key.
    """
    scores: dict = {}
    # Gather all unique chunk metadata keyed by chunk_id (prefer keyword list)
    meta: dict = {}

    for rank, r in enumerate(keyword_results, 1):
        cid = r["chunk_id"]
        scores[cid] = scores.get(cid, 0.0) + 1.0 / (k + rank)
        if cid not in meta:
            meta[cid] = r

    for rank, r in enumerate(semantic_results, 1):
        cid = r["chunk_id"]
        scores[cid] = scores.get(cid, 0.0) + 1.0 / (k + rank)
        if cid not in meta:
            meta[cid] = r

    sorted_ids = sorted(scores.keys(), key=lambda cid: scores[cid], reverse=True)
    fused = []
    for i, cid in enumerate(sorted_ids):
        entry = dict(meta[cid])
        entry["rrf_score"] = scores[cid]
        entry["rank"] = i + 1
        fused.append(entry)

    return fused


class IndexEngine:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.execute("PRAGMA journal_mode=WAL")
        self._create_schema()
        # {index_id: float} — updated on every search() call
        self.last_accessed: dict = {}

        # Vector / embedding capability flags (set during _init_vec())
        self.has_vec: bool = False
        self.has_embeddings: bool = False
        self._model = None

        self._init_vec()

    # ------------------------------------------------------------------
    # Schema
    # ------------------------------------------------------------------

    def _create_schema(self):
        self.db.executescript("""
            CREATE TABLE IF NOT EXISTS chunks (
                chunk_id      INTEGER PRIMARY KEY,
                chunk_text    TEXT NOT NULL,
                chunk_type    TEXT NOT NULL,
                source_tool   TEXT NOT NULL,
                metadata      TEXT,
                content_hash  TEXT NOT NULL UNIQUE
            );

            CREATE TABLE IF NOT EXISTS index_chunks (
                index_id      TEXT NOT NULL,
                chunk_id      INTEGER NOT NULL REFERENCES chunks(chunk_id),
                created_at    REAL NOT NULL,
                PRIMARY KEY (index_id, chunk_id)
            );

            CREATE VIRTUAL TABLE IF NOT EXISTS chunks_fts USING fts5(
                chunk_text,
                content='chunks',
                content_rowid='chunk_id',
                tokenize='porter unicode61'
            );

            CREATE TRIGGER IF NOT EXISTS chunks_ai AFTER INSERT ON chunks BEGIN
                INSERT INTO chunks_fts(rowid, chunk_text) VALUES (new.chunk_id, new.chunk_text);
            END;

            CREATE TRIGGER IF NOT EXISTS chunks_ad AFTER DELETE ON chunks BEGIN
                INSERT INTO chunks_fts(chunks_fts, rowid, chunk_text) VALUES('delete', old.chunk_id, old.chunk_text);
            END;

            CREATE TRIGGER IF NOT EXISTS chunks_au AFTER UPDATE ON chunks BEGIN
                INSERT INTO chunks_fts(chunks_fts, rowid, chunk_text) VALUES('delete', old.chunk_id, old.chunk_text);
                INSERT INTO chunks_fts(rowid, chunk_text) VALUES (new.chunk_id, new.chunk_text);
            END;
        """)

    # ------------------------------------------------------------------
    # sqlite-vec initialisation
    # ------------------------------------------------------------------

    def _init_vec(self):
        """Try to load the sqlite-vec extension and create the vector table.

        Sets self.has_vec = True on success, False on any failure.
        Also sets self.has_embeddings based on model2vec availability.
        """
        vec_path = _find_vec0_dll()
        if vec_path is None:
            logger.info(
                "sqlite-vec: vec0 shared library not found; "
                "semantic search will fall back to keyword-only."
            )
            self.has_vec = False
        else:
            try:
                self.db.enable_load_extension(True)
                self.db.load_extension(vec_path)
                self.db.enable_load_extension(False)

                # Create the virtual table for 256-dim embeddings
                self.db.execute("""
                    CREATE VIRTUAL TABLE IF NOT EXISTS chunks_vec USING vec0(
                        chunk_id INTEGER PRIMARY KEY,
                        embedding FLOAT[256]
                    )
                """)
                self.db.commit()
                self.has_vec = True
                logger.info("sqlite-vec loaded successfully from: %s", vec_path)
            except Exception as exc:  # noqa: BLE001
                logger.warning(
                    "sqlite-vec: failed to load extension (%s); "
                    "semantic search will fall back to keyword-only.",
                    exc,
                )
                self.has_vec = False

        # Embedding model availability (model2vec)
        if _MODEL2VEC_AVAILABLE:
            self.has_embeddings = True
        else:
            self.has_embeddings = False
            logger.info(
                "model2vec not available; "
                "install it with 'pip install model2vec' to enable semantic search."
            )

    # ------------------------------------------------------------------
    # Embedding helpers
    # ------------------------------------------------------------------

    def _get_model(self):
        """Lazy-load the model2vec StaticModel on first call.

        Returns the model or None if unavailable.
        """
        if self._model is not None:
            return self._model

        if not _MODEL2VEC_AVAILABLE:
            return None

        try:
            from model2vec import StaticModel  # type: ignore
            self._model = StaticModel.from_pretrained("minishlab/potion-base-8M")
            logger.info("model2vec: loaded 'minishlab/potion-base-8M' successfully.")
        except Exception as exc:  # noqa: BLE001
            logger.warning("model2vec: failed to load model (%s).", exc)
            self.has_embeddings = False
            self._model = None

        return self._model

    def embed(self, text: str) -> Optional[list]:
        """Embed a single text string.  Returns a list[float] or None."""
        model = self._get_model()
        if model is None:
            return None
        try:
            vec = model.encode([text])[0]
            return vec.tolist()
        except Exception as exc:  # noqa: BLE001
            logger.warning("embed(): encoding failed (%s).", exc)
            return None

    def embed_batch(self, texts: list) -> Optional[list]:
        """Embed a list of strings.  Returns list[list[float]] or None."""
        if not texts:
            return []
        model = self._get_model()
        if model is None:
            return None
        try:
            vecs = model.encode(texts)
            return [v.tolist() for v in vecs]
        except Exception as exc:  # noqa: BLE001
            logger.warning("embed_batch(): encoding failed (%s).", exc)
            return None

    # ------------------------------------------------------------------
    # TTL helper
    # ------------------------------------------------------------------

    def _auto_expire(self, max_age_seconds: float = 600.0):
        """Internal helper — silently expire old indexes without returning a result.

        Called from create_index() and search() to keep the database tidy.
        """
        now = time.time()
        cutoff = now - max_age_seconds

        old_rows = self.db.execute("""
            SELECT index_id
            FROM index_chunks
            GROUP BY index_id
            HAVING MIN(created_at) < ?
        """, (cutoff,)).fetchall()

        if not old_rows:
            return

        expired_ids = [r[0] for r in old_rows]
        for idx_id in expired_ids:
            self.db.execute("DELETE FROM index_chunks WHERE index_id = ?", (idx_id,))
            self.last_accessed.pop(idx_id, None)

        # Purge orphaned chunks (and their embeddings)
        self._purge_orphan_chunks()
        self.db.commit()

    def _purge_orphan_chunks(self):
        """Delete chunks and their embeddings that are no longer referenced."""
        if self.has_vec:
            # Remove orphaned embeddings first
            try:
                self.db.execute("""
                    DELETE FROM chunks_vec
                    WHERE chunk_id NOT IN (SELECT chunk_id FROM index_chunks)
                """)
            except Exception as exc:  # noqa: BLE001
                logger.warning("_purge_orphan_chunks: vec cleanup failed (%s).", exc)

        self.db.execute("""
            DELETE FROM chunks
            WHERE chunk_id NOT IN (SELECT chunk_id FROM index_chunks)
        """)

    # ------------------------------------------------------------------
    # Index management
    # ------------------------------------------------------------------

    def create_index(self, index_id: str, chunks) -> dict:
        """Insert chunks with dedup, create index_chunks entries, populate FTS5 and vec."""
        # Auto-expire stale indexes before adding new data
        self._auto_expire()

        now = time.time()
        inserted = 0
        reused = 0

        new_chunk_ids: list = []    # chunk_ids that were freshly inserted
        new_chunk_texts: list = []  # corresponding texts for batch embedding

        for chunk in chunks:
            content_hash = hashlib.sha256(chunk.text.encode('utf-8')).hexdigest()

            # Try to find existing chunk with same hash
            row = self.db.execute(
                "SELECT chunk_id FROM chunks WHERE content_hash = ?",
                (content_hash,)
            ).fetchone()

            if row:
                chunk_id = row[0]
                reused += 1
            else:
                cursor = self.db.execute(
                    "INSERT INTO chunks (chunk_text, chunk_type, source_tool, metadata, content_hash) "
                    "VALUES (?, ?, ?, ?, ?)",
                    (
                        chunk.text,
                        chunk.chunk_type,
                        chunk.source_tool,
                        json.dumps(chunk.metadata) if chunk.metadata else None,
                        content_hash,
                    )
                )
                chunk_id = cursor.lastrowid
                inserted += 1
                new_chunk_ids.append(chunk_id)
                new_chunk_texts.append(chunk.text)

            self.db.execute(
                "INSERT OR IGNORE INTO index_chunks (index_id, chunk_id, created_at) VALUES (?, ?, ?)",
                (index_id, chunk_id, now)
            )

        self.db.commit()

        # ------------------------------------------------------------------
        # Populate vector embeddings for newly inserted chunks
        # ------------------------------------------------------------------
        embeddings_added = 0
        if self.has_vec and self.has_embeddings and new_chunk_ids:
            embeddings = self.embed_batch(new_chunk_texts)
            if embeddings is not None:
                for chunk_id, embedding in zip(new_chunk_ids, embeddings):
                    # Skip if an embedding already exists (can happen on reload)
                    existing = self.db.execute(
                        "SELECT 1 FROM chunks_vec WHERE chunk_id = ?", (chunk_id,)
                    ).fetchone()
                    if existing:
                        continue
                    try:
                        import json as _json
                        self.db.execute(
                            "INSERT INTO chunks_vec (chunk_id, embedding) VALUES (?, ?)",
                            (chunk_id, _json.dumps(embedding))
                        )
                        embeddings_added += 1
                    except Exception as exc:  # noqa: BLE001
                        logger.warning(
                            "create_index: failed to insert embedding for chunk_id=%s (%s).",
                            chunk_id, exc,
                        )
                self.db.commit()

        result = {
            "index_id": index_id,
            "chunks_inserted": inserted,
            "chunks_reused": reused,
            "total": inserted + reused,
        }
        if self.has_vec and self.has_embeddings:
            result["embeddings_added"] = embeddings_added

        return result

    # ------------------------------------------------------------------
    # Search
    # ------------------------------------------------------------------

    def _keyword_search(self, index_id: str, query: str, max_results: int) -> list:
        """Internal BM25 / FTS5 keyword search scoped to index_id."""
        if not query or not query.strip():
            rows = self.db.execute("""
                SELECT c.chunk_id, c.chunk_text, c.chunk_type, c.metadata, 0.0 as score
                FROM chunks c
                JOIN index_chunks ic ON c.chunk_id = ic.chunk_id
                WHERE ic.index_id = ?
                ORDER BY c.chunk_id
                LIMIT ?
            """, (index_id, max_results)).fetchall()
        else:
            safe_query = query.replace('"', '""')
            rows = self.db.execute("""
                SELECT c.chunk_id, c.chunk_text, c.chunk_type, c.metadata,
                       rank as score
                FROM chunks_fts
                JOIN chunks c ON chunks_fts.rowid = c.chunk_id
                JOIN index_chunks ic ON c.chunk_id = ic.chunk_id
                WHERE chunks_fts MATCH ? AND ic.index_id = ?
                ORDER BY rank
                LIMIT ?
            """, (safe_query, index_id, max_results)).fetchall()

        results = []
        for i, (chunk_id, text, ctype, metadata, score) in enumerate(rows):
            results.append({
                "chunk_id": chunk_id,
                "chunk_text": text,
                "chunk_type": ctype,
                "metadata": json.loads(metadata) if metadata else None,
                "rank": i + 1,
                "score": score,
            })
        return results

    def _semantic_search(self, index_id: str, query: str, max_results: int) -> list:
        """KNN vector search scoped to index_id.

        Embeds the query, then finds the nearest neighbours from chunks_vec
        that also belong to the specified index via index_chunks.
        """
        if not self.has_vec or not self.has_embeddings:
            return []

        q_vec = self.embed(query)
        if q_vec is None:
            return []

        try:
            # vec0 KNN: distance_l2 ascending; we join back to index_chunks for scoping.
            # sqlite-vec uses vec_distance_l2() or the <-> operator depending on version.
            # We use the JSON vector literal form that works with vec0.
            import json as _json
            vec_json = _json.dumps(q_vec)

            # Retrieve a wider candidate set first (3x), then filter to index
            candidate_limit = max_results * 3
            rows = self.db.execute("""
                SELECT cv.chunk_id, vec_distance_l2(cv.embedding, ?) as dist
                FROM chunks_vec cv
                WHERE cv.embedding MATCH ?
                  AND k = ?
                ORDER BY dist ASC
            """, (vec_json, vec_json, candidate_limit)).fetchall()

            if not rows:
                return []

            # Filter to chunks belonging to this index
            index_chunk_ids = set(
                r[0] for r in self.db.execute(
                    "SELECT chunk_id FROM index_chunks WHERE index_id = ?", (index_id,)
                ).fetchall()
            )

            filtered = [(cid, dist) for cid, dist in rows if cid in index_chunk_ids]
            filtered = filtered[:max_results]

            results = []
            for i, (chunk_id, dist) in enumerate(filtered):
                row = self.db.execute(
                    "SELECT chunk_text, chunk_type, metadata FROM chunks WHERE chunk_id = ?",
                    (chunk_id,)
                ).fetchone()
                if row is None:
                    continue
                text, ctype, metadata = row
                results.append({
                    "chunk_id": chunk_id,
                    "chunk_text": text,
                    "chunk_type": ctype,
                    "metadata": json.loads(metadata) if metadata else None,
                    "rank": i + 1,
                    "score": -dist,   # negate so higher is better (consistent with BM25 convention)
                })
            return results

        except Exception as exc:  # noqa: BLE001
            logger.warning("_semantic_search: KNN query failed (%s).", exc)
            return []

    def search(
        self,
        index_id: str,
        query: str,
        max_results: int = 10,
        method: str = "hybrid",
    ) -> list:
        """Search chunks belonging to index_id.

        Parameters
        ----------
        index_id:
            The index to search within.
        query:
            The search query string.
        max_results:
            Maximum number of results to return.
        method:
            One of:
              "keyword"  — BM25/FTS5 only (original behaviour)
              "semantic" — KNN vector search only (requires sqlite-vec + model2vec)
              "hybrid"   — RRF fusion of keyword + semantic (default)
            Falls back to "keyword" automatically when vec/embeddings are unavailable.
        """
        # Auto-expire stale indexes
        self._auto_expire()

        # Update last_accessed timestamp
        self.last_accessed[index_id] = time.time()

        # Degrade to keyword when infrastructure is missing
        effective_method = method
        if method in ("semantic", "hybrid") and (not self.has_vec or not self.has_embeddings):
            effective_method = "keyword"

        if effective_method == "keyword":
            return self._keyword_search(index_id, query, max_results)

        if effective_method == "semantic":
            results = self._semantic_search(index_id, query, max_results)
            if not results:
                # Graceful fallback to keyword if semantic returns nothing
                return self._keyword_search(index_id, query, max_results)
            return results

        # hybrid — RRF fusion
        kw_results = self._keyword_search(index_id, query, max_results)
        sem_results = self._semantic_search(index_id, query, max_results)

        if not sem_results:
            # No semantic results — return keyword results as-is
            return kw_results

        fused = _rrf_fuse(kw_results, sem_results)
        return fused[:max_results]

    def get_index_info(self, index_id: str) -> dict:
        row = self.db.execute("""
            SELECT COUNT(*), MIN(ic.created_at),
                   (SELECT c.source_tool FROM chunks c
                    JOIN index_chunks ic2 ON c.chunk_id = ic2.chunk_id
                    WHERE ic2.index_id = ? LIMIT 1)
            FROM index_chunks ic WHERE ic.index_id = ?
        """, (index_id, index_id)).fetchone()

        if not row or row[0] == 0:
            return {"error": f"Index '{index_id}' not found"}

        return {
            "index_id": index_id,
            "chunk_count": row[0],
            "created_at": row[1],
            "source_tool": row[2],
        }

    def delete_index(self, index_id: str) -> dict:
        cursor = self.db.execute(
            "DELETE FROM index_chunks WHERE index_id = ?", (index_id,)
        )
        self.db.commit()
        return {"index_id": index_id, "removed": cursor.rowcount}

    def list_indexes(self) -> list:
        rows = self.db.execute("""
            SELECT ic.index_id, COUNT(*) as chunk_count, MIN(ic.created_at) as created_at,
                   (SELECT c.source_tool FROM chunks c
                    JOIN index_chunks ic2 ON c.chunk_id = ic2.chunk_id
                    WHERE ic2.index_id = ic.index_id LIMIT 1) as source_tool
            FROM index_chunks ic
            GROUP BY ic.index_id
            ORDER BY created_at DESC
        """).fetchall()
        return [
            {
                "index_id": r[0],
                "chunk_count": r[1],
                "created_at": r[2],
                "source_tool": r[3],
            }
            for r in rows
        ]

    def stats(self, index_id: str) -> dict:
        """Return detailed statistics for a single index.

        Returns a dict with keys:
          index_id, chunk_count, chunk_types (breakdown dict), total_text_bytes,
          age_seconds, created_at, last_accessed, has_vec, has_embeddings.
        """
        row = self.db.execute("""
            SELECT COUNT(*), MIN(ic.created_at)
            FROM index_chunks ic
            WHERE ic.index_id = ?
        """, (index_id,)).fetchone()

        if not row or row[0] == 0:
            return {"error": f"Index '{index_id}' not found"}

        chunk_count = row[0]
        created_at = row[1]

        # Chunk type breakdown
        type_rows = self.db.execute("""
            SELECT c.chunk_type, COUNT(*) as cnt
            FROM chunks c
            JOIN index_chunks ic ON c.chunk_id = ic.chunk_id
            WHERE ic.index_id = ?
            GROUP BY c.chunk_type
        """, (index_id,)).fetchall()
        chunk_types = {r[0]: r[1] for r in type_rows}

        # Total text bytes
        bytes_row = self.db.execute("""
            SELECT SUM(LENGTH(c.chunk_text))
            FROM chunks c
            JOIN index_chunks ic ON c.chunk_id = ic.chunk_id
            WHERE ic.index_id = ?
        """, (index_id,)).fetchone()
        total_bytes = bytes_row[0] or 0

        now = time.time()
        age_seconds = now - created_at if created_at else 0.0
        last_acc = self.last_accessed.get(index_id)

        return {
            "index_id": index_id,
            "chunk_count": chunk_count,
            "chunk_types": chunk_types,
            "total_text_bytes": total_bytes,
            "age_seconds": age_seconds,
            "created_at": created_at,
            "last_accessed": last_acc,
            "has_vec": self.has_vec,
            "has_embeddings": self.has_embeddings,
        }

    def search_all(self, query: str, max_results: int = 10) -> list:
        """Cross-index BM25 search.  Results are tagged with index_id.

        Returns up to max_results items sorted by BM25 rank, each dict
        containing an extra "index_id" key alongside the standard chunk fields.
        """
        if not query or not query.strip():
            rows = self.db.execute("""
                SELECT c.chunk_id, c.chunk_text, c.chunk_type, c.metadata,
                       0.0 as score, ic.index_id
                FROM chunks c
                JOIN index_chunks ic ON c.chunk_id = ic.chunk_id
                ORDER BY c.chunk_id
                LIMIT ?
            """, (max_results,)).fetchall()
        else:
            safe_query = query.replace('"', '""')
            rows = self.db.execute("""
                SELECT c.chunk_id, c.chunk_text, c.chunk_type, c.metadata,
                       rank as score, ic.index_id
                FROM chunks_fts
                JOIN chunks c ON chunks_fts.rowid = c.chunk_id
                JOIN index_chunks ic ON c.chunk_id = ic.chunk_id
                WHERE chunks_fts MATCH ?
                ORDER BY rank
                LIMIT ?
            """, (safe_query, max_results)).fetchall()

        results = []
        for i, (chunk_id, text, ctype, metadata, score, idx_id) in enumerate(rows):
            # Update last_accessed for each index that contributed a result
            self.last_accessed[idx_id] = time.time()
            results.append({
                "chunk_id": chunk_id,
                "chunk_text": text,
                "chunk_type": ctype,
                "metadata": json.loads(metadata) if metadata else None,
                "rank": i + 1,
                "score": score,
                "index_id": idx_id,
            })
        return results

    def dump(self, index_id: Optional[str] = None, name: Optional[str] = None) -> dict:
        """Serialize the in-memory database (or a specific index) to disk.

        Dumps are written to <ProjectSaved>/MCPIndex/<name>.db using the
        sqlite3 backup API.  When index_id is provided the dump still captures
        the full in-memory database; the caller can use index_id to document
        which index they intended to preserve.

        Returns a dict with 'path' and 'name'.
        """
        try:
            import unreal  # type: ignore
            project_saved = unreal.Paths.project_saved_dir()
        except Exception:
            project_saved = os.path.join(os.getcwd(), "Saved")

        dump_dir = os.path.join(project_saved, "MCPIndex")
        os.makedirs(dump_dir, exist_ok=True)

        if not name:
            name = index_id or f"dump_{int(time.time())}"

        # Sanitise name for use as filename
        safe_name = re.sub(r'[^\w\-.]', '_', name) if name else f"dump_{int(time.time())}"
        dump_path = os.path.join(dump_dir, f"{safe_name}.db")

        dest = sqlite3.connect(dump_path)
        self.db.backup(dest)
        dest.close()

        return {"name": safe_name, "path": dump_path, "index_id": index_id}

    def load(self, name: str) -> dict:
        """Restore an index from a dump file, merging with dedup.

        Reads <ProjectSaved>/MCPIndex/<name>.db and inserts any chunks that do
        not already exist in the in-memory database (dedup by content_hash).

        Returns a dict with merge statistics.
        """
        try:
            import unreal  # type: ignore
            project_saved = unreal.Paths.project_saved_dir()
        except Exception:
            project_saved = os.path.join(os.getcwd(), "Saved")

        dump_dir = os.path.join(project_saved, "MCPIndex")
        safe_name = re.sub(r'[^\w\-.]', '_', name)
        dump_path = os.path.join(dump_dir, f"{safe_name}.db")

        if not os.path.exists(dump_path):
            return {"error": f"Dump file not found: {dump_path}"}

        src = sqlite3.connect(dump_path)
        inserted = 0
        reused = 0

        try:
            src_chunks = src.execute(
                "SELECT chunk_text, chunk_type, source_tool, metadata, content_hash FROM chunks"
            ).fetchall()
            src_index_chunks = src.execute(
                "SELECT index_id, chunk_id, created_at FROM index_chunks"
            ).fetchall()

            # Build a mapping from old chunk_id -> new chunk_id
            old_to_new: dict = {}
            for (text, ctype, src_tool, metadata, content_hash) in src_chunks:
                row = self.db.execute(
                    "SELECT chunk_id FROM chunks WHERE content_hash = ?", (content_hash,)
                ).fetchone()
                if row:
                    # Dedup — record old id -> new id mapping would require knowing old id
                    reused += 1
                else:
                    cursor = self.db.execute(
                        "INSERT INTO chunks (chunk_text, chunk_type, source_tool, metadata, content_hash) "
                        "VALUES (?, ?, ?, ?, ?)",
                        (text, ctype, src_tool, metadata, content_hash),
                    )
                    inserted += 1
                    old_to_new[content_hash] = cursor.lastrowid

            # Re-insert index_chunks by matching via content_hash
            now = time.time()
            for (src_index_id, _old_chunk_id, created_at) in src_index_chunks:
                # Resolve chunk_id in the destination db
                # We can only do this for chunks we know about; simplest approach:
                # iterate over source chunks belonging to this index and look them up
                src_rows = src.execute(
                    "SELECT c.content_hash FROM chunks c "
                    "JOIN index_chunks ic ON c.chunk_id = ic.chunk_id "
                    "WHERE ic.index_id = ?",
                    (src_index_id,),
                ).fetchall()
                for (ch,) in src_rows:
                    new_id_row = self.db.execute(
                        "SELECT chunk_id FROM chunks WHERE content_hash = ?", (ch,)
                    ).fetchone()
                    if new_id_row:
                        self.db.execute(
                            "INSERT OR IGNORE INTO index_chunks (index_id, chunk_id, created_at) VALUES (?, ?, ?)",
                            (src_index_id, new_id_row[0], created_at),
                        )

            self.db.commit()
        finally:
            src.close()

        return {
            "name": name,
            "path": dump_path,
            "chunks_inserted": inserted,
            "chunks_reused": reused,
        }

    def clear(self, index_id: Optional[str] = None) -> dict:
        """Clear one specific index or all indexes.

        When index_id is None, removes all index_chunks entries and any chunks
        no longer referenced by any index.  When index_id is provided, only
        removes that index's entries (and unreferenced chunks).

        Returns counts of removed entries.
        """
        if index_id is not None:
            cursor = self.db.execute(
                "DELETE FROM index_chunks WHERE index_id = ?", (index_id,)
            )
            removed_links = cursor.rowcount
            self.last_accessed.pop(index_id, None)
        else:
            cursor = self.db.execute("DELETE FROM index_chunks")
            removed_links = cursor.rowcount
            self.last_accessed.clear()

        # Purge orphaned chunks (and their embeddings via _purge_orphan_chunks)
        self._purge_orphan_chunks()
        orphan_cursor = self.db.execute(
            "SELECT changes()"
        ).fetchone()
        removed_chunks = orphan_cursor[0] if orphan_cursor else 0

        self.db.commit()
        return {
            "index_id": index_id,
            "removed_index_links": removed_links,
            "removed_orphan_chunks": removed_chunks,
        }

    def expire(self, max_age_seconds: float = 600.0) -> dict:
        """Remove indexes older than max_age_seconds (TTL-based cleanup).

        Age is measured from the index's created_at timestamp (MIN of its
        index_chunks entries).  Also purges orphaned chunks and their embeddings.

        Returns a dict with expired_indexes list and chunk cleanup counts.
        """
        now = time.time()
        cutoff = now - max_age_seconds

        # Find indexes whose oldest chunk link was created before the cutoff
        old_rows = self.db.execute("""
            SELECT index_id, MIN(created_at) as oldest
            FROM index_chunks
            GROUP BY index_id
            HAVING MIN(created_at) < ?
        """, (cutoff,)).fetchall()

        expired_ids = [r[0] for r in old_rows]

        removed_links = 0
        for idx_id in expired_ids:
            cursor = self.db.execute(
                "DELETE FROM index_chunks WHERE index_id = ?", (idx_id,)
            )
            removed_links += cursor.rowcount
            self.last_accessed.pop(idx_id, None)

        # Purge orphaned chunks and embeddings
        self._purge_orphan_chunks()
        orphan_result = self.db.execute("SELECT changes()").fetchone()
        removed_chunks = orphan_result[0] if orphan_result else 0

        self.db.commit()
        return {
            "max_age_seconds": max_age_seconds,
            "expired_indexes": expired_ids,
            "removed_index_links": removed_links,
            "removed_orphan_chunks": removed_chunks,
        }


# Module-level singleton
_engine = None


def get_engine() -> IndexEngine:
    global _engine
    if _engine is None:
        _engine = IndexEngine()
    return _engine
