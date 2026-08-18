/**
 * @file inspect.h
 * @brief Header for block layout inspection, volume scan engine, and cross-file sharing analysis.
 */

#pragma once

#include <algorithm>
#include <expected>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "output.h"
#include "util.h"

namespace inspect {

// ============================================================================
// Extent Types
// ============================================================================

/// @brief One contiguous VCN/LCN run for a file, as reported by FSCTL_GET_RETRIEVAL_POINTERS.
struct Extent {
    LONGLONG start_vcn = 0;  ///< First VCN covered by this extent.
    LONGLONG next_vcn  = 0;  ///< VCN immediately past this extent (start of the next one).
    LONGLONG lcn       = 0;  ///< Starting LCN, or -1 if this extent is sparse.

    ULONGLONG cluster_count() const { return static_cast<ULONGLONG>(next_vcn - start_vcn); }
    bool is_sparse() const { return lcn == (LONGLONG)-1; }
};

/**
 * @brief Queries all retrieval-pointer extents for an already-open file handle.
 *
 * The single shared implementation of the FSCTL_GET_RETRIEVAL_POINTERS walk;
 * every module that needs a file's physical layout (inspect, copy) should call
 * this rather than reimplementing the walk.
 *
 * The handle is not opened or closed here - the caller owns its lifetime. This
 * lets callers that also need to read or hash file content do so through the
 * same handle afterward, without a second CreateFileW.
 *
 * @param file_handle  Open file handle.
 * @return Extents in ascending VCN order on success, or error string on failure.
 */
std::expected<std::vector<Extent>, std::wstring> collect_extents(HANDLE file_handle);

// ============================================================================
// Scan Engine Types
// ============================================================================

/// @brief Controls what the volume scan engine computes per cluster.
enum class ScanMode {
    kLcnOnly,   ///< Build LCN->file map without reading disk content (fast).
    kWithHash,  ///< Also SHA-256 hash each cluster to enable content-based matching.
};

/// @brief A record associating a physical cluster (LCN) with a file and byte offset.
struct BlockEntry {
    uint32_t  file_index;    ///< Index into ScanResult::file_table.
    ULONGLONG file_offset;   ///< Byte offset of this cluster within the file.
};

// ============================================================================
// LCN Interval Index
// ============================================================================
//
// Physical cluster ownership is represented as disjoint, sorted LCN intervals
// rather than one map entry per cluster. An interval's owner count is orders
// of magnitude smaller than its cluster count for large, non-pathologically-
// fragmented files, which is what makes this representation viable at scale.

/// @brief One maximal contiguous run of LCNs and the file(s) that own it.
/// owners.size() >= 2 means these clusters are physically shared across files.
/// owners are sorted ascending by file_index (lowest = first file to claim the run).
struct LcnInterval {
    LONGLONG  start_lcn = 0;  ///< First LCN in this run (inclusive).
    LONGLONG  end_lcn   = 0;  ///< One past the last LCN in this run (exclusive).
    std::vector<BlockEntry> owners;  ///< owners[i].file_offset is that file's offset AT start_lcn.
};

/// @brief Sorted, disjoint LCN intervals - the shared representation backing both
/// ScanResult::lcn_index (all-occurrence) and DestScanResult::lcn_index (first-occurrence).
using LcnIntervalIndex = std::vector<LcnInterval>;

/// @brief One file's claim on a contiguous LCN range - the input to build_lcn_interval_index().
struct LcnClaim {
    uint32_t  file_index;
    LONGLONG  start_lcn;     ///< Inclusive.
    LONGLONG  end_lcn;       ///< Exclusive.
    ULONGLONG file_offset;   ///< This file's byte offset at start_lcn.
};

/// @brief Whether build_lcn_interval_index() keeps every claiming file per run,
/// or only the first (lowest file_index, i.e. earliest-scanned) claimant.
enum class ClaimOccurrence { kAll, kFirst };

/**
 * @brief Sweep-line merge of per-file LCN claims into disjoint intervals with owner lists.
 *
 * Claims come from whole extents (collect_extents()), not individual clusters, so
 * this runs in O(claims log claims) - claim count tracks fragmentation, not data
 * volume. Overlaps between claims (including partial, staggered overlaps that don't
 * align to claim boundaries) are resolved exactly, at the cluster granularity a
 * point lookup would see.
 *
 * @param claims      Per-file LCN claims (order does not matter; file_index encodes scan order).
 * @param cluster_size Volume cluster size in bytes, used to compute owners' offsets within a run.
 * @param occurrence  kAll keeps every claimant per run; kFirst keeps only the lowest file_index.
 * @return Sorted, disjoint intervals covering every claimed LCN.
 */
LcnIntervalIndex build_lcn_interval_index(
    std::vector<LcnClaim> claims,
    DWORD cluster_size,
    ClaimOccurrence occurrence
);

/// @brief Returns the first owner of the interval containing lcn, or nullptr if unclaimed.
///
/// CAUTION: the returned BlockEntry::file_offset is that owner's offset AT THE
/// INTERVAL'S start_lcn, not at lcn itself (see LcnInterval::owners). For any lcn
/// other than the interval's first cluster, use lcn_interval_find_at() instead,
/// which corrects the offset for lcn's actual position within the interval.
const BlockEntry* lcn_interval_find(const LcnIntervalIndex& index, LONGLONG lcn);

/// @brief Returns the full owner list of the interval containing lcn, or nullptr if unclaimed.
/// Same file_offset caveat as lcn_interval_find() applies to every entry returned.
const std::vector<BlockEntry>* lcn_interval_find_all(const LcnIntervalIndex& index, LONGLONG lcn);

/// @brief Like lcn_interval_find(), but returns file_offset corrected for lcn's
/// exact position within its owning interval - safe to use for any lcn, not just
/// an interval's first cluster. Returns nullopt if lcn is unclaimed.
std::optional<BlockEntry> lcn_interval_find_at(
    const LcnIntervalIndex& index, LONGLONG lcn, DWORD cluster_size);

/**
 * @brief Invokes fn(const LcnInterval&) for every interval overlapping [lcn_start, lcn_end).
 *
 * index must be sorted by start_lcn (as returned by build_lcn_interval_index()).
 * Lets a caller classify a file's own cluster range (unique vs. shared, per sub-run)
 * without visiting individual clusters.
 */
template <typename Fn>
void lcn_interval_for_each_overlapping(const LcnIntervalIndex& index, LONGLONG lcn_start, LONGLONG lcn_end, Fn&& fn) {
    auto it = std::lower_bound(index.begin(), index.end(), lcn_start,
        [](const LcnInterval& iv, LONGLONG lcn) { return iv.end_lcn <= lcn; });
    for (; it != index.end() && it->start_lcn < lcn_end; ++it) {
        fn(*it);
    }
}

/// @brief Maps a SHA-256 hex digest to the list of LCNs that contain identical content.
/// Only populated when ScanMode::kWithHash is used.
using HashIndex = std::unordered_map<std::string, std::vector<LONGLONG>>;

/// @brief Result produced by build_lcn_index_for_files.
struct ScanResult {
    LcnIntervalIndex lcn_index;    ///< All-occurrence LCN-to-file mapping.
    HashIndex hash_index;          ///< Content hash index (kWithHash only).
    DWORD     cluster_size = 0;    ///< Volume cluster size in bytes.
    std::wstring volume_root;      ///< Volume root that was scanned.
    ULONGLONG files_scanned = 0;   ///< Total files successfully enumerated.
    ULONGLONG clusters_indexed = 0;///< Total clusters entered into the index.
    std::vector<std::wstring> errors; ///< Non-fatal per-file errors encountered.

