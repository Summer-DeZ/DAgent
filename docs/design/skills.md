# Skills

DAgent supports local Agent Skills packages: YAML frontmatter and Markdown instructions in
`<installation-root>/skills/<name>/SKILL.md`, with optional `references/`, `scripts/`, and `assets/`.
The installation root follows `DAGENT_HOME`; dev builds default to the repository's `home/`.
Only global, direct child directories are discovered. There is no installer, project scanning,
live refresh, or plugin registration. Restart to discover changes.

## Discovery and ownership

`app::load_skills` uses yaml-cpp 0.8.0 to parse files into an immutable `agent::SkillCatalog`.
It validates required names/descriptions and standard optional fields, preserves license,
compatibility, metadata and allowed-tools, and ignores unknown extensions. `allowed-tools` does
not change permissions or available tools. Skill instructions are literal Markdown, not templates.
Invalid files are skipped and reported through the log and skill list. An absent directory is empty.
Files must be valid UTF-8 and fit the configured file read limit; instructions are never truncated.
Canonical skill files must remain within the registered installation skills tree.

The backend builds the snapshot once and shares it with the query gateway and all session
instances. Only metadata enters the initial system prompt; bodies are kept in the snapshot and
enter model context on activation. Referenced resources are read using normal tools on demand.
The frontend sees metadata and diagnostics via `skills.list`, never the parser or the snapshot bodies.

## Selection and execution

- `/skills` opens the existing application panel. Selecting a row inserts `$name` without submitting.
- `$` completion inserts a skill reference. CLI `dagent run '$name task'` uses the same backend path;
  quote the prompt so the shell does not expand `$name`.
- The backend recognizes standalone `$name` references, excluding escapes, inline code and fenced
  code blocks. Unknown valid names fail the turn explicitly. Multiple references retain first-use order.
- The model can call the `skill` control action with `{"name":"name"}`. The name enum comes from the
  catalog. The action and prompt section are omitted when no skills are available.

Activation changes context, so it belongs to the core control action executor, not the filesystem
tool registry. It runs serially on the session execution thread. Explicit references use the same
activation implementation before the first model call. Repeated activation within a turn is a no-op.

## Context lifetime and history

`RunSkills` belongs to one `Run`. Model request construction appends its full instruction block to
the last user message in a request copy; stored conversation messages and the fixed system prompt
are unchanged. Actual requests and compaction estimates use the same projection. The summarizer
receives history, not the transient instructions, so it cannot retain old skill bodies in summaries.
Activation checks the irreducible request budget and reports an error rather than truncating a skill.
Automatic history compression does not discard active instructions.

Completion, failure and cancellation release the run and its skills. Session recovery and model
switching do not reactivate old skills. To continue using a skill in a later turn, select it again or
let the model activate it again. Explicit selections are stored as optional `skills` metadata on the
user record; automatic activation stores a `SkillView` and a short tool receipt. Neither stores a
copy of the instruction body. Older records without skill metadata remain readable; no SQL migration
is required. Historical `$name` text and skill receipts are records of past use, not active guidance.

## Permissions and subagents

Skill-relative references resolve against the skill directory; tools still receive absolute paths.
Registered canonical directories are readable without outside-workspace approval and enter the
Bash sandbox's read scopes. Sensitive files keep their existing protections. Resolved symlinks
outside registered directories receive ordinary path policy; directory registration does not grant
write access or execution approval. Bash scripts, network operations and mutations follow existing
permission and sandbox behavior. User requests and project instructions take precedence over skills.

Subagents share the immutable directory, not the parent's active skill set. Their tool whitelist
must include `skill`; the bundled explore and implement definitions do. Explicit skill references
in a delegated prompt activate only in that child's run. A skill never creates a child agent itself.

## Dependencies

Skill files remain under `home/skills`; installed dependencies live separately under
`home/runtime/envs/skills/<name>/<generation>`. Declare `skills/<name>` in
`config/runtime.json` and run `dagent runtime sync` explicitly. A matching environment adds
a bash environment selection instruction to the activated skill. It does not execute scripts
or install packages during discovery/activation. See [toolchain](toolchain.md) for locked
Python and Node environments, and [home](home.md) for global user instructions and prompt paths.

## Protocol

`skills.list` is a read-only query with empty parameters, returning:

- `skills`: entries with `name`, `description`, and absolute `path` to `SKILL.md`.
- `diagnostics`: entries with `path` and `message` for skipped skills or discovery failures.

`input.submit` remains unchanged. Tool view JSON adds `kind: "skill"`, `name`, and `path`;
frontend projection renders it with the standard tool card. User records optionally add
`skills: [{name, path}]` for explicit requests, without restoring active state.
