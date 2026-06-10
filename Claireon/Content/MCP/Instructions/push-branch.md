---
name: Auto Push Changes as New Branch
description: Move local changes to a new branch with standardized naming, rebase on main, and push for review
type: resource
uri: claireon://instructions/push-branch
---

Do not use instructions from this file unless asked.

# Auto Push Changes as New Branch

This script automates the process of moving local changes to a new branch with a standardized naming convention and commit message format.

## Overview

The script should:
- Take all unmerged commits from the current branch plus staged and unstaged changes (including new files) and move them to a new branch
- Attempt to rebase the new branch on latest main
- Handle rebase conflicts gracefully, leaving the user in control
- Generate a descriptive branch name and commit message
- Push the branch for review

## Steps

### 1. Pre-flight Checks

1. Verify current directory is a git repository
2. Store the original branch name for later reference
3. Check that there are staged, unstaged changes, or unmerged commits to process:
   - Check for staged/unstaged changes: `git status --porcelain`
   - Check for unmerged commits: `git log main..HEAD --oneline`
   - If no changes and no unmerged commits exist, exit with informational message
4. Capture the git user identifier using the utility script:
   ```powershell
   Scripts\Utilities\Get-GitUsername.ps1
   ```
   - Returns the username portion of git user.email (e.g., `<user>` from `you@example.com`)
5. Get the worktree name from the current directory basename

### 2. Update Main Branch

1. Fetch latest changes: `git fetch origin main:main`
2. If fetch fails, warn but continue (offline mode)

### 3. Create New Branch from Current State

1. Generate branch name in format: `llm/<git_user>/<worktree>/<generated-name>`
   - `<git_user>`: Output from `Scripts\Utilities\Get-GitUsername.ps1` (e.g., "<user>")
   - `<worktree>`: Current worktree directory name, lowercased
   - `<generated-name>`: Short descriptive name based on changed files/modules
     - If changes affect single module/component, use that name
     - If multiple modules, use a summary term like "multi-module-changes"
     - If unclear, use timestamp: `changes-YYYYMMDD-HHMMSS`
2. Create new branch from current HEAD (preserving all unmerged commits):
   ```bash
   git checkout -b <branch-name>
   ```
3. Store the commit hash before committing new changes: `git rev-parse HEAD`

### 4. Stage and Commit Any Uncommitted Changes

1. Check if there are staged or unstaged changes to commit:
   ```bash
   git status --porcelain
   ```
2. If changes exist, stage all changes including new files:
   ```bash
   git add -A
   ```
3. Verify files are staged: `git status --porcelain`

### 5. Generate Commit Message (if uncommitted changes exist)

If there are staged changes to commit, analyze them to create a commit message following this format:
```
<type>(<scope>) [ci:linux]: <description>
```

**Type** (choose one):
- `fix`: Bug fixes, corrections, patches
- `feat`: New features, additions, enhancements
- `chore`: Maintenance, refactoring, dependencies, configuration

**Scope**:
- Primary module/component affected
- If multiple modules: use parent module name or "multi"
- Examples: `auth`, `api`, `ui`, `config`, `deps`

**[ci:linux] flag**:
- Do NOT include CI tags (`[ci]`, `[ci:linux]`, etc.) on intermediate work-in-progress commits
- Include `[ci:linux]` ONLY on the final commit before squash-rebase for PR, and on the squash-rebased commit itself
- For mid-implementation build verification, use `Scripts\Utilities\Invoke-RemoteBuildVerification.ps1`
- Omit `[ci:linux]` entirely if only non-source files changed (documentation, config, scripts, etc.)

**Description**:
- Terse, passive voice, no period at end
- Focus on what changed, not why or how
- Max 50 characters for total subject line
- Examples:
  - `fix(auth): token validation logic corrected`
  - `feat(api): endpoint added for user preferences`
  - `chore(deps): packages updated to latest versions`

**Generation logic**:
1. Examine `git diff --cached --stat` for affected files
2. Identify primary module from file paths
3. Summarize nature of changes (additions, deletions, modifications)
4. Choose appropriate type and generate description

