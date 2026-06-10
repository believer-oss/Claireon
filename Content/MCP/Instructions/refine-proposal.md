---
name: Refine Proposal
description: Critically review a work proposal via Q&A dialogue and produce a structured review document with categorized findings
type: resource
uri: claireon://instructions/refine-proposal
---

<!-- claude-hint:
model: opus
effort: high
rationale: Multi-turn Q&A review; cross-references source; deep architectural judgment
-->

Do not use instructions from this file unless asked.

# Refine Proposal

This instruction defines a review workflow for critically evaluating a work proposal document. The reviewer assumes the role of a senior principal gameplay engineer on a co-op team-based multiplayer action game. Before producing the final review, the reviewer engages the author in a clarifying Q&A dialogue to build deep understanding of the proposal's intent, then produces a review document that reflects that understanding -- including specific guidance on how the proposal could be updated to better communicate its goals.

## Overview

The workflow:
- Reads and deeply analyzes an input proposal document (`$DOCUMENT`)
- Evaluates it from the perspective of a senior principal gameplay engineer responsible for shipping a co-op multiplayer action RPG
- Asks the author targeted clarifying questions to resolve ambiguities and understand intent before forming judgments
- Uses the author's answers to distinguish genuine gaps from merely unstated context
- Produces a single structured review document with categorized feedback and concrete suggestions for strengthening the proposal

## Prerequisites

- The user must provide a `$DOCUMENT` path pointing to a proposal file (Markdown, text, or similar)
  - This may be an absolute path (e.g., `<project>/SPAWNING.md`) or a path relative to the current working directory (e.g., `MCP_FEATURES_PROPOSAL.md`)
- The project source code must be accessible for cross-referencing claims in the proposal
- Unreal Engine source is available at `<engine-source>` for verifying engine-level assumptions

## Parameters

| Parameter | Required | Description |
|-----------|----------|-------------|
| `$DOCUMENT` | Yes | Path to the proposal document to review |

## Reviewer Persona

Adopt the following perspective throughout the review:

**Role**: Senior Principal Gameplay Engineer
**Experience**: 15+ years shipping multiplayer action games, deep expertise in Unreal Engine, gameplay ability systems, networking, and team-based cooperative gameplay
**Disposition**: Constructively critical. The goal is to make the proposal succeed, not to tear it down. Every question should help the author think more clearly about implementation.

**Review priorities** (in order):
1. **Correctness**: Will this actually work as described? Are the technical claims accurate?
2. **Completeness**: What's missing that would block implementation? What decisions are deferred that shouldn't be?
3. **Clarity**: Could two engineers read this and arrive at different implementations?
4. **Risk**: What are the highest-risk elements, and are they acknowledged?
5. **Integration**: How does this interact with existing systems? Are there hidden coupling or dependency issues?

## Steps

### 1. Read the Proposal

1. Read the full contents of `$DOCUMENT`
2. Take note of the proposal's stated goals, scope, architecture, and any open questions it already identifies
3. Form an initial list of areas that are unclear, ambiguous, or that you need more context to evaluate

### 2. Cross-Reference Against Source Code

Investigate claims made in the proposal by searching the project codebase:

1. **Verify referenced systems exist**: If the proposal mentions extending or integrating with existing classes, modules, or systems, confirm they exist and understand their current interfaces
2. **Check for conflicting patterns**: Look for existing code that solves similar problems differently, which could indicate a consistency issue or missed prior art
3. **Identify touched surface area**: Determine which modules, classes, and assets the proposal would need to modify or depend on
4. **Note what the source code makes clear**: Some questions that seem ambiguous in the proposal may be easily answered by reading the code. Filter these out -- they are not worth raising to the author

Do NOT exhaustively audit the entire codebase. Focus investigation on systems the proposal directly references or would need to interact with.

### 3. Cross-Reference Against Unreal Engine Source

If the proposal makes claims about Unreal Engine behavior, capabilities, or limitations:

1. Verify those claims against the engine source at `<engine-source>`
2. Note any engine constraints the proposal may not account for (e.g., replication limits, GAS restrictions, threading model assumptions)
3. Identify cases where the proposal assumes engine behavior that is version-specific or has changed in the custom engine build

Only investigate engine source when the proposal makes specific technical claims about engine capabilities. Do not speculatively audit engine code.

### 4. Ask Clarifying Questions

Before forming the final review, engage the author in a dialogue to resolve ambiguities and deepen understanding. This is the core iterative step -- it prevents the review from raising questions the author already has answers to.

#### 4.1. Formulate Questions

From Steps 1-3, identify questions where the author's answer would materially change the review. Good clarifying questions:
- Resolve ambiguity about intent ("Does this mean X or Y?")
- Surface unstated context the author may be carrying ("What's the expected scale/frequency of Z?")
- Probe design rationale ("Why X over Y? Is there a constraint I'm not seeing?")
- Verify understanding ("I'm reading this as [paraphrase]. Is that accurate?")

