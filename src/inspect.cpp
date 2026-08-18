/**
 * @file inspect.cpp
 * @brief Block layout inspection, volume scan engine, and cross-file sharing analysis.
 *
 * Provides four modes of operation:
 *  - Single-file: dump VCN/LCN extent map, optionally with fragmentation report (-r).
 *  - Multi-file: cross-file block sharing report, optionally with per-file frag stats (-r).
 *  - Volume scan: full LCN index with optional SHA-256 content hashing.
 */

#include <algorithm>
#include <array>
#include <expected>

#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <windows.h>
#include <bcrypt.h>

#include "inspect.h"
#include "output.h"
#include "util.h"

#pragma comment(lib, "Bcrypt.lib")

namespace inspect {

// ============================================================================
// Internal Types
// ============================================================================

/// @brief All physical-layout information for a single inspected file.
struct FileInspectResult {
    std::wstring path;
    std::wstring volume_root;
    DWORD        cluster_size = 0;
    ULONGLONG    file_size    = 0;
    std::vector<Extent> extents;
    std::wstring error;
};

/// @brief Fragmentation statistics computed from a file's extent list.
struct FragStat {
    ULONGLONG fragment_count      = 0; ///< Number of non-sparse extents.
    ULONGLONG total_clusters      = 0; ///< Total non-sparse clusters.
    ULONGLONG min_extent_clusters = 0; ///< Smallest non-sparse extent (clusters).
    ULONGLONG max_extent_clusters = 0; ///< Largest non-sparse extent (clusters).
    double    avg_extent_clusters = 0; ///< Mean extent size (clusters).
};

// ============================================================================
// Shared Extent Collector
// ============================================================================

/// Number of extent records fetched per FSCTL_GET_RETRIEVAL_POINTERS call.
/// Wide on purpose: a small buffer forces one DeviceIoControl round-trip per
/// few extents, which dominates wall time on heavily fragmented large files.
constexpr DWORD kRetrievalPointersBatch = 4096;

std::expected<std::vector<Extent>, std::wstring> collect_extents(HANDLE file_handle) {
    std::vector<Extent> extents;

    STARTING_VCN_INPUT_BUFFER input = {0};
    input.StartingVcn.QuadPart = 0;

    const DWORD buf_size = sizeof(RETRIEVAL_POINTERS_BUFFER) +
                           sizeof(RETRIEVAL_POINTERS_BUFFER::Extents[0]) * kRetrievalPointersBatch;
    std::vector<BYTE> buffer(buf_size);
    auto output_buf = reinterpret_cast<PRETRIEVAL_POINTERS_BUFFER>(buffer.data());

    bool done = false;
    LONGLONG current_vcn = 0;

    while (!done) {
        DWORD bytes_returned = 0;
        BOOL ok = DeviceIoControl(
            file_handle, FSCTL_GET_RETRIEVAL_POINTERS,
            &input, sizeof(input), output_buf, buf_size,
            &bytes_returned, NULL
        );

        DWORD err = GetLastError();
        if (!ok && err != ERROR_MORE_DATA) {
            if (err == ERROR_HANDLE_EOF) break;
            return std::unexpected(L"FSCTL_GET_RETRIEVAL_POINTERS failed: " +
                                   util::get_win32_error_message(err));
        }

        if (ok) done = true;

        current_vcn = output_buf->StartingVcn.QuadPart;
        for (DWORD i = 0; i < output_buf->ExtentCount; ++i) {
            Extent ext;
            ext.start_vcn = current_vcn;
            ext.next_vcn  = output_buf->Extents[i].NextVcn.QuadPart;
            ext.lcn       = output_buf->Extents[i].Lcn.QuadPart;
            extents.push_back(ext);
            current_vcn = ext.next_vcn;
        }

        input.StartingVcn.QuadPart = current_vcn;
    }

    return extents;
}

// ============================================================================
// LCN Interval Index
// ============================================================================

LcnIntervalIndex build_lcn_interval_index(
    std::vector<LcnClaim> claims,
    DWORD cluster_size,
    ClaimOccurrence occurrence
) {
    LcnIntervalIndex result;
    if (claims.empty()) return result;

    struct Event {
        LONGLONG  pos;
        bool      is_open;   ///< true = a claim starts here; false = a claim ends here.
        uint32_t  file_index;
        LONGLONG  claim_start_lcn;
        ULONGLONG claim_file_offset;
    };

    std::vector<Event> events;
    events.reserve(claims.size() * 2);
    for (const auto& c : claims) {
        events.push_back({c.start_lcn, true,  c.file_index, c.start_lcn, c.file_offset});
        events.push_back({c.end_lcn,   false, c.file_index, c.start_lcn, c.file_offset});
    }

    // Closes sort before opens at the same position, so a claim ending at X and
    // one starting at X don't appear to overlap at X (half-open semantics).
    std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
        if (a.pos != b.pos) return a.pos < b.pos;
        if (a.is_open != b.is_open) return !a.is_open;
        return a.file_index < b.file_index;
    });

    // Active claims at the current sweep position, keyed by file_index (a file's
    // own extents never overlap each other, so this key is always unambiguous).
    // std::map keeps iteration in ascending file_index order - i.e. earliest-
    // scanned file first - matching the original first-occurrence tie-break.
    std::map<uint32_t, std::pair<LONGLONG, ULONGLONG>> active;

    size_t i = 0;
    bool have_prev = false;
    LONGLONG prev_pos = 0;

    while (i < events.size()) {
        LONGLONG pos = events[i].pos;

        if (have_prev && pos > prev_pos && !active.empty()) {
            LcnInterval iv;
            iv.start_lcn = prev_pos;
            iv.end_lcn   = pos;
            for (const auto& [file_index, claim] : active) {
                LONGLONG  claim_start  = claim.first;
                ULONGLONG claim_offset = claim.second;
                ULONGLONG offset = claim_offset +
                    static_cast<ULONGLONG>(prev_pos - claim_start) * cluster_size;
                iv.owners.push_back({file_index, offset});
                if (occurrence == ClaimOccurrence::kFirst) break;
            }
            result.push_back(std::move(iv));
        }

        // Apply every event at this exact position before sweeping onward.
        while (i < events.size() && events[i].pos == pos) {
            const Event& e = events[i];
            if (e.is_open) {
                active[e.file_index] = {e.claim_start_lcn, e.claim_file_offset};
            } else {
                active.erase(e.file_index);
            }
            ++i;
        }

        prev_pos = pos;
        have_prev = true;
    }

    return result;
}

