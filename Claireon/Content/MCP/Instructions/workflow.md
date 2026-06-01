---
name: workflow
description: Creation Workflow orchestrator -- detect current stage and execute the next step (plan, refine, fracture, sequence, implement, test, stage, review, finalize)
type: prompt
role: user
---

<!-- claude-hint:
model: opus
effort: max
rationale: Creation Workflow orchestrator; stage decisions require depth reasoning; delegates heavy stages to subagents (which have their own hints)
-->

Do not use instructions from this file unless asked.

> **Token placeholders**: This document and the sub-instructions it invokes use `{{TOKEN}}` placeholders (e.g. `{{PROJECT_NAME}}`, `{{GIT_USER}}`, `{{REPO_URL}}`). Resolve each from your project/git/environment context before acting on or echoing the text -- see the legend at `claireon://instructions/token-legend` (fetch via `resources/read`).

> **Sub-instruction protocol**: When this workflow says to invoke or follow a `claireon://instructions/*` resource, call `resources/read <uri>` on the Claireon server to retrieve the instruction document and then follow its contents.

# Do Next Creation Workflow Step

This instruction is the orchestrator for the **Creation Workflow** — a multi-stage process that takes a feature idea from initial concept through planning, refinement, implementation, testing, and staging for PR. An instance of Claude Code reading this document should be able to detect the current stage and execute the next step, or start from scratch if no workflow is in progress.

## Overview

The Creation Workflow has 9 stages:

```
1. Plan              — Clarify intent, research codebase, produce a work proposal
2A. Refine           — Iterative improvement with user feedback
2B. Fracture         — Break plan into parallel-editable documents
3. Singletonize      — Harden each document to stand alone without ambiguity
4. Loss Checking     — Verify nothing was dropped between original plan and final documents
5. Sequencing        — Turn the plan into ordered implementation stages (skeleton → test → implement → ...)
6. Implementing      — Execute the sequenced stages as commits
7. Testing           — Verify all tests pass on rebased branch, debug failures
8. Staging           — Rebase+squash on latest main, write final commit message with [ci:linux], force-push, create PR
9. Review Gate       — Human reviews and approves the PR; PR merges directly
```

## Stage Model/Effort Hints

When executing a stage that spawns subagents, pass these model/effort overrides to `Agent(...)` calls. The table below captures the default policy; stage sections (1-9) may override per case.

| Stage | Recommended model | Recommended effort | Rationale |
|---|---|---|---|
| 1. Plan | `opus` | `max` | Intent clarification, codebase research, design |
| 2A. Refine | `opus` | `high` | Iterative improvement with feedback |
| 2B. Fracture | `sonnet` | `high` | Mechanical doc splitting w/ context preservation |
| 3. Singletonize | `sonnet` | `high` | Cross-check against codebase, fill gaps |
| 4. Loss Checking | `sonnet` | `medium` | Diff original vs final for dropped content |
| 5. Sequencing | `opus` | `high` | Ordered implementation plan |
| 6. Implementing | per-stage | per-stage | Each `NNN-stage-name.md` gets its own hint (mostly `sonnet`/`medium`; `opus`/`high` for architecturally sensitive stages) |
| 7. Testing | `sonnet` | `medium` | Build + test execution, failure triage |
| 8. Staging | `sonnet` | `medium` | Squash rebase + PR create; needs care because force-push is destructive |
| 9. Review Gate | `haiku` | `low` | CI poll + status summary |

## Git Command Convention

**Use `git -C <repo>` instead of `cd <repo> && git ...`** for all git commands. This avoids compound shell commands (`cd && git`) which trigger unnecessary permissions prompts in Claude Code, even for trusted repositories. Determine the repo path from the current working directory or worktree root once, then pass it via `-C` for every git invocation.

