# doc/features.md — retool Feature Specifications

This document provides technical detail for each feature in the retool 1.0 scope. It is intended to guide agent implementation and design decisions. For user-facing documentation see [README.md](../README.md).

---

## Index

* [Feature 1: inspect — Block Layout & Deduplication Analysis](#feature-1-inspect--block-layout--deduplication-analysis)
* [Feature 2: copy — Deduplication-Preserving File Copy](#feature-2-copy--deduplication-preserving-file-copy)
* [Feature 3: volume — Volume-Level Block Statistics](#feature-3-volume--volume-level-block-statistics)
* [CLI Design](#cli-design)
* [Output Format](#output-format)
* [Error Handling & Privilege Model](#error-handling--privilege-model)
* [Data Model](#data-model)

---

## Feature 1: `inspect` — Block Layout & Deduplication Analysis

### Purpose

Report the physical block layout of one or more files on a ReFS volume. When multiple files are provided, compute cross-file block sharing to measure deduplication savings.

### Single-File Mode

Invoked when exactly one file path is supplied.

**Goal:** Enumerate every extent (fragment) of the file and print each VCN→LCN mapping with size and cumulative offset.

**Win32 API Sequence:**

1. `CreateFileW` — open the file with `FILE_FLAG_BACKUP_SEMANTICS | GENERIC_READ | FILE_SHARE_READ | FILE_SHARE_WRITE`. Requires Administrator.
2. `GetVolumePathNameW` — extract the volume root from the file path.
3. `GetDiskFreeSpaceW` + `GetDiskFreeSpaceExW` — obtain cluster size and total cluster count. Note: `GetDiskFreeSpace` returns 32-bit cluster count which overflows above ~16 TB at 4 KB clusters; use `GetDiskFreeSpaceEx` for total bytes and divide.
4. Loop: `DeviceIoControl(FSCTL_GET_RETRIEVAL_POINTERS)` — iteratively query extents. Pass `STARTING_VCN_INPUT_BUFFER` starting at VCN 0; on each call the last `NextVcn` becomes the next starting VCN. Stop when the call returns `true` (all extents fit) or `ERROR_HANDLE_EOF` (sparse/small file).

**Key Note from blockstat:** Querying multiple extents per call (`ExtentCount > 1`) produces unreliable results on some ReFS configurations. Query one extent per call and loop.

**Output per extent:**

```
Extent #  VCN          LCN          Clusters    Bytes        Cumulative
0         0x00000000   0x001A3F00   128          524288       524288
1         0x00000080   0x001B0040   256          1048576      1572864
...
```

**ReFS Extent Metadata (Optional / Advanced):**

`FSCTL_QUERY_EXTENT_METADATA` is an undocumented ReFS-specific ioctl that can return additional metadata per extent (e.g., integrity stream info, reference count). If available and structurally stable, use it to report block reference counts directly. This must be wrapped in a best-effort path that gracefully degrades to VCN/LCN-only output if it fails.

### Multi-File Mode

Invoked when two or more file paths are supplied (directly on CLI or via `-i <filelist>`).

**Goal:** For each unique LCN present in more than one file, count how many files share it. Compute total bytes saved by deduplication.

**Algorithm (matching blockstat's approach):**

1. Determine total cluster count of the volume; allocate a `uint16_t refmap[total_clusters]` — one counter per cluster on the volume.
2. For each file: enumerate all extents via `FSCTL_GET_RETRIEVAL_POINTERS`. For each LCN in each extent, increment `refmap[lcn]`.
3. After all files are processed: scan `refmap`. Any cluster with count ≥ 2 is shared. Shared clusters × cluster size = bytes saved.
4. Produce per-file share ratios and a total-savings summary.

**Memory considerations:** At 4 KB cluster size, a 10 TB volume has ~2.5 billion clusters. `uint16_t` refmap = ~5 GB — not feasible for very large volumes. Strategy:
- Default: `uint16_t` (supports up to 65,535 references per cluster; covers all realistic dedup cases).
- If refmap allocation fails, fall back to a hash-map approach (`std::unordered_map<LONGLONG, uint16_t>`) keyed on LCN, populated only for clusters that appear in at least one input file. This is sparser but correct.
- Document memory implications clearly in output and help text.

**Output (multi-file):**

```
Files analyzed: 5
Total extents:  1,204
Shared clusters: 8,192 (32 MB)
Dedup savings:   32 MB (12.5% of total)

Per-file sharing:
  file1.vbk  -> 4 files share  16 MB
  file2.vib  -> 3 files share  8 MB
  ...
```

### Input File (`-i <file>`)

- One absolute path per line (UTF-8 or UTF-16 LE with BOM).
- Blank lines and lines starting with `#` are ignored.
- Paths are validated before processing begins; invalid paths are reported and skipped (default best-effort) or abort (with `--strict`).

---

## Feature 2: `copy` — Deduplication-Preserving File Copy

### Purpose

Copy one or more files (or a full directory tree) while preserving ReFS block sharing using `FSCTL_DUPLICATE_EXTENTS_TO_FILE` or target-volume deduplication.

### Same-Volume Clone (Primary Path)

When source and destination paths reside on the same volume:
1. `CreateFileW` on source — `GENERIC_READ | FILE_SHARE_READ`, `FILE_FLAG_BACKUP_SEMANTICS`.
2. `CreateFileW` on destination — `GENERIC_READ | GENERIC_WRITE`, `CREATE_ALWAYS`, `FILE_FLAG_BACKUP_SEMANTICS`.
3. Query source extents via `FSCTL_GET_RETRIEVAL_POINTERS` to enumerate all extents.
4. For each extent: call `DeviceIoControl(FSCTL_DUPLICATE_EXTENTS_TO_FILE)` passing the source file handle, source offset, destination offset, and length in bytes.
5. Set the destination file size via `SetEndOfFile` to match the source.
6. Copy file timestamps and basic attributes using `GetFileInformationByHandleEx` / `SetFileInformationByHandle`.

### Cross-Volume Copy

When source and destination paths reside on different volumes:

1. **Verify Destination Filesystem**:
   Use `GetVolumePathNameW` and `GetVolumeInformationW` on the destination path to query the filesystem type.

2. **Non-ReFS Destination (Fallback)**:
   If the destination filesystem is not ReFS (e.g., NTFS or FAT32), block cloning cannot be used on the destination. Fall back to standard copy via `CopyFileExW` with `COPY_FILE_ALLOW_DECRYPTED_DESTINATION` and print a warning:
   ```
   WARNING: Destination volume is non-ReFS. Falling back to standard copy.
   ```

3. **ReFS Destination (Preserve Sharing)**:
   If the destination filesystem is ReFS, we can preserve deduplication of the copied files by mapping identical blocks on the source and cloning them *on the target volume* after they are initially copied.
   
   **Win32 API Sequence**:
   - Call `inspect` logic on all files to be copied to identify matching source LCNs.
   - Maintain a tracker of source LCNs that have already been copied to the destination: `lcn_map[source_lcn] = {dest_file_path, dest_file_offset}`.
   - For each file being copied:
     - Open source and destination files.
     - For each extent of the source file:
       - If it is sparse (`lcn == (LONGLONG)-1`), skip writing/cloning (let target file growth handle it).
       - For each cluster in the extent:
         - If the cluster's source `lcn` is NOT in `lcn_map` (first time seeing this block):
           - Copy the cluster data (typically 4 KB or 64 KB) directly from source to destination file.
           - Record the copied destination address: `lcn_map[lcn] = {dest_file_path, dest_file_offset}`.
         - If the cluster's source `lcn` IS in `lcn_map` (duplicate block found):
           - Retrieve the already written target block location `{prev_dest_file, prev_dest_offset}`.
           - Open the `prev_dest_file` with read access.
           - Call `DeviceIoControl(FSCTL_DUPLICATE_EXTENTS_TO_FILE)` on the current destination file, passing the handle of `prev_dest_file`, source offset `prev_dest_offset`, target offset `current_dest_offset`, and block length.
           - This makes the destination files share blocks on the destination volume!

### Directory Copy

- Walk source directory recursively with `FindFirstFileW` / `FindNextFileW`.
- Mirror directory structure at destination using `CreateDirectoryW`.
- For each file: attempt same-volume clone; fall back to standard copy per the above logic.
- Default: best-effort — on error, record the failure and continue. Report all errors in a summary at the end.
- `--strict`: abort on first error.

### Dry-Run Mode (`--dry-run`)

- Walk source and compute what would be copied.
- Print each file that would be created, the copy method that would be used (clone vs. fallback), and the expected bytes.
- No files or directories are created.

### Options

| Flag | Description |
|------|-------------|
| `-r` | Recursive directory copy |
| `--strict` | Abort on first per-file error |
| `--dry-run` | Simulate without writing |

---

## Feature 3: `volume` — Volume-Level Block Statistics

### Purpose

Show ReFS volume metadata and a high-level summary of cluster usage.

### Output

```
Volume:        D:\
File System:   ReFS
Cluster Size:  4096 bytes
Total Clusters: 2,621,440
Total Space:   10.0 GB
Free Space:    4.2 GB
Used Space:    5.8 GB
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
| `inspect` | `i` | Inspect block layout and dedup stats |
| `copy` | `cp` | Copy files preserving deduplication |
| `volume` | `vol` | Show volume information |
| `help` | `h`, `?` | Show usage |
| `version` | | Print version string |

### Global Options

| Flag | Description |
|------|-------------|
| `--strict` | Abort on first error (where applicable) |
| `-o <file>` | Redirect output to a file |

### Argument Parsing

Implement a minimal argument parser in `src/util/Arg.cpp`. Use `CommandLineToArgvW` to obtain the wide-character argument array. No third-party CLI library. Rules:
- Short flags: single `-` + single character (e.g., `-r`, `-i`).
- Long flags: double `--` + word (e.g., `--strict`, `--dry-run`).
- Unknown flags: print a clear error and exit with code 1.

---

## Output Format

### Plain Text

Human-readable, column-aligned. Use `wprintf` throughout for Unicode safety. Widths should adapt to the largest values in the result set.

---

## Error Handling & Privilege Model

### Privilege Check

At startup, before any operation:
1. Call `OpenProcessToken` + `GetTokenInformation(TokenElevation)` to verify the process is elevated.
2. If not elevated, print a clear error and exit with code 1:
   ```
   ERROR: retool requires Administrator privileges.
          Please re-run from an elevated command prompt.
   ```

### Error Reporting

- All errors include the Win32 error code and message (via `FormatMessageW`).
- Default (best-effort): errors are collected in an `errors` list and printed as a block after the main output.
- `--strict`: any error immediately prints and exits with code 2.
- Exit codes:
  - `0` — success
  - `1` — usage / privilege error
  - `2` — operational error (file not found, ioctl failed, etc.)

---

## Data Model

Key structs in `src/model/`:

| Struct | File | Description |
|--------|------|-------------|
| `VolumeInfo` | `VolumeInfo.h` | Cluster size, total clusters, volume path |
| `VcnExtent` | `VcnExtent.h` | Single VCN→LCN extent (vcn, lcn, cluster count) |
| `InspectResult` | `InspectResult.h` | All extents for one file + volume info + errors |
| `CompareResult` | `CompareResult.h` | Multi-file compare output: share map, savings, errors |
| `CopyResult` | `CopyResult.h` | Per-file copy outcome (cloned, fallback, skipped, error) |
| `CliArg` | `CliArg.h` | Parsed command-line arguments |
