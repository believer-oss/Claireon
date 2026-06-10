Do not use instructions from this file unless asked.

# Reset Worktree to Main

This script resets a worktree to the latest main branch for starting fresh work, "parking" the current branch locally to avoid worktree conflicts.

## Overview

The script should:
- Park the current branch locally (never pushed) to free up worktree state
- Reset the worktree to the latest main branch
- Leave the worktree ready for new work

## Background

Git worktrees cannot check out the same branch simultaneously. When resetting to main, we create a local "parking" branch to hold the worktree's previous state. Parking branches:
- Are **never pushed** to remote
- Contain **only commits already in main** (no unique work)
- Exist solely to satisfy worktree branch uniqueness requirements

## Options

- `--force` or `-f`: Skip uncommitted changes check and discard all local changes

## Git Command Convention

**Use `git -C <repo-path>` instead of `cd <repo-path> && git ...`** for all git commands. This avoids compound shell commands (`cd && git`) which trigger unnecessary permissions prompts in Claude Code, even for trusted repositories. Determine the repo path from the current working directory or worktree root once, then pass it via `-C` for every git invocation.

## Steps

### 1. Pre-flight Checks

1. Determine the repository root path (referred to as `<repo>` below)
2. Verify it is a git repository:
   ```bash
   git -C <repo> rev-parse --is-inside-work-tree
   ```
3. Capture git user identifier using the utility script:
   ```powershell
   Scripts\Utilities\Get-GitUsername.ps1
   ```
   - Returns the username portion of git user.email (e.g., `jdoe` from `jdoe@example.com`)
4. Get the worktree name from the `<repo>` directory basename
5. Check for uncommitted changes (staged or unstaged):
   ```bash
   git -C <repo> status --porcelain
   ```
   - If `--force` flag provided: warn user that changes will be discarded, continue
   - Otherwise: **abort with error**: "Uncommitted changes detected. Commit or stash changes before resetting, or use --force to discard."

### 2. Fetch Latest Main

1. Fetch latest from origin:
   ```bash
   git -C <repo> fetch origin main
   ```
2. If fetch fails, abort with error

### 3. Create Parking Branch

1. Generate parking branch name in format: `llm/<git_user>/<worktree>-parking-<date>`
   - `<git_user>`: Output from `Scripts\Utilities\Get-GitUsername.ps1` (e.g., "jdoe")
   - `<worktree>`: Current worktree directory name, lowercased
   - `<date>`: Current date in format `YYYYMMDD`
   - Example: `llm/arcmage404/worktree-3-parking-20250205`

2. Check if parking branch already exists:
   ```bash
   git -C <repo> branch --list "<parking-branch-name>"
   ```
   - If exists, delete it first: `git -C <repo> branch -D <parking-branch-name>`

3. Create parking branch from current HEAD:
   ```bash
   git -C <repo> checkout -b <parking-branch-name>
   ```

### 4. Reset to Main

1. Reset the parking branch to match origin/main exactly:
   ```bash
   git -C <repo> reset --hard origin/main
   ```
2. This ensures the parking branch has no commits that aren't in main

### 5. Verify State

1. Confirm worktree is now at origin/main:
   ```bash
   git -C <repo> log -1 --oneline
   git -C <repo> status
   ```
2. Verify no uncommitted changes remain

### 6. Output Results

Display summary information:
```
Worktree reset to main
  Parking branch: <parking-branch-name> (local only, do not push)
  Now at: <commit-hash> <commit-subject>
  Ready for new work
```

## Error Handling

- **Not a git repository**: Exit with error
- **Uncommitted changes**: Exit with error and instructions to commit, stash, or use `--force`
- **Fetch fails**: Exit with error (may be offline or remote unavailable)
- **Branch operations fail**: Exit with error and current state description

## Important Notes

- **Never push parking branches**: They serve only as local placeholders
- **Parking branches are disposable**: Delete old parking branches freely with `git branch -D`
- **No work should exist on parking branches**: They always match main exactly
- **Use AutoPushChangesAsNewBranch first**: If you have work to preserve, use that workflow before this one

## Example Execution

Starting state:
- On branch: `llm/arcmage404/worktree-3/auth-fixes`
- No uncommitted changes
- Git username: "arcmage404"
- Worktree: "worktree-3"
- Date: 2025-02-05

Result:
- Parking branch created: `llm/arcmage404/worktree-3-parking-20250205` (local only)
- Worktree now at latest `origin/main`
- Ready to start new work with `AutoPushChangesAsNewBranch`
