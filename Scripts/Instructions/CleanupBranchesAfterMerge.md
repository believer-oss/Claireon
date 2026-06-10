Do not use instructions from this file unless asked.

# Cleanup Branches After Merge

This script rebases all local branches onto main after their dependencies have merged, ensuring PR diffs remain clean and show only the intended changes for each branch.

## Overview

The script should:
- Identify all local branches for the current user
- Rebase each branch onto latest main to remove redundant commits
- Force-push cleaned branches to update remote PRs
- Delete branches that are fully merged (no unique commits)
- Skip branches with conflicts and report them for manual resolution
- Return to original branch when complete

## Goals

- **Clean PR diffs**: Remove commits that have merged to main, showing only unique changes
- **Bulk maintenance**: Update all branches in one operation
- **Automatic cleanup**: Delete branches that are fully merged to reduce clutter
- **Safe updates**: Use `--force-with-lease` to prevent overwriting concurrent changes
- **Non-blocking**: Skip problematic branches rather than stopping entire operation

## When to Run

Run this cleanup:
- **After your PRs merge**: Clean up remaining branches that depended on merged work
- **Periodically**: Keep all branches current with main
- **Before PR reviews**: Ensure reviewers see minimal, accurate diffs
- **After syncing**: Follow up synchronization with cleanup to maintain all branches

## Steps

### 1. Pre-flight Checks

1. Verify current directory is a git repository
2. Check for uncommitted changes:
   ```bash
   git status --porcelain
   ```
   - If uncommitted changes exist, exit with error: "Commit or stash changes before cleanup."
3. Capture current branch name: `git branch --show-current`
4. Capture git user identifier from email prefix:
   ```bash
   git config user.email | cut -d@ -f1
   ```
   - Example: `jdoe@example.com` → `jdoe`

### 2. Fetch Latest Main

1. Fetch main and all remote branches:
   ```bash
   git fetch origin main
   git fetch origin
   ```
2. Fetch tags and prune deleted branches:
   ```bash
   git fetch --tags --prune
   ```
3. Output: `✓ Fetched latest main (<commit-hash>)`

### 3. Identify Local Branches

1. List all local branches for current user:
   ```bash
   git branch --list "llm/<username>/*"
   ```
   - `<username>`: Email prefix from `git config user.email` (e.g., "jdoe")
2. Exclude current branch if on main (cleanup continues)
3. If no branches found, exit with: "No branches to clean up."
4. Output: `Found <N> branches to clean`

### 4. Process Each Branch

For each local branch:

#### 4.1. Switch to Branch

1. Checkout the branch:
   ```bash
   git checkout <branch>
   ```
2. If checkout fails:
   - Output: `✗ Failed to checkout <branch>`
   - Add to error list, continue to next branch

#### 4.2. Check if Cleanup Needed

1. Check if branch is already up-to-date with main:
   ```bash
   git merge-base --is-ancestor origin/main HEAD
   ```
   - If true, branch already includes all of main
   - Check if any commits are redundant:
     ```bash
     git rev-list --count HEAD ^origin/main
     ```
   - If count is 0, branch is identical to main:
     - Output: `→ <branch>: already up-to-date`
     - Continue to next branch

2. Check if branch has any commits not in main:
   ```bash
   git log HEAD --not origin/main --oneline
   ```
   - If empty, branch is fully merged:
     - Output: `→ <branch>: fully merged to main`
     - Add to merged list, continue to Step 4.2.1

#### 4.2.1. Delete Fully Merged Branches

For branches that are fully merged (no unique commits):

1. Return to main branch temporarily:
   ```bash
   git checkout main
   ```

2. Delete the local branch:
   ```bash
   git branch -d <branch>
   ```
   - Uses `-d` (safe delete) which will fail if branch isn't fully merged

3. If deletion succeeds:
   - Output: `  ✓ Deleted local branch`

4. Add to deleted list, continue to next branch

#### 4.3. Rebase onto Main

1. Store current commit: `git rev-parse HEAD`
2. Attempt rebase:
   ```bash
   git rebase origin/main
   ```
3. If rebase succeeds:
   - Count cleaned commits:
     ```bash
     # Commits that were dropped during rebase
     git rev-list --count <old-head> ^HEAD ^origin/main
     ```
   - Output: `✓ <branch>: rebased onto main (removed <N> commits)`
   - Continue to Step 4.4

4. If rebase fails with conflicts:
   - Abort the rebase:
     ```bash
     git rebase --abort
     ```
   - Output:
     ```
     ✗ <branch>: conflicts detected
       Conflicting files: <list from git status>
       Manual rebase required: git checkout <branch> && git rebase origin/main
     ```
   - Add to conflict list, continue to next branch

