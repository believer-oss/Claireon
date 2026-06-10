# Claireon/Resources/Models

ONNX sentence-embedding models and matching vocab files for the hybrid
semantic tool_search (Phase 2).

All binary files in this directory are tracked via Git LFS (see the
`Plugins/Claireon/Resources/Models/** lfs` rule in repo-root .gitattributes).
Add the LFS rule BEFORE committing any model binary.

## Models vendored in Stage 003

- `all-MiniLM-L6-v2-int8.onnx` -- int8 quantized sentence-transformer (~23 MB,
  Apache-2.0). Run on CPU via NNE/NNERuntimeORTCpu. 384-d embeddings, mean
  pooling over the attention mask.
- `vocab.txt` -- BERT WordPiece vocabulary (30522 tokens, ~230 KB). Shared by
  MiniLM and BGE-small model families.
- `LICENSE` -- Apache-2.0 license for the vendored model weights.

## Adding a new model

1. Ensure the LFS rule in .gitattributes covers the new file pattern.
2. Add the model card (license, provenance, pooling/prefix metadata) alongside
   the binary.
3. Update `FClaireonEmbedderMeta` defaults in `ClaireonEmbeddingModel.h` if the new
   model is the production default.