/// @brief Binary-searches for the interval containing lcn, or nullptr if unclaimed.
/// Shared by every lcn_interval_find* variant below.
static const LcnInterval* find_owning_interval(const LcnIntervalIndex& index, LONGLONG lcn) {
    auto it = std::upper_bound(index.begin(), index.end(), lcn,
        [](LONGLONG value, const LcnInterval& iv) { return value < iv.start_lcn; });
    if (it == index.begin()) return nullptr;
    --it;
    if (lcn >= it->end_lcn) return nullptr;
    return &(*it);
}

const std::vector<BlockEntry>* lcn_interval_find_all(const LcnIntervalIndex& index, LONGLONG lcn) {
    const LcnInterval* iv = find_owning_interval(index, lcn);
    return iv ? &iv->owners : nullptr;
}

const BlockEntry* lcn_interval_find(const LcnIntervalIndex& index, LONGLONG lcn) {
    const auto* owners = lcn_interval_find_all(index, lcn);
    return (owners && !owners->empty()) ? &(*owners)[0] : nullptr;
}

std::optional<BlockEntry> lcn_interval_find_at(
    const LcnIntervalIndex& index, LONGLONG lcn, DWORD cluster_size
) {
    const LcnInterval* iv = find_owning_interval(index, lcn);
    if (!iv || iv->owners.empty()) return std::nullopt;
    const BlockEntry& first = iv->owners[0];
    ULONGLONG corrected_offset = first.file_offset +
        static_cast<ULONGLONG>(lcn - iv->start_lcn) * cluster_size;
    return BlockEntry{first.file_index, corrected_offset};
}

// ============================================================================
// Internal Helpers
// ============================================================================


/**
 * @brief Inspects the physical extents of a single file.
 *
 * Volume root and cluster size are resolved once per input specifier by the
 * caller (util::resolve_target) and passed in here - never re-queried per file.
 *
 * @param path          The target file path.
 * @param volume_root   Volume root already resolved for this file's specifier.
 * @param cluster_size  Cluster size already resolved for this file's specifier.
 * @return FileInspectResult containing extents, size, and any error.
 */
FileInspectResult inspect_file(const std::wstring& path, const std::wstring& volume_root, DWORD cluster_size) {
    FileInspectResult res;
    res.path         = path;
    res.volume_root  = volume_root;
    res.cluster_size = cluster_size;

    util::ScopedHandle file_handle(CreateFileW(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL
    ));

    if (!file_handle) {
        res.error = L"CreateFileW failed: " + util::get_win32_error_message(GetLastError());
        return res;
    }

    LARGE_INTEGER fs_size;
    if (!GetFileSizeEx(file_handle.get(), &fs_size)) {
        res.error = L"GetFileSizeEx failed: " + util::get_win32_error_message(GetLastError());
        return res;
    }
    res.file_size = fs_size.QuadPart;

    auto extents = collect_extents(file_handle.get());
    if (!extents) {
        res.error = extents.error();
        return res;
    }
    res.extents = std::move(*extents);
    return res;
}

// ============================================================================
// SHA-256 Hashing via Windows CNG
// ============================================================================

/// @brief RAII wrapper for a BCrypt algorithm provider handle.
struct BcryptAlgHandle {
    BCRYPT_ALG_HANDLE handle = nullptr;
    ~BcryptAlgHandle() { if (handle) BCryptCloseAlgorithmProvider(handle, 0); }
};

/**
 * @brief Computes a SHA-256 hash of a data buffer.
 *
 * Uses the Windows CNG API (BCrypt), which leverages SHA-NI hardware acceleration
 * automatically when available.
 *
 * @param alg       Open BCrypt algorithm provider (BCRYPT_SHA256_ALGORITHM).
 * @param data      Pointer to the data to hash.
 * @param data_len  Length of the data in bytes.
 * @return 64-character lowercase hex string on success, or empty string on failure.
 */
std::string hash_sha256(BCRYPT_ALG_HANDLE alg, const BYTE* data, DWORD data_len) {
    BCRYPT_HASH_HANDLE hash_handle = nullptr;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash_handle, nullptr, 0, nullptr, 0, 0))) {
        return {};
    }

    if (!BCRYPT_SUCCESS(BCryptHashData(hash_handle, const_cast<PUCHAR>(data), data_len, 0))) {
        BCryptDestroyHash(hash_handle);
        return {};
    }

    std::array<BYTE, 32> digest{};
    if (!BCRYPT_SUCCESS(BCryptFinishHash(hash_handle, digest.data(), static_cast<ULONG>(digest.size()), 0))) {
        BCryptDestroyHash(hash_handle);
        return {};
    }

    BCryptDestroyHash(hash_handle);

    std::ostringstream oss;
    for (BYTE b : digest) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    }
    return oss.str();
}

std::string compute_sha256(const BYTE* data, DWORD data_len) {
    BcryptAlgHandle alg;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg.handle, BCRYPT_SHA256_ALGORITHM, NULL, 0))) {
        return {};
    }
    return hash_sha256(alg.handle, data, data_len);
}

Sha256Hasher::~Sha256Hasher() {
    if (alg_) BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(alg_), 0);
}

Sha256Hasher::Sha256Hasher(Sha256Hasher&& other) noexcept : alg_(other.alg_) {
    other.alg_ = nullptr;
}

Sha256Hasher& Sha256Hasher::operator=(Sha256Hasher&& other) noexcept {
    if (this != &other) {
        if (alg_) BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(alg_), 0);
        alg_ = other.alg_;
        other.alg_ = nullptr;
    }
    return *this;
}