Do NOT ask questions that:
- Are already answered in the document
- Can be answered by reading the source code (you already did that in Steps 2-3)
- Are rhetorical or argumentative
- Are about minor wording choices

#### 4.2. Present Questions to the Author

Ask the author your clarifying questions. Group them by topic area for readability. Aim for the minimum number of questions needed to meaningfully improve the review -- typically 3-8 questions per round.

Use `AskUserQuestion` when questions have clear enumerable options. Use conversational text when questions are open-ended.

#### 4.3. Incorporate Answers

After the author responds:
1. Update your understanding of the proposal based on their answers
2. Determine if any answers reveal new areas that need follow-up questions
3. If significant new ambiguities surfaced, ask a second (shorter) round of follow-up questions. Avoid more than 2-3 total question rounds -- the goal is understanding, not interrogation.
4. Note cases where the author's answer reveals that the proposal document doesn't adequately communicate the intent. These become suggestions for improving the document.

### 5. Generate the Review

Produce a structured review document organized into the following sections. Each item should be a specific, actionable question or observation -- not vague commentary.

#### Review Document Structure

```markdown
# Review: <Proposal Title>

**Document**: <$DOCUMENT path>
**Reviewer**: Senior Principal Gameplay Engineer (AI-assisted)
**Date**: <current date>

---

## Critical Issues

Items that would likely block or derail implementation if not addressed.
Each item should explain WHY it's critical and WHAT information is needed.

- **[C1]** <Title>
  <Detailed question or observation. Reference specific sections of the proposal.>

## Design Questions

Architectural or design decisions that are ambiguous, unstated, or could reasonably go multiple ways. These are questions where the answer materially affects implementation.

- **[D1]** <Title>
  <Question with context about why the answer matters.>

## Consistency Issues

Places where the proposal contradicts itself, contradicts existing code patterns, or makes incompatible claims.

- **[I1]** <Title>
  <Description of the inconsistency with references to both sides.>

## Unstated Assumptions

Things the proposal assumes to be true but does not explicitly state. These are particularly dangerous when they involve cross-team or cross-system boundaries.

- **[A1]** <Title>
  <The assumption, why it might not hold, and what happens if it doesn't.>

## Multiplayer & Networking Concerns

Since this is a co-op team-based multiplayer game, any network replication, authority, prediction, or synchronization implications deserve special attention.

- **[N1]** <Title>
  <Concern with explanation of the multiplayer implications.>

## Scope & Risk

Observations about scope creep, underestimated complexity, or high-risk elements that deserve explicit mitigation plans.

- **[R1]** <Title>
  <Risk description and suggested mitigation or question.>

## Minor Clarifications

Lower-priority items that would improve the proposal but are not blocking. Phrasing improvements, missing details that are nice-to-have, etc.

- **[M1]** <Title>
  <Suggestion or question.>

---

## Suggested Document Updates

Specific recommendations for how the proposal document itself could be improved to better communicate the author's intent. These are informed by the clarifying Q&A -- places where the author had a good answer that simply wasn't captured in the document.

- **[U1]** <Section or area to update>
  <What to add, clarify, or restructure, and why it would help a reader.>

---

## Summary

<2-3 sentence overall assessment: What's strong about the proposal, what's the single most important thing to address, and a general confidence level in the proposal's implementability.>
```

#### Review Quality Guidelines

- **Be specific**: "How does X handle Y when Z?" is better than "X needs more detail"
- **Reference the proposal**: Quote or cite the specific section being questioned
- **Explain the stakes**: For each question, briefly explain what goes wrong if the answer is wrong or missing
- **Distinguish what you know from what you're asking**: If you verified something in the source code, say so. If you couldn't find evidence either way, say that too.
- **Don't pad**: If a section has no items, include it with "None identified." Do not fabricate concerns to fill sections.
- **Prioritize within sections**: List the most important items first within each section

### 6. Write the Review Document

1. Determine the output path:
   - Place the review file adjacent to the input document
   - Name format: `<document-basename>_REVIEW.md`
   - Example: `MCP_FEATURES_PROPOSAL_REVIEW.md`
   - If a file with this name already exists, confirm with the user before overwriting
2. Write the review document to the output path
3. Display a summary to the user:
   ```
   Review complete
     Document: <$DOCUMENT>
     Review:   <output-path>

     Critical issues:    <count>
     Design questions:    <count>
     Consistency issues:  <count>
     Assumptions:         <count>
     Multiplayer:         <count>
     Scope/Risk:          <count>
     Minor:               <count>
     Document updates:    <count>
     Total items:         <count>
   ```

## Error Handling

- **$DOCUMENT not found**: Exit with error, ask user to verify the path
- **$DOCUMENT is empty or unreadable**: Exit with error
- **Source code not accessible**: Warn that cross-referencing is limited, proceed with proposal-only review
- **Unreal Engine source not accessible**: Warn that engine claims cannot be verified, flag all engine-related claims as unverified assumptions
- **Author unresponsive to clarifying questions**: Proceed with the review based on available information, noting which items would benefit from author clarification
- **Output path not writable**: Ask user for an alternative output location