#### 4.4. Force-Push with Lease

1. Check if remote branch exists:
   ```bash
   git rev-parse --verify origin/<branch>
   ```

2. If remote exists, force-push with lease:
   ```bash
   git push --force-with-lease origin <branch>
   ```
   - `--force-with-lease`: Only pushes if remote hasn't changed since last fetch

3. If push succeeds:
   - Output: `  ↑ Pushed to remote`

4. If push fails:
   - Output:
     ```
     ✗ <branch>: push rejected (remote was updated)
       Fetch and retry: git fetch origin && git push --force-with-lease origin <branch>
     ```
   - Add to failed-push list
   - Branch is still cleaned locally

5. If no remote exists:
   - Output: `  → No remote branch (local only)`

### 5. Return to Original Branch

1. Checkout original branch:
   ```bash
   git checkout <original-branch>
   ```
2. If checkout fails:
   - Output warning: `⚠ Could not return to original branch: <original-branch>`
   - Output current branch: `Currently on: $(git branch --show-current)`

### 6. Output Summary

Display cleanup results:
```
✓ Cleanup complete

Cleaned and pushed: <count>
  - <branch-1> (removed N commits)
  - <branch-2> (removed N commits)
  - ...

Deleted (fully merged): <count>
  - <branch-3>
  - <branch-4>
  - ...

Conflicts (manual rebase required): <count>
  - <branch-5>
  - ...

Push failed (remote updated): <count>
  - <branch-6>
  - ...

Already up-to-date: <count>

Total branches processed: <count>
```

### 7. Suggest Follow-up Actions

Based on results, suggest actions:

- If branches have conflicts:
  ```
  To resolve conflicts:
    git checkout <branch>
    git rebase origin/main
    # Resolve conflicts
    git rebase --continue
    git push --force-with-lease origin <branch>
  ```

- If pushes failed:
  ```
  To retry failed pushes:
    git fetch origin
    git push --force-with-lease origin <branch>
  ```

## Error Handling

- **Uncommitted changes**: Exit with error, require clean working directory
- **Not a git repository**: Exit with error
- **Fetch fails**: Exit with error, check network/remote access
- **Branch checkout fails**: Skip branch, add to error list, continue
- **Rebase conflicts**: Abort rebase, report conflicts, continue to next branch
- **Push fails**: Report error, keep local changes, continue to next branch
- **Cannot return to original branch**: Report warning, show current branch

## Safety Features

- **--force-with-lease**: Prevents overwriting concurrent changes by another worktree
- **Individual branch failures don't stop cleanup**: Continue processing remaining branches
- **Conflict detection**: Never auto-resolves conflicts
- **Dry-run capable**: Can be extended to add `--dry-run` flag for preview

## Example Execution

Starting state:
- Current branch: `llm/arcmage404/worktree-3/new-feature`
- Local branches:
  - `llm/arcmage404/worktree-1/auth-fixes` (2 commits now in main)
  - `llm/arcmage404/worktree-2/api-updates` (in PR, clean)
  - `llm/arcmage404/worktree-3/ui-refactor` (fully merged)
  - `llm/arcmage404/main-worktree/bug-fix` (has conflicts with main)

Result:
```
✓ Fetched latest main (a1b2c3d)
Found 4 branches to clean

✓ llm/arcmage404/worktree-1/auth-fixes: rebased onto main (removed 2 commits)
  ↑ Pushed to remote
→ llm/arcmage404/worktree-2/api-updates: already up-to-date
→ llm/arcmage404/worktree-3/ui-refactor: fully merged to main
  ✓ Deleted local branch
✗ llm/arcmage404/main-worktree/bug-fix: conflicts detected
  Conflicting files: src/auth.ts
  Manual rebase required: git checkout llm/arcmage404/main-worktree/bug-fix && git rebase origin/main

✓ Cleanup complete

Cleaned and pushed: 1
  - llm/arcmage404/worktree-1/auth-fixes (removed 2 commits)

Deleted (fully merged): 1
  - llm/arcmage404/worktree-3/ui-refactor

Conflicts (manual rebase required): 1
  - llm/arcmage404/main-worktree/bug-fix

Already up-to-date: 1

Total branches processed: 4
```

## Notes

- Operates on **local branches only** - does not create new branches
- Uses branch naming convention: `llm/<git_user>/<worktree>/<generated-name>`
- Safe to run multiple times - idempotent operation
- **Automatically deletes fully merged local branches** - removes local branches that have no unique commits (remote branches are preserved)
- Can run while working on a branch - returns to original branch when complete
- `--force-with-lease` protects against race conditions between worktrees
- Uses safe delete (`-d`) which prevents deletion of unmerged branches
