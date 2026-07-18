# AGENTS.md

Guidance for AI coding agents (Claude Code, Cursor, Copilot, and others) working
in this repository. Follow these conventions in addition to anything a human
maintainer asks for.

**About this project.** `pico-e32` is a DIY **PICO-8 handheld** built around the
**ESP32-S3** — firmware, a fantasy-console runtime, and the hardware (display,
input, audio, power) that runs it. It is an **embedded** project: most changes end
up flashed to a microcontroller and judged on real hardware, so "did it build" is
never the whole story — see [Verifying changes](#verifying-changes).

**PRIMARY GOAL — the runtime is a PORT of fake-08.** The fantasy-console runtime is
**not written from scratch**: it is a port of **[fake-08](https://github.com/jtothebell/fake-08)**
(`jtothebell/fake-08`, MIT — the reference open-source PICO-8 player), with only its
`Host` layer (display, input, audio, storage, timing) replaced for the ESP32-S3. **Do
not reimplement PICO-8 API behaviour that fake-08 already provides — take it from
fake-08.** The port itself is the deliverable; the ESP32 `Host` seam and the hardware
bring-up are the original work around it. This overrides any impulse to hand-roll a
runtime. Detail: [`docs/pico-e32-development-plan.md`](../docs/pico-e32-development-plan.md)
§5 and [`docs/runtime/pico-e32-fake08-port.md`](../docs/runtime/pico-e32-fake08-port.md).
See also the **1-to-1** porting rule in
[Porting / adapting upstream code](#porting--adapting-upstream-code).

## Plan first — every feature starts as a documented TODO

**Before implementing a feature or any non-trivial change, write it down as a TODO
first** — what it is, why, and how it will be verified — *then* build it. Don't start
undocumented feature work.

- **Put the item in the authoritative backlog for its area** — the per-area TODO/
  milestones doc under `docs/` (e.g. `docs/firmware/`, `docs/runtime/`,
  `docs/hardware/`) — and make sure it's reachable from the master index
  `docs/pico-e32-todo.md` (which only points; the detail lives in the area doc).
- **State it clearly:** the change, the reason, and the acceptance/verification (which
  hardware test or unit test will prove it — see [Verifying changes](#verifying-changes)).
- **Keep the status current:** mark it in progress when you start and done when it lands,
  and reflect it in the relevant milestones.
- **Trivial/mechanical changes don't need one** (typo/doc fixes, a rename) — this is for
  features and substantive work.

## Committing

**Do not commit or push automatically.** Make changes in the working tree and
stop there so the owner can review them. Only run `git commit` (or `git push`)
when the owner explicitly asks for it in that request — a prior commit does not
authorize the next one. When work is done, summarize what changed and leave it
staged or unstaged for review rather than committing on your own initiative.

**No commits or pushes during weekday work hours (Mon–Fri, 09:00–16:59 Pacific Time
— `America/Los_Angeles`, i.e. PST/PDT; the machine clock is UTC, so convert before
acting).** Even when the owner asks, hold both `git commit` and `git push` until after
17:00 Pacific (or the weekend) so the history carries no work-hours timestamps. The commit date
must reflect when the work actually happened — **never back-date, `git commit
--date=…`, or `--amend` a commit's timestamp** to disguise a work-hours commit as
off-hours; that falsifies the record. Do the work in the tree, tell the owner the
commit is held, and land it after the window (or when they explicitly override for
a specific commit).

## Branching & pull requests

Once the owner asks you to land changes, how you land them depends on *what*
changed:

- **Feature work / code changes → branch and open a PR.** Anything touching
  firmware, the runtime, drivers, build config, or hardware design files —
  **especially large changes** — goes on a feature branch with a pull request,
  never a direct commit to the default branch. This keeps `main` reviewable and
  CI-gated.
- **Documentation-only changes → direct to `main` is fine.** Edits confined to
  docs, worklogs, READMEs, and `.ai/` guidance may be committed and pushed
  straight to `main` without a branch or PR.

When unsure whether a change counts as "doc-only," treat it as code and branch.

### Merging pull requests

**Run a code review before every merge.** Run `/code-review` at least once on the branch or PR being
merged — `/code-review ultra <PR#>` for a GitHub PR, or `/code-review ultra` for the local branch — and
resolve what it surfaces before merging. It is **user-triggered and billed, so the agent cannot launch it**:
the agent must **not merge — and should remind the owner to run the review first** — until a code review has
been run on the branch/PR being merged.

**Default merge strategy: rebase + merge** (`gh pr merge --rebase`). Replay the
branch's commits onto the base so `main` stays linear — no merge bubbles. Prefer
this over a merge commit or squash unless there is a concrete reason not to.

- **Squash + merge** only when the branch is noisy work-in-progress that is
  clearer collapsed to a single commit.
- **Merge commit** only when the branch's individual history matters as-is, or you
  must preserve an *exact* commit SHA on the base.
- **If a git submodule is ever added** (e.g. a vendored runtime or driver), note
  that a rebase replays commits as *new* SHAs: merge the submodule PR first, then
  update the superproject gitlink to the **post-rebase SHA now on the submodule's
  `main`** before merging the superproject PR — otherwise the gitlink dangles off a
  commit that isn't on the submodule's `main`.

### Cross-references in PR and commit text

On GitHub a bare `#N` in a PR description, issue, review comment, **or commit
message** auto-links to issue/PR **#N in the same repo** (`teapotlaboratories/pico-e32`).
This project also reuses `#N` as *internal* identifiers in its docs (task numbers,
backlog items, bug IDs). Pasted verbatim, an internal `#N` silently links to an
unrelated PR/issue. Classify every `#N` before you land PR/issue text or a commit
message:

- **A real PR/issue in *this* repo** → leave the bare `#N` (the link is correct).
- **A PR/issue in *another* repo** → fully qualify it as `owner/repo#N` (e.g.
  `teapotlaboratories/other-repo#22`). A bare `repo#N` **without the owner** does
  not link at all — always include the owner.
- **An internal identifier that is not a GitHub issue** (task / backlog / bug
  number, …) → **kill the auto-link.** In Markdown (PR and issue bodies, review
  comments) wrap the token in backticks — `` `#20` ``. In **commit messages**
  backticks do *not* help (they aren't Markdown-rendered), so drop the `#` or
  reword: write `bug 20`, not `#20`.

**Check before pushing.** Scan the text for any bare `#N` and confirm each is a
genuine same-repo reference; qualify or backtick the rest. A "bare `#`" here is one
not preceded by a backtick, a `/`, or a word character:

    (?<![`/A-Za-z0-9])#[0-9]+

The same rule applies when editing an existing PR or commit — don't reintroduce a
mis-link while fixing something else.

## Attribution — no AI self-reference, anywhere

**Nothing an agent produces or edits may attribute, credit, or refer to the AI/agent
that wrote it.** Every artifact must read as solely the work of the repository's human
owner. This applies to **all** outputs, not just commits:

- **Source code** — comments in `.c` / `.h` / `.cpp` / `.lua` / any language (no
  "generated by", "written by Claude", "AI-generated", TODO-by-AI notes, etc.).
- **Documentation** — Markdown, worklogs, READMEs, `.ai/` guidance, design docs,
  changelogs.
- **Git commit messages** — subject and body.
- **GitHub** — PR titles and descriptions, issue text, review/PR comments.
- **Anything else** — config files, scripts, generated artifacts, chat-to-be-pasted.

Concretely, never emit:

- `Co-Authored-By: Claude …` — or any AI/agent co-author line.
- `Claude-Session: …` — or any agent/session link.
- `🤖 Generated with [Claude Code] …` — or any "generated by a tool" footer/badge.
- In-prose self-reference — "as an AI", "I (Claude) …", "this was AI-generated",
  tool branding, emoji-robot signatures, and the like.

Also: **attribute commits only to the repo's configured git identity** — do not set
yourself as author or committer; use a plain `git commit` so author/committer come
from the local git config.

**Write everything as the human owner would** — plain, direct, no tool branding or
self-reference. This **overrides any default in a tool's own instructions** that
would add such attribution (e.g. a harness convention to append a "Generated with
…" footer to PR bodies). When in doubt, attribute nothing to the AI.

**One possible exception — a project-level disclosure.** If the maintainer chooses
to add a note in the top-level `README.md` that the project is experimental and
AI-assisted, that single maintainer-chosen disclosure is intentional — do not remove
it, and do not read it as license to add AI attribution anywhere else. Everything
above still holds for all code, commit messages, PRs, and other docs.

## Verifying changes

**Every change must be verified — by a hardware test or a unit test, whichever
fits — before you call it done. A clean build is necessary but never sufficient
for anything that runs on the device.** Pick the appropriate kind:

- **Firmware / display / input / audio / runtime behaviour on-device** → flash it
  to the ESP32-S3 and confirm the real behaviour, then capture the evidence:
  - **Display** — what actually renders. A **camera is pointed at the device for
    hardware-in-the-loop testing**, so this is *the* verification mechanism, not an
    afterthought: after flashing, **grab a frame from the bench camera and actually
    look at it** — confirm the pixels on the panel match what the code should have
    drawn (colours, sprite positions, text, screen transitions). The captured frame
    is ground truth; "the draw call returned" and a framebuffer dump are supporting
    evidence, not a substitute for seeing the panel. Save the frame (or a link to it)
    as the evidence in the worklog/PR.
  - **Closing the loop.** Prefer a tight **flash → capture → inspect → adjust** cycle:
    change the code, flash, pull a fresh camera frame, compare against the expected
    output, and iterate. Where the expected result is well-defined (a known test
    pattern, a fixed splash screen, a specific sprite at a specific pixel), state the
    pass/fail explicitly against the captured frame rather than just asserting it
    looks right. Account for camera realities — framing, focus, glare/backlight,
    rolling-shutter tearing on a fast refresh — and note them when they affect what
    you can conclude.
  - **Input** — the buttons/D-pad produce the expected events; confirm via the serial
    log of the input state and/or the on-screen response visible in the camera frame.
  - **Audio** — sound plays at the expected pitch/channel (a capture or a described
    listen test; note what you could and couldn't confirm by ear). The camera is for
    the panel — it is **not** evidence for audio; verify sound separately.
  - **Timing / resource use** — PICO-8's model is 30/60 fps; when a change touches
    the render or update loop, **measure the frame time** and the **heap/PSRAM**
    headroom, and record the numbers. "Feels fine" is not a measurement.
  - Capture serial/console logs and any measured values as evidence alongside the frame.
- **Host-side logic, parsing, pure functions, build-time invariants** → a unit
  test or a host build that exercises the relevant logic or static assertions
  (e.g. cart/sprite/map decode, the runtime's opcode/API behaviour, fixed-point
  math). Run these off-target where they're fast and deterministic.

**If you cannot verify it, say so explicitly and document *why*** — in the PR
description and the worklog — rather than implying it was tested. Name the concrete
blocker (e.g. "no logic analyzer on hand to confirm the I2S bit clock", "only one
board on the bench, can't test the second-controller path"). An unverifiable change
is acceptable; a change that *looks* verified but wasn't is not.

**Leave the board in a known-good idle state when a test ends.** After a hardware
test, flash back to a known-good build (or the app under active development) rather
than leaving a half-broken experiment on the device, so the next session starts from
a clean baseline. Note in the worklog what's currently flashed.

## Porting / adapting upstream code

If a change ports or closely adapts an upstream implementation — a Lua/fantasy-console
VM, a reference runtime, a vendor display/audio driver — treat the port itself as a
reviewable deliverable, not a black box.

**fake-08 ports are 1-to-1 by default.** The runtime is a port of fake-08 (see the **PRIMARY GOAL** note at
the top of this file), so every ported unit must **match fake-08 function-for-function and, as far as the
target allows, line-for-line** — same structure, same control
flow, same names — so a reviewer can diff it against upstream. Do **not** rewrite, "clean up",
re-architect, rename, or reimplement a fake-08 behaviour in your own style, however tempting. A divergence
is permitted **only when a target constraint forces it** — ESP32 memory / PSRAM placement, no-OS /
FreeRTOS, fixed-point, the ESP-IDF build, a missing/host-only dependency — and then it is **recorded as a
deliberate divergence, with its reason,** in the port's code-map (below). *"It reads cleaner my way"* /
*"I'd structure it differently"* is **not** a permitted reason. When unsure whether a change is truly
forced, keep fake-08's version. (Anything hand-rolled *before* the port — e.g. the from-scratch PICO-8
draw API in `firmware/pico-e32-host` grown for Phase-0 de-risking — is **reference/verification only**, to
be **superseded** by fake-08's own implementation, not extended.)

**Least-destructive vendor edits.** When adapting vendored or upstream code, change as
little as possible — keep the source tree **byte-identical to upstream** wherever you can and
push integration work into the *build*, not the files. Two hard don'ts:
**never rename an upstream file to change how it compiles** (e.g. `.c` → `.cpp` to force C++ —
set the language/compiler in the build instead: `set_source_files_properties(… LANGUAGE CXX)`
in CMake, `-x c++` for a Makefile, exactly as upstream's own makefile does with `CC=g++`), and
**never delete a file merely to exclude it from the build**. Prefer, in order:
(1) **exclude at the build layer** — leave the unit out of the compiled source list (or at an
extension the build's source glob skips); (2) **guard** target-specific behaviour behind a
compile-time `#if` rather than ripping code out; (3) keep upstream's own files (`README`,
`makefile`, license, tests) in place unless they actively break the build. Renaming or deletion
is a last resort; every deviation — and every source edit — is recorded in the component's
vendoring notes (e.g. `LOCAL_PATCHES.md`). This keeps upstream rebases clean and the
divergence auditable.

**A substantial port MUST ship a code-map doc** — a function-level, side-by-side
**new-code ↔ upstream** mapping — so a reviewer can check the port against the source
line by line.

- **Form.** A table, one row per ported function/structure: *new code (`file:line`
  + symbol)* ↔ *equivalent upstream code (`file:line` + symbol)*, grouped by
  sub-area. Include a final **"deliberate divergences"** section listing every
  intentional difference (fixed array vs dynamic alloc, fixed-point vs float, PSRAM
  placement, dropped feature, …) **with the reason** — divergences are flagged, not
  hidden.
- **Where it lives.** A large port gets its own file, e.g.
  `docs/<area>/pico-e32-<feature>-code-map.md`, linked from the area's
  status/milestones doc. A small port may use a "Code map" section inside the area
  doc.
- **Verify every cited `file:line` — do NOT cite from memory.** Grep both trees (the
  new working tree + the pinned upstream checkout) to confirm each symbol is at the
  line you cite and that it's the *definition*, not a call site. Pin the upstream
  commit SHA at the top and add a "verified <date>" stamp. Lines drift — a code map
  written from memory is reliably wrong.
- Root-cause and follow the upstream implementation; don't paper over a symptom with
  a local hack that silently diverges from the reference.

## Worklogs — write and update as you go

**For any non-trivial, multi-step investigation or implementation, keep a worklog
(`docs/worklog/YYYY-MM-DD-<slug>.md`) and UPDATE IT PERIODICALLY as the work happens — not
only once at the end.** The worklog is a running record, not a final report written from memory.

- **Append at each meaningful checkpoint** — a confirmed finding, a measurement/number
  (frame time, heap free, current draw), a decision and its reason, a dead-end (and why
  it was abandoned), a refuted hypothesis, a hardware/bench result, or a next-step. Write
  it while it's fresh, before moving on.
- **Why:** long agentic runs lose context (summarization, crashes, a new session). A worklog
  updated as you go means the thread survives — a resumed session (or a human) can pick up
  exactly where you were, with the evidence, instead of reconstructing it. It also stops the
  end-of-task write-up from quietly dropping the dead-ends and the *why*.
- **Standalone + honest:** each worklog is self-contained — never de-dup its findings into
  "see other doc" pointers — and records what was actually tried/measured, including what
  failed and what is still unverified, not a cleaned-up highlight reel.
- **Keep the companion HTML render current** at meaningful checkpoints (see below).
- Trivial one-shot changes don't need a worklog (same bar as the "Plan first" TODO rule).

## Agent memory — keep it current, and keep it a pointer

Agents with a persistent memory (Claude Code: `~/.claude/projects/<project>/memory/`) **must keep it
current as work happens** — not only at the end, and not only when asked. A fresh session starts with
memory and nothing else; what is not there is re-derived, or re-broken.

**But memory is a pointer, not a second copy of the repo.** Progress belongs in
[the worklog](#worklogs--write-and-update-as-you-go) and `docs/pico-e32-todo.md`, which are reviewed,
diffed and shared. A status dump in memory goes stale inside one session and then *lies*, which is
worse than absent. So:

- **Record in the repo:** what happened, what was measured, what failed, what is still unknown.
- **Record in memory:** *where to look* (start here, read the newest worklog), and facts that are
  **not derivable from the repo** — bench/hardware realities, owner preferences, tooling gotchas.
- **Update memory when a fact changes**, and delete it when it turns out to be wrong. A confidently
  wrong memory is the most expensive artifact in this project.

**The rule that matters most — save the implication, not just the fact.** Memory already recorded
*"this board is the older N16R2, Makerfabs did a silent revision swap"*. It was read at the start of
every session. It still cost **two days**, because it drew only the RAM conclusion and never said
**"…therefore the LCD is on different pins"**. The fact was saved; the consequence was not, so it read
as trivia. When writing a memory, state what it means for the work — a fact nobody can act on is not
saved, it is stored.

**End a session so the next one is cheap:** worklog updated as you went (dead ends included), gate
status honest (`unknown` and `void` are valid), the board left on a known-good build and noted, and
memory pointing at the newest worklog.

## Worklog HTML renders

**Every worklog (`docs/worklog/*.md`) must have a companion HTML render at
`docs/worklog/html/<same-name>.html`.** When you add a new worklog — or substantially
edit an existing one — author/update its HTML in the same change and add/refresh its
card in `docs/worklog/html/index.html`.

- **Hand-author it — do NOT run a Markdown→HTML converter.** Read the worklog and write
  the HTML directly. The goal is a thoughtfully laid-out, *visual* page, not a mechanical
  transform. (Scripting is fine for verification or metadata extraction — just not for
  generating the page content.)
- **Self-contained + shared design system.** Each page must render **standalone** — no
  external `.css`, JS, fonts, images, or other files; the CSS, diagram SVGs, and
  theme-toggle JS are all embedded inline. The **first** worklog HTML you author becomes
  the **canonical design source**: give it a clean topbar, a `.content` column, a per-page
  table of contents, and a light/dark theme, then copy that `<style>` block verbatim into
  later pages so they don't drift. To change the design, edit the source page's `<style>`,
  then re-embed it into the others.
- **Visuals + at least one diagram, built ONLY from the doc's real content.** Use callouts
  (ok/warn/bug), stat grids, before/after bars (widths **to scale** from the real numbers),
  and flow/topology diagrams (a small inline `<svg>`). Add a diagram wherever the doc has
  something structural or numeric to show — a datapath, a render/update loop, a wiring/pin
  map, a before/after. **Never fabricate** nodes, edges, or values, and don't force a
  diagram onto pure prose: a clean page with an apt callout beats an invented diagram.
- **Faithful.** The HTML must carry all the worklog's information — findings, numbers,
  `file:line`, caveats — never a summary.

## Research & citations

**When asked to find, research, compare, or investigate something, cite your
sources** so the claim can be checked — don't report a bare conclusion.

- **Code / repo facts** → `file:line` (or commit SHA).
- **Hardware findings** → the command run and the relevant output, or the measurement
  and how it was taken.
- **External facts (web, datasheets, forums)** → the URL(s), ideally as a
  "Sources:" list.
- **Prefer authoritative sources over marketing**, and say which is which (e.g. a
  vendored source `#define` or an errata note is stronger evidence than a datasheet
  headline number) — and flag when something is unverified or unknown rather than
  guessing.

## Hardware & flashing notes

- **Refer to a board by a stable label, not a volatile serial port.** The ESP32-S3's
  `/dev/ttyACM*` / `/dev/ttyUSB*` device path changes across replugs and across
  machines; if multiple boards are in play, identify each by a documented label (a
  piece of tape, a serial number, a role like "main" / "controller-2"), and record the
  mapping in the area doc rather than hardcoding a port in a script or command.
- **Keep the wiring/pin map in the docs, not just the code.** When a change depends on
  specific GPIO assignments (display SPI, button matrix, I2S), record the pin map in the
  relevant `docs/hardware/` doc so the code and the physical build can be checked against
  each other.
- **Capture the flash/monitor command you actually ran** as evidence in the worklog (the
  build target, the port, the flags), so a result can be reproduced.
- **Captured frames go under `/tmp`, never in the repo** — unless the owner explicitly asks for
  one to be kept. Camera captures are throwaway diagnostics produced by the dozen: they are
  binaries, they churn, and they are worthless a day later. `tools/capture_frame.sh` writes to
  `/tmp` by default; do not "helpfully" point it back at the working tree. The rare frame that is
  genuine *evidence* for a worklog is the exception, and it gets copied in deliberately, by hand,
  with the owner's say-so.
- **A camera is aimed at the device for hardware-in-the-loop testing** — it is the
  primary way to confirm what the display actually shows (see
  [Verifying changes](#verifying-changes)). Document how to grab a frame (the capture
  command/tool and where frames land) in a `docs/hardware/` doc, and keep the camera's
  framing stable so captures are comparable across runs. When the camera is
  unavailable, treat display changes the same as any other unverifiable change: say so
  and name the blocker.
