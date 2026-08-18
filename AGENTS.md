# AGENTS.md - Agent Guidelines for retool

This file provides guidance for AI coding agents working on this repository.

## Project Overview

`retool` is a Windows C++ command-line utility for inspecting and working with ReFS (Resilient File System) files at the block level. It is built with CMake and targets the Windows SDK/DDK only - no external libraries.

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
│   ├── inspect.cpp         # Block layout inspection, volume scan engine & dedup analysis
│   ├── inspect.h
│   ├── copy.cpp            # Deduplication-preserving copy (block cloning)
│   ├── copy.h
│   ├── dedup.cpp           # In-place file deduplication (pair-wise and volume-wide)
│   ├── dedup.h
│   ├── output.cpp          # Polymorphic output (CLI, JSON, silent)
│   ├── output.h
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
| `main.cpp` | CLI entry point, environment setup (e.g., elevation checks), command dispatching, outputter construction. |
| `inspect` | Retrieval pointer queries (`FSCTL_GET_RETRIEVAL_POINTERS`), single-file extent dumps, cross-file deduplication mapping, volume-wide LCN scan engine with optional SHA-256 hashing via Windows CNG. |
| `copy` | Directory tree recursion, block duplication (`FSCTL_DUPLICATE_EXTENTS_TO_FILE`), fallback to standard copy, dry-run simulation, optional destination pre-scan (`--scan-dest`). |
| `dedup` | In-place deduplication: volume-wide (`VolumeWideDedupStrategy`) and pair-wise (`PairwiseDedupStrategy`) modes; uses `inspect::build_lcn_index` with `kWithHash` and issues `FSCTL_DUPLICATE_EXTENTS_TO_FILE`. |
| `output` | Polymorphic output abstraction (`IOutput` base): `NoOutput` (silent), `CliOutput` (console + progress bar), `JsonOutput` (nlohmann/json). |
| `volume` | Volume information retrieval (cluster size, total clusters, free/used space). |
| `util` | CLI argument parsing, wide/narrow string conversions, date/time formatting, memory sizing helpers. |

## Technical Constraints

- **Windows SDK/DDK only.** Do not introduce any third-party libraries (Boost, etc.) without explicit user approval. Approved: `nlohmann/json` (header-only, via CMake FetchContent).
- **C++20** minimum (`std::expected`, structured bindings, ranges).
- **Administrator privileges** are required at runtime; do not attempt to work around this.
- **Unicode throughout.** Use `wchar_t` / `std::wstring` / `LPWSTR` for all paths and system strings.
- **ReFS ioctls** of primary interest:
  - `FSCTL_GET_RETRIEVAL_POINTERS` - VCN→LCN extent map for a file
  - `FSCTL_DUPLICATE_EXTENTS_TO_FILE` - block clone (same-volume ReFS only)
  - `FSCTL_QUERY_EXTENT_METADATA` - ReFS-specific extent metadata (undocumented; use carefully)

## Workflow

Before any implementation task:
1. Read the relevant skill files listed in the table above.
2. Review `doc/features.md` for the full technical spec of the feature being implemented.
3. If introducing a new idea or notable deferral, add an entry to `doc/ideas.md` following the `manage-ideas` skill format.
4. Follow the Implementation Workflow in `code-convention` (doc → implement → build → test).

## Build & Test

The easiest way to setup the build environment is to call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

```cmd
# Configure (once, or after CMakeLists.txt changes)
cmake --preset debug
cmake --preset release

# Debug build
cmake --build build/debug --config Debug

# Release build
cmake --build build/release --config Release
```

> `CMakePresets.json` only defines `configurePresets`, not `buildPresets` - `cmake --build --preset <name>` will fail. Always build with `cmake --build <dir> --config <Debug|Release>` as shown above. See [doc/build.md](doc/build.md) for full instructions, including the fallback path when `cmake` isn't on your system PATH.

> Note: Test infrastructure is not yet defined. When adding tests, follow the conventions established by the first test added and document the approach here.

## Production

- **Branching Strategy:** Do not perform any development directly on the `main` branch. Use the `devel` branch or feature-specific branches (e.g., `feature/...`) for all development work.
- **Commit Cadence:** Make frequent commits. Only commit changes after verifying that the code compiles successfully, passes linting (`clang-tidy`), and passes all existing tests.
- **Standards:**
  - **Versioning:** The project uses semantic versioning.
  - **Commit Messages:** Follow the Conventional Commits specification.
  - **Documentation:** Use Doxygen formatting for inline code documentation.
- **Tags:** Version tags (e.g., `v*`) must only be created/added by the user directly. Do not automate or push version tags.

## Architectual Considerations

### 1. High-Level Architectural Patterns

To describe the structure where the command-line utility delegates specific subcommands to independent files/execution paths, use the following terms:

