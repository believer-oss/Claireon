---
name: Begin Work on New Task
description: Create a local task workspace with task-info.md and a new work branch on latest main
type: resource
uri: claireon://instructions/begin-work
---

Do not use instructions from this file unless asked.

# Begin Work on New Task

This instruction creates a local task workspace and a new work branch. All workflow
documentation (plans, stage files, state) lives in `Saved/Claireon/Workflow/<task-name>/`
and is never committed to git — only implementation code gets committed.

## Overview

The instruction:
- Creates `Saved/Claireon/Workflow/<task-name>/` as the local workspace
- Creates `task-info.md` with goal, details, and a Notes section
- Saves any uncommitted changes, resets to latest main
- Creates a new branch based on latest main
- Leaves the worktree ready for development

## Prerequisites

The user must provide **at least one** of:
- A description of the work to be done (goal, context, scope)
- Uncommitted local changes that represent work in progress

If neither is provided, ask the user to describe what they're working on.

## Git Command Convention

Use `git -C <repo>` instead of `cd <repo> && git ...` for all git commands.

## Steps

### 1. Gather Context

1. Collect the user's description of the work (goal, approach, scope)
   - If only uncommitted changes exist, analyze them to understand the work
2. Determine the repository root path (`<repo>`)
3. Get the git user identifier:
   ```bash
   git -C <repo> config user.email
   ```
   Use the local-part before `@` as `<git_user>` (e.g., `{{GIT_USER}}` from `{{GIT_EMAIL}}`)
4. Get the worktree name from the `<repo>` directory basename

### 2. Derive Task Name

Generate a kebab-case task name from the work description:
- Format: 2-4 words, no numbers, e.g., `spawning-system`, `mcp-editor-tools`, `rotation-perf-fix`
- If the description is ambiguous, prefix with today's date: `YYYYMMDD-<summary>`
- Confirm with the user if the name is unclear

### 3. Create Task Workspace

```bash
mkdir -p Saved/Claireon/Workflow/<task-name>
```

Or on Windows:
```powershell
New-Item -ItemType Directory -Force "Saved/Claireon/Workflow/<task-name>"
```

### 4. Create task-info.md

Write `Saved/Claireon/Workflow/<task-name>/task-info.md`:

```markdown
# Task: <Short Title>

Started: <ISO 8601 timestamp>
Branch: (set in Step 7)
Workspace: Saved/Claireon/Workflow/<task-name>/

## Goal

<One-sentence description of what this work achieves>

## Details

<Approach, context, scope, key constraints>

## Changes

<!-- Updated as the implementation is completed and the PR is created -->

## Open Questions

<!-- Unresolved decisions — cleared as they are answered -->

## Notes

<!-- Running log of discoveries, decisions, and progress observations -->
```

### 5. Save Uncommitted Changes (if any)

If there are uncommitted changes (staged or unstaged):

1. Stage all changes:
   ```bash
   git -C <repo> add -A
   ```
2. Create a temporary commit:
   ```bash
   git -C <repo> commit -m "wip: work in progress for new task"
   ```
3. Record the commit hash: `git -C <repo> rev-parse HEAD`
4. Record the current branch name

### 6. Fetch Latest Main

```bash
git -C <repo> fetch origin main
```

If fetch fails, abort with error.

### 7. Create New Branch from Latest Main

1. Generate branch name: `llm/<git_user>/<worktree>/<task-name>`
   - Example: `llm/{{GIT_USER}}/{{PROJECT_NAME}}/spawning-system`

2. Create a parking branch to free the worktree (park the current state):
   ```bash
   git -C <repo> checkout -b llm/<git_user>/<worktree>-parking-<YYYYMMDD>
   git -C <repo> reset --hard origin/main
   ```

3. Create the new work branch from the now-clean parking branch:
   ```bash
   git -C <repo> checkout -b llm/<git_user>/<worktree>/<task-name>
   ```

4. Update `task-info.md` with the branch name.

### 8. Apply Saved Changes (if any)

If changes were saved in Step 5:

1. Cherry-pick the temporary commit:
   ```bash
   git -C <repo> cherry-pick <saved-commit-hash>
   ```
2. On success, soft-reset to unstage the wip message:
   ```bash
   git -C <repo> reset --soft HEAD~1
   ```
3. On cherry-pick conflict, inform the user and provide resolution guidance.

### 9. Verify and Output

1. Confirm the new branch:
   ```bash
   git -C <repo> branch --show-current
   git -C <repo> log -1 --oneline
   ```
2. Confirm `Saved/Claireon/Workflow/<task-name>/task-info.md` exists.

Display summary:
```
✓ New task started
  Task:      <task-name>
  Branch:    llm/<git_user>/<worktree>/<task-name>
  Base:      origin/main (<commit-hash>)
  Workspace: Saved/Claireon/Workflow/<task-name>/
  Changes:   <applied / none>
  Ready for development
```

## Error Handling

- **No description and no changes**: Ask the user to describe the work before proceeding
- **Not a git repository**: Exit with error
- **Fetch fails**: Exit with error (may be offline or no remote)
- **Branch creation fails**: Exit with error and show current git state
- **Cherry-pick conflicts**: Leave state for the user to resolve; print resolution steps

## Important Notes

- **`Saved/` is not committed**: The workspace is local-only. All workflow tracking stays there. Only implementation code gets committed to the branch.
- **task-info.md is the tracking record**: Update its Notes section as work progresses. Add a Changes entry when the PR is created.
- **Parking branches are disposable**: They exist only to satisfy git worktree constraints. Do not push them.
- **No work numbers**: Tasks are identified by their `<task-name>` alone. The branch name encodes it.

## Example Execution

### Example 1: Starting Fresh with a Description

User says: "I need to add a new spawning system with wave management"

Starting state: on branch `llm/{{GIT_USER}}/{{PROJECT_NAME}}-parking-20260218`, no uncommitted changes.

```
✓ New task started
  Task:      spawning-system
  Branch:    llm/{{GIT_USER}}/{{PROJECT_NAME}}/spawning-system
  Base:      origin/main (619bf8a)
  Workspace: Saved/Claireon/Workflow/spawning-system/
  Changes:   none
  Ready for development
```

### Example 2: Starting with Existing Changes

User says: "start a new task" (has uncommitted changes to {{GAME_MODULE}} files)

```
✓ New task started
  Task:      targeting-component-fixes
  Branch:    llm/{{GIT_USER}}/{{PROJECT_NAME}}/targeting-component-fixes
  Base:      origin/main (abc1234)
  Workspace: Saved/Claireon/Workflow/targeting-component-fixes/
  Changes:   applied (cherry-picked from previous branch)
  Ready for development
```