### 6. Commit Changes (if uncommitted changes exist)

1. Create commit with generated message:
   ```bash
   git commit -m "<generated-message>"
   ```
2. Store the new commit hash: `git rev-parse HEAD`
3. Verify commit succeeded: `git log -1 --oneline`

### 7. Rebase on Latest Main

1. Attempt to rebase the new branch onto main:
   ```bash
   git rebase main
   ```
2. Check the rebase result:
   - **Success**: Continue to push step
   - **Conflict**: Handle gracefully (see conflict handling below)

### 8. Handle Rebase Conflicts

If the rebase results in conflicts:

1. Do NOT automatically abort or resolve - leave the state as-is for user control
2. Display conflict information:
   ```
   ⚠ Rebase conflicts detected

   Original branch: <original-branch-name>
   New branch: <new-branch-name>
   New commit(s): <commit-hash(es)> (before rebase)

   Your changes are safe on the new branch.
   You have the following options:

   1. Resolve conflicts and continue:
      git add <resolved-files>
      git rebase --continue
      git push -u origin <new-branch-name>

   2. Abort the rebase and keep new branch as-is:
      git rebase --abort
      git push -u origin <new-branch-name>

   3. Cherry-pick specific commits from new branch to another branch:
      git checkout <target-branch>
      git cherry-pick <commit-hash>
   ```
3. Exit with informational status code (not failure, since operation partially succeeded)

### 9. Push Branch (if rebase succeeded)

1. Push the new branch to remote:
   ```bash
   git push -u origin <branch-name>
   ```
2. Capture the remote branch URL for output

### 10. Output Results

**If rebase succeeded**, display summary:
```
✓ Changes moved to new branch and rebased on main
  Original branch: <original-branch-name>
  New branch: <new-branch-name>
  Commits moved: <count>
  Latest commit: <commit-hash>
  Message: <commit-message>
  Remote: <remote-url>
```

**If rebase conflicts** (see step 8 for detailed output)

## Error Handling

- **No changes to migrate**: Exit gracefully with message
- **Not a git repository**: Exit with error
- **Cannot fetch main**: Warn but continue (offline mode acceptable)
- **Branch creation fails**: Exit with error
- **Commit fails**: Keep changes staged, exit with error
- **Rebase conflicts**: Leave state for user to resolve, provide detailed instructions (see step 8)
- **Push fails**: Keep local branch, exit with error and instructions to push manually

## Example Execution

### Example 1: Clean Rebase

Starting state:
- On branch: `feature/old-work`
- Unmerged commits: 2 commits (not in main)
- Changed files: `src/auth/login.ts`, `src/auth/token.ts`
- Git username: "<user>"
- Worktree: "<workspace>"

Result:
```
✓ Changes moved to new branch and rebased on main
  Original branch: feature/old-work
  New branch: llm/<user>/<workspace>/auth-fixes
  Commits moved: 3
  Latest commit: abc1234
  Message: fix(auth): token validation logic corrected [ci:linux]
  Remote: https://github.com/user/repo/tree/llm/<user>/<workspace>/auth-fixes
```

### Example 2: Rebase Conflicts

Starting state:
- On branch: `feature/conflicting-work`
- Unmerged commits: 1 commit that conflicts with main
- Changed files: `src/api/routes.ts`
- Git username: "<user>"
- Worktree: "<workspace>"

Result:
```
⚠ Rebase conflicts detected

Original branch: feature/conflicting-work
New branch: llm/<user>/<workspace>/api-routes
New commit(s): def5678 (before rebase)

Your changes are safe on the new branch.
You have the following options:

1. Resolve conflicts and continue:
   git add <resolved-files>
   git rebase --continue
   git push -u origin llm/<user>/<workspace>/api-routes

2. Abort the rebase and keep new branch as-is:
   git rebase --abort
   git push -u origin llm/<user>/<workspace>/api-routes

3. Cherry-pick specific commits from new branch to another branch:
   git checkout <target-branch>
   git cherry-pick def5678
```