    /// @brief Interned file paths table. BlockEntry::file_index indexes into this.
    std::vector<std::wstring> file_table;

    /// @brief Resolves a file_index to its path string.
    const std::wstring& resolve_path(uint32_t index) const {
        return file_table[index];
    }
};

/// @brief Lightweight scan result for copy destination pre-scans.
///
/// lcn_index is built with ClaimOccurrence::kFirst (one owner per interval) rather
/// than kAll, minimizing RAM on large volumes with high deduplication ratios.
/// Only the hash_index field is populated when ScanMode::kWithHash is used.
struct DestScanResult {
    LcnIntervalIndex          lcn_index;      ///< First-occurrence LCN-to-file mapping.
    HashIndex                 hash_index;     ///< SHA-256 → LCNs (kWithHash only).
    std::vector<std::wstring> file_table;     ///< Interned file paths.
    std::wstring              volume_root;    ///< Volume root that was scanned.
    DWORD                     cluster_size  = 0;
    ULONGLONG                 files_scanned = 0;
    std::vector<std::wstring> errors;
};

// ============================================================================
// Public API
// ============================================================================

/**
 * @brief Builds an LCN (and optionally hash) index for the given volume.
 *
 * When @p files is empty (the default), the entire volume tree rooted at
 * @p volume_root is enumerated via FindFirstFileW before indexing.
 * When @p files is non-empty, only those paths are indexed — no directory
 * walk is performed.
 *
 * For each file, physical extents are queried via FSCTL_GET_RETRIEVAL_POINTERS.
 * In kWithHash mode each cluster-aligned block is SHA-256 hashed.
 * Respects util::g_cancel_requested for prompt Ctrl+C response.
 * Progress and non-fatal errors are reported through @p status_out.
 *
 * @param volume_root  Volume root path (e.g. L"E:\\").
 * @param mode         Scan mode (LCN-only or with content hashing).
 * @param status_out   Output interface for status, progress, and warnings.
 * @param files        Explicit file list to index. Empty = full volume scan.
 * @return ScanResult on success, or error string on fatal failure.
 */
std::expected<ScanResult, std::wstring> build_lcn_index_for_files(
    const std::wstring& volume_root,
    ScanMode mode,
    output::IOutput& status_out,
    const std::vector<std::wstring>& files = {}
);

/**
 * @brief Builds a first-occurrence LCN index and optional hash index for copy dest pre-scans.
 *
 * Unlike build_lcn_index_for_files, each physical cluster is recorded only once
 * (the first file found to contain it), significantly reducing RAM usage on volumes
 * with high deduplication ratios.
 *
 * The @p spec controls which files are scanned:
 *  - kVolume: full recursive scan of the volume root.
 *  - kPath + directory or glob: scans only the matching files.
 *  - kPath + single file: scans that file only.
 *
 * Pass ScanMode::kLcnOnly when both source and destination are ReFS (avoids SHA-256
 * computation since lcn_map handles within-operation dedup without content hashing).
 * Pass ScanMode::kWithHash when source is non-ReFS and content-based matching is needed.
 *
 * @param spec         Specifies which files to scan (volume, directory, glob, or single file).
 * @param mode         LCN-only (no hash) or with content hashing.
 * @param status_out   Output interface for progress and messages.
 * @return DestScanResult on success, or error string on fatal failure.
 */
std::expected<DestScanResult, std::wstring> build_dest_lcn_index(
    const util::FileSpecifier& spec,
    ScanMode mode,
    output::IOutput& status_out
);

// ============================================================================
// Command Phase API
// ============================================================================

/// @brief Validated context produced by prepare() and consumed by execute().
struct InspectContext {
    std::vector<util::FileSpecifier> file_specs;  ///< Non-empty, pre-validated specifiers.
    bool show_extents = false;                    ///< -e flag: emit full extent table.
    bool recursive    = false;                    ///< -r flag: recursive enumeration.
    bool strict       = false;                    ///< -s flag: abort on first error.
};

/**
 * @brief Phase 1 - Validates arguments and constructs an InspectContext.
 *
 * Accepts a single kVolume specifier (full volume scan) or one or more kPath
 * specifiers (file, directory, or glob).
 *
 * @param args Parsed CLI arguments.
 * @return Populated InspectContext on success, or error string on failure.
 */
std::expected<InspectContext, std::wstring> prepare(const util::CliArg& args);

/**
 * @brief Phase 2 - Performs the inspection using the validated context.
 *
 * Dispatches to volume-scan mode (single kVolume specifier) or file-inspect
 * mode (one or more kPath specifiers, with glob/directory expansion).
 *
 * @param ctx  Context produced by prepare().
 * @param out  Output interface.
 * @return Exit code on success, or error string on failure.
 */
std::expected<int, std::wstring> execute(InspectContext& ctx, output::IOutput& out);

/**
 * @brief Phase 3 - Releases any resources held by the context.
 * No-op for InspectContext; provided for API consistency.
 */
void cleanup(InspectContext& ctx) noexcept;

/**
 * @brief Computes a SHA-256 hex digest of a data buffer.
 *
 * Self-contained: opens and closes its own BCrypt algorithm provider per call.
 * For bulk hashing (thousands of calls), use Sha256Hasher / make_sha256_hasher()
 * instead to open the provider once and reuse it.
 *
 * @param data      Pointer to the data to hash.
 * @param data_len  Length of the data in bytes.
 * @return 64-character lowercase hex string on success, or empty string on failure.
 */
std::string compute_sha256(const BYTE* data, DWORD data_len);

/**
 * @brief Reusable SHA-256 hasher backed by a single BCrypt algorithm provider.
 *
 * Unlike compute_sha256() (which opens and closes a provider per call - fine for
 * one-off use, but thrashing in a tight per-cluster loop), a Sha256Hasher opens the
 * provider once, via make_sha256_hasher(), and reuses it for every hash() call.
 * Move-only; a default-constructed instance holds no provider and must not be
 * hashed with - only instances returned by make_sha256_hasher() are valid.
 */
class Sha256Hasher {
public:
    Sha256Hasher() = default;
    ~Sha256Hasher();

    Sha256Hasher(const Sha256Hasher&) = delete;
    Sha256Hasher& operator=(const Sha256Hasher&) = delete;
    Sha256Hasher(Sha256Hasher&& other) noexcept;
    Sha256Hasher& operator=(Sha256Hasher&& other) noexcept;

    /// @brief Hashes a data buffer using the provider opened by make_sha256_hasher().
    std::string hash(const BYTE* data, DWORD data_len) const;

private:
    friend std::expected<Sha256Hasher, std::wstring> make_sha256_hasher();
    /// @brief BCRYPT_ALG_HANDLE, kept opaque here to avoid pulling <bcrypt.h> into this header.
    void* alg_ = nullptr;
};

/// @brief Opens a Sha256Hasher, or an error string if the BCrypt provider fails to open.
std::expected<Sha256Hasher, std::wstring> make_sha256_hasher();

} // namespace inspect
