# retool

**ReFS Tool** — A Windows command-line utility for inspecting and working with Resilient File System (ReFS) files at the block level.

> Inspired by [blockstat](https://github.com/tdewin/blockstat) by tdewin.

## Overview

`retool` exposes ReFS block-level internals that Windows does not surface through normal file APIs. It lets you inspect how files are laid out on disk, measure actual deduplication savings, and copy files in a way that preserves block sharing — keeping your ReFS deduplication intact rather than breaking it with a conventional copy.

> [!WARNING]
> **Administrator privileges are required** to run `retool`. All queries and block operations use low-level filesystem ioctls that require full administrative rights. Run the utility from an elevated Command Prompt or PowerShell window.

## Requirements

- **Windows 10 / Windows Server 2016** or later (ReFS v3.x)
- **Administrator privileges** — required for all operations (block-level ioctls mandate elevation)
- **ReFS-formatted volume** — NTFS volumes are not supported

## Features

### 1. `inspect` — Block Layout & Deduplication Analysis

Inspect a file's physical block layout on a ReFS volume using ReFS-specific extent metadata queries.

```
retool inspect <file> [options]
retool inspect <file1> <file2> ... [options]
retool inspect -i <filelist.txt> [options]
```

**Single file:** Dumps all extents (VCN → LCN mappings), showing each fragment's virtual and logical cluster numbers, size, and offset within the file.

**Multiple files:** Computes shared-block statistics across the set, reporting:
- Which file pairs share blocks
- Total shared data in bytes and MB
- Estimated deduplication savings
- Per-file fragment counts

**Options:**
| Flag | Description |
|------|-------------|
| `-i <file>` | Read file paths from a newline-delimited input file |
| `-o <file>` | Write output to a file instead of stdout |
| `--strict` | Abort on first error (default: best-effort with error summary) |

---

### 2. `copy` — Deduplication-Preserving File Copy

Copy a file or directory tree while preserving ReFS block sharing using `FSCTL_DUPLICATE_EXTENTS_TO_FILE`.

```
retool copy <source> <dest> [options]
retool copy <source-dir> <dest-dir> [options]
```

**Same-volume copies** use extent duplication — the copied file shares physical blocks with the source, consuming no additional disk space for shared content. The deduplication relationship established by ReFS integrity streams or block cloning is preserved.

**Cross-volume copies** automatically fall back to a standard byte-for-byte copy with a clear warning in the output.

**Options:**
| Flag | Description |
|------|-------------|
| `-r` | Recursive directory copy |
| `--strict` | Abort on first error (default: best-effort, errors reported at end) |
| `--dry-run` | Simulate the operation without writing any data |

---

### 3. `volume` — Volume-Level Block Statistics

Display ReFS volume-level information including cluster size, total clusters, and an estimate of deduplication space savings across all files.

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
cmake --preset release
cmake --build --preset release
```

Output binary: `build/release/retool.exe`

Debug build:
```powershell
cmake --preset debug
cmake --build --preset debug
```

## Usage Examples

```powershell
# Inspect block layout of a single file
retool inspect C:\Data\backup.vbk

# Compare block sharing between multiple backup files
retool inspect C:\Data\backup.vbk C:\Data\backup-inc.vib

# Read file list from a file, write results to a text file
retool inspect -i files.txt -o results.txt

# Copy a directory preserving dedup (same volume)
retool copy D:\Backups\2024 D:\Backups\2024-clone -r

# Dry-run to preview a copy operation
retool copy D:\Backups\2024 D:\Backups\2025 -r --dry-run

# Show volume statistics
retool volume D:\
```

## Technical Notes

- Block inspection uses `FSCTL_GET_RETRIEVAL_POINTERS` and ReFS-specific extent metadata ioctls.
- Block cloning uses `FSCTL_DUPLICATE_EXTENTS_TO_FILE` (ReFS only; requires both files on the same volume).
- All operations require the process to run as Administrator.
- LCN values are logical cluster numbers relative to the volume; they are directly comparable across files on the same volume to detect block sharing.

## License

MIT License. Copyright (c) 2026. See [LICENSE](LICENSE) for details.