std::string Sha256Hasher::hash(const BYTE* data, DWORD data_len) const {
    return hash_sha256(static_cast<BCRYPT_ALG_HANDLE>(alg_), data, data_len);
}

std::expected<Sha256Hasher, std::wstring> make_sha256_hasher() {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        return std::unexpected(L"Failed to open BCrypt SHA-256 algorithm provider.");
    }
    Sha256Hasher hasher;
    hasher.alg_ = alg;
    return hasher;
}

// ============================================================================
// Scan Engine
// ============================================================================

/// @brief Result of opening one file and claiming its non-sparse extents' LCN ranges.
struct FileClaimResult {
    util::ScopedHandle    handle;        ///< Open handle to the file (caller may keep using it).
    std::vector<LcnClaim> claims;        ///< One claim per non-sparse extent.
    ULONGLONG             clusters = 0;  ///< Sum of claimed cluster counts across all claims.
};

/**
 * @brief Opens path, queries its extents, and builds one LcnClaim per non-sparse extent.
 *
 * The step genuinely shared by index_file_list and build_dest_lcn_index's first pass -
 * both need exactly this "open -> collect_extents -> claim each extent" sequence before
 * diverging into different hashing strategies (index_file_list hashes inline per-cluster
 * as it walks; build_dest_lcn_index defers hashing to a later pass, after first-occurrence
 * ownership is resolved - see doc/dupe_code.md finding #2 for why those two strategies
 * are intentionally not unified here). The opened handle is returned to the caller rather
 * than closed here, since index_file_list needs it afterward for hashing.
 *
 * @param path         Absolute file path to open.
 * @param file_idx     file_index to stamp into each produced claim.
 * @param cluster_size Volume cluster size, used to compute each claim's file_offset.
 * @return FileClaimResult on success, or error string (CreateFileW/collect_extents
 *         failure) on failure.
 */
static std::expected<FileClaimResult, std::wstring> open_and_claim_extents(
    const std::wstring& path, uint32_t file_idx, DWORD cluster_size
) {
    FileClaimResult result;
    result.handle = util::ScopedHandle(CreateFileW(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL
    ));
    if (!result.handle) {
        return std::unexpected(util::get_win32_error_message(GetLastError()));
    }

    auto extents_res = collect_extents(result.handle.get());
    if (!extents_res) {
        return std::unexpected(extents_res.error());
    }

    for (const auto& ext : *extents_res) {
        if (ext.is_sparse()) continue;
        ULONGLONG extent_file_offset = static_cast<ULONGLONG>(ext.start_vcn) * cluster_size;
        result.claims.push_back({file_idx, ext.lcn, ext.lcn + static_cast<LONGLONG>(ext.cluster_count()),
                                  extent_file_offset});
        result.clusters += ext.cluster_count();
    }

    return result;
}

/**
 * @brief Indexes a pre-built list of file paths into a ScanResult.
 *
 * Shared by build_lcn_index_for_files in both full-volume and targeted modes.
 * (targeted). Performs BCrypt initialization (Phase B) and the per-file
 * extent-query + optional-hash loop (Phase C).
 *
 * Checks util::g_cancel_requested at file-loop entry and inside the
 * cluster loop for prompt Ctrl+C response. Calls status_out.progress()
 * once per file so the caller's progress bar moves steadily.
 *
 * @param file_paths  Absolute paths to index.
 * @param result      ScanResult to populate (volume_root and cluster_size
 *                    must already be set by the caller).
 * @param mode        LCN-only or with content hashing.
 * @param status_out  Output interface for progress and messages.
 * @return true, or error string if BCrypt initialisation fails.
 */
static std::expected<bool, std::wstring> index_file_list(
    const std::vector<std::wstring>& file_paths,
    ScanResult& result,
    ScanMode mode,
    output::IOutput& status_out
) {
    const ULONGLONG total_files = static_cast<ULONGLONG>(file_paths.size());

    // Phase B: Open BCrypt provider for hashing (if requested)
    BcryptAlgHandle alg;
    if (mode == ScanMode::kWithHash) {
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
                &alg.handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
            return std::unexpected(L"Failed to open BCrypt SHA-256 algorithm provider.");
        }
    }

    // Phase C: Inspect each file, build index.
    // Each file is opened once: extents are queried and (optionally) cluster
    // data is hashed through the same handle.
    std::vector<BYTE> read_buffer;
    if (mode == ScanMode::kWithHash && result.cluster_size > 0) {
        read_buffer.resize(result.cluster_size);
    }

    // Claims accumulate per extent, not per cluster; the interval index is
    // built once at the end via a single sweep (see build_lcn_interval_index).
    std::vector<LcnClaim> claims;

    for (const auto& path : file_paths) {
        if (util::g_cancel_requested) {
            status_out.message(output::Level::info,
                L"[build_lcn_index] Cancelled after " +
                std::to_wstring(result.files_scanned) + L" of " +
                std::to_wstring(total_files) + L" files.");
            break;
        }

        // Progress: report before opening each file so the bar moves steadily.
        status_out.progress(result.volume_root, result.files_scanned, total_files);

        // Open the file, query its extents, and claim each non-sparse extent's LCN
        // range - the step shared with build_dest_lcn_index (see open_and_claim_extents).
        uint32_t file_idx = static_cast<uint32_t>(result.file_table.size());
        auto claim_res = open_and_claim_extents(path, file_idx, result.cluster_size);
        if (!claim_res) {
            result.errors.push_back(path + L": " + claim_res.error());
            continue;
        }

        // Get file size (needed for hash read-bounds; queried unconditionally to
        // match this function's existing file-viability check regardless of mode).
        LARGE_INTEGER fs_size;
        if (!GetFileSizeEx(claim_res->handle.get(), &fs_size)) {
            result.errors.push_back(path + L": GetFileSizeEx failed: " +
                                    util::get_win32_error_message(GetLastError()));
            continue;
        }
        ULONGLONG file_size = fs_size.QuadPart;

        // Intern the file path.
        result.file_table.push_back(path);
        result.files_scanned++;
        result.clusters_indexed += claim_res->clusters;

        // Record each extent's claim, and optionally hash each cluster within it
        // (hashing is content-addressed and must stay per-cluster - it is the
        // "external call" exception to per-extent).
        bool cancelled = false;
        for (const auto& claim : claim_res->claims) {
            claims.push_back(claim);

            if (mode == ScanMode::kWithHash) {
                ULONGLONG run_clusters = static_cast<ULONGLONG>(claim.end_lcn - claim.start_lcn);
                for (ULONGLONG c = 0; c < run_clusters; ++c) {
                    if (util::g_cancel_requested) { cancelled = true; break; }

                    LONGLONG  lcn         = claim.start_lcn + static_cast<LONGLONG>(c);
                    ULONGLONG file_offset = claim.file_offset + c * result.cluster_size;

                    LARGE_INTEGER li;
                    li.QuadPart = static_cast<LONGLONG>(file_offset);
                    if (!SetFilePointerEx(claim_res->handle.get(), li, NULL, FILE_BEGIN)) continue;

                    DWORD bytes_read = 0;
                    DWORD to_read = static_cast<DWORD>(
                        std::min<ULONGLONG>(result.cluster_size,
                            file_size > file_offset ? file_size - file_offset : 0));

                    if (to_read == 0) continue;

                    if (ReadFile(claim_res->handle.get(), read_buffer.data(), to_read, &bytes_read, NULL)
                            && bytes_read > 0) {
                        std::string digest = hash_sha256(alg.handle, read_buffer.data(), bytes_read);
                        if (!digest.empty()) {
                            result.hash_index[digest].push_back(lcn);
                        }
                    }
                }
            }
            if (cancelled) break;
        }
    }

    // Final progress tick at 100%.
    if (!util::g_cancel_requested) {
        status_out.progress(result.volume_root, total_files, total_files);
    }

    result.lcn_index = build_lcn_interval_index(std::move(claims), result.cluster_size, ClaimOccurrence::kAll);

    status_out.message(output::Level::info, L"[build_lcn_index] Complete: " +
                      std::to_wstring(result.files_scanned) + L" files, " +
                      std::to_wstring(result.clusters_indexed) + L" clusters indexed.");

    return true;
}

