# AGENTS.md — Agent Guidelines for retool

This file provides guidance for AI coding agents working on this repository.

## Project Overview

`retool` is a Windows C++ command-line utility for inspecting and working with ReFS (Resilient File System) files at the block level. It is built with CMake and targets the Windows SDK/DDK only — no external libraries.

See [README.md](README.md) for full feature descriptions and [doc/features.md](doc/features.md) for detailed technical specifications.

## Active Skills

The following skills are active for this project. **Read the referenced SKILL.md before performing any task that falls within that skill's domain.**

| Skill | Path | When to Use |
|-------|------|-------------|
| `code-convention` | [`.agents/skills/code-convention/SKILL.md`](.agents/skills/code-convention/SKILL.md) | All C++ implementation work |
| `cpp-development` | (global) | CMake configuration, build management |
| `manage-ideas` | [`.agents/skills/manage-ideas/SKILL.md`](.agents/skills/manage-ideas/SKILL.md) | Adding entries to `doc/ideas.md` |

## Code Conventions Summary

> Full rules are in `.agents/skills/code-convention/SKILL.md`. The summary below is for quick reference.

- **Casing:** `PascalCase` for classes/structs/types; `snake_case` for namespaces; `kPascalCase` or `UPPERFLAT` for constants; `snake_case` or `camelCase` for variables/methods (be consistent within a file).
- **Singularity Rule:** Classes, structs, enums, files, namespaces must be singular.
- **Error Handling:** Prefer `std::expected<T, E>` or similar result types; avoid exceptions as control flow.
- **Imports:** System headers with `<>`, project headers with `""`. Sorted alphabetically within each group, separated by blank lines.
- **Logging:** Syslog-style: `[FileName.memberName] message`. `info` at task entry, `debug` at method entry/exit, `trace` for detail.

## Architecture

`retool` uses a feature-driven architecture. Because of the simplistic nature of the project, all layered subfolders (`controller`, `service`, `model`, `util`) are omitted. Each feature is self-contained within its own source file (and optional header) directly in `src/`.

```
retool/
├── src/
│   ├── main.cpp            # Entry point and CLI parsing
│   ├── inspect.cpp         # Block layout inspection & dedup analysis
│   ├── inspect.h
│   ├── copy.cpp            # Deduplication-preserving copy (block cloning)
│   ├── copy.h
│   ├── volume.cpp          # Volume-level statistics
│   ├── volume.h
│   ├── util.cpp            # Project-wide pure functions and shared helpers
│   └── util.h
├── doc/
│   ├── features.md         # Detailed feature specifications (technical)
│   └── ideas.md            # Feature idea backlog
├── CMakeLists.txt
├── CMakePresets.json
├── README.md
└── AGENTS.md               # This file
```

### Module Responsibilities

| Module | Responsibility |
|-------|---------------|
| `main.cpp` | CLI entry point, environment setup (e.g., elevation checks), command dispatching. |
| `inspect` | Retrieval pointer queries (`FSCTL_GET_RETRIEVAL_POINTERS`), single-file extent dumps, cross-file deduplication mapping, and plain-text output serialization for inspection. |
| `copy` | Directory tree recursion, block duplication (`FSCTL_DUPLICATE_EXTENTS_TO_FILE`), fallback to standard copy, dry-run simulation. |
| `volume` | Volume information retrieval (cluster size, total clusters, free/used space). |
| `util` | CLI argument parsing, wide/narrow string conversions, date/time formatting, memory sizing helpers. |

## Technical Constraints

- **Windows SDK/DDK only.** Do not introduce any third-party libraries (Boost, etc.) without explicit user approval.
- **C++20** minimum (`std::expected`, structured bindings, ranges).
- **Administrator privileges** are required at runtime; do not attempt to work around this.
- **Unicode throughout.** Use `wchar_t` / `std::wstring` / `LPWSTR` for all paths and system strings.
- **ReFS ioctls** of primary interest:
  - `FSCTL_GET_RETRIEVAL_POINTERS` — VCN→LCN extent map for a file
  - `FSCTL_DUPLICATE_EXTENTS_TO_FILE` — block clone (same-volume ReFS only)
  - `FSCTL_QUERY_EXTENT_METADATA` — ReFS-specific extent metadata (undocumented; use carefully)

## Workflow

Before any implementation task:
1. Read the relevant skill files listed in the table above.
2. Review `doc/features.md` for the full technical spec of the feature being implemented.
3. If introducing a new idea or notable deferral, add an entry to `doc/ideas.md` following the `manage-ideas` skill format.
4. Follow the Implementation Workflow in `code-convention` (doc → implement → build → test).

## Build & Test

```powershell
# Debug build
cmake --preset debug
cmake --build --preset debug

# Release build
cmake --preset release
cmake --build --preset release
```

> Note: Test infrastructure is not yet defined. When adding tests, follow the conventions established by the first test added and document the approach here.