**Invocation protocol**: Every time this document is invoked, the orchestrator must:
1. Detect the current workflow stage (see [State Detection](#state-detection))
2. Describe what the next action will be to the user
3. Wait for user approval before executing (unless the user has opted into auto-advance)
4. Execute the stage
5. Update `WORKFLOW_STATE.json` with the new state
6. Report results and what comes next

---

## Task Workspace

All workflow documentation for a task lives in a local workspace directory:

```
Saved/Claireon/Workflow/<task-name>/
  WORKFLOW_STATE.json         — primary state tracker
  task-info.md                — replaces an external task tracker: goal, details, progress notes
  <proposal>.md               — plan document (created in Stage 1)
  <fractured-docs>/           — subdirectory created during fracture (Stage 2B)
  stages/                     — stage files created during sequencing (Stage 5)
    000-prompt-to-implement-thing.md
    001-skeleton.md
    ...
```

`Saved/` is not committed to git. Only implementation code (and its test/asset changes) is committed. Workflow tracking is purely local.

## State File: WORKFLOW_STATE.json

Located at `Saved/Claireon/Workflow/<task-name>/WORKFLOW_STATE.json`. This is the primary source of truth for workflow state.

### Schema

```json
{
  "version": 1,
  "stage": "plan|refine|fracture|singletonize|loss_check|sequencing|implementing|testing|staging|review_gate|complete",
  "stage_detail": "Free-text description of where within the stage we are",
  "task_name": "kebab-case task name (matches workspace directory name)",
  "task_workspace": "Saved/Claireon/Workflow/<task-name>",
  "plan_document": "path to the primary plan document within the workspace",
  "branch": "git branch name for this workflow",
  "pr_url": "Pull request URL (created during staging)",
  "created": "ISO 8601 timestamp of workflow start",
  "last_updated": "ISO 8601 timestamp of last state change",
  "auto_advance": false,
  "design_questions_block_auto_advance": false,
  "refinement_history": [
    {
      "timestamp": "ISO 8601",
      "action": "description of what was done",
      "user_feedback": "summary of user feedback that prompted this iteration"
    }
  ],
  "fractured_documents": ["list of document paths relative to workspace"],
  "sequenced_stages": ["list of stage file paths relative to workspace"],
  "implementation_progress": {
    "current_stage": "NNN",
    "completed_stages": ["001", "002"],
    "failed_stages": []
  }
}
```

### State File Location

`Saved/Claireon/Workflow/<task-name>/WORKFLOW_STATE.json`. The task name matches the workspace directory and the git branch suffix.

---

## State Detection

When invoked, determine the current stage using this priority order:

### 1. Primary: Read WORKFLOW_STATE.json

Search for `WORKFLOW_STATE.json` in `Saved/Claireon/Workflow/*/WORKFLOW_STATE.json`.
If multiple exist, use the one whose `branch` matches the current git branch.

If found, read it and trust the `stage` field. Validate by spot-checking:
- Does the `branch` match the current git branch?
- Does `task_workspace` point to an existing directory?
- Do the referenced files still exist in the workspace?
- If validation fails, warn the user and offer to re-detect via heuristics

### 2. Fallback: Heuristic Detection

If no `WORKFLOW_STATE.json` is found in `Saved/Claireon/Workflow/`:

| Condition | Detected Stage |
|-----------|---------------|
| On a parking branch (`*-parking-*`) or `main` | **No workflow in progress** — start Stage 1 |
| On a work branch, no `Saved/Claireon/Workflow/` tasks exist | **Stage 1** — planning not yet started |
| Workspace exists with plan doc but no `*_REVIEW.md` and no fractured docs | **Stage 2A** — ready for refinement |
| Workspace exists with plan doc and fractured subdirectory | **Stage 2B → 3** — check if documents are self-contained |
| Workspace `stages/` directory has numbered stage files (`NNN-*.md`) | **Stage 5 complete → 6** — ready for implementation |
| Stage files exist and some implementation commits are on branch | **Stage 6 → 7** — in progress or ready for testing |
| All tests referenced in stage files appear to pass | **Stage 7 → 8** — ready for staging |
| PR exists and branch is unsquashed | **Stage 8** — resume staging |
| PR exists, branch is squashed, not yet approved | **Stage 9** — awaiting human review |
| PR exists and is approved | **Stage 9** (terminal) — merge directly |

When using heuristic detection, **always confirm the detected stage with the user** before proceeding.

### 3. Fresh Start

If on a parking branch or main with no workflow state:
- Inform the user that no workflow is in progress
- Ask if they want to start a new Creation Workflow (Stage 1)
- If yes, proceed to Stage 1

### 4. Phase 0.2 Supervisor Merge-Conflict Reset (Belt-and-Suspenders)

When this document is invoked by a dispatched session that is resuming, Phase 0.2 of `Scripts/Instructions/DispatchWorkItem.md` performs an operator-comment scan on the Assigned card before handing control here. As part of that scan, if the most recent `[AUTOMATION]` comment on the card announces a merge-conflict reset (the comment mentions `mergeable=CONFLICTING` and states that `Status` was reset to `Workflow:implementing`), the resumed session is specifically coming back to rebase on main and resolve conflicts.

In that case, before running State Detection above, set `WORKFLOW_STATE.json` `stage` to `"implementing"` if it is currently further ahead (e.g., `"complete"` or `"staging"`). Do **not** jump directly to Stage 7 -- the normal `implementing -> testing` control loop handles the rebase via Stage 7.1 (`git fetch origin main && git rebase origin/main`), and stepping back through `implementing` first preserves loss-checking / sequencing invariants if the conflict turns out to require plan changes.

This is a documentary reinforcement of the Phase 0.2 paragraph; the actual logic lives in `DispatchWorkItem.md`. Sessions invoked outside the dispatcher (direct `/workflow` calls with no Assigned card) should skip this check.

---

## Stage 1: Plan

**Entry condition**: No active workflow, or `stage == "plan"`
**Goal**: Clarify the user's intent and produce a grounded work proposal document

### Process

#### 1.1 Clarify Intent

Engage the user in an iterative Q&A to fully specify what they want:
- What is the feature or change?
- What is the desired user experience?
- What existing systems does this touch?
- What are the boundaries / what is explicitly out of scope?
- Are there performance, networking, or platform constraints?

Ask questions until the request is unambiguous. Use `AskUserQuestion` for enumerable choices, free-text for open-ended exploration. **Do not stop at the first answer** — dig into implications, edge cases, and integration points.

#### 1.2 Research

Using the {{PROJECT_NAME}} source code, Unreal Engine source (at `{{UNREAL_ENGINE_ROOT}}`), and MCP editor tools:
- Identify existing systems the feature must integrate with
- Find relevant classes, interfaces, and data assets
- Understand the current architecture in the affected area
- Identify potential conflicts or constraints
- Note any existing patterns that the implementation should follow

This research must produce **concrete references** to actual code — not guesses about what might exist. The plan should be grounded in the real codebase.

#### 1.3 Write the Work Proposal

Create a markdown document in the task workspace at `Saved/Claireon/Workflow/<task-name>/<proposal-name>.md` (e.g., `spawning-proposal.md`). The proposal should contain:

- **Goal**: What this achieves and why it matters
- **Scope**: What's included and explicitly excluded
- **Architecture**: How this fits into the existing system, which modules are affected
- **Technical Design**: Key classes, interfaces, data flow, and integration points — referencing actual existing code
- **Dependencies**: What must exist before this can be built
- **Risks**: What could go wrong, what's uncertain
- **Open Questions**: Anything not yet resolved

The proposal should be detailed enough that a senior engineer could evaluate feasibility, but high-level enough that it describes *what* and *why* rather than dictating *how* line-by-line. Avoid writing implementation code in the proposal.

#### 1.4 Present for Review

Show the proposal to the user. If they approve, proceed to create a work branch.

#### 1.5 Create Task Workspace and Work Branch

Invoke **[begin-work](claireon://instructions/begin-work)** to:
- Create `Saved/Claireon/Workflow/<task-name>/` with `task-info.md`
- Create a new work branch and push it

#### 1.6 Update State

Create `Saved/Claireon/Workflow/<task-name>/WORKFLOW_STATE.json` with `stage: "refine"`, `task_name`, `task_workspace`, `plan_document` (relative path within workspace), and `branch`.

**Exit condition**: Plan document written in workspace, work branch created and pushed, state file created.

---

## Stage 2A: Refine

**Entry condition**: `stage == "refine"`
**Goal**: Iteratively improve the plan with user feedback

### Important Principles

- **Refinement requires fresh user input**. Running automated tools (like RefineProposal) in a loop without user feedback does not improve the plan — it tends toward "implementation-as-instructions" where code ends up written inside the plan document.
- The plan should remain a **high-level design document**, not a set of copy-paste instructions.
- Each refinement cycle should incorporate the user's perspective, domain knowledge, or changed requirements.

### Design Question Blocking Mode

When `design_questions_block_auto_advance` is true in WORKFLOW_STATE.json, design questions discovered during refinement are treated as blocking events rather than interactive prompts. This mode is used by automated orchestrators that handle operator interaction externally.

In this mode:

1. **Skip the interactive options menu.** Do not present choices 1-6 or wait for user input.
2. **Run the automated review directly**: invoke [refine-proposal](claireon://instructions/refine-proposal) against the plan document (path is in `WORKFLOW_STATE.json` field `plan_document`).
3. **Extract design questions from the review output**: look for items under the `## Design Questions` heading in the generated review document. These are tagged `[D1]`, `[D2]`, etc. Report them verbatim in your response.
4. **Report issue counts by severity**: parse the review for critical (`[C*]`), design (`[D*]`), consistency (`[I*]`), assumption (`[A*]`), scope/risk (`[R*]`), and minor (`[M*]`) items. Provide a count for each category.
5. **Do NOT attempt to resolve design questions yourself.** The caller is responsible for routing design questions to a human operator. Your job is to surface them accurately.
6. **Do NOT use AskUserQuestion.** In this mode, there is no interactive user. All questions must be routed through the caller's own resolution mechanism.

If the review contains design questions, your response MUST include a clearly delimited section using this exact format:

```
DESIGN_QUESTIONS:
[D1] <title>: <full question text>
[D2] <title>: <full question text>
...
END_DESIGN_QUESTIONS
```

This structured output allows the caller to parse design questions reliably. The delimiters `DESIGN_QUESTIONS:` and `END_DESIGN_QUESTIONS` must appear on their own lines. Each `[DN]` entry must include the full question text from the review, not a summary.

If answers from previous design questions are provided in the stage directive (as "resolved decisions"), incorporate those decisions into the proposal document before running the review. Specifically: for each resolved decision, find the corresponding section of the proposal and update it to reflect the operator's choice. Then run the review on the updated proposal. For any `[DEFERRED]` items in the directive, leave them as-is in the proposal and do not re-raise them as design questions.

### Process

When entering this stage, present the user with their options:

> **The plan is ready for refinement.** How would you like to proceed?

Offer these tools/actions:

1. **Run automated review** — Invoke [refine-proposal](claireon://instructions/refine-proposal) to catch internal inconsistencies, verify source code references, and generate a structured review document. Useful as a starting point for discussion, not as a final product.

2. **Generate architecture visualization** — Invoke [architecture-viz](claireon://instructions/architecture-viz) to produce an interactive diagram of the systems involved. Helps the user see the design spatially.

3. **Generate mermaid diagram** — Produce a mermaid chart for a specific aspect of the design (data flow, class hierarchy, sequence diagram, state machine). Embed it in the plan or present it separately.

4. **Update task-info.md** — Sync the current plan state into `Saved/Claireon/Workflow/<task-name>/task-info.md` (goal, decisions made, open questions).

5. **Provide feedback directly** — The user gives free-text feedback, asks questions, or requests specific changes to the plan.

6. **Mark refinement complete** — The user is satisfied with the plan and wants to move to the next stage.

After each refinement cycle:
- Record the iteration in `WORKFLOW_STATE.json`'s `refinement_history` array
- Update the plan document with changes
- Ask the user what they'd like to do next (loop back to options above)

### task-info.md Sync (Before Marking Complete)

**Before marking refinement complete**, ensure `task-info.md` contains a comprehensive summary of the plan. This is the record future-you will reference when returning to the task:

1. **Goal** — what the work achieves and why
2. **Scope** — what's in scope and explicitly out of scope
3. **Technical design** — per-feature summaries with file impacts and key API shapes
4. **Key decisions** — design choices made and why
5. **Risks** — what could go wrong, with mitigations

**Exit condition**: User explicitly marks refinement complete AND task-info.md is up to date. Update state to `stage: "fracture"`.

---

## Stage 2B: Fracture

**Entry condition**: `stage == "fracture"`
**Goal**: Break the plan into multiple documents to enable parallel editing and clear ownership of sections

### When to Fracture

Fracturing is required when:
- The plan is complex enough that editing one section could inadvertently affect another
- Multiple aspects of the plan benefit from independent, focused attention
- The eventual implementation will have clear module boundaries that map to document boundaries

### Process

#### 2B.1 Design the Document Structure

Analyze the plan and identify natural boundaries:
- By module or system (e.g., `SPAWNING_spawn-points.md`, `SPAWNING_wave-manager.md`)
- By concern (e.g., `SPAWNING_architecture.md`, `SPAWNING_networking.md`, `SPAWNING_data-assets.md`)
- By implementation phase (if the plan has clear phases)

Each fractured document should:
- Have a clear, non-overlapping scope
- Reference (but not duplicate) content from other documents
- Be editable independently without breaking coherence

#### 2B.2 Update task-info.md

Update `Saved/Claireon/Workflow/<task-name>/task-info.md` to reflect that fracturing is underway:
- Add a note listing the planned fractured document names
- This is the running record of the work structure

#### 2B.3 Execute the Fracture

All fracture work happens within `Saved/Claireon/Workflow/<task-name>/`:

1. Create a subdirectory inside the workspace (e.g., `Saved/Claireon/Workflow/<task-name>/SPAWNING/`)
2. Move the original plan document into the subdirectory as the "master" or "overview" document
3. Create individual documents for each identified section
4. Add cross-references between documents
5. Update `WORKFLOW_STATE.json` with `fractured_documents` list (paths relative to workspace root)

#### 2B.4 Parallel Refinement (Optional)

After fracturing, individual documents can be refined using sub-agents working in parallel. Each sub-agent:
- Reads only its assigned document plus the overview
- Makes focused improvements
- Does not modify other documents

This is optional and depends on the user's preference and the complexity of the plan.

**Exit condition**: Plan is fractured into self-contained documents in the workspace. Update state to `stage: "singletonize"`.

---

## Stage 3: Singletonize

**Entry condition**: `stage == "singletonize"`
**Goal**: Harden every document so it can be implemented with zero ambiguity by a less-capable agent

### Principles

- This is **not further iteration on the design**. The design decisions are made. This stage clarifies what is written.
- Every document must contain everything needed to implement its scope without consulting other documents (though it may reference them for context).
- Assumptions and prerequisites must be made explicit.
- Vague language ("as needed", "appropriate", "similar to") must be replaced with specifics.

### Process

For each document in the plan:

#### 3.1 Verify Against Codebase

- Confirm every referenced class, function, module, and asset path still exists in the current codebase
- Verify that described interfaces match actual signatures
- Check that assumed behaviors match actual implementations
- Update any stale references

#### 3.2 Expose Assumptions

Scan for implicit assumptions and make them explicit:
- "This will use the existing X" → verify X exists and describe exactly how it will be used
- "The system handles Y" → specify which system, which function, what inputs/outputs
- "Standard Lyra/GAS pattern" → name the specific pattern and reference an example in the codebase

#### 3.3 Fill Gaps

Identify and fill any missing information:
- Missing error handling descriptions
- Unspecified edge cases
- Unclear ordering of operations
- Missing data types or struct definitions
- Unaddressed networking/replication concerns

#### 3.4 Simplify for Implementation

Rewrite complex descriptions so they are implementable as direct instructions:
- Replace "consider using X or Y" with a decision: "use X because [reason]"
- Replace "the system should handle [vague scenario]" with specific behavior descriptions
- Ensure every described behavior maps to a concrete implementation action

#### 3.5 Commit

Commit the hardened documents with a descriptive message.

**Exit condition**: All documents are self-contained, unambiguous, and verified against the codebase. Update state to `stage: "loss_check"`.

---

## Stage 4: Loss Checking

**Entry condition**: `stage == "loss_check"`
**Goal**: Verify that nothing from the original plan or refinement feedback was dropped, ignored, or distorted

### Process

#### 4.1 Gather the Record

Collect the full history of the plan's evolution:
1. **Original plan**: The proposal document as first written in Stage 1 (in `Saved/Claireon/Workflow/<task-name>/`). The `refinement_history` in `WORKFLOW_STATE.json` records all changes made since.
2. **Refinement feedback**: Read `refinement_history` entries in `WORKFLOW_STATE.json` for descriptions of each iteration and what user feedback drove it.
3. **Current state**: The hardened, singletonized documents in the workspace.

#### 4.2 Compare

For each piece of the original plan and each piece of refinement feedback:
- Is it present in the final documents?
- If it was intentionally removed or changed, is the reason documented?
- If it was split across multiple documents, is the full intent preserved?

#### 4.3 Report

Produce a brief **loss report** documenting:
- Items preserved faithfully
- Items modified (with justification)
- Items intentionally dropped (with justification)
- Items **unintentionally dropped** (these need to be restored or explicitly addressed)

If unintentional losses are found:
- Present them to the user
- Ask whether to restore them or explicitly mark them as out of scope
- Update the documents accordingly

#### 4.4 Commit

Commit any restorations or clarifications.

**Exit condition**: All plan content is accounted for. Update state to `stage: "sequencing"`.

---

## Stage 5: Sequencing

**Entry condition**: `stage == "sequencing"`
**Goal**: Turn the plan into an ordered sequence of implementable stages

### Process

Invoke **[sequencing](claireon://instructions/sequencing)** with the plan document(s) as input.

### Important Modifications for This Workflow

The sequencing step from BreakDownWorkProposal.md should be applied with these additional considerations:

#### Test Rigor Depends on Implementation Structure

- **Sequential, stacking implementation** (each phase builds on the last): Tests at each stage must **actually verify** that the implementation compiles and functions correctly before moving on. This is test-verified implementation — not test-driven development, but each layer must be solid before the next is added.

- **Independent, parallel implementation** (phases are unrelated to each other, e.g., individual State Tree nodes, individual MCP tools): **Build verification** can be batched — a single build check after implementing several independent units is fine, since fixing a compile error in unit A won't affect unit B. However, **functional testing** should not be deferred if the individual units have behavioral depth (session management, persistence, error handling). Each unit that has external callers or persists state still needs its own functional test stage, even if the units are independent of each other. What can be deferred is only the integration test across units.

The sequencing step should explicitly identify which pattern applies and annotate the stage files accordingly.

#### Entry Point

The output must include `000-prompt-to-implement-thing.md` (or equivalent named `000-*.md`) as the single entry point. This file must:
- Reference the source proposal for full context
- List all stages in order with brief descriptions
- Include the branch name and workspace path (`Saved/Claireon/Workflow/<task-name>/`)
- Provide the scripts reference table
- Explain the test rigor expectations for this specific breakdown

### Post-Sequencing

After BreakDownWorkProposal produces the stage files:
1. Review the breakdown for completeness
2. Ensure stage files are placed in `Saved/Claireon/Workflow/<task-name>/stages/`
3. Update `WORKFLOW_STATE.json` with `sequenced_stages` list (paths relative to workspace)
4. Present the breakdown to the user for review

**Exit condition**: Stage files are in the workspace and reviewed. Update state to `stage: "implementing"`.

---

## Stage 6: Implementing

**Entry condition**: `stage == "implementing"`
**Goal**: Execute the sequenced stages as commits

### Process

#### 6.1 Determine Starting Point

Read `WORKFLOW_STATE.json`'s `implementation_progress` to find where to resume:
- If `current_stage` is set, resume from that stage
- If no progress recorded, start from stage `001`

#### 6.2 Execute Stages

For each stage, in order:

1. Read the stage document (`NNN-stage-name.md`)
2. **Use sub-agents** for implementation — each stage should be delegated to a sub-agent with the stage document, the source proposal, and relevant context. This preserves the orchestrator's context window for coordination.
3. **Cost optimization**: Read the stage document's front-of-file `<!-- claude-hint -->` block if present, or check the hint table in this document's "Stage Model/Effort Hints" section. Pass matching `model:`/`effort:` values to the subagent invocation via `Agent({ model: "...", ... })`. If no hint is available, default to `sonnet`/`medium`.
4. After the sub-agent completes, verify the stage's validation criteria
5. If validation passes, commit per the stage document's instructions
6. Update `implementation_progress` in `WORKFLOW_STATE.json`
7. If validation fails, attempt debugging (possibly with a higher-cost sub-agent), then retry

#### 6.3 Progress Tracking

After each stage:
- Update `WORKFLOW_STATE.json` with progress
- Append a brief note to the **Notes** section of `task-info.md` documenting the completed stage (what was done, any surprises)
- If a stage fails repeatedly (3+ attempts), mark it as failed and inform the user

#### 6.4 Make Maximum Progress

The implementing stage should push through as many stages as possible in a single invocation. Do not stop after each stage to ask the user — keep going until:
- All stages are complete
- A stage fails and cannot be resolved
- The context window is getting full (delegate more aggressively to sub-agents)
- The user intervenes

**Exit condition**: All implementation stages complete. Update state to `stage: "testing"`.

---

## Stage 7: Testing

**Entry condition**: `stage == "testing"`
**Goal**: Verify all tests pass on a rebased branch

### Process

#### 7.1 Rebase on Latest Main

```bash
git -C <repo> fetch origin main
git -C <repo> rebase origin/main
```

If conflicts arise, resolve them. This is part of the "test" — the implementation must work with the latest codebase, not just the snapshot it was developed against.

#### 7.2 Build Verification

Prefer remote build verification:
```powershell
Scripts\Utilities\Invoke-RemoteBuildVerification.ps1 -Wait
```

This has a 90-minute timeout. If the remote build does not complete in time (e.g., network issues or a stalled remote queue), fall back to a local build:
```powershell
Scripts\Utilities\Invoke-EditorBuild.ps1
```

Fix any build errors.

#### 7.3 Run All Relevant Tests

Execute the test suites appropriate for the changes:
- `Scripts\Testing\Invoke-UntestTests.ps1` with relevant filters
- `Scripts\Utilities\Invoke-CompileBlueprints.ps1`
- `Scripts\Utilities\Invoke-ValidateAssets.ps1`
- `Scripts\Testing\Test-EditorBuildAndPlay.ps1` for smoke tests

#### 7.4 Debug and Fix Failures

For each test failure:
1. Diagnose the root cause
2. Fix the implementation
3. Re-run the failing test
4. Ensure the fix doesn't break other tests

Use the `debug-agent` sub-agent for complex failures. Use the `testing-agent` for test analysis.

- If the failure is on linux-build-server-v2 while linux-build and windows-build pass, run the v2 triage checklist before iterating: it names the four v2-only failure classes (duplicate .cpp basenames, non-static free functions across TUs, coroutine promise_type mismatches, Linux-clang-strict patterns) and prescribes a static-analysis pass on the diff as the first move. See DispatchWorkItem.md -- Linux-Build-Server-v2 Failure Triage.

#### 7.5 Commit Fixes

Commit any fixes as additional commits (they'll be squashed in staging).

**Exit condition**: All tests pass on the rebased branch. Update state to `stage: "staging"`.

---

## Stage 8: Staging

**Entry condition**: `stage == "staging"`
**Goal**: Rebase+squash on latest main, write the final commit with `[ci:linux]`, force-push the merge-ready branch, create the PR

Workflow documents live in `Saved/Claireon/Workflow/<task-name>/` and were never committed to the branch, so there is nothing to archive or delete from git. Only the implementation commits need to be staged.

### Process

#### 8.1 Update task-info.md

Update `Saved/Claireon/Workflow/<task-name>/task-info.md`:
- Add a **Changes** entry describing what was implemented
- Note the PR will be linked once created
- Confirm the task-info.md accurately reflects what was built

#### 8.2 Rebase and Squash on Latest Main

Fetch the latest main and rebase+squash into a single commit:

```bash
git -C <repo> fetch origin main
git -C <repo> rebase -i origin/main
```
(Mark all commits except the first as `squash`)

**Idempotency check**: If HEAD is already exactly one commit ahead of
`origin/main` (a previous Stage 8.4 run already squashed), skip this step
and proceed to 8.3. Detect via:

```bash
git -C <repo> rev-list --count origin/main..HEAD
```

If the output is `1`, the squash was already applied -- proceed directly to
8.3 to verify/finalize the commit message. If the output is `0`, the branch
is already on main (nothing to stage); abort with an error. If the output
is `>= 2`, run the rebase as normal.

If conflicts arise during rebase, resolve them carefully -- main may have
moved significantly since implementation began.

#### 8.3 Write the Squashed Commit Message

The squashed commit message MUST follow this format:

```
<type>(<scope>) [ci:linux]: <description>

<optional body>

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
```

Format requirements:
- `<type>(<scope>) [ci:linux]: <description>` is REQUIRED on the title line
- `[ci:linux]` is REQUIRED unless the entire diff is asset-only with zero C++
  changes. Verify by running:
  ```bash
  git -C <repo> diff --name-only origin/main...HEAD -- '*.cpp' '*.h'
  ```
  If this produces output, `[ci:linux]` is required.
- `Co-Authored-By` trailer is required in the body
- Description should be concise but descriptive

Example:
```
feat(spawning) [ci:linux]: spawn system with wave management and point-based placement

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
```

#### 8.4 Pre-flight [ci:linux] Verification (Defensive)

Before force-pushing, verify the squashed commit's subject contains
`[ci:linux]`. This is the last-chance catch before the finalized commit
leaves the local machine.

1. Read the HEAD commit subject:
   ```bash
   git -C <repo> log -1 --pretty=%s
   ```
   If the subject contains the literal string `[ci:linux]`, proceed to 8.5.

2. Check if C++ files were changed on this branch:
   ```bash
   git -C <repo> diff --name-only origin/main...HEAD -- '*.cpp' '*.h'
   ```
   If the command produces no output, proceed to 8.7 -- non-C++ changes do
   not need Linux CI.

3. Amend HEAD to insert `[ci:linux]`: read the full commit message with
   `git -C <repo> log -1 --pretty=%B`, insert `[ci:linux]` at the end of
   the subject line (before any blank line separating it from the body),
   then amend:
   ```bash
   git -C <repo> commit --amend -m "<updated-subject>" --no-edit
   ```
   Use `git -C <repo> commit --amend` (interactive) if the message has a body
   that must be preserved verbatim.

#### 8.5 Force-Push the Squashed Branch and Create the PR

Push the merge-ready commit to the remote and open the PR:

```bash
git -C <repo> push -u origin <branch-name> --force-with-lease
```

Modern git (>= 2.30) treats a missing remote ref as "no expected value" and
succeeds with a normal fast-forward push, so no fallback path is required for
the first push of a new branch. Subsequent re-staging passes (e.g., after a
Workflow:testing -> Workflow:staging cycle) use `--force-with-lease` against
the existing remote ref.

Then create the PR using `gh`:

```bash
gh pr create --title "<concise title>" --body "$(cat <<'EOF'
## Summary
- <bullet points describing the changes>

## Work Tracking
- Workspace: Saved/Claireon/Workflow/<task-name>/

## Test plan
- [ ] Build verification passes
- [ ] Untested tests pass with relevant filters
- [ ] Functional tests pass (tools exercised with real data, error paths tested, round-trip verified)
- [ ] Blueprint compilation succeeds
- [ ] Asset validation passes
- [ ] Editor smoke test (PIE) succeeds

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

After PR creation:
- Update `task-info.md` with the PR link in the **Changes** section
- Update `WORKFLOW_STATE.json` with `pr_url`

```
✓ Staging Complete
  Branch:    <branch-name>
  PR:        <pr-url>
  Workspace: Saved/Claireon/Workflow/<task-name>/
  Commit:    <type>(<scope>) [ci:linux]: <description>

  The squashed merge-ready commit has been pushed and the PR is open
  for human review.
```

**Exit condition**: PR created, task-info.md updated. Update `WORKFLOW_STATE.json` to `stage: "review_gate"`.

---

## Stage 9: Review Gate

**Entry condition**: `stage == "review_gate"`
**Goal**: Wait for human review and approval of the PR

### Process

This is a **human gate**. The PR must be reviewed and approved by a human reviewer before it can merge.

#### 9.1 Check PR Status

When the workflow is invoked at this stage, check the PR status:

```bash
gh pr view <pr-number> --json state,reviewDecision,statusCheckRollup --repo <owner/repo>
```

Alternatively, use `Scripts\Utilities\Wait-PRTests.ps1` to monitor CI:
```powershell
Scripts\Utilities\Wait-PRTests.ps1 -PRNumber <pr-number>
```

#### 9.2 Status Reporting

If the PR is **not yet approved**:
```
📍 Review Gate — Awaiting human approval
   PR:        <pr-url>
   Review:    <pending/changes_requested/approved>
   CI:        <passing/pending/failing>

   The PR needs human review and approval before it can merge.
   Once approved, the operator merges from the GitHub PR UI.
```

If CI is **failing** on the unsquashed branch:
- Diagnose and fix failures
- Push fixes as additional commits (preserving review history)
- Re-check CI

If the PR **has been approved** and CI is passing:
```
✓ PR approved and CI passing — ready to merge.

The PR is squashed, archived, and merge-ready. The operator merges from
the GitHub PR UI. No further workflow steps are required.
```

**Exit condition**: PR approved by a human reviewer AND CI passing. Update state to `stage: "complete"`. The workflow is terminal -- the operator merges from the GitHub PR UI.

---

## User Interaction Protocol

### Before Every Stage Transition

Unless `auto_advance` is `true` in `WORKFLOW_STATE.json`, present the user with:

```
📍 Current stage: <stage name>
   <brief description of current state>

⏭️  Next action: <stage name>
   <description of what will happen>

Proceed?
```

Options:
- **Yes, proceed** — Execute the next stage
- **Skip to stage...** — Jump to a specific stage (with warning about skipped steps)
- **Enable auto-advance** — Stop asking for approval, just proceed (sets `auto_advance: true`)
- **Describe in more detail** — Explain what the next stage involves before committing

### When the User Returns After a Break

When this document is invoked and a workflow is already in progress:

```
📍 Resuming Creation Workflow
   Plan:    <plan document name>
   Branch:  <branch name>
   Stage:   <current stage> — <stage detail>
   Last updated: <timestamp>

What would you like to do?
```

Options:
- **Continue where we left off** — Proceed with the detected next action
- **Review current state** — Show the plan, progress, and any open issues
- **Go back to stage...** — Return to an earlier stage for revision
- **Abandon workflow** — Archive state and return to parking/main

---

## Relationship to Existing Instructions

This orchestrator delegates to existing Scripts/Instructions/ documents where they exist:

| Stage | Delegates To | Notes |
|-------|-------------|-------|
| 1 (Plan) | `claireon://instructions/begin-work` | Creates workspace + branch |
| 2A (Refine) | `claireon://instructions/refine-proposal` | As one of several refinement tools |
| 2A (Refine) | `claireon://instructions/architecture-viz` | Optional visualization |
| 5 (Sequencing) | `claireon://instructions/sequencing` | Primary sequencing engine |
| 9 (Review Gate) | `Scripts\Utilities\Wait-PRTests.ps1` | CI monitoring during review (project-specific) |

Stages 3 (Singletonize), 4 (Loss Checking), 6 (Implementing), 7 (Testing), and 9 (Review Gate) do not yet have standalone instruction documents. Their logic is defined entirely within this orchestrator. If standardized scripts are later created for these stages, this document should be updated to delegate to them.

---

## Error Handling

- **WORKFLOW_STATE.json is corrupted or inconsistent**: Warn the user, offer to re-detect state via heuristics or start fresh
- **Referenced files are missing**: Check git history for the files; they may have been moved or deleted. Ask the user before reconstructing.
- **Branch has diverged from expected state**: Run `git -C <repo> log` to understand what happened. If commits exist that the workflow didn't create, ask the user about them.
- **Workspace directory missing**: Recreate `Saved/Claireon/Workflow/<task-name>/` and re-create `task-info.md` from WORKFLOW_STATE.json fields.
- **Sub-agent fails during implementation**: Record the failure, try once more with additional context, then escalate to the user
- **User wants to change the plan mid-implementation**: This is valid. Return to Stage 2A (Refine) with the understanding that completed implementation stages may need to be revisited. Update `WORKFLOW_STATE.json` accordingly.

---

## Example: Full Workflow Execution

### Invocation 1: Starting Fresh

User is on `llm/{{GIT_USER}}/{{PROJECT_NAME}}-parking-20260218` (a parking branch).

```
📍 No active Creation Workflow detected
   Currently on parking branch: llm/{{GIT_USER}}/{{PROJECT_NAME}}-parking-20260218

Would you like to start a new Creation Workflow?
```

User: "Yes, I want to add a new spawning system with wave management"

→ Execute Stage 1 (Plan): Ask clarifying questions, research codebase, write `SPAWNING_PROPOSAL.md`, create branch, create `WORKFLOW_STATE.json`.

### Invocation 2: Continuing After Planning

User invokes this document again. State file shows `stage: "refine"`.

```
📍 Resuming Creation Workflow
   Plan:    SPAWNING_PROPOSAL.md
   Branch:  llm/{{GIT_USER}}/{{PROJECT_NAME}}/6203-spawning-system
   Stage:   refine — Plan written, awaiting refinement

⏭️  Next action: Refine
   Present refinement options (automated review, visualization, direct feedback, etc.)

Proceed?
```

User: "Run the automated review first, then I'll give feedback"

→ Execute RefineProposal.md, present review, collect user feedback, update plan, record iteration.

### Invocation 3: Picking Up Mid-Implementation

User invokes this document. State file shows `stage: "implementing"`, `current_stage: "005"`.

```
📍 Resuming Creation Workflow
   Plan:    SPAWNING/SPAWNING_PROPOSAL.md
   Branch:  llm/{{GIT_USER}}/{{PROJECT_NAME}}/6203-spawning-system
   Stage:   implementing — Stage 005 (implement-spawn-rules) in progress
   Last updated: 2026-02-17T14:30:00Z
   Completed: 001, 002, 003, 004
   Remaining: 005, 006, 007, 008, 009, 010

⏭️  Next action: Continue implementing from Stage 005

Proceed?
```

→ Resume implementation from Stage 005, using sub-agents, pushing through as many stages as possible.

### Invocation 4: PR Approved, Ready to Merge

User invokes this document. State file shows `stage: "review_gate"`.
PR has been approved and is squashed on latest main.

```
📍 Resuming Creation Workflow
   Plan:    SPAWNING/SPAWNING_PROPOSAL.md
   Branch:  llm/{{GIT_USER}}/{{PROJECT_NAME}}/6203-spawning-system
   Stage:   review_gate — PR approved, CI passing
   PR:      {{REPO_URL}}/pull/42

✓ PR approved and CI passing — ready to merge.

The PR is squashed, archived, and merge-ready. The operator merges from
the GitHub PR UI. No further workflow steps are required.
```

→ Update state to `stage: "complete"` and exit. The operator clicks
Merge in the GitHub PR UI; the supervisor's existing `Ready to Merge` /
`Queued` / merge-detection logic handles the rest.
