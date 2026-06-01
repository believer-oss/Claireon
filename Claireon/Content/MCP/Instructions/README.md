---
type: resource
uri: claireon://instructions/token-legend
name: instructions-token-legend
description: Legend for the {{TOKEN}} placeholders in Claireon workflow Instructions and how an agent should resolve each. Read before running a workflow.
---

# Instructions: token legend

The Markdown files in this directory are the workflow Instructions served to a
connecting agent over MCP (see the plugin's Instructions surface). They are
**templates**: they contain `{{TOKEN}}` placeholders that are *not* substituted
by the plugin at load time. There is no runtime/settings expansion pass — by
design, since the connecting agent already has the project and git context and
substituting server-side is more fragile.

So: when you (the agent) read an instruction, fill each `{{TOKEN}}` from your
own context before acting on or echoing the text. Tokens come in two classes.

## Class A — project-context tokens

Resolve these once from the project, the git config, and the environment. They
recur across multiple instruction files (branch names, paths, commit formats,
PR links). Most are stable for the whole session.

| Token | Meaning | How to resolve | Example |
|-------|---------|----------------|---------|
| `{{PROJECT_NAME}}` | The Unreal project name | `.uproject` filename (sans extension) / `FApp::GetProjectName()` | `MyGame` |
| `{{PROJECT_ROOT}}` | Absolute path to the project root | Directory containing the `.uproject` | `C:/Projects/MyGame` |
| `{{GAME_MODULE}}` | Primary game/runtime module | Project's main runtime module name | `MyGame` |
| `{{GIT_USER}}` | Git username used in branch names | Local-part before `@` of `git config user.email`; `Scripts/Utilities/Get-GitUsername.ps1` | `<user>` |
| `{{GIT_EMAIL}}` | Git committer email | `git config user.email` | `user@example.com` |
| `{{REPO_URL}}` | Remote repository base URL | `git config remote.origin.url` (normalized to https) | `https://github.com/Org/repo` |
| `{{UNREAL_ENGINE_ROOT}}` | Absolute path to the Unreal Engine source | Engine install resolved from the project's `EngineAssociation` | `C:/UnrealEngine` |
| `{{WORKTREE}}` | Name of the current git worktree | Leaf directory name of the active worktree | `main` |

Notes:
- Branch-name templates use either `{{GIT_USER}}/{{PROJECT_NAME}}/...` or
  `{{GIT_USER}}/{{WORKTREE}}/...` depending on the instruction; fill whichever
  the template references.
- `{{GIT_USER}}` is always the email local-part, never the full email.

## Class B — example-local diagram placeholders

`architecture-viz.md` embeds an HTML/SVG diagram template whose `{{TOKEN}}`s are
**per-element fill-ins for the diagram you are generating**, not project
context. You assign each one while emitting a node, layer, edge, or style. Do
not try to resolve them from the project/git environment.

These live only in `architecture-viz.md`:

- Identity / labels: `{{SYSTEM_NAME}}`, `{{NODE_ID}}`, `{{NODE_NAME}}`,
  `{{LABEL}}`, `{{BADGE_TEXT}}`, `{{LAYER_KEY}}`, `{{LAYER_NAME}}`,
  `{{LAYER_LABEL}}`, `{{PARENT}}`, `{{CPP_CLASS}}`
- Edges: `{{SOURCE}}`, `{{TARGET}}`
- Geometry / animation: `{{SVG_HEIGHT}}`, `{{DUR}}`, `{{SAME_DUR}}`
- Color / style: `{{COLOR}}`, `{{COLOR_NAME}}`, `{{ACCENT_COLOR}}`,
  `{{PATH_COLOR}}`, `{{LAYER_COLOR}}`, `{{LAYER_BG}}`
- Prose / content: `{{SYSTEM_OVERVIEW_TEXT}}`, `{{NOTABLE_PATTERNS}}`,
  `{{PROPERTY_LINE}}`

If you add a Class B placeholder to a template, document its meaning inline in
that instruction file. If you add a Class A token, add a row to the table
above.