#### Routing and Execution: **Command Pattern**

The **Command Pattern** is the standard behavioral design pattern for CLI applications with subcommands (`inspect`, `copy`, `deduplicate`).

* **Application:** The main entry point acts as the *Invoker* or *Router*, parsing the command-line arguments and routing execution to a specific, self-contained command object or function (e.g., `execute_inspect`).
* **Documentation Terminology:** *"The application implements the **Command Pattern** for subcommand routing. Each subcommand (e.g., `inspect`, `copy`) must encapsulate its execution logic within an isolated module."*

#### Separation of Concerns: **Modular Design / Layered Architecture**

By isolating each subcommand into its own dedicated file (e.g., `inspect.cpp`), you are enforcing strict modularity.

* **Documentation Terminology:** *"The codebase enforces a **Modular Architecture** with strict **Separation of Concerns (SoC)**. Subcommand implementations must remain completely decoupled from one another; shared logic must be abstracted into common utility modules."*

---

### 2. Subcommand Execution Lifecycle (Prepare, Execute, Cleanup)

Your three-step execution sequence aligns with several critical defensive programming and transactional execution paradigms:

```
[ Step 1: Prepare ] --------> [ Step 2: Execute ] --------> [ Step 3: Cleanup ]
(Validation & Phase)         (Non-Destructive Work)       (RAII & Summary Phase)

```

#### Phase 1 (Prepare): **Input Validation and Precondition Checking**

This phase focuses on ensuring the system state and inputs are valid before any side effects occur.

* **Design by Contract (DbC):** A paradigm where software correctness is guaranteed by checking **Preconditions** before a function runs.
* **Fail-Fast Principle:** The practice of checking constraints early and terminating execution immediately if a violation is detected, preventing the system from entering an unstable state.
* **Documentation Terminology:** *"The **Prepare** phase must implement a **Fail-Fast** approach by validating all **Preconditions** and constraints before mutating state or initiating data operations."*

#### Phase 2 (Execute): **Transactional Execution & Single Mutation Point**

To avoid destructive operations during execution (e.g., preventing partial writes or corrupted files if a failure occurs mid-process), you are describing transactional safety.

* **ACID Semantics (specifically Atomicity):** The operation should either completely succeed or fail with no side effects (all-or-nothing).
* **Copy-on-Write / Shadow Staging:** If writing data, working on a temporary copy or staging area before committing ensures the original data is never corrupted mid-execution.
* **Documentation Terminology:** *"The **Execute** phase must prioritize **Atomic Operations** and transactional safety. Avoid in-place destructive operations; state mutations or file writes should utilize staging mechanisms to ensure data integrity in the event of an interruption."*

#### Phase 3 (Cleanup): **RAII and Postcondition Validation**

C++ handles resource management through a specific core paradigm that automates cleanup.

* **RAII (Resource Acquisition Is Initialization):** The fundamental C++ paradigm where resource lifecycle (closing files, freeing memory, releasing locks) is bound to object lifetime via destructors. This ensures cleanup happens even if the program throws an error.
* **Postcondition Validation:** Verifying that the output matches expectations and the system is left in a valid state.
* **Documentation Terminology:** *"The **Cleanup** phase must handle **Postcondition Validation** and reporting. All resource management (memory, file handles, descriptors) must strictly adhere to **RAII** principles to guarantee leaks are avoided during normal or exceptional termination."*

---

### 3. Helper Functions and Object Passing

Your requirements for reusable functions and object-oriented data passing map directly to core SOLID design principles and clean code clean practices:

#### Object Passing: **Domain-Driven Objects / Data Transfer Objects (DTO)**

Instead of passing loose, primitive variables (like strings and integers) through helper functions, you want to pass structured objects that represent the data entities.

* **Domain Modeling:** Grouping related data and behavior into structured types that reflect real-world entities.
* **Data Transfer Objects (DTO):** Structures or classes specifically designed to carry data between processes or functions to reduce function signature complexity.
* **Documentation Terminology:** *"Helper functions must favor passing **Domain Objects** or **Data Transfer Objects (DTOs)** rather than long lists of primitive parameters. Avoid primitive obsession."*

#### Code Reusability: **DRY Principle & Parameterization**

To avoid "highly similar functions that do almost the same thing," you are invoking a foundational software engineering axiom.

* **DRY (Don't Repeat Yourself):** A core principle aimed at reducing the repetition of software patterns.
* **Parameterization / Abstraction:** Creating a single, highly flexible function that uses parameters, configuration objects, or templates to alter its behavior, rather than duplicating the code logic.
* **Documentation Terminology:** *"Strictly adhere to the **DRY (Don't Repeat Yourself)** principle. Avoid creating redundant, highly similar helper functions. Instead, design single, highly flexible functions utilizing **Parameterization** or configuration structs to handle behavioral variations."*