std::expected<ScanResult, std::wstring> build_lcn_index_for_files(
    const std::wstring& volume_root,
    ScanMode mode,
    output::IOutput& status_out,
    const std::vector<std::wstring>& files
) {
    ScanResult result;
    result.volume_root = volume_root;

    auto vol_info = util::resolve_volume_info(volume_root);
    if (!vol_info) return std::unexpected(vol_info.error());
    result.cluster_size = vol_info->cluster_size;

    // If no explicit file list was provided, enumerate the whole volume.
    std::vector<std::wstring> enumerated;
    const std::vector<std::wstring>* to_index = &files;
    if (files.empty()) {
        status_out.message(output::Level::info,
            L"[build_lcn_index] Enumerating files on " + volume_root + L"...");
        util::enumerate_files_recursive(volume_root, enumerated, result.errors);
        status_out.message(output::Level::info,
            L"[build_lcn_index] Found " + std::to_wstring(enumerated.size()) +
            L" files. Indexing...");
        to_index = &enumerated;
    } else {
        status_out.message(output::Level::info,
            L"[build_lcn_index] Indexing " + std::to_wstring(files.size()) +
            L" file(s) on " + volume_root + L"...");
    }

    auto ok = index_file_list(*to_index, result, mode, status_out);
    if (!ok) return std::unexpected(ok.error());

    return result;
}

