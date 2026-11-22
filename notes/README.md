# Meridian Notes Library

Knowledge base for building and maintaining the Meridian Mastering Suite ([mwbtrue/meridian-mastering-suite](https://github.com/mwbtrue/meridian-mastering-suite)). AI agents read this before every build session and update it afterwards.

Seeded: 2026-11-21, from the repo at commit `04f4be3` (RESUME.md, ARCH.md, commit history).

## Files

| File | Holds | Read when |
|---|---|---|
| `systems/meridian.md` | How Meridian works: architecture, signal chain, latency, build, test suite, platform status | Always, at session start |
| `patterns.md` | Techniques that worked, each tied to proof | Before designing or changing DSP, threading, tests, or builds |
| `failures.md` | Bugs and dead ends: symptom, root cause, fix, prevention rule | Before touching any area listed in it; when something breaks |
| `RULES.md` | Lessons seen twice or more, promoted to hard rules | Always, at session start |

## How agents should use this library

1. **Start of session:** read `RULES.md` and `systems/meridian.md`. Skim the index tables in `patterns.md` and `failures.md`, then open only the entries that match the task. State which rules and entries apply.
2. **During work:** when something works or breaks, write a draft entry right away.
3. **End of session:** finalize entries with proof (a commit hash, test name, or measured number). Update `systems/meridian.md` if behavior changed. Promote any lesson seen twice into `RULES.md`.

## Entry status tags

- `VERIFIED` — backed by a test result, measurement, or commit on the stated platform.
- `PARTIAL` — verified on one platform only (for example Linux but not Windows).
- `UNVERIFIED` — a hypothesis. Do not build on it without testing.
- `STALE?` — may no longer match current code. Re-check before using.

## Hygiene rules

- Every fact names its source: commit hash, file, test, or measurement.
- Every entry carries the platform it was verified on (Linux, Windows/MSVC, macOS).
- If a note contradicts current behavior, fix or delete the note in the same session.
- Keep entries short. Link to ARCH.md or RESUME.md for detail instead of copying it.

## Hook into your agent

Add this line to the repo's `CLAUDE.md` (Claude Code) or `AGENTS.md` (Codex, Cursor, others):

```text
Before any work, read notes/RULES.md and notes/systems/meridian.md, then the relevant entries
in notes/patterns.md and notes/failures.md. After any work, update these notes following
notes/README.md. Record only verified facts with their source; tag guesses UNVERIFIED.
```

Suggested location in the repo: `meridian-mastering-suite/notes/`.
