# doc/ideas.md — retool Feature Backlog

* [Structured Output (JSON/CSV)](#structured-output-jsoncss)
* [Reverse LCN Map](#reverse-lcn-map-cross-volume-block-sharing-lookup)
* [FSCTL_QUERY_EXTENT_METADATA Integration](#fsctl_query_extent_metadata-integration)
* [File List Input from stdin (Pipe Mode)](#file-list-input-from-stdin-pipe-mode)
* [Volume-Wide Deduplication Scan](#volume-wide-deduplication-scan)
* [Fragmentation Report](#fragmentation-report)
* [In-Place File Deduplication](#in-place-file-deduplication)

---

### Structured Output (JSON/CSV)

Post-1.0, retool should offer machine-readable output formats for integration with
PowerShell scripts, monitoring tools, and report pipelines.

* **Objective**: Allow programmatic consumption of `inspect` and `volume` output without
  screen-scraping plain text. Enables workflows like: enumerate all backup files → inspect
  → parse JSON → compute savings report in PowerShell.
* **Implementation**: Add `--json` and `--csv` flags. A minimal JSON serializer lives in
  `src/util/Json.cpp`; CSV is straightforward tabular output. The `--json` flag is
  already partially described in `doc/features.md` but deferred from 1.0 scope.
* **Effort Estimate**: Low (~1 day). JSON schema is already drafted in features.md; it is
  primarily a serialization layer on top of existing model structs.

---

### Reverse LCN Map: Cross-Volume Block-Sharing Lookup

Enable `retool inspect` to report which *other* files on the volume share blocks with a
target file, without the caller supplying all candidate files up front.

* **Objective**: Provide a "who else uses this block?" query. This is the feature that
  most clearly differentiates retool from blockstat's compare mode.
* **Implementation**: Two approaches are viable:
  1. **Full scan**: Enumerate all files on the volume using `FindFirstFileW` recursively or
     `NtQueryDirectoryFile` from a volume root handle. Build an LCN→file-path map, then
     intersect with the target file's extents. Memory-intensive for large volumes.
  2. **FSCTL_QUERY_EXTENT_METADATA**: If ReFS exposes a reference count per extent via
     this undocumented ioctl, use it to report sharing without a full scan. Requires
     research and testing on multiple Windows/ReFS versions.
  The full-scan approach should be implemented first as it is reliable. The ioctl approach
  should be introduced behind a feature flag once its structure is confirmed.
* **Effort Estimate**: High (~1 week). The full-scan approach requires robust directory
  enumeration, error handling at scale, and memory management for large volumes.

---

### FSCTL_QUERY_EXTENT_METADATA Integration

Integrate the undocumented ReFS-specific `FSCTL_QUERY_EXTENT_METADATA` ioctl to expose
richer per-extent metadata (e.g., block reference count, integrity stream status).

* **Objective**: Surface ReFS-native deduplication metadata directly rather than
  inferring it from LCN overlap across a file set.
* **Implementation**: Research the ioctl's input/output buffer layout by inspecting ReFS
  driver behavior (e.g., via kernel debugger or driver reverse engineering). Implement a
  wrapper in `src/service/RefsExtent.cpp` that issues the ioctl and maps the response to
  a `RefExtentMetadata` model struct. Gate the feature behind a runtime version check
  (ReFS version via `FSCTL_GET_INTEGRITY_INFORMATION`). The `inspect` command should
  display the extra fields when available, with a graceful no-op on older systems.
* **Effort Estimate**: High (~1–2 weeks). The ioctl is undocumented and may change between
  Windows versions; research and stability testing dominate the effort.

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

Scan an entire volume and report all groups of files that share blocks, with aggregate
deduplication savings — a "full dedup audit" report.

* **Objective**: Give users a volume-wide picture of deduplication efficiency without
  needing to supply a file list. Useful for auditing backup repositories.
* **Implementation**: Combines the full-scan LCN map approach (from Reverse LCN Map idea)
  with the multi-file compare algorithm from `inspect`. Produce a report grouped by shared-
  block cluster. Output can be plain text or JSON. This command would be exposed as
  `retool scan <volume>`.
* **Effort Estimate**: High (~1.5 weeks). Depends on the Reverse LCN Map idea being
  implemented first. Performance and memory management for volumes > 1 TB are the main
  engineering challenges.

---

### Fragmentation Report

Report the fragmentation level of a file or set of files — extent count, average extent
size, and a fragmentation score.

* **Objective**: Complement dedup analysis with a practical file health report. A file
  with thousands of small extents on ReFS performs worse at read time even if dedup is
  intact.
* **Implementation**: Leverage the existing `FSCTL_GET_RETRIEVAL_POINTERS` enumeration
  already used by `inspect`. Compute: `fragment_count`, `min/max/avg_extent_size`,
  `fragmentation_score = fragment_count / ideal_fragment_count` (where ideal = 1 for a
  fully contiguous file). Surface as an additional section in `inspect` output, or as a
  dedicated `retool frag <file>` subcommand.
* **Effort Estimate**: Low (~half day). The extent enumeration code is already implemented
  as part of `inspect`; this is purely an additional calculation and output section.

---

### In-Place File Deduplication

Scan two existing files on the same ReFS volume to find identical content blocks,
and deduplicate them in-place so they share the same physical storage.

* **Objective**: Reclaim space by deduplicating existing duplicate files that were not
  originally copied using block cloning (e.g., created by separate download or backup
  processes).
* **Implementation**:
  1. Open both target files with read/write access.
  2. Map both files to their physical extents via `FSCTL_GET_RETRIEVAL_POINTERS`.
  3. Identify candidate duplicate regions. Read and compare matching data chunks in-place
     (e.g., using a hash-based sliding window or aligned block-by-block byte comparison).
  4. For identical content blocks, issue `FSCTL_DUPLICATE_EXTENTS_TO_FILE` to replace the
     physical allocation of one block with a reference to the other, releasing the redundant
     storage.
  5. Ensure all offset operations are cluster-aligned (e.g., multiples of 4 KB or 64 KB).
* **Effort Estimate**: Medium (~4–5 days). Requires safe, transactional chunk verification
  to prevent data corruption if files are modified, and robust handling of cluster alignment
  boundary constraints.