std::expected<DestScanResult, std::wstring> build_dest_lcn_index(
    const util::FileSpecifier& spec,
    ScanMode mode,
    output::IOutput& status_out
) {
    DestScanResult result;

    status_out.message(output::Level::info,
        L"[build_dest_lcn_index] Enumerating files from " + spec.path + L"...");
    auto target = util::resolve_target(spec, result.errors);
    if (!target) return std::unexpected(target.error());

    result.volume_root  = target->volume_root;
    result.cluster_size = target->cluster_size;
    const std::vector<std::wstring>& file_paths = target->files;

    status_out.message(output::Level::info,
        L"[build_dest_lcn_index] Found " + std::to_wstring(file_paths.size()) +
        L" files. Building first-occurrence LCN index...");

    const ULONGLONG total_files = static_cast<ULONGLONG>(file_paths.size());

    // Pass 1: collect extents only (cheap - no reads, no hashing). Ownership
    // for overlapping claims is resolved afterward by the sweep below, so
    // hashing can't happen inline here the way it did in the old per-cluster
    // "first occurrence wins" loop - see the hashing pass further down.
    std::vector<LcnClaim> claims;

    for (const auto& path : file_paths) {
        if (util::g_cancel_requested) break;

        status_out.progress(result.volume_root, result.files_scanned, total_files);

        uint32_t file_idx = static_cast<uint32_t>(result.file_table.size());
        auto claim_res = open_and_claim_extents(path, file_idx, result.cluster_size);
        if (!claim_res) {
            result.errors.push_back(path + L": " + claim_res.error());
            continue;
        }

        result.file_table.push_back(path);
        result.files_scanned++;

        for (auto& claim : claim_res->claims) {
            claims.push_back(std::move(claim));
        }
    }

    if (!util::g_cancel_requested) {
        status_out.progress(result.volume_root, total_files, total_files);
    }

    // Pass 2: sweep claims into a first-occurrence interval index. Overlapping
    // claims resolve to whichever file was scanned earliest, same tie-break
    // as the old try_emplace-based "first LCN write wins" behavior.
    result.lcn_index = build_lcn_interval_index(std::move(claims), result.cluster_size, ClaimOccurrence::kFirst);

    // Pass 3: hash each interval's winning owner (only in kWithHash mode).
    // Each physical run is hashed once, from whichever file won it - the
    // same "hash only the first occurrence" property the old inline check
    // gave, just computed after ownership is fully resolved instead of during.
    if (mode == ScanMode::kWithHash && !util::g_cancel_requested) {
        BcryptAlgHandle alg;
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
                &alg.handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
            return std::unexpected(L"Failed to open BCrypt SHA-256 provider.");
        }

        std::vector<BYTE> read_buffer(result.cluster_size);
        util::ScopedHandle cur_handle;
        uint32_t  cur_file_idx  = UINT32_MAX;
        ULONGLONG cur_file_size = 0;

        for (const auto& iv : result.lcn_index) {
            if (util::g_cancel_requested) break;
            if (iv.owners.empty()) continue;
            const BlockEntry& owner = iv.owners[0];

            if (owner.file_index != cur_file_idx) {
                cur_file_idx = owner.file_index;
                cur_handle = util::ScopedHandle(CreateFileW(result.file_table[cur_file_idx].c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL));
                cur_file_size = 0;
                if (cur_handle) {
                    LARGE_INTEGER fs_size;
                    if (GetFileSizeEx(cur_handle.get(), &fs_size)) cur_file_size = static_cast<ULONGLONG>(fs_size.QuadPart);
                }
            }
            if (!cur_handle) continue;

            ULONGLONG run_clusters = static_cast<ULONGLONG>(iv.end_lcn - iv.start_lcn);
            for (ULONGLONG c = 0; c < run_clusters; ++c) {
                if (util::g_cancel_requested) break;

                ULONGLONG file_off = owner.file_offset + c * result.cluster_size;
                ULONGLONG rem = cur_file_size > file_off ? cur_file_size - file_off : 0;
                DWORD to_read = static_cast<DWORD>(std::min<ULONGLONG>(result.cluster_size, rem));
                if (to_read == 0) continue;

                LARGE_INTEGER li;
                li.QuadPart = static_cast<LONGLONG>(file_off);
                if (!SetFilePointerEx(cur_handle.get(), li, NULL, FILE_BEGIN)) continue;

                DWORD bytes_read = 0;
                if (ReadFile(cur_handle.get(), read_buffer.data(), to_read, &bytes_read, NULL) && bytes_read > 0) {
                    std::string digest = hash_sha256(alg.handle, read_buffer.data(), bytes_read);
                    if (!digest.empty()) {
                        result.hash_index[digest].push_back(iv.start_lcn + static_cast<LONGLONG>(c));
                    }
                }
            }
        }
    }

    status_out.message(output::Level::info, L"[build_dest_lcn_index] Complete: " +
                      std::to_wstring(result.files_scanned) + L" files, " +
                      std::to_wstring(result.lcn_index.size()) + L" intervals indexed.");
    return result;
}

// ============================================================================
// Output Helpers
// ============================================================================

FragStat compute_frag_stat(const FileInspectResult& res);
void output_frag_report(const FileInspectResult& res, const FragStat& stat, output::IOutput& out);

/**
 * @brief Outputs the single-file inspect report via IOutput.
 *
 * The output is wrapped in a named section so that JSON output produces
 * a clean nested object rather than polluting the root.
 *
 * @param res          The inspection result for the file.
 * @param show_extents If true, emit the full VCN/LCN extent table.
 *                     If false, emit only the summary fields.
 */
void output_single_file(const FileInspectResult& res, output::IOutput& out, bool show_extents, bool recursive) {
    // Use the bare filename as the section name for a clean JSON key.
    std::wstring section_name = res.path;
    size_t last_sep = section_name.find_last_of(L"\\/ ");
    if (last_sep != std::wstring::npos) section_name = section_name.substr(last_sep + 1);

    out.begin_section(section_name);
    out.field(L"File",         res.path);
    out.field(L"Volume",       res.volume_root);
    out.field(L"Cluster Size", std::to_wstring(res.cluster_size) + L" bytes");
    out.field(L"File Size",    std::to_wstring(res.file_size) + L" bytes");
    out.field(L"Fragments",    std::to_wstring(res.extents.size()));

    if (show_extents) {
        out.begin_table({L"Extent", L"VCN", L"LCN", L"Clusters", L"Bytes", L"Cumulative"});

        ULONGLONG cumulative_bytes = 0;
        for (size_t i = 0; i < res.extents.size(); ++i) {
            const auto& ext = res.extents[i];
            ULONGLONG bytes = ext.cluster_count() * res.cluster_size;
            cumulative_bytes += bytes;

            std::wstring lcn_str = ext.lcn == (LONGLONG)-1 ? L"SPARSE" : std::to_wstring(ext.lcn);

            out.table_row({
                std::to_wstring(i),
                std::to_wstring(ext.start_vcn),
                lcn_str,
                std::to_wstring(ext.cluster_count()),
                std::to_wstring(bytes),
                std::to_wstring(cumulative_bytes)
            });
        }

        out.end_table();
    }

    if (recursive) {
        auto stat = compute_frag_stat(res);
        output_frag_report(res, stat, out);
    }

    out.end_section();
}

/**
 * @brief Computes fragmentation statistics from a file's extent list.
 *
 * Only non-sparse extents contribute to the counts. Returns a zeroed
 * FragStat for files with no allocated extents.
 *
 * @param res The inspect result for a single file.
 * @return FragStat populated with fragment count, min/max/avg extent size.
 */
FragStat compute_frag_stat(const FileInspectResult& res) {
    FragStat stat;

    for (const auto& ext : res.extents) {
        if (ext.is_sparse()) continue;

        stat.fragment_count++;
        stat.total_clusters += ext.cluster_count();

        if (stat.fragment_count == 1) {
            stat.min_extent_clusters = ext.cluster_count();
            stat.max_extent_clusters = ext.cluster_count();
        } else {
            if (ext.cluster_count() < stat.min_extent_clusters)
                stat.min_extent_clusters = ext.cluster_count();
            if (ext.cluster_count() > stat.max_extent_clusters)
                stat.max_extent_clusters = ext.cluster_count();
        }
    }

    if (stat.fragment_count > 0) {
        stat.avg_extent_clusters = static_cast<double>(stat.total_clusters) /
                                   static_cast<double>(stat.fragment_count);
    }

    return stat;
}

