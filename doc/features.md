# doc/features.md — retool Feature Specifications

This document provides technical detail for each feature implemented in retool. It is intended to guide agent implementation and design decisions. For user-facing documentation see [README.md](../README.md).

---

## Index

* [Feature 1: inspect — Block Layout, Sharing Analysis & Volume Scan](#feature-1-inspect--block-layout-sharing-analysis--volume-scan)
* [Feature 2: copy — Deduplication-Preserving File Copy](#feature-2-copy--deduplication-preserving-file-copy)
* [Feature 3: dedup — In-Place File Deduplication](#feature-3-dedup--in-place-file-deduplication)
* [Feature 4: volume — Volume-Level Block Statistics](#feature-4-volume--volume-level-block-statistics)
* [CLI Design](#cli-design)
* [Output Format](#output-format)
* [Error Handling & Privilege Model](#error-handling--privilege-model)
* [Data Model](#data-model)

---

## Feature 1: `inspect` — Block Layout, Sharing Analysis & Volume Scan

### Purpose

Report the physical block layout of one or more files on a ReFS volume. When multiple files are provided, compute cross-file block sharing to measure deduplication savings. When given a volume root, perform a full-volume scan and report aggregate deduplication potential.

### Single-File Mode

Invoked when exactly one file path is supplied (and it is not a volume root).

**Goal:** Enumerate every extent (fragment) of the file and print each VCN→LCN mapping with size and cumulative offset.

**Win32 API Sequence:**

1. `CreateFileW` — open the file with `FILE_FLAG_BACKUP_SEMANTICS | GENERIC_READ | FILE_SHARE_READ | FILE_SHARE_WRITE`. Requires Administrator.
2. `GetVolumePathNameW` — extract the volume root from the file path.
3. `GetDiskFreeSpaceW` — obtain cluster size.
4. Loop: `DeviceIoControl(FSCTL_GET_RETRIEVAL_POINTERS)` — iteratively query extents. Pass `STARTING_VCN_INPUT_BUFFER` starting at VCN 0; on each call the last `NextVcn` becomes the next starting VCN. Stop when the call returns `true` (all extents fit) or `ERROR_HANDLE_EOF` (sparse/small file).

**Output per extent:**

```
Extent #  VCN          LCN          Clusters    Bytes        Cumulative
0         0x00000000   0x001A3F00   128          524288       524288
1         0x00000080   0x001B0040   256          1048576      1572864
...
```

### Multi-File Mode

Invoked when two or more file paths are supplied (directly on CLI or via `-i <filelist>`).

**Goal:** For each unique LCN present in more than one file, count how many files share it. Report cross-file sharing as a matrix and compute aggregate savings.

**Algorithm:**

1. Enumerate all extents for each file via `FSCTL_GET_RETRIEVAL_POINTERS`.
2. Build an `unordered_map<LONGLONG, vector<size_t>> lcn_to_files` — LCN → list of file indices that contain it.
3. Scan the map: clusters with two or more file references are shared. Compute shared bytes and savings percentage.
4. Build a per-file sharing matrix for tabular output.

**Output (multi-file):**

```
Volume:        E:\
Cluster Size:  65536 bytes
Files Analyzed: 3
Total Sizes:   10.23 MB (10726400 bytes)
Shared Blocks: 4.00 MB (4194304 bytes)
Saved Space:   4.00 MB (4194304 bytes)
Dedup Savings: 39.08%

[Sharing matrix table: each cell shows MB shared between file pair Fx and Fy]
```

### Volume Scan Mode

Invoked when a single argument is a volume root (e.g. `E:\`).

**Goal:** Walk every file on the volume, build a full LCN index, and report aggregate deduplication savings. Optionally hash every cluster to enable content-based matching.

**Implementation — `inspect::build_lcn_index()`:**

1. `enumerate_files_recursive()` — walks the volume tree with `FindFirstFileW` / `FindNextFileW`. Skips `FILE_ATTRIBUTE_SYSTEM` files and `FILE_ATTRIBUTE_REPARSE_POINT` junctions.
2. For each file, calls `inspect_file()` to obtain all extents via `FSCTL_GET_RETRIEVAL_POINTERS`.
3. Populates `LcnIndex` (`unordered_map<LONGLONG, vector<BlockEntry>>`): maps each LCN to the file(s) and byte offsets that reference it.
4. In `kWithHash` mode: additionally reads each cluster and computes a SHA-256 digest via the Windows CNG BCrypt API (`BCryptOpenAlgorithmProvider(BCRYPT_SHA256_ALGORITHM)`, `BCryptCreateHash`, `BCryptHashData`, `BCryptFinishHash`). The hardware SHA-NI instruction set is used automatically when available. Populates `HashIndex` (`unordered_map<string, vector<LONGLONG>>`): SHA-256 hex digest → list of LCNs with identical content.
5. Reports progress via `IOutput::status()` every 500 files.

**Scan modes (`ScanMode` enum):**

| Mode | Description |
|------|-------------|
| `kLcnOnly` | Build LCN→file map without reading disk data (fast; reports structurally shared clusters only) |
| `kWithHash` | Also SHA-256 hash every cluster; enables content-based dedup matching across unrelated files |

**Public types exported from `inspect.h`:**

```cpp
struct BlockEntry { std::wstring file_path; ULONGLONG file_offset; };
using LcnIndex  = std::unordered_map<LONGLONG, std::vector<BlockEntry>>;
using HashIndex = std::unordered_map<std::string, std::vector<LONGLONG>>;

struct ScanResult {
    LcnIndex  lcn_index;
    HashIndex hash_index;       // kWithHash only
    DWORD     cluster_size;
    std::wstring volume_root;
    ULONGLONG files_scanned;
    ULONGLONG clusters_indexed;
    std::vector<std::wstring> errors;
};

std::expected<ScanResult, std::wstring> build_lcn_index(
    const std::wstring& volume_root, ScanMode mode, output::IOutput& status_out);
```

**Output (volume scan):**

```
Volume:           E:\
Cluster Size:     65536 bytes
Files Scanned:    14382
Clusters Indexed: 892104
Shared Blocks:    1.23 GB (19847 clusters)
Dedup Savings:    512 MB saved
Savings %:        14.23%
Content Groups:   8841       (kWithHash only)
```

### Input File (`-i <file>`)

- One absolute path per line (UTF-8 or UTF-16 LE with BOM).
- Blank lines and lines starting with `#` are ignored.
- Paths are validated before processing begins; invalid paths are reported and skipped (default best-effort) or abort (with `--strict`).

### Options

| Flag | Description |
|------|-------------|
| `-i <file>` | Read file paths from a newline-delimited input file |
| `-o <file>` | Write output to a file instead of stdout |
| `--strict` | Abort on first error (default: best-effort with error summary) |
| `--json` | Output results in JSON format |
| `-q` | Suppress all output |

---

## Feature 2: `copy` — Deduplication-Preserving File Copy

### Purpose

Copy one or more files (or a full directory tree) while preserving ReFS block sharing using `FSCTL_DUPLICATE_EXTENTS_TO_FILE` or target-volume deduplication.

### Pipeline Architecture

`execute_copy` is a three-phase pipeline:

1. **Inspection** (`inspect_and_prepare`) — resolve paths, query volume topology, select strategy.
2. **Operation** (`copy_directory_recursive` / `ICopyStrategy::copy_file`) — perform the copy.
3. **Finalization** (`finalize_and_report`) — emit summary statistics.

### Strategy Selection

| Condition | Strategy |
|-----------|----------|
| Same volume | `SameVolumeCopyStrategy` — pure `FSCTL_DUPLICATE_EXTENTS_TO_FILE` |
| Different volumes, dest is ReFS, cluster sizes match | `CrossVolumeRefsCopyStrategy` — LCN-mapped dedup-preserving copy |
| Otherwise | `FallbackCopyStrategy` — standard `CopyFileExW` |

### Same-Volume Clone (`SameVolumeCopyStrategy`)

1. `CreateFileW` on source — `GENERIC_READ | FILE_SHARE_READ`, `FILE_FLAG_BACKUP_SEMANTICS`.
2. `CreateFileW` on destination — `GENERIC_READ | GENERIC_WRITE`, `CREATE_ALWAYS`, `FILE_FLAG_BACKUP_SEMANTICS`. Mark sparse via `FSCTL_SET_SPARSE`.
3. Pre-size destination with `SetEndOfFile` (falls back to incremental sizing on disk-full).
4. Query source extents via `FSCTL_GET_RETRIEVAL_POINTERS`.
5. For each non-sparse extent: `DeviceIoControl(FSCTL_DUPLICATE_EXTENTS_TO_FILE)`.
6. Finalize size and copy timestamps/attributes via `SetFileInformationByHandle(FileBasicInfo)`.

### Cross-Volume Copy (`CrossVolumeRefsCopyStrategy`)

Maintains `CopyContext::lcn_map` — a mapping from source LCN to `{dest_file_path, dest_byte_offset}` tracking every cluster already written to the destination.

For each extent of each source file, processes clusters in runs:

- **Duplicate LCN run** (`clone_duplicate_run`): the source LCN is already in `lcn_map` — issue `FSCTL_DUPLICATE_EXTENTS_TO_FILE` using the previously-copied destination block. Scans forward to find the longest contiguous run that maps to contiguous destination offsets, cloning in a single ioctl call.
- **New LCN run** (`copy_new_run`): LCN not seen before — physically copy bytes from source to destination in 4 MB chunks (`copy_bytes_physical`), then record every LCN in the run in `lcn_map`.

Progress is reported per-cluster run via `IOutput::progress()`.

### Destination Pre-Scan (`--scan-dest`)

When `--scan-dest` is passed and the strategy is `CrossVolumeRefsCopyStrategy`, an additional **Phase 1b** runs before any file is copied:

1. Calls `inspect::build_lcn_index(dest_volume_root, kWithHash, out)`.
2. Seeds `CopyContext::lcn_map` from the resulting `LcnIndex` — each destination LCN is recorded as a pre-existing clone source.
3. Stores the `HashIndex` in `CopyContext::hash_index` for future content-based matching.

This allows blocks already physically present on the destination (from a prior copy or dedup operation) to be cloned rather than re-transferred.

> **Performance note:** `--scan-dest` reads every cluster on the destination volume to compute SHA-256 hashes. On large volumes this adds significant setup time. Use when the destination already holds substantial overlapping data.

### Fallback Copy (`FallbackCopyStrategy`)

Uses `CopyFileExW` with a progress callback forwarded to `IOutput::progress()`. Issued when the destination is non-ReFS or cluster sizes differ.

### Directory Copy

- Walk source with `FindFirstFileW` / `FindNextFileW`.
- Mirror directory structure at destination using `CreateDirectoryW`.
- Skip `FILE_ATTRIBUTE_SYSTEM` entries.
- Best-effort by default — errors recorded in `CopyStats::errors`, reported in finalization. `--strict` aborts on first error.

### Cancellation

A global `std::atomic<bool> copy::g_cancel_requested` is checked at every copy-loop iteration and extent boundary. A `ConsoleCtrlHandler` sets it on `Ctrl+C`. Partial destination files are deleted on cancellation.

### Options

| Flag | Description |
|------|-------------|
| `-r` | Recursive directory copy |
| `--dry-run` | Simulate without writing |
| `--scan-dest` | Pre-scan destination volume to seed the dedup block index |
| `--strict` | Abort on first error |
| `--json` | Output results in JSON format |
| `-q` | Suppress all output |
| `-o <file>` | Redirect output to a file |

---

## Feature 3: `dedup` — In-Place File Deduplication

### Purpose

Deduplicate files already resident on a ReFS volume in-place using `FSCTL_DUPLICATE_EXTENTS_TO_FILE`. Identifies clusters with identical SHA-256 content and replaces physical duplicates with shared block references — reclaiming disk space without modifying file content.

### Pipeline Architecture

`execute_dedup` is a three-phase pipeline:

1. **Inspection** (`inspect_and_prepare`) — validate arguments, verify ReFS, run `build_lcn_index(kWithHash)`, select strategy.
2. **Operation** (`execute_operation`) — apply `FSCTL_DUPLICATE_EXTENTS_TO_FILE` to each candidate cluster.
3. **Finalization** (`finalize_and_report`) — emit summary statistics.

### Modes and Strategy Selection

| Invocation | Strategy | Description |
|-----------|----------|-------------|
| `retool dedup <volume-root>` | `VolumeWideDedupStrategy` | Deduplicates all hash-matched clusters across the entire volume |
| `retool dedup <file1> <file2>` | `PairwiseDedupStrategy` | Deduplicates matching clusters between exactly two named files |

### Volume-Wide Deduplication (`VolumeWideDedupStrategy`)

1. Receives the `ScanResult` from `build_lcn_index(kWithHash)`.
2. Iterates `HashIndex` — for each SHA-256 digest with two or more LCNs:
   - Designates `lcns[0]` as the canonical (master) cluster.
   - For each subsequent LCN in the group: produces a `DedupCandidate` with `canonical_path/offset` and `duplicate_path/offset`.
3. Returns the full candidate list for Phase 2 execution.

### Pair-Wise Deduplication (`PairwiseDedupStrategy`)

1. Partitions the `LcnIndex` by file — identifies exactly two distinct file paths.
2. Iterates `HashIndex` — for each digest where one LCN belongs to file A and one to file B: produces a `DedupCandidate` pointing from file A (canonical) to file B (duplicate).
3. Skips hashes where both LCNs belong to the same file.

### Operation — Cluster-Level Dedup

For each `DedupCandidate`:

1. Opens the canonical file read-only and the duplicate file read/write (handles cached across candidates to avoid per-cluster `CreateFileW` overhead).
2. Issues `DeviceIoControl(FSCTL_DUPLICATE_EXTENTS_TO_FILE)` with `ByteCount = cluster_size`.
3. Respects `g_cancel_requested` for cooperative Ctrl+C cancellation.
4. In `--dry-run` mode: increments stats counters without issuing the ioctl.

### DedupCandidate Structure

```cpp
struct DedupCandidate {
    std::wstring canonical_path;    // File that owns the canonical cluster
    ULONGLONG    canonical_offset;  // Byte offset within canonical_path
    std::wstring duplicate_path;    // File to receive the clone
    ULONGLONG    duplicate_offset;  // Byte offset within duplicate_path
    ULONGLONG    cluster_size;      // Bytes in this cluster
};
```

### Output (summary)

```
Files Processed:  142
Clusters Deduped: 8192
Space Reclaimed:  512.00 MB (536870912 bytes)
```

### Options

| Flag | Description |
|------|-------------|
| `--dry-run` | Report savings without writing |
| `--strict` | Abort on first error |
| `--json` | Output results in JSON format |
| `-q` | Suppress all output |

> [!IMPORTANT]
> Both files must reside on the same ReFS volume. The operation modifies file allocation metadata; ensure backups exist before running volume-wide dedup on production data.

---

## Feature 4: `volume` — Volume-Level Block Statistics

### Purpose

Show ReFS volume metadata and a high-level summary of cluster usage.

### Output

```
Volume:         E:\
File System:    ReFS
Cluster Size:   65536 bytes
Total Clusters: 2,621,440
Total Space:    10.0 GB
Free Space:     4.2 GB
Used Space:     5.8 GB
```

### Win32 APIs

- `GetVolumeInformationW` — file system name and flags.
- `GetDiskFreeSpaceW` — sectors per cluster, bytes per sector.
- `GetDiskFreeSpaceExW` — total and free bytes (64-bit safe).

---

## CLI Design

### Subcommand Dispatch

```
retool <command> [options] [arguments]
```

| Command | Alias | Description |
|---------|-------|-------------|
| `inspect` | `i` | Inspect block layout, sharing analysis, or volume scan |
| `copy` | `cp` | Copy files preserving deduplication |
| `dedup` | `dd` | Deduplicate files in-place on a ReFS volume |
| `volume` | `vol` | Show volume information |
| `help` | `h`, `?` | Show usage |
| `version` | | Print version string |

### Global Options

| Flag | Description |
|------|-------------|
| `--json` | Output in JSON format (uses nlohmann/json) |
| `-q` | Suppress all output (quiet/silent mode) |
| `-o <file>` | Redirect output to a file |

### Argument Parsing

Implemented in `src/util.cpp` (`util::parse_arguments`). Uses the wide-character `argv[]` array from `wmain`. No third-party CLI library. Rules:
- Short flags: single `-` + single character (e.g., `-r`, `-i`, `-q`).
- Long flags: double `--` + word (e.g., `--strict`, `--dry-run`, `--scan-dest`).
- Unknown flags: print a clear error and exit with code 1.

---

## Output Format

### Plain Text (CLI)

Human-readable, column-aligned. Rendered via `output::CliOutput`:
- `status()` — single-line informational messages.
- `field(name, value)` — key/value pairs.
- `begin_table(columns)` / `table_row(values)` / `end_table()` — tabular output with auto-sized columns.
- `begin_section(title)` / `end_section()` — groups related output.
- `progress(filename, bytes_done, bytes_total)` — in-place progress bar overwriting the current line.
- `warn()` / `error()` — prefixed warning and error messages.

### JSON

`output::JsonOutput` buffers all structured output into a `nlohmann::json` document and serializes it on `flush()` (called at program exit). Progress calls are ignored.

### Silent

`output::NoOutput` discards all output. All virtual methods are no-ops.

---

## Error Handling & Privilege Model

### Privilege Check

At startup, before any operation:
1. `OpenProcessToken` + `GetTokenInformation(TokenElevation)` — verify the process is elevated.
2. If not elevated, print a clear error and exit with code 1:
   ```
   ERROR: retool requires Administrator privileges.
          Please re-run from an elevated command prompt.
   ```

### Error Reporting

- All errors include the Win32 error code and message via `FormatMessageW` (wrapped in `util::get_win32_error_message`).
- Default (best-effort): errors collected and reported in finalization summary.
- `--strict`: any error immediately exits with code 2.
- Exit codes:
  - `0` — success
  - `1` — usage / privilege error
  - `2` — operational error (file not found, ioctl failed, etc.)

---

## Data Model

Key types as implemented:

| Type | Location | Description |
|------|----------|-------------|
| `util::CliArg` | `src/util.h` | Parsed command-line arguments (command, positional, flags) |
| `inspect::ScanMode` | `src/inspect.h` | Enum: `kLcnOnly` or `kWithHash` |
| `inspect::BlockEntry` | `src/inspect.h` | File path + byte offset for one cluster reference |
| `inspect::LcnIndex` | `src/inspect.h` | `unordered_map<LONGLONG, vector<BlockEntry>>` — LCN→file map |
| `inspect::HashIndex` | `src/inspect.h` | `unordered_map<string, vector<LONGLONG>>` — SHA-256→LCN map |
| `inspect::ScanResult` | `src/inspect.h` | Output of `build_lcn_index`: indexes, stats, errors |
| `copy::CopyContext` | `src/copy.cpp` | Pipeline state: paths, volumes, strategy, lcn_map, hash_index, stats |
| `copy::ICopyStrategy` | `src/copy.cpp` | Abstract interface: `copy_file(src, dest, args, context)` |
| `copy::CopyStats` | `src/copy.cpp` | Accumulated totals: files, bytes, cloned, fallback, errors |
| `dedup::DedupContext` | `src/dedup.cpp` | Pipeline state: volume, scan, strategy, candidates, stats |
| `dedup::IDedupStrategy` | `src/dedup.cpp` | Abstract interface: `build_candidates(scan, context)` |
| `dedup::DedupCandidate` | `src/dedup.cpp` | One cluster-level dedup operation: canonical+duplicate path/offset |
| `dedup::DedupStats` | `src/dedup.cpp` | Accumulated totals: files, clusters, bytes, errors |
| `output::IOutput` | `src/output.h` | Abstract output interface: status, field, table, progress, warn, error |
