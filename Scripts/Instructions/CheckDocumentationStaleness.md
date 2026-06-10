Do not use instructions from this file unless asked.

# Check Documentation Staleness

This instruction scans all HTML documentation files in `Docs/`, parses their embedded tracking metadata, and reports which documents have gone stale relative to the source files they reference.

## Overview

The instruction should:
- Discover all `.html` files in `Docs/` (excluding `index.html`)
- Parse the `DOC-TRACKING` JSON comment from each file
- Check whether referenced source files have changed since the document's recorded git commit
- Print a summary table of document status (CURRENT, STALE, NO METADATA)
- Prompt the user to choose which stale documents to update
- Update chosen documents: refresh content sections, update DOC-TRACKING metadata, update the visible tracking appendix, and update the `docs` array in `index.html`

## Prerequisites

- The working directory must be a git repository
- `Docs/` directory must exist with at least one `.html` file
- Git must be available on the system PATH

## Steps

### 1. Discover Documentation Files

1. List all `.html` files in the `Docs/` directory
2. Exclude `index.html` from the list (it is the dashboard, not a content document)
3. If no files are found:
   - Output: "No documentation files found in Docs/"
   - Exit

### 2. Parse DOC-TRACKING Metadata

For each HTML file:

1. Search for a comment block matching this pattern:
   ```
   <!-- DOC-TRACKING
   { ... JSON ... }
   DOC-TRACKING -->
   ```
2. Extract the JSON object between the markers
3. Parse these fields:
   - `title` (string) — document display name
   - `description` (string) — one-line summary
   - `lastUpdated` (string, YYYY-MM-DD) — when the document was last updated
   - `gitCommit` (string) — full or abbreviated commit SHA at time of last update
   - `author` (string) — who last updated the document
   - `referencedFiles` (string[]) — source file paths relative to repo root
   - `referencedBlueprints` (string[]) — blueprint asset paths (may be empty)
4. If no `DOC-TRACKING` block is found, mark the document as `NO METADATA`

### 3. Check Referenced Files for Changes

For each document that has valid metadata:

1. For each path in `referencedFiles`:
   ```bash
   git log --oneline <gitCommit>..HEAD -- <filePath>
   ```
2. If the commit SHA is invalid or not found in history:
   - **Fallback**: use date-based check instead:
     ```bash
     git log --oneline --since="<lastUpdated>" -- <filePath>
     ```
3. If a referenced file has been **deleted or renamed**:
   - Run `git log --oneline --diff-filter=R --follow -1 -- <filePath>` to detect renames
   - Note the rename in the report (e.g., "RENAMED: old/path -> new/path")
   - If simply deleted, note "DELETED: old/path"
4. For `referencedBlueprints`: these are binary `.uasset` files. Check for changes the same way, but note that content diffs are not possible — only detect whether the file was modified.
5. Count total changed files per document

### 4. Generate Summary Report

Print a table to the user:

```
Documentation Staleness Report
==============================

| Document                  | Status      | Changed Files | Last Updated |
|---------------------------|-------------|---------------|--------------|
| MVVM Resolver Pattern     | CURRENT     | 0             | 2026-02-18   |
| Some Other Doc            | STALE       | 3             | 2025-10-15   |
| Untitled Doc              | NO METADATA | —             | —            |
```

Status definitions:
- **CURRENT**: Zero referenced files have changed since `gitCommit`
- **STALE**: One or more referenced files have changed since `gitCommit`
- **NO METADATA**: File has no `DOC-TRACKING` comment block

### 5. Prompt User for Update Selection

If any documents are STALE:

1. List the stale documents with their changed files
2. Ask the user which documents they want to update (allow "all" or specific selections)
3. If the user selects none, exit

### 6. Update Selected Documents

For each document the user chose to update:

1. **Read changed source files**: For each file that changed since the document's commit, read the current version to understand what changed
2. **Analyze the changes**: Compare the current source code against what the document describes. Identify which sections of the document are affected.
3. **Propose updates**: Show the user what sections need updating and what the new content should be. Wait for approval before making changes.
4. **Update DOC-TRACKING metadata**:
   - Set `lastUpdated` to today's date (YYYY-MM-DD format)
   - Set `gitCommit` to the current HEAD commit SHA (`git rev-parse HEAD`)
   - Update `referencedFiles` if files were renamed or new files are now relevant
5. **Update the visible tracking appendix**: Update the "Document Tracking" section at the bottom of the HTML file to match the new metadata
6. **Update index.html**: Find the document's entry in the `docs` array in `Docs/index.html` and update its `lastUpdated` field

### 7. Error Handling

- **Git not available**: Output "Error: git is not available on PATH" and exit
- **Invalid JSON in DOC-TRACKING**: Warn "Malformed DOC-TRACKING in <filename>, skipping" and treat as NO METADATA
- **Commit SHA not in history**: Fall back to date-based `--since` check (see Step 3.2)
- **All referenced files deleted**: Report document as STALE with note "All referenced files have been removed"
- **No stale documents**: Output "All documents are current" and exit
- **index.html missing**: Skip the index update step with a warning

## Important Notes

- This instruction is **read-only by default** — it only modifies files in Step 6 after explicit user approval
- Binary assets (`.uasset`, `.umap`) in `referencedBlueprints` can only be checked for modification, not diffed for content
- The `gitCommit` field should always be a full 40-character SHA for reliability, though abbreviated SHAs are tolerated
- When updating a document, preserve all existing HTML structure — only modify content within sections that are affected by source code changes
- After updating documents, the user should verify the changes by opening the HTML files in a browser
- Every document page must include a navigation link back to the index at the top of `<body>`, before the DOC-TRACKING comment: `<a href="index.html" class="doc-nav-home">&larr; All Documents</a>`

## Example Execution

```
> Run CheckDocumentationStaleness

Scanning Docs/ for HTML files...
Found 2 documents (excluding index.html)

Parsing DOC-TRACKING metadata...
  mvvm-resolver-breakdown.html — MVVM Resolver Pattern (2026-02-18)
  networking-overview.html     — NO METADATA

Checking referenced files for changes...
  MVVM Resolver Pattern:
    Source/MyModule/Private/UI/Crafting/CraftingDisplayWidget.cpp — 2 commits since 5940e48
    Source/MyModule/Private/UI/Inventory/InventoryPanelWidget.cpp — no changes

Documentation Staleness Report
==============================

| Document              | Status      | Changed Files | Last Updated |
|-----------------------|-------------|---------------|--------------|
| MVVM Resolver Pattern | STALE       | 1             | 2026-02-18   |
| networking-overview   | NO METADATA | —             | —            |

1 document is stale. Which would you like to update?
  [1] MVVM Resolver Pattern (1 changed file)
  [A] All stale documents
  [N] None (exit)

> 1

Reading changed files...
  CraftingDisplayWidget.cpp: NativeConstruct was refactored to use Resolver pattern

Proposed updates for "MVVM Resolver Pattern":
  - Section 6 "What the Project Does Today": Update crafting example to reflect new Resolver usage
  - Section 14 "Before & After": Mark crafting widget as migrated

Apply these updates? (y/n)
```