/**
 * @brief Outputs a fragmentation report section for a single file.
 *
 * The score is simply the fragment count: 1 = perfectly contiguous,
 * higher values indicate increasing fragmentation.
 *
 * @param res   The inspect result for a single file.
 * @param stat  Pre-computed fragmentation statistics.
 * @param out   The output interface.
 */
void output_frag_report(const FileInspectResult& res, const FragStat& stat, output::IOutput& out) {
    out.begin_section(L"Fragmentation Report");
    out.field(L"File",             res.path);
    out.field(L"Fragments",        std::to_wstring(stat.fragment_count));
    out.field(L"Frag Score",       std::to_wstring(stat.fragment_count) +
                                       (stat.fragment_count == 1 ? L" (optimal)" :
                                        stat.fragment_count <= 4  ? L" (good)" :
                                        stat.fragment_count <= 16 ? L" (moderate)" :
                                                                    L" (high)"));
    {
        std::wostringstream ss;
        ULONGLONG bytes = stat.min_extent_clusters * res.cluster_size;
        ss << stat.min_extent_clusters << L" clusters (" << util::format_size(bytes) << L")";
        out.field(L"Smallest Extent", ss.str());
    }
    {
        std::wostringstream ss;
        ULONGLONG bytes = stat.max_extent_clusters * res.cluster_size;
        ss << stat.max_extent_clusters << L" clusters (" << util::format_size(bytes) << L")";
        out.field(L"Largest Extent", ss.str());
    }
    {
        std::wostringstream ss;
        ULONGLONG avg_bytes = static_cast<ULONGLONG>(stat.avg_extent_clusters * res.cluster_size);
        ss << std::fixed << std::setprecision(1) << stat.avg_extent_clusters
           << L" clusters (" << util::format_size(avg_bytes) << L")";
        out.field(L"Avg Extent",     ss.str());
    }
    out.end_section();
}


/**
 * @brief Outputs the multi-file inspect report via IOutput.
 *
 * Default (show_extended = false): emits the aggregate summary fields and the
 * per-file unique/shared cluster breakdown table only - no cross-file sharing
 * matrix and no per-cluster correlation detail.
 *
 * Extended (show_extended = true, -e flag): additionally emits the cross-file
 * sharing matrix section showing how many bytes each file pair shares.
 *
 * @param results       One FileInspectResult per input file.
 * @param errors        Non-fatal error strings accumulated during inspection.
 * @param out           The output interface.
 * @param show_extended Whether to include the extended sharing matrix.
 */
