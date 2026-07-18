# pico-e32 docs

Documentation for pico-e32 — a DIY **PICO-8 player on the ESP32-S3**. Everything here is split by *what
each doc is about*. New to the repo? Read the two top-level files first, then open a folder as needed.

## Start here

- **[`pico-e32-development-plan.md`](pico-e32-development-plan.md)** — the **plan of record**: the
  decision-locked strategy and phasing. The single source of truth; everything else is detail or evidence.
  (A visual [`.html`](pico-e32-development-plan.html) render sits beside it.)
- **[`pico-e32-todo.md`](pico-e32-todo.md)** — the **backlog index**. It only *points*; each area's actual
  TODO items live in the linked docs.

## The folders

| Folder | What it's about |
|--------|-----------------|
| **[`runtime/`](runtime/)** | The **game-player software** — the [fake-08](https://github.com/jtothebell/fake-08) PICO-8 runtime ported to the ESP32-S3 (the project's primary goal). Holds the port plan, the code-map (our code ↔ upstream fake-08), the `Host` seams (input; plus a superseded hand-written graphics harness kept for reference), and visual `.html` reports. |
| **[`hardware/`](hardware/)** | The **physical device** — the ILI9488 display path (pins, bus, orientation) and the bench camera (the hardware-in-the-loop verification rig aimed at the panel). |
| **[`reference/`](reference/)** | **Background facts & research** gathered before/around building — verified board pinouts, z8lua speedup research, and the runtime-feasibility + silicon-decision studies. Has its own [README](reference/README.md). |
| **[`worklog/`](worklog/)** | A **dated diary** of bring-up work, written as it happens (the *why* and the dead-ends, not just the result). Each entry has an HTML render under [`worklog/html/`](worklog/html/). |

One-line distinction: **`reference/` is what we learned and decided *before* building; `runtime/` and
`hardware/` are what we're *building* and how it works; `worklog/` is the day-by-day record of doing it.**

## Conventions (see [`.ai/AGENTS.md`](../.ai/AGENTS.md))

- Every feature starts as a **documented TODO** in its area, indexed from `pico-e32-todo.md`.
- Non-trivial work keeps a **worklog**, updated as you go — each with a hand-authored **HTML render**.
- Visual or diagram-heavy material ships a self-contained **`.html` companion** beside the Markdown.
