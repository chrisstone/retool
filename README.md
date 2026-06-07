# retool

**ReFS Tool** — A Windows command-line utility for inspecting and working with Resilient File System (ReFS) files at the block level.

> Inspired by [blockstat](https://github.com/tdewin/blockstat) by tdewin.

## Overview

`retool` exposes ReFS block-level internals that Windows does not surface through normal file APIs. It lets you inspect how files are laid out on disk, measure actual deduplication savings, copy files in a way that preserves block sharing, and deduplicate files in-place — keeping your ReFS deduplication intact rather than breaking it with a conventional copy.

> [!WARNING]
> **Administrator privileges are required** to run `retool`. All queries and block operations use low-level filesystem ioctls that require full administrative rights. Run the utility from an elevated Command Prompt or PowerShell window.

## Requirements

- **Windows 10 / Windows Server 2016** or later (ReFS v3.x)
- **Administrator privileges** — required for all operations (block-level ioctls mandate elevation)
- **ReFS-formatted volume** — required for block cloning and deduplication; `inspect` works on any volume

## Features

### 1. `inspect` — Block Layout, Sharing Analysis & Volume Scan

Inspect a file's physical block layout, compare sharing across multiple files, or scan an entire volume to measure deduplication potential.

```
retool inspect <file> [options]
retool inspect <file1> <file2> ... [options]
retool inspect <directory> [options]
retool inspect <glob> [options]        (e.g. E:\Data\*.vbk)
retool inspect -i <filelist.txt> [options]
retool inspect <volume-root>
```

**Single file:** Shows a summary (File, Volume, Cluster Size, File Size, Fragments). Add `-e` to include the full VCN → LCN extent table. Add `-r` to append a fragmentation report.

**Multiple files / directory / glob:** Computes shared-block statistics across the set, reporting:
- Which file pairs share blocks (cross-file sharing matrix)
- Per-file breakdown of unique vs. shared clusters and bytes
- Total shared data in bytes and MB
- Estimated deduplication savings

A **directory** argument recursively enumerates all non-system files under it.
A **glob pattern** (e.g. `E:\Data\*.vbk`) expands to matching files non-recursively.
Multiple arguments (mixed files, directories, globs) are each expanded independently and merged.

Add `-r` to include a per-file fragmentation report after the sharing matrix.

**Volume scan:** When given a volume root (e.g. `E:\`), walks all files on the volume and reports:
- Total files scanned and clusters indexed
- Shared block count and estimated space savings

**Options:**
| Flag | Description |
|------|-------------|
| `-e` | Show VCN/LCN extent table (single-file mode only; hidden by default) |
| `-r` | Append a fragmentation report (fragment count, min/max/avg extent size, score) |
| `-i <file>` | Read file paths from a newline-delimited input file |
| `-o <file>` | Write output to a file instead of stdout |
| `-s` | Abort on first error (default: best-effort with error summary) |
| `-j` | Output results in JSON format |
| `-q` | Suppress all output (quiet mode) |

---

### 2. `copy` — Deduplication-Preserving File Copy

Copy a file or directory tree while preserving ReFS block sharing using `FSCTL_DUPLICATE_EXTENTS_TO_FILE`.

```
retool copy <source> <dest> [options]
retool copy <source-dir> <dest-dir> -r [options]
```

**Same-volume copies** use extent duplication — the copied file shares physical blocks with the source, consuming no additional disk space for shared content. The deduplication relationship is preserved exactly.

**Cross-volume copies** (ReFS → ReFS, matching cluster size) preserve deduplication by tracking which source logical cluster numbers (LCNs) have already been copied to the destination and issuing `FSCTL_DUPLICATE_EXTENTS_TO_FILE` for duplicate blocks, rather than copying bytes twice.

**`-d`** pre-scans the destination volume before copying begins. Blocks already present on the destination (matched by SHA-256 content hash) are cloned instead of physically transferred, maximizing space savings when copying into a volume that already holds related data.

For incompatible volumes (non-ReFS destination, cluster size mismatch), retool falls back to a standard copy with a clear warning.

**Options:**
| Flag | Description |
|------|-------------|
| `-r` | Recursive directory copy |
| `-n` | Simulate the operation without writing any data |
| `-d` | Pre-scan destination volume to seed the dedup block index |
| `-s` | Abort on first error (default: best-effort, errors reported at end) |
| `-j` | Output results in JSON format |
| `-q` | Suppress all output |
| `-o <file>` | Redirect output to a file |

> [!NOTE]
> `-d` performs a full volume hash scan before copying begins. On large volumes this adds significant setup time but can substantially reduce the data physically written.

---

### 3. `dedup` — In-Place File Deduplication

Deduplicate files already resident on a ReFS volume using `FSCTL_DUPLICATE_EXTENTS_TO_FILE`. Identifies clusters with identical SHA-256 content and replaces physical duplicates with shared block references — reclaiming disk space without touching file data.

```
retool dedup <volume-root>
retool dedup <file1> <file2>
```

**Volume-wide mode:** Scans all files on the volume, groups clusters by SHA-256 hash, and deduplicates every cluster that appears more than once.

**Pair-wise mode:** Compares two explicitly named files and deduplicates only the clusters they share.

Both modes support `-n` to report what would be reclaimed without making any changes.

**Options:**
| Flag | Description |
|------|-------------|
| `-n` | Report dedup savings without writing any data |
| `-s` | Abort on first error |
| `-j` | Output results in JSON format |
| `-q` | Suppress all output |

> [!IMPORTANT]
> Deduplication requires both files to reside on the same ReFS volume. The operation modifies file allocation metadata; ensure you have a current backup before running volume-wide dedup on production data.

---

### 4. `volume` — Volume-Level Block Statistics

Display ReFS volume-level information including cluster size, total clusters, free space, and used space.

```
retool volume <drive-letter or path>
```

---

## Building

### Prerequisites

- Visual Studio 2022 (with C++ Desktop workload)
- CMake 3.25 or later

### Build Steps

```powershell
git clone https://github.com/your-org/retool.git
cd retool

# Configure
cmake --preset debug    # or: cmake --preset release

# Build (use the build directory directly — build presets require VS CMake integration)
cmake --build build/debug --config Debug
cmake --build build/release --config Release
```

Output binary: `build/debug/Debug/retool.exe` (debug) or `build/release/Release/retool.exe` (release).

---

## Usage Examples

```powershell
# Inspect block layout of a single file
retool inspect E:\Data\backup.vbk

# Inspect a file and include its fragmentation report
retool inspect E:\Data\backup.vbk -r

# Compare block sharing between multiple backup files
retool inspect E:\Data\backup.vbk E:\Data\backup-inc.vib

# Compare files and include per-file fragmentation reports
retool inspect E:\Data\backup.vbk E:\Data\backup-inc.vib -r

# Scan an entire volume for dedup potential
retool inspect E:\

# Copy a directory preserving dedup (same volume)
retool copy E:\Backups\2024 E:\Backups\2024-clone -r

# Copy cross-volume, pre-scanning destination for existing blocks
retool copy E:\Backups F:\Backups -r --scan-dest

# Dry-run to preview a copy operation
retool copy E:\Backups\2024 E:\Backups\2025 -r --dry-run

# Deduplicate an entire ReFS volume in-place (dry-run first)
retool dedup E:\ --dry-run
retool dedup E:\

# Deduplicate two specific files
retool dedup E:\vms\base.vmdk E:\vms\clone.vmdk

# Show volume statistics
retool volume E:\

# Output any command as JSON
retool inspect E:\Data\backup.vbk --json
retool dedup E:\ --dry-run --json -o dedup-report.json
```

---

## Technical Notes

- Block inspection uses `FSCTL_GET_RETRIEVAL_POINTERS` to query VCN → LCN extent maps for each file.
- Block cloning uses `FSCTL_DUPLICATE_EXTENTS_TO_FILE` (ReFS only; both files must reside on the same volume).
- SHA-256 hashing for content-based deduplication uses the Windows **CNG BCrypt API**, which automatically leverages SHA-NI processor instructions for hardware acceleration when available.
- LCN values (logical cluster numbers) are volume-relative and directly comparable across files on the same volume — two files referencing the same LCN share that physical block.
- All block-level operations require Administrator privileges.

## License

MIT License. Copyright (c) 2026. See [LICENSE](LICENSE) for details.