void output_multi_file(
    const std::vector<FileInspectResult>& results,
    const std::vector<std::wstring>& errors,
    output::IOutput& out,
    bool show_extended,
    bool recursive
) {
    // ── Build the LCN interval index ──────────────────────────────────────────
    // Claims accumulate per extent (cheap - fragment count, not cluster count);
    // a single sweep afterward resolves ownership at cluster granularity without
    // ever materializing one entry per cluster.
    std::vector<LcnClaim> claims;
    ULONGLONG total_file_bytes    = 0;
    DWORD     common_cluster_size = 0;
    std::wstring common_volume;

    const ULONGLONG analyze_total = static_cast<ULONGLONG>(results.size()) * 2;

    for (size_t f_idx = 0; f_idx < results.size(); ++f_idx) {
        // Phase 2a progress: claim collection (first half of analyzing bar)
        out.progress(L"analyzing", static_cast<ULONGLONG>(f_idx), analyze_total);

        const auto& res = results[f_idx];
        if (!res.error.empty()) continue;

        if (common_cluster_size == 0) {
            common_cluster_size = res.cluster_size;
            common_volume = res.volume_root;
        }

        total_file_bytes += res.file_size;

        for (const auto& ext : res.extents) {
            if (ext.is_sparse()) continue;
            claims.push_back({static_cast<uint32_t>(f_idx), ext.lcn,
                              ext.lcn + static_cast<LONGLONG>(ext.cluster_count()),
                              static_cast<ULONGLONG>(ext.start_vcn) * common_cluster_size});
        }
    }

    LcnIntervalIndex lcn_index = build_lcn_interval_index(
        std::move(claims), common_cluster_size, ClaimOccurrence::kAll);

    ULONGLONG grand_total     = 0; ///< Distinct LCNs claimed by any file.
    ULONGLONG grand_shared    = 0; ///< Distinct LCNs claimed by >= 2 files.
    ULONGLONG grand_unique    = 0; ///< Distinct LCNs claimed by exactly 1 file.
    ULONGLONG saved_clusters  = 0;
    for (const auto& iv : lcn_index) {
        ULONGLONG run_clusters = static_cast<ULONGLONG>(iv.end_lcn - iv.start_lcn);
        grand_total += run_clusters;
        if (iv.owners.size() >= 2) {
            grand_shared   += run_clusters;
            saved_clusters += run_clusters * (iv.owners.size() - 1);
        } else {
            grand_unique += run_clusters;
        }
    }

    ULONGLONG shared_bytes = grand_shared   * common_cluster_size;
    ULONGLONG saved_bytes  = saved_clusters * common_cluster_size;
    double savings_pct = total_file_bytes > 0
        ? (static_cast<double>(saved_bytes) / total_file_bytes) * 100.0
        : 0.0;


    // ── Phase 2b: per-file cluster breakdown ─────────────────────────────────
    // Collect rows locally so no output is produced while the progress bar
    // is still running. All IOutput calls are deferred until after the bar
    // completes at 100%.
    struct BreakdownRow {
        std::wstring name;
        ULONGLONG total_c  = 0;
        ULONGLONG unique_c = 0;
        ULONGLONG shared_c = 0;
    };
    std::vector<BreakdownRow> breakdown_rows;

    for (size_t f_idx = 0; f_idx < results.size(); ++f_idx) {
        out.progress(L"analyzing",
                     static_cast<ULONGLONG>(results.size() + f_idx), analyze_total);

        const auto& res = results[f_idx];
        if (!res.error.empty()) continue;

        std::wstring path = res.path;
        size_t last_slash2 = path.find_last_of(L"\\/ ");
        std::wstring name2 = (last_slash2 != std::wstring::npos)
            ? path.substr(last_slash2 + 1) : path;
        // Full filename is passed; CliOutput::end_table() truncates for display.

        ULONGLONG total_c  = 0;
        ULONGLONG unique_c = 0;
        ULONGLONG shared_c = 0;

        // Classify this file's own clusters by walking the (few) intervals
        // overlapping each of its extents, instead of visiting each cluster.
        for (const auto& ext : res.extents) {
            if (ext.is_sparse()) continue;
            LONGLONG ext_start = ext.lcn;
            LONGLONG ext_end   = ext.lcn + static_cast<LONGLONG>(ext.cluster_count());
            total_c += static_cast<ULONGLONG>(ext_end - ext_start);

            lcn_interval_for_each_overlapping(lcn_index, ext_start, ext_end,
                [&](const LcnInterval& iv) {
                    LONGLONG lo = (std::max)(iv.start_lcn, ext_start);
                    LONGLONG hi = (std::min)(iv.end_lcn, ext_end);
                    if (hi <= lo) return;
                    ULONGLONG n = static_cast<ULONGLONG>(hi - lo);
                    if (iv.owners.size() >= 2) shared_c += n;
                    else                       unique_c += n;
                });
        }

        breakdown_rows.push_back({name2, total_c, unique_c, shared_c});
    }

    // ── Complete the analyzing bar ────────────────────────────────────────────
    // This clears the progress line. All output below begins on a clean line.
    out.progress(L"analyzing", analyze_total, analyze_total);

    // ── Summary header ────────────────────────────────────────────────────────
    out.begin_section(L"inspect multi-file report");
    out.field(L"Volume",         common_volume);
    out.field(L"Cluster Size",   std::to_wstring(common_cluster_size) + L" bytes");
    out.field(L"Files Analyzed", std::to_wstring(results.size()));
    out.field(L"Total Size",     util::format_size_detailed(total_file_bytes));
    out.field(L"Shared Blocks",  util::format_size_detailed(shared_bytes));
    out.field(L"Saved Space",    util::format_size_detailed(saved_bytes));
    {
        std::wostringstream ss;
        ss << std::fixed << std::setprecision(2) << savings_pct << L"%";
        out.field(L"Dedup Savings", ss.str());
    }

    // ── Per-file unique / shared cluster breakdown ────────────────────────────
    out.begin_section(L"Per-File Cluster Breakdown");
    out.begin_table({L"File", L"Total Clusters", L"Unique Clusters",
                     L"Shared Clusters", L"Unique Bytes", L"Shared Bytes"});

    for (const auto& row : breakdown_rows) {
        out.table_row({
            row.name,
            std::to_wstring(row.total_c),
            std::to_wstring(row.unique_c),
            std::to_wstring(row.shared_c),
            util::format_size(row.unique_c * common_cluster_size),
            util::format_size(row.shared_c * common_cluster_size)
        });
    }

    out.table_row({
        L"[TOTAL]",
        std::to_wstring(grand_total),
        std::to_wstring(grand_unique),
        std::to_wstring(grand_shared),
        util::format_size(grand_unique * common_cluster_size),
        util::format_size(grand_shared * common_cluster_size)
    });

    out.end_table();
    out.end_section();

    // ── Extended: sharing matrix (only with -e) ───────────────────────────────
    if (show_extended) {
        std::vector<std::vector<ULONGLONG>> shared_matrix(
            results.size(), std::vector<ULONGLONG>(results.size(), 0));
        for (const auto& iv : lcn_index) {
            if (iv.owners.size() < 2) continue;
            ULONGLONG run_clusters = static_cast<ULONGLONG>(iv.end_lcn - iv.start_lcn);
            for (size_t i = 0; i < iv.owners.size(); ++i) {
                for (size_t j = i + 1; j < iv.owners.size(); ++j) {
                    uint32_t fi = iv.owners[i].file_index;
                    uint32_t fj = iv.owners[j].file_index;
                    shared_matrix[fi][fj] += run_clusters;
                    shared_matrix[fj][fi] += run_clusters;
                }
            }
        }

        std::vector<std::wstring> columns;
        columns.push_back(L"File");
        for (size_t j = 0; j < results.size(); ++j) {
            columns.push_back(L"F" + std::to_wstring(j));
        }

        out.begin_section(L"Sharing Matrix");
        out.begin_table(columns);
        for (size_t i = 0; i < results.size(); ++i) {
            std::wstring path = results[i].path;
            size_t last_slash = path.find_last_of(L"\\/ ");
            std::wstring name = (last_slash != std::wstring::npos) ? path.substr(last_slash + 1) : path;
            // Full filename is passed; CliOutput::end_table() truncates for display.

            std::vector<std::wstring> row;
            row.push_back(name);
            for (size_t j = 0; j < results.size(); ++j) {
                if (i == j) {
                    row.push_back(L"-");
                } else {
                    row.push_back(util::format_size(shared_matrix[i][j] * common_cluster_size));
                }
            }
            out.table_row(row);
        }
        out.end_table();
        out.end_section();
    }

    if (recursive) {
        for (const auto& res : results) {
            if (!res.error.empty()) continue;
            auto stat = compute_frag_stat(res);
            output_frag_report(res, stat, out);
        }
    }

    for (const auto& err : errors) {
        out.message(output::Level::error, err);
    }

    out.end_section();
}

/**
 * @brief Outputs a volume-wide deduplication scan report from a ScanResult.
 */
