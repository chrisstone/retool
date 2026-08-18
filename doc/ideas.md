# doc/ideas.md - retool Feature Backlog

### Progress Function with Units and ETA

Extend the `IOutput::progress()` interface to accept a unit label and compute an
estimated time of arrival (ETA). The current signature reports a dimensionless
`(filename, done, total)` count, which is meaningful for file copies but
awkward when the unit of work is clusters, files scanned, or bytes hashed.
A labelled variant (e.g. "clusters", "files", "MB") makes progress readable for
inspection and dedup scan phases. An ETA derived from elapsed time and completion
percentage surfaces actionable feedback during long volume-wide operations.

* **Objective**: Make `progress()` useful beyond file-copy contexts — particularly
  for `inspect::build_lcn_index` scan phases and `dedup::rebuild_file` cluster
  cloning — and surface an ETA so users know how long to expect a volume-wide
  operation to take.
* **Implementation**: Update `IOutput::progress(filename, done, total, unit)` with
  a `unit` parameter (defaulting to `L"files"` for backwards compatibility). Add an
  optional `eta_seconds` parameter, or compute ETA internally in `CliOutput` by
  recording a start timestamp (`std::chrono::steady_clock`) on the first progress
  call per section and deriving remaining time from `(elapsed / done) * (total - done)`.
  `JsonOutput` can emit `eta_seconds` as a field. Update all `progress()` call sites
  in `inspect.cpp`, `copy.cpp`, and `dedup.cpp` to pass appropriate units.
* **Effort Estimate**: Low–Medium (~2–3 hours). Interface change is minor; the main
  work is plumbing the unit string and wiring up the ETA calculation in `CliOutput`.