## Important Notes

- **Do not rewrite the proposal**: The output is a review document with questions and observations, not an edited version of the original. The author retains full ownership of the proposal.
- **Filter out code-answerable questions**: If a question can be definitively answered by reading the project source code or Unreal Engine source, answer it yourself rather than raising it. Only raise questions that require human judgment, domain knowledge, or design decisions.
- **Respect the author's intent**: The goal is to strengthen the proposal, not redirect it. If you disagree with a fundamental design choice, frame it as a risk with alternatives, not as a directive.
- **Multiplayer is always relevant**: Every feature in this project runs in a networked co-op context. Even proposals that seem single-player should be evaluated for multiplayer implications.
- **Don't assume the proposal is wrong**: A question is not an accusation. Many items may have good answers the author simply didn't include. Frame feedback as "this isn't addressed" rather than "this is wrong."
- **Engine source is a reference, not a constraint**: The custom engine build may differ from stock Unreal. Note when you're relying on engine source findings and acknowledge the custom build may diverge.

## Example Execution

User invokes: "Refine `MCP_FEATURES_PROPOSAL.md`"

### Step 1: Read the Proposal

Read `MCP_FEATURES_PROPOSAL.md` -- a proposal for adding new MCP server tools for Blueprint search and asset management.

Initial observations:
- Proposal references a `UBlueprintSearchHelper` class -- is this new or existing?
- Caching strategy for search results is not specified
- Threading model for search operations is unclear

### Step 2: Cross-Reference Source Code

- Verify `UMyMCPServer` class exists and review its current tool registration pattern
- Check how existing MCP tools handle Blueprint assets
- Confirm `UBlueprintSearchHelper` does not exist in the codebase -- this is either a new class or a typo
- Note that existing MCP tools follow a synchronous request/response pattern

### Step 3: Cross-Reference Engine Source

- Verify the proposed use of `FAssetRegistryModule` for Blueprint discovery -- confirmed, API exists
- Check `FindInBlueprintsSearchManager` -- confirmed, but requires game thread access (not callable from background threads)

### Step 4: Ask Clarifying Questions

Present questions to the author:

> I have a few questions to make sure I understand the proposal correctly before writing up my review:
>
> 1. The proposal references `UBlueprintSearchHelper`. I don't see this class in the codebase -- is this a new class you intend to create, or should this reference an existing class?
>
> 2. For Blueprint search results, do you envision caching results across calls, or fetching fresh each time? The answer affects both performance characteristics and cache invalidation complexity.
>
> 3. I see `FindInBlueprintsSearchManager` requires game thread access. The current MCP tools use a synchronous pattern. Is the intent to block the game thread during search, or did you have an async approach in mind?

**Author responds**:
- "UBlueprintSearchHelper is a new utility class we'd create to wrap the search logic."
- "Caching with a short TTL -- maybe 30 seconds. Blueprints don't change that fast during an editor session."
- "I was thinking we'd queue onto the game thread and wait. The MCP call is already async from the client's perspective."

**Follow-up**: The 30-second TTL answer raises a new question:

> Thanks. Quick follow-up on caching: if a user modifies a Blueprint and immediately searches for the change, the stale cache could return outdated results. Is that an acceptable trade-off, or should Blueprint save events invalidate the cache?

**Author responds**: "Good point -- cache invalidation on save is better. We can hook into `OnBlueprintCompiled`."

### Step 5-6: Generate and Write Review

The review document now reflects the reviewer's full understanding, including the author's clarifications. Items that were resolved through Q&A become "Suggested Document Updates" rather than open questions.

```
Review complete
  Document: MCP_FEATURES_PROPOSAL.md
  Review:   MCP_FEATURES_PROPOSAL_REVIEW.md

  Critical issues:    0
  Design questions:    3
  Consistency issues:  1
  Assumptions:         2
  Multiplayer:         0
  Scope/Risk:          2
  Minor:               2
  Document updates:    4
  Total items:         14
```

Key items in the review:
- **[U1]**: Proposal should specify that `UBlueprintSearchHelper` is a new class (not referencing existing code), and describe its responsibilities
- **[U2]**: Proposal should document the caching strategy: cache with invalidation on `OnBlueprintCompiled`, not a fixed TTL
- **[U3]**: Proposal should note the game thread requirement for `FindInBlueprintsSearchManager` and the chosen approach (synchronous game thread dispatch)
- **[D1]**: What's the maximum expected result set size? Large projects may have thousands of Blueprint matches -- is pagination needed?
- **[A1]**: Assumes `OnBlueprintCompiled` fires for all Blueprint modification scenarios (hot reload, bulk operations, source control sync). Needs verification.