void output_scan_report(const ScanResult& scan, output::IOutput& out) {
    // Count shared clusters and compute savings
    ULONGLONG shared_clusters = 0;
    ULONGLONG saved_clusters  = 0;

    for (const auto& iv : scan.lcn_index) {
        if (iv.owners.size() >= 2) {
            ULONGLONG run_clusters = static_cast<ULONGLONG>(iv.end_lcn - iv.start_lcn);
            shared_clusters += run_clusters;
            saved_clusters  += run_clusters * (iv.owners.size() - 1);
        }
    }

    ULONGLONG shared_bytes = shared_clusters * scan.cluster_size;
    ULONGLONG saved_bytes  = saved_clusters  * scan.cluster_size;
    ULONGLONG total_bytes  = scan.clusters_indexed * scan.cluster_size;
    double savings_pct = total_bytes > 0
        ? (static_cast<double>(saved_bytes) / total_bytes) * 100.0
        : 0.0;

    out.begin_section(L"retool inspect volume scan report");
    out.field(L"Volume",           scan.volume_root);
    out.field(L"Cluster Size",     std::to_wstring(scan.cluster_size) + L" bytes");
    out.field(L"Files Scanned",    std::to_wstring(scan.files_scanned));
    out.field(L"Clusters Indexed", std::to_wstring(scan.clusters_indexed));
    out.field(L"Shared Blocks", util::format_size(shared_bytes) +
              L" (" + std::to_wstring(shared_clusters) + L" clusters)");
    out.field(L"Dedup Savings", util::format_size_detailed(saved_bytes));
    {
        std::wostringstream ss;
        ss << std::fixed << std::setprecision(2) << savings_pct << L"%";
        out.field(L"Savings %", ss.str());
    }

    // Hash-based groups (only if kWithHash scan was run)
    if (!scan.hash_index.empty()) {
        ULONGLONG hash_groups = 0;
        for (const auto& [digest, lcns] : scan.hash_index) {
            if (lcns.size() >= 2) hash_groups++;
        }
        out.field(L"Content Groups", std::to_wstring(hash_groups));
    }

    out.end_section();

    for (const auto& err : scan.errors) {
        out.message(output::Level::error, err);
    }
}

// ============================================================================
// Command Phase API
// ============================================================================

std::expected<InspectContext, std::wstring> prepare(const util::CliArg& args) {
    if (args.file_specs.empty()) {
        return std::unexpected(L"Error: No target files, directory, or volume specified for inspection.");
    }
    InspectContext ctx;
    ctx.file_specs   = args.file_specs;
    ctx.show_extents = args.show_extents;
    ctx.recursive    = args.recursive;
    ctx.strict       = args.strict;
    return ctx;
}

std::expected<int, std::wstring> execute(InspectContext& ctx, output::IOutput& out) {
    // ── Volume scan mode: single kVolume specifier ────────────────────────────
    if (ctx.file_specs.size() == 1 &&
            ctx.file_specs[0].kind == util::FileSpecKind::kVolume) {
        ScanMode mode = ScanMode::kLcnOnly;
        std::wstring vol_root = util::normalize_volume_root(ctx.file_specs[0].path);
        auto scan_res = build_lcn_index_for_files(vol_root, mode, out);
        if (!scan_res) return std::unexpected(scan_res.error());
        output_scan_report(*scan_res, out);
        return 0;
    }

    // ── Single or multi-file mode ─────────────────────────────────────────────
    // Resolve volume root and cluster size once per specifier - never per file -
    // then expand each specifier's file list under that resolved volume info.
    struct TargetFile {
        std::wstring path;
        std::wstring volume_root;
        DWORD        cluster_size;
    };
    std::vector<TargetFile> target_files;
    std::vector<std::wstring> expand_errors;

    for (const auto& spec : ctx.file_specs) {
        auto target = util::resolve_target(spec, expand_errors);
        if (!target) {
            if (ctx.strict) {
                return std::unexpected(L"Strict Mode: Failed to resolve target '" + spec.path +
                                       L"'. Error: " + target.error());
            }
            expand_errors.push_back(spec.path + L": " + target.error());
            continue;
        }
        for (auto& path : target->files) {
            target_files.push_back({std::move(path), target->volume_root, target->cluster_size});
        }
    }

    for (const auto& err : expand_errors) {
        out.message(output::Level::warn, err);
    }

    if (target_files.empty()) {
        return std::unexpected(L"Error: No files matched the specified path(s).");
    }

    // ── Single or multi-file mode ─────────────────────────────────────────────
    std::vector<FileInspectResult> results;
    std::vector<std::wstring> errors;

    if (target_files.size() == 1) {
        // Single-file: emit a brief status then inspect
        const std::wstring& path = target_files[0].path;
        std::wstring name = path;
        size_t last_sep = name.find_last_of(L"\\/ ");
        if (last_sep != std::wstring::npos) name = name.substr(last_sep + 1);
        out.message(output::Level::info, L"Inspecting: " + name);
    } else {
        // Multi-file: announce the count up-front, then log each file
        out.message(output::Level::info, L"Inspecting " + std::to_wstring(target_files.size()) + L" files...");
    }

    const ULONGLONG total_files = static_cast<ULONGLONG>(target_files.size());

    for (ULONGLONG f_idx = 0; f_idx < total_files; ++f_idx) {
        const TargetFile& tf = target_files[static_cast<size_t>(f_idx)];

        // Drive the reading progress bar (extent collection phase)
        if (total_files > 1) {
            out.progress(L"reading", f_idx, total_files);
        }

        auto res = inspect_file(tf.path, tf.volume_root, tf.cluster_size);
        if (!res.error.empty()) {
            errors.push_back(tf.path + L": " + res.error);
            if (ctx.strict) {
                return std::unexpected(L"Strict Mode: Failed to inspect " + tf.path +
                                       L". Error: " + res.error);
            }
        }
        results.push_back(std::move(res));
    }

    // Complete the reading bar before analysis begins
    if (total_files > 1) {
        out.progress(L"reading", total_files, total_files);
    }

    if (results.size() == 1) {
        const auto& res = results[0];
        if (!res.error.empty()) return std::unexpected(res.error);
        output_single_file(res, out, ctx.show_extents, ctx.recursive);
    } else {
        output_multi_file(results, errors, out, ctx.show_extents, ctx.recursive);
    }

    return 0;
}

void cleanup(InspectContext& /*ctx*/) noexcept {}

} // namespace inspect
