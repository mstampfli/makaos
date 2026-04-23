# Engineering Guidelines

This directory contains the rules and principles that govern how code is
written, reviewed, and accepted across systems-level projects. The
guidelines target AI agents first (Claude Code and similar), but humans
should follow them too.

## Files

- [`CODE_STYLE.md`](./CODE_STYLE.md) — **How code looks.** Formatting,
  naming, file layout, commenting rules, language-specific conventions
  for C and NASM. The mechanical stuff.
- [`PRINCIPLES.md`](./PRINCIPLES.md) — **How code is built.** The
  architectural philosophy: modularity, performance, correctness, error
  handling, concurrency, and how decisions should be made when the style
  guide runs out.
- [`DEBUGGING.md`](./DEBUGGING.md) — **How bugs get found.** Serial
  logging format, panic handling, tracing, stack walking, GDB + QEMU
  monitor workflow, assertions, memory debugging, and a methodology
  checklist for diagnosing crashes.
- [`MAKAOS.md`](./MAKAOS.md) — **Project-specific rules.** Conventions
  that only apply to MakaOS: the build system, subsystem boundaries,
  flagged do-not-touch areas, and tooling quirks.

## Precedence

When two rules appear to conflict, resolve in this order:

1. `MAKAOS.md` — project-specific reality wins over general style.
2. `PRINCIPLES.md` — architecture wins over cosmetics.
3. `CODE_STYLE.md` — style is the default, not the tiebreaker.

## How to use as an agent

Before writing or modifying code:

1. Read the relevant section of `PRINCIPLES.md` for the kind of change
   you're about to make (e.g., new subsystem → read the modularity
   section; touching a hot path → read the performance section).
2. Check `MAKAOS.md` for any project-specific overrides or do-not-touch
   flags.
3. Apply `CODE_STYLE.md` as you write.
4. If a rule seems wrong for the situation, **stop and ask** — do not
   silently deviate.

## Status

These are strong defaults with documented exceptions. A rule can be
broken when the rule itself says it can, or when a deviation is
explicitly justified in the code or commit message. Silent deviation is
not a valid exception.
