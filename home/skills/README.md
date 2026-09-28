# Global skills

Place each skill in its own directory: `skills/<name>/SKILL.md`.
DAgent discovers these directories at startup. Restart after adding or changing a skill.

`SKILL.md` contains YAML frontmatter with `name` and `description`, followed by Markdown instructions.
The name must match the directory and use 1–64 lowercase ASCII letters, digits, or single hyphens;
it must not start or end with a hyphen. The description must contain 1–1024 characters.
Use `references/`, `scripts/`, and `assets/` for supporting resources.

In DAgent, open `/skills` or type `$` to choose a skill. `$name` explicitly loads it for the current
turn. The model can also select a skill from its description. Instructions expire at the end of the
turn; scripts and file changes always use the normal tool permissions.

Only this installation's skills directory is scanned. No project or other agent directories are
searched. Invalid skills appear in `/skills` with their diagnostic. See `docs/design/skills.md` in
the source repository for the complete behavior and implementation.
