Do not use instructions from this file unless asked.

# Synchronize with Shared Work

This script enables cooperative synchronization between AI workers across multiple worktrees, integrating completed work from other workers and staying current with main branch.

## Overview

The script should:
- Fetch and integrate all pushed branches from all worktrees for the current user
- Rebase current work onto latest main
- Apply other workers' completed changes to current branch
- Clean up branch history after dependencies merge to main
- Stop on conflicts for manual resolution

## Goals

- **Minimal PR diffs**: Rebase onto main to show only intended changes
- **Avoid duplicate work**: Build on changes already pushed by other workers
- **Prevent conflicts**: Integrate work early, before it diverges
- **Clean evaluation**: Each PR displays only its own changes, not dependencies

## Steps

### 1. Pre-flight Checks

1. Verify current directory is a git repository
2. Verify not on main branch
   - If on main, exit with error: "Cannot sync on main branch. Switch to feature branch first."
3. Check for uncommitted changes:
   ```bash
   git status --porcelain
   ```
   - If uncommitted changes exist, exit with error: "Commit or stash changes before synchronizing."
4. Capture current branch name: `git branch --show-current`
5. Capture git user identifier from email prefix:
   ```bash
   git config user.email | cut -d@ -f1
   ```
   - Example: `jdoe@example.com` → `jdoe`

### 2. Fetch Latest Changes

1. Fetch all remote branches and main:
   ```bash
   git fetch origin
   ```
2. Fetch all tags and prune deleted branches:
   ```bash
   git fetch --tags --prune
   ```

### 3. Identify Relevant Branches

1. List all remote branches for current user:
   ```bash
   git branch -r | grep "origin/llm/<username>/"
   ```
   - `<username>`: Email prefix from `git config user.email` (e.g., "jdoe")
   - Includes branches from ALL worktrees (main-worktree, worktree-1, worktree-2, worktree-3, etc.)
2. Filter out current branch from the list
3. Store list of branches to integrate

### 4. Rebase onto Main

1. Store original commit hash: `git rev-parse HEAD`
2. Rebase current branch onto latest main:
   ```bash
   git rebase origin/main
   ```
3. If rebase fails with conflicts:
   - Abort the rebase: `git rebase --abort`
   - Output detailed conflict information:
     ```
     ✗ Conflict detected while rebasing onto main
       Branch: <branch-name>
       Conflicting files: <list files>

       ACTION REQUIRED:
       Manually resolve conflicts, then run:
         git rebase origin/main
         git rebase --continue

       Or abort synchronization:
         git rebase --abort
     ```
   - Exit with failure code
4. Output: `✓ Rebased onto main (origin/main)`

### 5. Integrate Other Worktrees' Changes

For each remote branch identified in Step 3:

1. Check if branch commits are already in current branch:
   ```bash
   git merge-base --is-ancestor origin/<branch> HEAD
   ```
   - If true, skip this branch (already integrated or in main)

2. Check if branch has been merged to main:
   ```bash
   git merge-base --is-ancestor origin/<branch> origin/main
   ```
   - If true, skip this branch (already in main via Step 4 rebase)

3. Attempt to merge the branch:
   ```bash
   git merge --no-ff origin/<branch> -m "sync: integrate <branch>"
   ```

4. If merge fails with conflicts:
   - Abort the merge: `git merge --abort`
   - Output detailed conflict information:
     ```
     ✗ Conflict detected while integrating <branch>
       Conflicting files: <list files>

       ACTION REQUIRED:
       Manually resolve conflicts, then run:
         git merge --continue

       Or skip this branch:
         git merge --abort

       Then re-run synchronization.
     ```
   - Exit with failure code

5. Output: `✓ Integrated <branch>`

### 6. Clean Up After Remote Merges

After integrating all branches, check if any integrated commits are now in main:

1. Identify commits that exist in both current branch and main:
   ```bash
   git log HEAD --not origin/main --format="%H"
   ```
   For each commit hash:
   ```bash
   git cherry origin/main <commit-hash>
   ```
   - If output starts with `-`, commit is in main (merged)

2. If commits from other worktrees are now in main, perform interactive rebase cleanup:
   ```bash
   git rebase -i origin/main
   ```
   - Automatically mark redundant commits (those in main) as `drop`
   - Keep only commits unique to current branch

3. If rebase fails with conflicts:
   - Abort: `git rebase --abort`
   - Output error and exit (as in Step 4)

4. Output: `✓ Cleaned up <N> redundant commits`

### 7. Verify Clean State

1. Verify no merge conflicts remain:
   ```bash
   git diff --check
   ```
2. Verify repository is in clean state:
   ```bash
   git status --porcelain
   ```
3. Show commit graph summary:
   ```bash
   git log --oneline --graph -10
   ```

### 8. Output Results

Display synchronization summary:
```
✓ Synchronization complete
  Branch: <branch-name>
  Base: origin/main (<main-commit-hash>)
  Integrated branches: <count>
    - <branch-1>
    - <branch-2>
    - ...
  Cleaned up commits: <count>
  Current HEAD: <commit-hash>

  Branch is ready for continued work or PR creation.
```

## Conflict Resolution Workflow

When conflicts occur, the script stops and provides detailed guidance:

1. **Conflict during rebase onto main**:
   - Indicates current work conflicts with recent main changes
   - Manually resolve, then continue: `git rebase --continue`
   - Or abort and investigate: `git rebase --abort`

2. **Conflict during branch integration**:
   - Indicates work in another worktree conflicts with current changes
   - Manually resolve which version to keep
   - Continue: `git merge --continue`
   - Or skip that branch: `git merge --abort` and re-run sync

3. After resolving ANY conflict manually, re-run synchronization to complete remaining steps.

## Error Handling

- **On main branch**: Exit with error, require feature branch
- **Uncommitted changes**: Exit with error, require clean working directory
- **Not a git repository**: Exit with error
- **Fetch fails**: Exit with error, check network/remote access
- **Rebase conflicts**: Abort, output conflict details, exit for manual resolution
- **Merge conflicts**: Abort, output conflict details, exit for manual resolution
- **Interactive rebase fails**: Abort, output error, require manual cleanup

## When to Run

Run this synchronization:
- **Before starting new work**: Ensure you're building on latest shared progress
- **After pushing a branch**: Other workers can sync to get your changes
- **Periodically during long tasks**: Stay current with other workers' progress
- **After PR merges**: Rebase to clean up branch history

## Example Execution

Starting state:
- Current branch: `llm/arcmage404/worktree-3/auth-fixes`
- Other branches pushed:
  - `llm/arcmage404/worktree-1/api-updates` (not in main)
  - `llm/arcmage404/worktree-2/ui-components` (merged to main)
- Main has advanced 3 commits

Result:
```
✓ Rebased onto main (origin/main)
✓ Integrated llm/arcmage404/worktree-1/api-updates
✓ Cleaned up 2 redundant commits
✓ Synchronization complete
  Branch: llm/arcmage404/worktree-3/auth-fixes
  Base: origin/main (a1b2c3d)
  Integrated branches: 1
    - llm/arcmage404/worktree-1/api-updates
  Cleaned up commits: 2
  Current HEAD: e4f5g6h
```

## Notes

- Only integrates **pushed** branches (not work-in-progress in other worktrees)
- Uses branch naming convention: `llm/<git_user>/<worktree>/<generated-name>`
- Preserves merge commits for traceability (`--no-ff`)
- Interactive rebase cleanup is automatic where possible
- Always stops on conflicts - never auto-resolves to prevent data loss
