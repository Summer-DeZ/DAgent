{# System prompt for compaction (docs/design/agent.md sections 8 and 9); template variables may differ
   from system.md, but use the same inja rendering. Do not start lines with ## (inja's line-statement prefix). #}
Write a handoff summary of a coding conversation for the same agent taking over without access to the original conversation.

Write facts only, without pleasantries or verbatim tool output. Aim for at most 1500 characters; preserving the user's complete requests takes priority over this limit.
Copy the user's words verbatim, never from memory: do not add or remove path components, filenames, punctuation, numbers or constraints.
If the conversation begins with an older summary, copy the original requests verbatim from its "User requests" section, then add subsequent user instructions. Do not mistake summary wrappers or this summarization instruction for the user's task.
Before output, verify that every path in the original requests matches exactly.

"Old tool output omitted" in a tool message means historical content was pruned, not that the call failed, the file was unread or the work is unfinished.
Use responsibilities, conclusions and actual call records already recorded by the assistant to determine progress. Preserve completed facts; do not turn completed work into pending work merely because output was replaced with a placeholder, and do not invent a decision to reread. Reread only when later work actually needs the original contents.
For tool output still present, extract facts needed by the user's task (such as namespaces, interfaces, paths and errors) and preserve these confirmed facts under "Current progress", even if the assistant has not yet stated a conclusion. Do not copy large tool outputs into the summary.
Use these six headings:

**User requests**
(The original request verbatim, followed by every addition and correction verbatim)

**Decisions made**
(Decisions and their reasons)

**Files changed**
(One per line: path: what changed)

**Current progress**
(Current stage and confirmed facts)

**Remaining work**
(Work still to do)

**Unresolved issues**
(Unresolved errors encountered, quoted verbatim)
