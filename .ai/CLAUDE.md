# CLAUDE.md

Project guidance for AI coding agents lives in [AGENTS.md](AGENTS.md) — read it.

`pico-e32` is a DIY **PICO-8 handheld** built around the **ESP32-S3** — an embedded
project, so most changes are judged on real hardware, not just a clean build.

Most important rules:
- **Every feature starts as a documented TODO** (in its area backlog, indexed from
  `docs/pico-e32-todo.md`) before you build it. See
  [AGENTS.md → Plan first](AGENTS.md#plan-first--every-feature-starts-as-a-documented-todo).
- **Do not commit or push automatically** — only when explicitly asked, and
  **never during weekday work hours (Mon–Fri 9 AM–5 PM local); no back-dating to
  dodge it.** See [AGENTS.md → Committing](AGENTS.md#committing).
- **No AI attribution anywhere** — not in code comments, docs, commit messages, or
  GitHub PR/issue text. Everything reads as the human owner's work; commits use the
  repo's git identity only. See
  [AGENTS.md → Attribution](AGENTS.md#attribution--no-ai-self-reference-anywhere).
- **Code/feature changes → branch + PR; doc-only changes → may push to `main`.**
  See [AGENTS.md → Branching & pull requests](AGENTS.md#branching--pull-requests).
- **Merge PRs with rebase + merge by default** (`gh pr merge --rebase`); keep
  `main` linear. See
  [AGENTS.md → Merging pull requests](AGENTS.md#merging-pull-requests).
- **No mis-linking `#N` in PR/commit text** — a bare `#N` auto-links to a
  *same-repo* issue/PR, so cross-repo refs must be qualified as `owner/repo#N` and
  internal IDs (task/backlog/bug numbers) backticked in Markdown (`` `#20` ``; in
  commit messages drop the `#`). Scan before pushing. See
  [AGENTS.md → Cross-references in PR and commit text](AGENTS.md#cross-references-in-pr-and-commit-text).
- **Verify every change on real hardware or with a unit test** — a clean build is
  never enough for on-device behaviour: flash the ESP32-S3 and confirm the actual
  result. A **camera is pointed at the device for hardware-in-the-loop testing**, so
  for display changes run a tight **flash → capture a camera frame → inspect the
  panel → adjust** loop and save the frame as evidence; **measure** frame time and
  heap/PSRAM when you touch the render/update loop. If you can't verify, document why. See
  [AGENTS.md → Verifying changes](AGENTS.md#verifying-changes).
- **A substantial port ships a code-map doc** — a function-level, side-by-side
  new-code ↔ upstream mapping (`file:line` ↔ `file:line`) + a deliberate-divergences
  section, with every cited line grepped in both trees, never from memory. See
  [AGENTS.md → Porting / adapting upstream code](AGENTS.md#porting--adapting-upstream-code).
- **Least-destructive vendor edits** — keep the tree byte-identical to upstream and push
  integration into the build: never rename a file to change how it compiles (set the language
  in the build, e.g. `LANGUAGE CXX` / `-x c++`), and never delete a file just to exclude it
  (leave it out of the source list, or guard behind a compile flag). See
  [AGENTS.md → Porting / adapting upstream code](AGENTS.md#porting--adapting-upstream-code).
- **Keep a worklog and update it AS YOU GO** — for any non-trivial, multi-step
  investigation/implementation, maintain `docs/worklog/YYYY-MM-DD-<slug>.md` and append each
  finding/measurement/decision/dead-end/next-step at the time it happens, not once at the end,
  so the thread survives context loss and keeps the *why* + the failures. See
  [AGENTS.md → Worklogs — write and update as you go](AGENTS.md#worklogs--write-and-update-as-you-go).
- **Every worklog gets an HTML render** — when you add or substantially edit a
  `docs/worklog/*.md`, hand-author its companion `docs/worklog/html/<name>.html`
  (self-contained — no external files; visuals + a content-driven diagram; no
  Markdown→HTML converter) and update the index. See
  [AGENTS.md → Worklog HTML renders](AGENTS.md#worklog-html-renders).
- **Cite sources** when finding, researching, or comparing. See
  [AGENTS.md → Research & citations](AGENTS.md#research--citations).
- **Identify a board by a stable label, not a volatile `/dev/tty*` port**, and keep the
  wiring/pin map in the docs. See
  [AGENTS.md → Hardware & flashing notes](AGENTS.md#hardware--flashing-notes).
