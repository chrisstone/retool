# doc/features.md - retool Feature Specifications

This document provides technical detail for each feature implemented in retool. It is intended to guide agent implementation and design decisions. For user-facing documentation see [README.md](file:///c:/Users/chris.stone/workspace/retool/README.md).

---

## Index

* [Feature 1: inspect - Block Layout, Sharing Analysis & Volume Scan](#feature-1-inspect--block-layout-sharing-analysis--volume-scan)
* [Feature 2: copy - Deduplication-Preserving File Copy](#feature-2-copy--deduplication-preserving-file-copy)
* [Feature 3: dedup - In-Place File Deduplication](#feature-3-dedup--in-place-file-deduplication)
* [Feature 4: volume - Volume-Level Block Statistics](#feature-4-volume--volume-level-block-statistics)
* [CLI Design](#cli-design)
* [Output Format](#output-format)
* [Error Handling & Privilege Model](#error-handling--privilege-model)
* [Data Model](#data-model)

---

## Feature 1: [inspect](file:///c:/Users/chris.stone/workspace/retool/src/inspect.cpp) - Block Layout, Sharing Analysis & Volume Scan

### Purpose

Report the physical block layout of one or more files on a ReFS volume. When multiple files are provided, compute cross-file block sharing to measure deduplication savings. When given a volume root, perform a full-volume scan and report aggregate deduplication potential.

### Single-File Mode

Invoked when exactly one file path is supplied (and it is not a volume root).

**Goal:** Enumerate every extent (fragment) of the file and print a summary. Add `-e` to include the full VCN→LCN extent table. Add `-r` to append a fragmentation report.

**Win32 API Sequence:**

1. `CreateFileW` - open the file with `FILE_FLAG_BACKUP_SEMANTICS | GENERIC_READ | FILE_SHARE_READ | FILE_SHARE_WRITE`. Requires Administrator.
2. `GetVolumePathNameW` - extract the volume root from the file path.
3. `GetDiskFreeSpaceW` - obtain cluster size.
4. Loop: `DeviceIoControl(FSCTL_GET_RETRIEVAL_POINTERS)` - iteratively query extents. Pass `STARTING_VCN_INPUT_BUFFER` starting at VCN 0; on each call the last `NextVcn` becomes the next starting VCN. Stop when the call returns `true` (all extents fit) or `ERROR_HANDLE_EOF` (sparse/small file).

**Default output (summary only):**

```
File:         E:\Data\backup.vbk
Volume:       E:\
Cluster Size: 65536 bytes
File Size:    104857600 bytes
Fragments:    3
```

**With `-e` (extent table appended):**

```
Extent #  VCN          LCN          Clusters    Bytes        Cumulative
0         0x00000000   0x001A3F00   128          524288       524288
1         0x00000080   0x001B0040   256          1048576      1572864
...
```

### Multi-File Mode

Invoked when two or more file paths are supplied (directly on CLI, via a directory or glob argument, or via `-i <filelist>`).

**Goal:** For each unique LCN present in more than one file, count how many files share it. Report a per-file unique/shared cluster breakdown, and compute aggregate savings. With `-e`, also append a cross-file sharing matrix table.

**Algorithm:**

1. Enumerate all extents for each file via `FSCTL_GET_RETRIEVAL_POINTERS`.
2. Build an interned map mapping LCN to file indices: `unordered_map<LONGLONG, vector<size_t>> lcn_to_files`.
3. Scan the map: clusters with two or more file references are shared. Compute shared bytes and savings percentage.
4. If `-e` is set, build a per-file sharing matrix for tabular output.
5. Compute per-file unique vs. shared cluster counts from the same `lcn_to_files` map.

**Default output (multi-file, summary & breakdown only):**

```
Volume:         E:\
Cluster Size:   65536 bytes
Files Analyzed: 2
Total Size:     200.00 MB (209715200 bytes)
Shared Blocks:  50.00 MB (52428800 bytes)
Saved Space:    50.00 MB (52428800 bytes)
Dedup Savings:  25.00%

── Per-File Cluster Breakdown ──

File             Total Clusters  Unique Clusters  Shared Clusters  Unique Bytes  Shared Bytes
backup.vbk       1600            800              800              50.00 MB      50.00 MB
backup2.vbk      1600            800              800              50.00 MB      50.00 MB
[TOTAL]          3200            1600             1600             100.00 MB     100.00 MB
```

**With `-e` (sharing matrix table appended):**

```
── Sharing Matrix ──

File             F0           F1
backup.vbk       -            50.00 MB
backup2.vbk      50.00 MB     -
```

### Volume Scan Mode

Invoked when a single argument is a volume root (e.g. `E:\`).

**Goal:** Walk every file on the volume, build a full LCN index, and report aggregate deduplication savings. Optionally hash every cluster to enable content-based matching.

**Implementation - [inspect::build_lcn_index()](file:///c:/Users/chris.stone/workspace/retool/src/inspect.h#L79-L83):**

1. [enumerate_files_recursive()](file:///c:/Users/chris.stone/workspace/retool/src/inspect.cpp#L270-L305) - walks the volume tree with `FindFirstFileW` / `FindNextFileW`. Skips `FILE_ATTRIBUTE_SYSTEM` files and `FILE_ATTRIBUTE_REPARSE_POINT` junctions.
2. For each file, calls `inspect_file()` to obtain all extents via `FSCTL_GET_RETRIEVAL_POINTERS`.
3. Populates [LcnIndex](file:///c:/Users/chris.stone/workspace/retool/src/inspect.h#L36) (`unordered_map<LONGLONG, vector<BlockEntry>>`): maps each LCN to the file(s) (by interned index) and byte offsets that reference it.
4. In `kWithHash` mode: additionally reads each cluster and computes a SHA-256 digest via the Windows CNG BCrypt API (`BCryptOpenAlgorithmProvider(BCRYPT_SHA256_ALGORITHM)`, `BCryptCreateHash`, `BCryptHashData`, `BCryptFinishHash`). The hardware SHA-NI instruction set is used automatically when available. Populates [HashIndex](file:///c:/Users/chris.stone/workspace/retool/src/inspect.h#L40) (`unordered_map<string, vector<LONGLONG>>`): SHA-256 hex digest → list of LCNs with identical content.
5. Reports progress via [IOutput::message()](file:///c:/Users/chris.stone/workspace/retool/src/output.h#L45) every 500 files.

**Scan modes ([ScanMode](file:///c:/Users/chris.stone/workspace/retool/src/inspect.h#L23-L26) enum):**

| Mode | Description |
|------|-------------|
| `kLcnOnly` | Build LCN→file map without reading disk data (fast; reports structurally shared clusters only) |
| `kWithHash` | Also SHA-256 hash every cluster; enables content-based dedup matching across unrelated files |

**Public types exported from [inspect.h](file:///c:/Users/chris.stone/workspace/retool/src/inspect.h):**

```cpp
struct BlockEntry {
    uint32_t  file_index;       // Index into ScanResult::file_table
    ULONGLONG file_offset;      // Byte offset within the file
};
using LcnIndex  = std::unordered_map<LONGLONG, std::vector<BlockEntry>>;
using HashIndex = std::unordered_map<std::string, std::vector<LONGLONG>>;

struct ScanResult {
    LcnIndex  lcn_index;
    HashIndex hash_index;       // kWithHash only
    DWORD     cluster_size = 0;
    std::wstring volume_root;
    ULONGLONG files_scanned = 0;
    ULONGLONG clusters_indexed = 0;
    std::vector<std::wstring> errors;
    std::vector<std::wstring> file_table; // Interned file paths

    const std::wstring& resolve_path(uint32_t index) const;
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
Dedup Savings:    512.00 MB (536870912 bytes)
Savings %:        14.23%
Content Groups:   8841       (kWithHash only)
```

### Input Expansion

Each positional argument is resolved before mode selection by `expand_path_glob()`:

| Argument type | Behaviour |
|---------------|-----------|
| Volume root (e.g. `E:\`) | Passed through to volume scan mode |
| Plain file path | Used as-is |
| Plain directory (no wildcards) | Recursively enumerates all non-system files via `enumerate_files_recursive` |
| Glob pattern (contains `*` or `?`) | Expanded non-recursively with `FindFirstFileW` in the pattern's directory portion |

Multiple arguments are each expanded independently and merged. A glob matching zero files is silently skipped (non-fatal warning emitted).

### Input File (`-i <file>`)

- One absolute path per line (UTF-8 or UTF-16 LE with BOM).
- Blank lines and lines starting with `#` are ignored.
- Paths are validated before processing begins; invalid paths are reported and skipped (default best-effort) or abort (with `-s`).

### Options

| Flag | Description |
|------|-------------|
| `-e` | Show VCN/LCN extent table (single-file mode) or sharing matrix (multi-file mode) |
| `-r` | Append a fragmentation report for each file (fragment count, min/max/avg extent, score) |
| `-i <file>` | Read file paths from a newline-delimited input file |
| `-o <file>` | Write output to a file instead of stdout |
| `-s` | Abort on first error (default: best-effort with error summary) |
| `-j` | Output results in JSON format |
| `-q` | Suppress all output (quiet mode) |

### Fragmentation Report (`-r`)

When `-r` is passed, a **Fragmentation Report** section is appended after the standard extent
table (single-file mode) or once per file (multi-file mode). Sparse extents are excluded from
all counts.

**`compute_frag_stat(const FileInspectResult&) → FragStat`:**

Iterates the file's `VcnExtent` list and accumulates:
- `fragment_count` - number of non-sparse extents
- `total_clusters` - sum of all non-sparse extent cluster counts
- `min/max_extent_clusters` - smallest and largest non-sparse extent
- `avg_extent_clusters` - mean extent size (`total_clusters / fragment_count`)

**`output_frag_report()` emitted fields:**

| Field | Description |
|-------|-------------|
| File | File path |
| Fragments | Number of non-sparse extents (1 = perfectly contiguous) |
| Frag Score | Fragment count with qualitative label: 1 = optimal, ≤4 = good, ≤16 = moderate, >16 = high |
| Smallest Extent | Min extent size in clusters and human-readable bytes |
| Largest Extent | Max extent size in clusters and human-readable bytes |
| Avg Extent | Mean extent size in clusters and human-readable bytes |

Byte formatting uses `util::format_size(ULONGLONG bytes)` - renders as KB/MB/GB/TB with 2 decimal places.

> [!NOTE]
> `-r` deliberately avoids `-f` (reserved for future `--force` semantics). In `copy` context, `-r` retains its meaning as recursive; in `inspect`, it means fragmentation report. Both map to `CliArg::recursive`.

---

## Feature 2: `copy` - Deduplication-Preserving File Copy

### Purpose

Copy one or more files (or a full directory tree) while preserving ReFS block sharing using `FSCTL_DUPLICATE_EXTENTS_TO_FILE` or target-volume deduplication.

### Pipeline Architecture

`execute_copy` is a three-phase pipeline:

1. **Inspection** (`inspect_and_prepare`) - resolve paths, query volume topology, select strategy.
2. **Operation** (`copy_directory_recursive` / `ICopyStrategy::copy_file`) - perform the copy.
3. **Finalization** (`finalize_and_report`) - emit summary statistics.

### Strategy Selection

| Condition | Strategy |
|-----------|----------|
| Same volume | `SameVolumeCopyStrategy` - pure `FSCTL_DUPLICATE_EXTENTS_TO_FILE` |
| Different volumes, dest is ReFS, cluster sizes match | `CrossVolumeRefsCopyStrategy` - LCN-mapped dedup-preserving copy |
| Otherwise | `FallbackCopyStrategy` - standard `CopyFileExW` |

### Same-Volume Clone (`SameVolumeCopyStrategy`)

1. `CreateFileW` on source - `GENERIC_READ | FILE_SHARE_READ`, `FILE_FLAG_BACKUP_SEMANTICS`.
2. `CreateFileW` on destination - `GENERIC_READ | GENERIC_WRITE`, `CREATE_ALWAYS`, `FILE_FLAG_BACKUP_SEMANTICS`. Mark sparse via `FSCTL_SET_SPARSE`.
3. Pre-size destination with `SetEndOfFile` (falls back to incremental sizing on disk-full).
4. Query source extents via `FSCTL_GET_RETRIEVAL_POINTERS`.
5. For each non-sparse extent: `DeviceIoControl(FSCTL_DUPLICATE_EXTENTS_TO_FILE)`.
6. Finalize size and copy timestamps/attributes via `SetFileInformationByHandle(FileBasicInfo)`.

### Cross-Volume Copy (`CrossVolumeRefsCopyStrategy`)

Maintains `CopyContext::lcn_map` - a mapping from source LCN to `{dest_file_path, dest_byte_offset}` tracking every cluster already written to the destination.

For each extent of each source file, processes clusters in runs:

- **Duplicate LCN run** (`clone_duplicate_run`): the source LCN is already in `lcn_map` - issue `FSCTL_DUPLICATE_EXTENTS_TO_FILE` using the previously-copied destination block. Scans forward to find the longest contiguous run that maps to contiguous destination offsets, cloning in a single ioctl call.
- **New LCN run** (`copy_new_run`): LCN not seen before - physically copy bytes from source to destination in 4 MB chunks (`copy_bytes_physical`), then record every LCN in the run in `lcn_map`.

Progress is reported per-cluster run via `IOutput::progress()`.

### Destination Pre-Scan (`-d`)

When `-d` is passed and the strategy is `CrossVolumeRefsCopyStrategy`, an additional **Phase 1b** runs before any file is copied:

1. Calls `inspect::build_lcn_index(dest_volume_root, kWithHash, out)`.
2. Seeds `CopyContext::lcn_map` from the resulting `LcnIndex` - each destination LCN is recorded as a pre-existing clone source.
3. Stores the `HashIndex` in `CopyContext::hash_index` for future content-based matching.

This allows blocks already physically present on the destination (from a prior copy or dedup operation) to be cloned rather than re-transferred.

> **Performance note:** `-d` reads every cluster on the destination volume to compute SHA-256 hashes. On large volumes this adds significant setup time. Use when the destination already holds substantial overlapping data.

### Fallback Copy (`FallbackCopyStrategy`)

Uses `CopyFileExW` with a progress callback forwarded to `IOutput::progress()`. Issued when the destination is non-ReFS or cluster sizes differ.

### Directory Copy

- Walk source with `FindFirstFileW` / `FindNextFileW`.
- Mirror directory structure at destination using `CreateDirectoryW`.
- Skip `FILE_ATTRIBUTE_SYSTEM` entries.
- Best-effort by default - errors recorded in `CopyStats::errors`, reported in finalization. `-s` aborts on first error.

### Cancellation

A global `std::atomic<bool> copy::g_cancel_requested` is checked at every copy-loop iteration and extent boundary. A `ConsoleCtrlHandler` sets it on `Ctrl+C`. Partial destination files are deleted on cancellation.

### Options

| Flag | Description |
|------|-------------|
| `-r` | Recursive directory copy |
| `-n` | Simulate without writing |
| `-d` | Pre-scan destination volume to seed the dedup block index |
| `-s` | Abort on first error |
| `-j` | Output results in JSON format |
| `-q` | Suppress all output (quiet mode) |
| `-o <file>` | Redirect output to a file |

---

## Feature 3: `dedup` - In-Place File Deduplication

### Purpose

Deduplicate files already resident on a ReFS volume in-place using `FSCTL_DUPLICATE_EXTENTS_TO_FILE`. Identifies clusters with identical SHA-256 content and rebuilds each file so its physical blocks are shared with other files — reclaiming disk space without modifying file content.

### Pipeline Architecture

`execute_dedup` is a three-phase pipeline:

1. **Inspection** (`inspect_and_prepare`) - validate arguments, verify ReFS, run `build_lcn_index(kWithHash)`, precompute auxiliary maps (`lcn_hash`, `file_clusters`), select strategy.
2. **Operation** (`execute_operation`) - rebuild each file one at a time using the uniform `rebuild_file()` protocol.
3. **Finalization** (`finalize_and_report`) - emit summary statistics.

### Modes and Strategy Selection

Strategies determine **which files to rebuild** and in what order. The rebuild operation itself is identical regardless of mode.

| Invocation | Strategy | Files Selected |
|-----------|----------|----------------|
| `retool dedup <volume-root>` | `VolumeWideDedupStrategy` | Every file on the volume with ≥1 hash-matched cluster in a different file |
| `retool dedup <file1> <file2>` | `PairwiseDedupStrategy` | `file2` only; `file1` is left untouched and serves as a source |

### File Rebuild Protocol

The same operation is used for every file regardless of how it was selected. For each `FileRebuildPlan` in order:

1. **Pre-check:** Evaluate source priority for all clusters. Skip this file if zero dedup candidates exist.
2. **Rename:** `MoveFileW(original_path → original_path + ".old")`.
3. **Create:** `CreateFileW(original_path, CREATE_NEW, GENERIC_READ|GENERIC_WRITE)`.
4. **Pre-size:** `SetFileInformationByHandle(FileEndOfFileInfo)` rounded up to a full cluster boundary. Trimmed to exact byte size after cloning.
5. **Clone all clusters** via `FSCTL_DUPLICATE_EXTENTS_TO_FILE` using `select_source()` with the following **priority per cluster**:
   - **Priority 1 — already-rebuilt files:** Files successfully rebuilt earlier in this same run. Maximises chain deduplication and avoids circular block dependencies.
   - **Priority 2 — other non-origin files:** Any other file on the volume with a matching SHA-256 hash at a different LCN.
   - **Priority 3 — origin `.old`:** Unique content (no match in another file). No space savings for this cluster.
6. **Trim:** `SetFileInformationByHandle(FileEndOfFileInfo)` to the exact original file size.
7. **Delete:** `DeleteFileW(original_path + ".old")`.
8. **Mark processed:** Added to `DedupContext::processed` so it is available as a Priority 1 source for all subsequent files.

**On failure at any step:** the partial new file is deleted and `.old` is renamed back to the original path. A `CRITICAL` error message is emitted if the rename-back also fails.

**Dry-run (`-n`):** The same source priority evaluation runs, but no filesystem changes are made. The `processed` set is still updated so estimated savings account for chain deduplication effects.

### `FileRebuildPlan` Structure

```cpp
struct FileRebuildPlan {
    uint32_t     file_index;       // Index into ScanResult::file_table
    std::wstring original_path;    // Path where the rebuilt file will land
    std::wstring old_path;         // Backup during rebuild (original_path + ".old")
    ULONGLONG    file_size;        // Exact byte size of the file
    // Clusters sorted ascending by file_offset: {file_offset, lcn}
    std::vector<std::pair<ULONGLONG, LONGLONG>> ordered_clusters;
};
```

Source selection happens dynamically in `rebuild_file()` via `select_source()`, consulting `DedupContext::lcn_hash` (LCN→digest reverse map), `DedupContext::scan` (hash_index + lcn_index), and `DedupContext::processed`.

### Output (summary)

```
Files Processed:  142
Clusters Deduped: 8192
Space Reclaimed:  512.00 MB (536870912 bytes)
```

### Options

| Flag | Description |
|------|-------------|
| `-n` | Report savings without writing |
| `-s` | Abort on first error |
| `-j` | Output results in JSON format |
| `-q` | Suppress all output (quiet mode) |

> [!IMPORTANT]
> Both files must reside on the same ReFS volume. The operation modifies file allocation metadata; ensure backups exist before running volume-wide dedup on production data.

---

## Feature 4: `volume` - Volume-Level Block Statistics

### Purpose

Show ReFS volume metadata and a high-level summary of cluster usage.

### Output

```
Volume:         E:\
File System:    ReFS
Cluster Size:   65536 bytes
Total Clusters: 2621440
Total Space:    10.00 GB (10737418240 bytes)
Free Space:     4.20 GB (4509715660 bytes)
Used Space:     5.80 GB (6227702580 bytes)
```

### Win32 APIs

- `GetVolumeInformationW` - file system name and flags.
- `GetDiskFreeSpaceW` - sectors per cluster, bytes per sector.
- `GetDiskFreeSpaceExW` - total and free bytes (64-bit safe).

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
| `-j` | Output in JSON format (uses nlohmann/json) |
| `-q` | Suppress all output (quiet mode) |
| `-o <file>` | Redirect output to a file |

### Argument Parsing

Implemented in `src/util.cpp` (`util::parse_arguments`). Uses the wide-character `argv[]` array from `wmain`. No third-party CLI library. Rules:
- Short flags only: single `-` followed by a single character (e.g., `-r`, `-e`, `-i`, `-o`, `-j`, `-q`, `-s`, `-n`, `-d`). Long options are not supported.
- Unknown options: print a clear error and exit with code 1.

---

## Output Format

### Plain Text (CLI)

Human-readable, column-aligned. Rendered via `output::CliOutput`:
- `message(Level, text)` - outputs informational (`Level::info`), warning (`Level::warn`), or error (`Level::error`) messages.
- `field(name, value)` - key/value pairs.
- `begin_table(columns)` / `table_row(values)` / `end_table()` - tabular output with auto-sized columns.
- `begin_section(title)` / `end_section()` - groups related output.
- `progress(filename, bytes_done, bytes_total)` - in-place progress bar overwriting the current line.

### JSON

`output::JsonOutput` buffers all structured output into a `nlohmann::json` document and serializes it when the root section is closed (or on destruction). Progress calls and info/warn messages are ignored. Error messages are added to the top-level `"errors"` array.

### Quiet

`output::QuietOutput` discards all output. All virtual methods are no-ops.

---

## Error Handling & Privilege Model

### Privilege Check

At startup, before any operation:
1. `OpenProcessToken` + `GetTokenInformation(TokenElevation)` - verify the process is elevated.
2. If not elevated, print a clear error and exit with code 1:
   ```
   ERROR: retool requires Administrator privileges.
          Please re-run from an elevated command prompt.
   ```

### Error Reporting

- All errors include the Win32 error code and message via `FormatMessageW` (wrapped in `util::get_win32_error_message`).
- Default (best-effort): errors collected and reported in finalization summary.
- `-s`: any error immediately exits with code 2.
- Exit codes:
  - `0` - success
  - `1` - usage / privilege error
  - `2` - operational error (file not found, ioctl failed, etc.)

---

## Data Model

Key types as implemented:

| Type | Location | Description |
|------|----------|-------------|
| [`util::CliArg`](file:///c:/Users/chris.stone/workspace/retool/src/util.h) | [`src/util.h`](file:///c:/Users/chris.stone/workspace/retool/src/util.h) | Parsed command-line arguments (command, positional, flags) |
| [`inspect::ScanMode`](file:///c:/Users/chris.stone/workspace/retool/src/inspect.h#L23) | [`src/inspect.h`](file:///c:/Users/chris.stone/workspace/retool/src/inspect.h) | Enum: `kLcnOnly` or `kWithHash` |
| [`inspect::BlockEntry`](file:///c:/Users/chris.stone/workspace/retool/src/inspect.h#L29) | [`src/inspect.h`](file:///c:/Users/chris.stone/workspace/retool/src/inspect.h) | Interned file path index + byte offset for one cluster reference |
| [`inspect::LcnIndex`](file:///c:/Users/chris.stone/workspace/retool/src/inspect.h#L36) | [`src/inspect.h`](file:///c:/Users/chris.stone/workspace/retool/src/inspect.h) | `unordered_map<LONGLONG, vector<BlockEntry>>` - LCN→file map |
| [`inspect::HashIndex`](file:///c:/Users/chris.stone/workspace/retool/src/inspect.h#L40) | [`src/inspect.h`](file:///c:/Users/chris.stone/workspace/retool/src/inspect.h) | `unordered_map<string, vector<LONGLONG>>` - SHA-256→LCN map |
| [`inspect::ScanResult`](file:///c:/Users/chris.stone/workspace/retool/src/inspect.h#L43) | [`src/inspect.h`](file:///c:/Users/chris.stone/workspace/retool/src/inspect.h) | Output of `build_lcn_index`: indexes, stats, errors, file_table |
| [`copy::CopyContext`](file:///c:/Users/chris.stone/workspace/retool/src/copy.cpp#L142) | [`src/copy.cpp`](file:///c:/Users/chris.stone/workspace/retool/src/copy.cpp) | Pipeline state: paths, volumes, strategy, lcn_map, hash_index, stats |
| [`copy::ICopyStrategy`](file:///c:/Users/chris.stone/workspace/retool/src/copy.cpp#L205) | [`src/copy.cpp`](file:///c:/Users/chris.stone/workspace/retool/src/copy.cpp) | Abstract interface: `copy_file(src, dest, args, context)` |
| [`copy::CopyStats`](file:///c:/Users/chris.stone/workspace/retool/src/copy.cpp#L27) | [`src/copy.cpp`](file:///c:/Users/chris.stone/workspace/retool/src/copy.cpp) | Accumulated totals: files, bytes, cloned, fallback, errors |
| [`dedup::DedupContext`](file:///c:/Users/chris.stone/workspace/retool/src/dedup.cpp) | [`src/dedup.cpp`](file:///c:/Users/chris.stone/workspace/retool/src/dedup.cpp) | Pipeline state: volume, scan, lcn_hash, file_clusters, strategy, plans, processed, stats |
| [`dedup::IDedupStrategy`](file:///c:/Users/chris.stone/workspace/retool/src/dedup.cpp) | [`src/dedup.cpp`](file:///c:/Users/chris.stone/workspace/retool/src/dedup.cpp) | Abstract interface: `build_plans(context)` — selects files to rebuild |
| [`dedup::FileRebuildPlan`](file:///c:/Users/chris.stone/workspace/retool/src/dedup.cpp) | [`src/dedup.cpp`](file:///c:/Users/chris.stone/workspace/retool/src/dedup.cpp) | One file's rebuild plan: original/old paths, file size, ordered cluster list |
| [`dedup::DedupStats`](file:///c:/Users/chris.stone/workspace/retool/src/dedup.cpp) | [`src/dedup.cpp`](file:///c:/Users/chris.stone/workspace/retool/src/dedup.cpp) | Accumulated totals: files_rebuilt, clusters_deduped, bytes_reclaimed, errors |
| [`output::IOutput`](file:///c:/Users/chris.stone/workspace/retool/src/output.h#L41) | [`src/output.h`](file:///c:/Users/chris.stone/workspace/retool/src/output.h) | Abstract output interface: message, field, table, progress, graceful_teardown |
