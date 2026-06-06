# doc/ideas.md — retool Feature Backlog

* [Reverse LCN Map](#reverse-lcn-map-volume-wide-block-sharing-lookup) ✅ Implemented
* [FSCTL_QUERY_EXTENT_METADATA Integration](#fsctl_query_extent_metadata-integration) ❌ Abandoned
* [File List Input from stdin (Pipe Mode)](#file-list-input-from-stdin-pipe-mode)
* [Volume-Wide Deduplication Scan](#volume-wide-deduplication-scan) ✅ Implemented
* [Fragmentation Report](#fragmentation-report) ✅ Implemented
* [In-Place File Deduplication](#in-place-file-deduplication) ✅ Implemented
* [Cross-Volume Copy target-side Dedup](#cross-volume-copy-target-side-dedup) ✅ Implemented

---

### Reverse LCN Map: Volume-Wide Block-Sharing Lookup

> **Status: ✅ Implemented** — available as `retool inspect <volume-root>` (e.g. `retool inspect E:\`).

Enable `retool inspect` to report which *other* files on the volume share blocks with a
target file, without the caller supplying all candidate files up front.

* **Objective**: Provide a "who else uses this block?" query. This is the feature that
  most clearly differentiates retool from blockstat's compare mode.
* **Implementation**: Full scan approach was chosen.
  `inspect::build_lcn_index()` enumerates all files on the volume using `FindFirstFileW`
  recursively from the volume root. Builds an `LcnIndex` (`unordered_map<LONGLONG, vector<BlockEntry>>`)
  mapping each LCN to every file that physically references it. `execute_inspect` detects
  a volume root argument and calls `build_lcn_index(kLcnOnly)` then `output_scan_report`.
  The `FSCTL_QUERY_EXTENT_METADATA` approach was abandoned (see below).
* **Notes**: Skips `FILE_ATTRIBUTE_SYSTEM` and `FILE_ATTRIBUTE_REPARSE_POINT` entries.
  Progress reported every 500 files. Non-fatal per-file errors collected and reported at end.

---

### FSCTL_QUERY_EXTENT_METADATA Integration

> **Status: ❌ Abandoned** — `FSCTL_QUERY_EXTENT_METADATA` does not appear to be a valid
> or recognized ioctl on tested Windows/ReFS configurations. The full-scan approach
> (`build_lcn_index`) supersedes this entirely.

Integrate the undocumented ReFS-specific `FSCTL_QUERY_EXTENT_METADATA` ioctl to expose
richer per-extent metadata (e.g., block reference count, integrity stream status).

* **Original Objective**: Surface ReFS-native deduplication metadata directly rather than
  inferring it from LCN overlap across a file set.
* **Why Abandoned**: The ioctl was not confirmed to exist or respond correctly on any
  tested Windows version. The volume-scan approach provides equivalent or better
  information without depending on an undocumented interface.

---

### File List Input from stdin (Pipe Mode)

Accept file paths piped via stdin in addition to `-i <file>` and positional CLI arguments.

* **Objective**: Allow `Get-ChildItem | retool inspect` workflows directly in PowerShell,
  matching the ergonomics blockstat advertised (though blockstat itself discouraged it).
* **Implementation**: When no file arguments and no `-i` flag are present, detect if stdin
  is redirected (`GetFileType(GetStdHandle(STD_INPUT_HANDLE)) == FILE_TYPE_PIPE`). If so,
  read lines from stdin as file paths. Enforce the same validation as `-i`. Emit a clear
  warning that interactive stdin mode is not supported.
* **Effort Estimate**: Low (~0.5 day). Path-reading and validation logic already exists for
  `-i` mode; this adds a detection and redirect step.

---

### Volume-Wide Deduplication Scan

> **Status: ✅ Implemented** — available as `retool inspect <volume-root>` (basic LCN scan)
> and `retool dedup <volume-root> --dry-run` (content-hash scan with savings report).

Scan an entire volume and report all groups of files that share blocks, with aggregate
deduplication savings — a "full dedup audit" report.

* **Objective**: Give users a volume-wide picture of deduplication efficiency without
  needing to supply a file list. Useful for auditing backup repositories.
* **Implementation**: `inspect::build_lcn_index(kLcnOnly)` provides the LCN-level scan;
  `build_lcn_index(kWithHash)` adds SHA-256 content hashing for full content-based grouping.
  The `dedup` command uses `kWithHash` mode and reports all matching groups via
  `VolumeWideDedupStrategy`. `inspect` volume-root mode uses `kLcnOnly` for speed.
  Output is plain text or JSON. The command is exposed via `retool inspect <volume>` and
  `retool dedup <volume>` rather than a separate `retool scan` command.

---

### Fragmentation Report

> **Status: ✅ Implemented** — available via `retool inspect <file> -r` or
> `retool inspect <file1> <file2> -r` (appends a report per file).

Report the fragmentation level of a file or set of files — extent count, average extent
size, and a fragmentation score.

* **Objective**: Complement dedup analysis with a practical file health report. A file
  with thousands of small extents on ReFS performs worse at read time even if dedup is
  intact.
* **Implementation**: Leverages the existing `FSCTL_GET_RETRIEVAL_POINTERS` enumeration
  already used by `inspect`. The `-r` flag triggers `compute_frag_stat()` on each
  inspected file's `VcnExtent` list, computing `fragment_count`, `min/max/avg_extent_clusters`.
  `output_frag_report()` then emits a `Fragmentation Report` section per file. Sparse
  extents are excluded from all counts. Score thresholds: 1 = optimal, ≤4 = good,
  ≤16 = moderate, >16 = high. `util::format_size()` was added to produce human-readable
  byte strings (KB/MB/GB/TB) used throughout the report.
* **Notes**: `-r` is deliberately chosen over `-f` (`-f` is reserved for `--force`
  semantics). In `copy` context, `-r` still means recursive; in `inspect` context it means
  report. The shared `args.recursive` field in `CliArg` carries the flag for both.

---

### In-Place File Deduplication

> **Status: ✅ Implemented** — available as `retool dedup <volume>` (volume-wide) and
> `retool dedup <file1> <file2>` (pair-wise). See [features.md § Feature 3](features.md#feature-3-dedup--in-place-file-deduplication).

Scan two existing files on the same ReFS volume to find identical content blocks,
and deduplicate them in-place so they share the same physical storage.

* **Objective**: Reclaim space by deduplicating existing duplicate files that were not
  originally copied using block cloning (e.g., created by separate download or backup
  processes).
* **Implementation**:
  1. `build_lcn_index(kWithHash)` scans all target files and builds a SHA-256 HashIndex.
  2. `IDedupStrategy::build_candidates()` identifies cluster pairs to deduplicate.
  3. `execute_operation()` opens each file pair and issues `FSCTL_DUPLICATE_EXTENTS_TO_FILE`
     per candidate cluster, with handle caching to avoid per-cluster `CreateFileW` overhead.
  4. `--dry-run` reports savings without issuing ioctls.
  5. Ctrl+C cancellation observed via shared `copy::g_cancel_requested` atomic.
* **Notes**: Both modes (`VolumeWideDedupStrategy`, `PairwiseDedupStrategy`) implemented.
  Supports `--strict`, `--json`, `-q` flags.

---

### Cross-Volume Copy target-side Dedup

> **Status: ✅ Implemented** — available via `retool copy <src> <dest> -r --scan-dest`.
> See [features.md § Destination Pre-Scan](features.md#destination-pre-scan---scan-dest).

Extend cross-volume copy deduplication to reference existing matching blocks in other files already residing on the destination ReFS volume.

* **Objective**: Avoid writing duplicate blocks when copying files onto a ReFS volume if those blocks are already physically stored in existing files on that volume.
* **Implementation**:
  1. `--scan-dest` flag triggers Phase 1b in `execute_copy`: calls
     `inspect::build_lcn_index(dest_volume_root, kWithHash, out)` before any file is copied.
  2. Seeds `CopyContext::lcn_map` from the destination `LcnIndex` and stores the
     `HashIndex` in `CopyContext::hash_index`.
  3. `CrossVolumeRefsCopyStrategy` already uses `lcn_map` for clone decisions — pre-seeding
     it with destination-side blocks means those blocks are cloned rather than physically
     transferred from source.
  4. Content verification is by SHA-256 hash (stored in `hash_index`).
* **Notes**: `--scan-dest` adds a full volume hash scan to the setup phase. Use when the
  destination already holds substantial overlapping data and setup time is acceptable.
