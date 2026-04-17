---
name: Never add Claude as Co-Author
description: User does not want any "Co-Authored-By: Claude ..." trailer in git commit messages, ever
type: feedback
---

Never add `Co-Authored-By: Claude ...` (or any Claude/Anthropic attribution) to commit messages. Write the commit message body and stop — no trailer.

**Why:** User explicitly asked to strip all Claude co-author trailers from history across every branch and to never add them again. This is a hard rule.

**How to apply:** When writing any `git commit -m`, omit the `Co-Authored-By: Claude ...` line entirely. Applies to every repo, every branch, every commit. Same for PR bodies — no "🤖 Generated with Claude Code" footer either, unless the user explicitly asks.
