---
name: Sequencing Instructions
description: Break a work proposal into ordered skeleton-test-implement stage documents ready for automated implementation
type: resource
uri: claireon://instructions/sequencing
---

<!-- claude-hint:
model: opus
effort: high
rationale: Senior-engineer persona; produces stage documents from proposal
-->

Do not use instructions from this file unless asked.

> **Token placeholders**: This document uses `{{TOKEN}}` placeholders (e.g. `{{PROJECT_NAME}}`, `{{PROJECT_ROOT}}`). Resolve each from your project/git/environment context before acting on or echoing the text -- see the legend at `claireon://instructions/token-legend` (fetch via `resources/read`).

# Break Down Work Proposal

This instruction takes a work proposal document and breaks it down into sequentially implementable stages, each with its own document, following a skeleton-test-(implement-test)+ pattern. The output is a set of numbered stage files and a master prompt that an implementer can follow start to finish.

## Overview

The instruction should:
- Review an input proposal document and understand the full scope of work
- Invoke **[begin-work](claireon://instructions/begin-work)** to create a task workspace and branch (if not already done)
- Break the work into discrete, ordered stages following the skeleton-test pattern
- Author individual `###-stage-name.md` files alongside the proposal document
- Author `000-prompt-to-implement-thing.md` as the master implementation guide
- Commit all stage files and the proposal to the working branch for review

## Prerequisites

The user must provide:
- **`$INPUT_PROPOSAL_FILE`**: Absolute path to the proposal document to break down (e.g., `{{PROJECT_ROOT}}/SPAWNING.md`)

The invoker will derive:
- **`$PROPOSAL_DIR`**: Directory containing `$INPUT_PROPOSAL_FILE` (stage files are placed here)
- **`$PROPOSAL_NAME`**: Filename stem of the proposal (e.g., `SPAWNING`) used for naming context

## Role

Assume the role of a **senior principal gameplay engineer** working on a co-op team-based multiplayer action RPG built on Unreal Engine 5. Approach the breakdown with:
- Deep familiarity with UE5 module architecture, GAS, and Lyra patterns
- Emphasis on incremental correctness — every stage must leave the project in a buildable, testable state
- Awareness that each stage will be implemented by an LLM agent (Claude Code) that can write code, run scripts, and commit work

## Steps

### 1. Review the Proposal

1. Read `$INPUT_PROPOSAL_FILE` thoroughly
2. Identify:
   - All new **types** to be introduced (classes, structs, enums, interfaces, delegates)
   - All new **systems** or **subsystems** and their responsibilities
   - **Dependencies** between proposed types and existing {{PROJECT_NAME}} modules
   - **Data assets**, **blueprints**, or **config** changes required
   - **Risk areas** — places where incorrect implementation would cascade into future stages
3. Build a mental model of the dependency graph: what must exist before what

### 2. Invoke BeginWorkOnNewTask

Follow **[begin-work](claireon://instructions/begin-work)** with these modifications:
- Create the workspace and branch as normal
- **Fill in only** `task-info.md` **Goal** and **Details** sections based on the proposal content
- Leave **Changes** and **Notes** empty — the user fills these as work progresses

### 3. Design the Stage Breakdown

Break the work into stages following this pattern:

```
001-skeleton.md          — Type framework: headers, stubs, module registration
002-test-skeleton.md     — Verify skeleton compiles into a working editor
003-implement-<area>.md  — Fill out first functional area of the skeleton
004-test-<area>.md       — Test the first functional area
005-implement-<area>.md  — Fill out next functional area
006-test-<area>.md       — Test the next functional area
...
NNN-final-validation.md  — End-to-end validation of the complete implementation
000-prompt-to-implement-thing.md — Master guide for the implementer
```

#### Stage Design Principles

1. **Stage 001 is always the skeleton**: Create all types, headers, stubs, module registrations, and build file changes. No implementation logic — just the framework that subsequent stages fill in. This establishes the full type graph so later stages can reference any type without forward-declaration surprises.

2. **Every odd stage (after 001) implements; every even stage tests**: The test stage validates the preceding implementation stage. Test stages end with a commit to the branch.

3. **Each implementation stage fills in the skeleton** rather than iteratively expanding it. Stage 003 should not add new types — it should flesh out types created in 001.

4. **Each stage must leave the project buildable**: No stage should introduce compile errors that the next stage is expected to fix. If a stub needs a return value, provide a sensible default.

5. **Test type and thoroughness scale with risk**:
   - **CPP changes** → Almost always require a build verification; prefer `Scripts\Utilities\Invoke-RemoteBuildVerification.ps1` (90-minute timeout, falls back to local `Invoke-EditorBuild.ps1` on timeout)
   - **Comment-only or doc-only changes** → If the next stage also requires recompiling, skip the redundant build; instead, do a quick regex/grep check on the diffs for formatting correctness
   - **Blueprint or asset changes** → Use `Invoke-CompileBlueprints.ps1` or `Invoke-ValidateAssets.ps1`
   - **Functional behavior changes** → Use `Invoke-UntestTests.ps1` with appropriate `-TestFilter`
   - **MCP tools or externally-callable systems** → Call the tool directly via the MCP server with real project data. Exercise all operations, test error paths, verify round-trip persistence. Untested tests are insufficient here because the tool's external interface (JSON schema, error format, session lifecycle) is the contract being tested, not just internal logic.
   - **Full integration** → Use `Test-EditorBuildAndPlay.ps1` for end-to-end smoke tests
   - **Formatting** → Use `Invoke-ClangFormat.ps1 -Check` on changed files

6. **Each test stage ends with a commit** following the {{PROJECT_NAME}} commit message format:
   ```
   <type>(<scope>): <description>
   ```
   Do NOT include CI tags (`[ci]`, `[ci:linux]`, etc.) on implementation-stage commits. `[ci:linux]` is added automatically at staging time (Stage 8) on the squash-rebased commit.

7. **The pattern is flexible**: The skeleton-test-(implement-test)+ structure is the default, but stages can be freely reordered, combined, or restructured to fit the specific work. For example:
   - Multiple small implementation stages might share a single test stage
   - A risky stage might get two test stages (unit + integration)
   - A purely additive stage (adding comments) might not need its own test stage if the next stage will compile anyway

### 4. Author Stage Documents

Each stage document follows this format:

```markdown
# Stage NNN: <Stage Title>

## Goal
One-sentence description of what this stage accomplishes.

## Prerequisites
- List stages that must be completed first (e.g., "Stage 001 completed and committed")
- Any tools or editor state required

## Inputs
- Files, assets, or data this stage reads or depends on

## Steps

### Step 1: <Action>
Detailed instructions the implementer follows.
Include exact file paths, class names, and code patterns where possible.

### Step 2: <Action>
...

## Outputs
- Files created or modified
- Expected state after completion

## Validation
How to verify this stage succeeded before moving on.
Reference the appropriate script(s):

- **Build check**: `Scripts\Utilities\Invoke-EditorBuild.ps1`
- **Untested tests**: `Scripts\Testing\Invoke-UntestTests.ps1 -TestFilter "<relevant-filter>"`
- **Blueprint compile**: `Scripts\Utilities\Invoke-CompileBlueprints.ps1`
- **Asset validation**: `Scripts\Utilities\Invoke-ValidateAssets.ps1`
- **Smoke test**: `Scripts\Testing\Test-EditorBuildAndPlay.ps1 -SkipBuild`
- **Format check**: `Scripts\Utilities\Invoke-ClangFormat.ps1 -Check -Files "<changed-files>"`
- **Diff regex**: Quick grep/regex on staged diffs for non-compile stages

For the **final validation stage** (the last test stage in the breakdown), also include:
- A code-reading checklist of architectural properties that runtime tests cannot catch: memory safety patterns, lock discipline (acquire/release/touch), transaction scoping, null-safety on weak pointers, TODO comment cleanup, API contract compliance
- Verification that no test artifacts remain in the working tree
- Blueprint compilation check (`Invoke-CompileBlueprints.ps1`) to catch dependency-related breakage

## Commit
Commit message for this stage (only for test stages or stages that produce committed work):
```
<type>(<scope>): <description>
```
Note: Do NOT include CI tags (`[ci]`, `[ci:linux]`, etc.) on stage commits. `[ci:linux]` is added automatically at staging time (Stage 8) on the squash-rebased commit.

## Notes
Any caveats, known issues, or things the implementer should watch for.
```

#### Skeleton Stage (001) Specifics

The skeleton stage must:
- Add all new `.h` and `.cpp` files with class/struct declarations and minimal stubs
- Update relevant `.Build.cs` files with new module dependencies
- Register any new modules in `.uproject` or `.uplugin` files
- Include `#pragma once`, proper includes, and `GENERATED_BODY()` macros
- Provide stub implementations that compile (e.g., empty function bodies, default return values)
- Add `// TODO: Implement in Stage NNN` comments marking where each subsequent stage will work

#### Test Stage Specifics

Test stages fall into two categories. The stage author must choose the appropriate category based on what was implemented.

##### Verification Test Stages (post-skeleton, post-minor-change)

For stages where the implementation is stubs, scaffolding, or low-risk changes:
- Reference the specific validation scripts from `Scripts/`
- Specify the exact command to run, including flags and filters
- Define clear pass/fail criteria
- End with a commit instruction

These are appropriate after skeleton stages, formatting-only changes, or build configuration changes.

##### Functional Test Stages (post-meaningful-implementation)

For stages that implemented behavior a user or external system will interact with — MCP tools, APIs, subsystems, gameplay mechanics with observable effects — the test stage must **actually call the system and verify its outputs**. A build check alone provides false confidence.

Functional test stages must include:

1. **Discovery step**: Use the implemented tool (or Python / related tools) to discover real project data for testing. Do not hardcode asset paths or synthetic fixtures — find them live. This also tests the tool's ability to load and enumerate.

2. **Happy-path exercise**: Call each implemented operation/function with real inputs. Verify the output contains expected data, is correctly formatted, and is non-empty. For multi-operation systems (like session-based MCP tools), exercise the full lifecycle: open → operate → close.

3. **Error-path exercise**: For each operation, call it with at least one invalid input (nonexistent path, invalid ID, missing required param). Verify the tool returns a clean error message — not a crash, not a silent failure, not an unhandled exception. For session-based tools, test: expired/invalid session IDs, double-open on locked resources, operations on closed sessions.

4. **Round-trip verification** (for any system that persists state): Mutate state → persist (save) → close/release → reload from scratch → verify the mutation survived serialization. This is the single most important test for editor tools. If save doesn't round-trip, the tool is broken regardless of what other tests say.

5. **Cross-validation** (recommended): After a mutation verified by the tool under test, confirm the result using an *independent* code path — a different MCP tool, a Python script, or direct file inspection. This catches bugs where the tool correctly writes to its own state representation but doesn't actually persist to the asset.

6. **Fix-iterate loop** (explicit step): After running all tests, catalog every failure. For each: diagnose root cause → fix code → rebuild → re-run failing test → repeat. **Do not commit until all tests pass.** Iteration is expected and normal at this stage — it is cheaper to find bugs here than in later stages or review. Include a "Common bug categories" list specific to what was implemented, to help the implementer triage.

7. **Commit only when clean**: The commit at the end of a functional test stage certifies that the implementation works, not just that it compiles.

##### Choosing the Test Type

| What was implemented | Test type |
|---------------------|-----------|
| Skeleton / stubs / scaffolding | Verification |
| Build.cs / config / formatting changes | Verification |
| MCP tool with external callers | Functional |
| Gameplay ability / mechanic with observable behavior | Functional |
| Data pipeline that persists to disk | Functional |
| Internal refactor with no new behavior | Verification |

When in doubt, use functional. A test stage that over-tests is better than one that under-tests.

### 5. Author the Master Prompt (000)

Create `000-prompt-to-implement-thing.md` in the same directory as the proposal. This document tells an implementer (typically a Claude Code LLM) how to execute the stages:

```markdown
# Implementation Guide: <Proposal Name>

## Overview
Brief description of what is being implemented and why.

## Source Proposal
`$INPUT_PROPOSAL_FILE` — Read this first for full context.

## Branch and Workspace
Created via [begin-work](claireon://instructions/begin-work).

- **Branch**: `<to be filled>`
- **Workspace**: `Saved/Claireon/Workflow/<task-name>/`

## How to Follow the Stages

1. Read this document and the source proposal first
2. Execute stages in numerical order (001, 002, 003, ...)
3. Each stage is a self-contained document: `NNN-stage-name.md`
4. Follow every step in the stage document
5. Run the validation specified at the end of each stage
6. Commit when the stage document says to commit
7. Do not skip ahead — later stages depend on earlier ones

## Stage Summary

| Stage | Name | Type | Description |
|-------|------|------|-------------|
| 001   | ...  | Skeleton | ... |
| 002   | ...  | Test | ... |
| ...   | ...  | ...  | ... |

## Scripts Reference

These scripts are available and should be invoked as specified in each stage:

| Script | Purpose | Example |
|--------|---------|---------|
| `Scripts\Utilities\Invoke-EditorBuild.ps1` | Build the editor (waits for UBT by default) | `-SkipWaitForUBT` |
| `Scripts\Testing\Invoke-UntestTests.ps1` | Run Untested framework tests | `-TestFilter "MySystem"` |
| `Scripts\Testing\Test-EditorBuildAndPlay.ps1` | Full smoke test (build + PIE) | `-SkipBuild` |
| `Scripts\Utilities\Invoke-CompileBlueprints.ps1` | Compile all blueprints | |
| `Scripts\Utilities\Invoke-ValidateAssets.ps1` | Validate asset integrity | |
| `Scripts\Utilities\Invoke-ClangFormat.ps1` | Check/apply code formatting | `-Check -Files "..."` |
| `Scripts\Utilities\Invoke-CleanProject.ps1` | Clean build artifacts | `-IncludePlugins` |
| `Scripts\Utilities\Invoke-FixupRedirectors.ps1` | Fix asset redirectors | |

## Tracking Work

- Update `task-info.md` as work progresses (Changes, Open Questions, Notes sections)
- Each test stage commit marks a checkpoint
- If a stage fails validation, fix the issue before moving on — do not accumulate debt

## Commit Convention

Follow the {{PROJECT_NAME}} commit message format:
```
<type>(<scope>): <description>
```

- **type**: `feat` (new functionality), `fix` (bug fix), `chore` (maintenance)
- **scope**: Primary module affected
- **[ci:linux]**: Do NOT include CI tags on stage commits. `[ci:linux]` is added automatically at staging time (Stage 8) on the squash-rebased commit
- **description**: Terse, passive voice, no trailing period
```

### 6. Commit and Request Review

1. Stage all authored files:
   - `$INPUT_PROPOSAL_FILE` (the original proposal)
   - `000-prompt-to-implement-thing.md`
   - All `NNN-stage-name.md` files
2. Commit with message:
   ```
   chore(<proposal-scope>): work breakdown authored for <proposal-name>
   ```
3. Push the branch
4. Wait for review — do not begin implementation until the breakdown is approved

## Error Handling

- **Proposal is too vague**: Ask the user for clarification on scope, types, and boundaries before breaking down
- **Proposal scope is enormous**: Suggest splitting into multiple proposals, each with its own breakdown
- **Cannot determine type graph**: Document assumptions in stage 001 and flag for review
- **BeginWorkOnNewTask fails**: Follow its error handling, then resume from Step 3
- **Circular dependencies in stages**: Restructure stages to break the cycle; consider combining stages if necessary

## Important Notes

- **Do not implement anything during breakdown**: This instruction only produces documents. Implementation happens when the stages are executed.
- **Stage files are living documents**: The implementer or reviewer may revise stages after initial authoring. The breakdown is a plan, not a contract.
- **Each stage must be independently understandable**: An implementer reading stage 005 should be able to understand what to do without re-reading stages 001-004 (though they should reference prerequisites).
- **Prefer more stages over fewer**: Smaller stages are easier to validate, review, and roll back. A stage that takes more than ~30 minutes to implement is probably too large.
- **Reference existing scripts, never re-implement**: Build via `Invoke-EditorBuild.ps1`, test via `Invoke-UntestTests.ps1`, etc. Do not inline build commands or test logic into stage documents.
- **The 000 file is the entry point**: An implementer starting from scratch reads 000 first, then proceeds through the stages. It must be self-sufficient as a starting guide.
- **Test stages for interactive systems are not build wrappers**: A test stage that only runs `Invoke-EditorBuild.ps1` is appropriate after skeleton stages. For stages implementing MCP tools, APIs, or any system with external callers, the test stage must call the system with real inputs, verify real outputs, test error paths, and verify round-trip correctness for persistent operations. A test stage that doesn't exercise the implementation's behavior provides false confidence and defeats the purpose of the test-after-implement pattern.

## Example Stage Breakdown

For a proposal that adds a new "Spawning" system with spawn points, spawn rules, and wave management:

```
000-prompt-to-implement-thing.md    — Master guide
001-skeleton.md                     — All types: FSpawnPoint, USpawnRule, UWaveManager, etc.
002-test-skeleton.md                — Build + tool registration verification (stubs return expected errors)
003-implement-spawn-points.md       — Flesh out FSpawnPoint and placement logic
004-test-spawn-points.md            — Functional: call spawn API with real level data, verify placement, test error cases, fix-iterate
005-implement-spawn-rules.md        — Flesh out USpawnRule evaluation
006-test-spawn-rules.md             — Functional: exercise rule matching against real gameplay tags, verify filtering, round-trip rule config
007-implement-wave-manager.md       — Flesh out UWaveManager orchestration
008-test-wave-manager.md            — Functional: run wave sequence, verify timing/ordering, test edge cases (empty waves, interrupted sequences)
009-implement-integration.md        — Wire into existing encounter/mission systems
010-test-integration.md             — End-to-end: full spawn workflow in PIE, cross-validate with gameplay debugger, code review checklist
```
