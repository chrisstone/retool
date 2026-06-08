/**
 * @file dedup.cpp
 * @brief In-place file deduplication engine for ReFS volumes.
 *
 * Implements a three-phase pipeline (Inspect → Operate → Finalize) for both
 * volume-wide and pair-wise deduplication of files on ReFS using
 * FSCTL_DUPLICATE_EXTENTS_TO_FILE.
 */

#include <algorithm>
#include <expected>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <windows.h>

#include "copy.h"
#include "dedup.h"
#include "inspect.h"
#include "output.h"
#include "util.h"

namespace dedup {

// ============================================================================
// Internal Types
// ============================================================================

/// @brief Accumulated statistics for the dedup operation.
struct DedupStats {
    ULONGLONG files_processed  = 0;
    ULONGLONG clusters_deduped = 0;
    ULONGLONG bytes_reclaimed  = 0;
    std::vector<std::wstring> errors;
};

/// @brief A candidate pair of LCNs to deduplicate: the duplicate should become
///        a clone of the canonical cluster.
struct DedupCandidate {
    std::wstring canonical_path;    ///< Path of the file owning the canonical cluster.
    ULONGLONG    canonical_offset;  ///< Byte offset within canonical_path.
    std::wstring duplicate_path;    ///< Path of the file that should be deduplicated.
    ULONGLONG    duplicate_offset;  ///< Byte offset within duplicate_path.
    ULONGLONG    cluster_size;      ///< Number of bytes to deduplicate.
};

/// @brief Shared state threaded through the dedup pipeline.
struct DedupContext {
    // Phase 1: set during inspection
    std::wstring         volume_root;
    DWORD                cluster_size = 0;
    bool                 is_volume_wide = false;   ///< true = volume scan, false = pair-wise.
    inspect::ScanResult  scan;                     ///< Volume/pair scan results.
    output::IOutput*     out = nullptr;            ///< Non-owning pointer to output interface.

    // Phase 2: populated during operation
    std::vector<DedupCandidate> candidates;
    DedupStats stats;

    DedupContext() = default;
    DedupContext(const DedupContext&) = delete;
    DedupContext& operator=(const DedupContext&) = delete;
};

// ============================================================================
// Strategy Interface
// ============================================================================

/// @brief Abstract interface for selecting and producing dedup candidates.
struct IDedupStrategy {
    virtual ~IDedupStrategy() = default;

    /**
     * @brief Produces a list of DedupCandidate items from the scan result.
     *
     * @param scan     The volume or pair scan result.
     * @param context  The shared context (for cluster_size and output).
     * @return Vector of candidates on success, or error string on failure.
     */
    virtual std::expected<std::vector<DedupCandidate>, std::wstring> build_candidates(
        const inspect::ScanResult& scan,
        DedupContext& context
    ) = 0;
};

// ============================================================================
// Strategy: Volume-Wide Deduplication
// ============================================================================

/**
 * @brief Finds all groups of LCNs that share the same SHA-256 content hash and
 *        builds candidates to replace duplicates with clones of the first LCN in
 *        each group.
 */
struct VolumeWideDedupStrategy : IDedupStrategy {
    std::expected<std::vector<DedupCandidate>, std::wstring> build_candidates(
        const inspect::ScanResult& scan,
        DedupContext& context
    ) override {
        std::vector<DedupCandidate> candidates;

        for (const auto& [digest, lcns] : scan.hash_index) {
            if (lcns.size() < 2) continue;

            // The first LCN in the list is the canonical (master) cluster.
            LONGLONG canonical_lcn = lcns[0];
            auto canonical_it = scan.lcn_index.find(canonical_lcn);
            if (canonical_it == scan.lcn_index.end() || canonical_it->second.empty()) continue;

            const auto& canonical_entry = canonical_it->second[0];

            for (size_t i = 1; i < lcns.size(); ++i) {
                LONGLONG dup_lcn = lcns[i];
                auto dup_it = scan.lcn_index.find(dup_lcn);
                if (dup_it == scan.lcn_index.end() || dup_it->second.empty()) continue;

                // Skip if already the same physical cluster (shouldn't happen, but be safe)
                if (dup_lcn == canonical_lcn) continue;

                const auto& dup_entry = dup_it->second[0];

                DedupCandidate cand;
                cand.canonical_path   = scan.resolve_path(canonical_entry.file_index);
                cand.canonical_offset = canonical_entry.file_offset;
                cand.duplicate_path   = scan.resolve_path(dup_entry.file_index);
                cand.duplicate_offset = dup_entry.file_offset;
                cand.cluster_size     = context.cluster_size;
                candidates.push_back(std::move(cand));
            }
        }

        return candidates;
    }
};

// ============================================================================
// Strategy: Pair-Wise Deduplication
// ============================================================================

/**
 * @brief For exactly two files, identifies clusters in file B whose content
 *        matches a cluster in file A, and builds candidates to replace them.
 */
struct PairwiseDedupStrategy : IDedupStrategy {
    std::expected<std::vector<DedupCandidate>, std::wstring> build_candidates(
        const inspect::ScanResult& scan,
        DedupContext& context
    ) override {
        std::vector<DedupCandidate> candidates;

        // Partition the LCN index by file: find two distinct file paths.
        std::unordered_map<std::wstring, std::vector<std::pair<LONGLONG, ULONGLONG>>> file_lcns;
        for (const auto& [lcn, entries] : scan.lcn_index) {
            for (const auto& entry : entries) {
                file_lcns[scan.resolve_path(entry.file_index)].emplace_back(lcn, entry.file_offset);
            }
        }

        if (file_lcns.size() != 2) {
            return std::unexpected(L"Pair-wise dedup requires exactly 2 files in the scan.");
        }

        auto it = file_lcns.begin();
        const std::wstring& path_a = it->first;
        ++it;
        const std::wstring& path_b = it->first;

        // For each hash group with exactly 2 LCNs (one from each file), create a candidate.
        for (const auto& [digest, lcns] : scan.hash_index) {
            if (lcns.size() != 2) continue;

            LONGLONG lcn0 = lcns[0];
            LONGLONG lcn1 = lcns[1];

            auto it0 = scan.lcn_index.find(lcn0);
            auto it1 = scan.lcn_index.find(lcn1);
            if (it0 == scan.lcn_index.end() || it0->second.empty()) continue;
            if (it1 == scan.lcn_index.end() || it1->second.empty()) continue;

            const auto& entry0 = it0->second[0];
            const auto& entry1 = it1->second[0];

            // Determine which entry belongs to which file
            const inspect::BlockEntry* from = nullptr;
            const inspect::BlockEntry* to   = nullptr;

            if (scan.resolve_path(entry0.file_index) == path_a && scan.resolve_path(entry1.file_index) == path_b) {
                from = &entry0; to = &entry1;
            } else if (scan.resolve_path(entry0.file_index) == path_b && scan.resolve_path(entry1.file_index) == path_a) {
                from = &entry1; to = &entry0;
            } else {
                continue; // Both LCNs in the same file - skip
            }

            DedupCandidate cand;
            cand.canonical_path   = scan.resolve_path(from->file_index);
            cand.canonical_offset = from->file_offset;
            cand.duplicate_path   = scan.resolve_path(to->file_index);
            cand.duplicate_offset = to->file_offset;
            cand.cluster_size     = context.cluster_size;
            candidates.push_back(std::move(cand));
        }

        return candidates;
    }
};

// ============================================================================
// RAII Handle Helper
// ============================================================================

/// @brief Minimal RAII handle wrapper for dedup file handles.
struct DedupHandle {
    HANDLE handle = INVALID_HANDLE_VALUE;
    explicit DedupHandle(HANDLE h = INVALID_HANDLE_VALUE) : handle(h) {}
    ~DedupHandle() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    DedupHandle(const DedupHandle&) = delete;
    DedupHandle& operator=(const DedupHandle&) = delete;
    HANDLE get() const { return handle; }
    explicit operator bool() const { return handle != INVALID_HANDLE_VALUE; }
};

// ============================================================================
// Cluster-Level Dedup Operation
// ============================================================================

/**
 * @brief Issues FSCTL_DUPLICATE_EXTENTS_TO_FILE to replace one cluster with a clone.
 *
 * @param dest_handle     Write handle to the file receiving the clone.
 * @param source_handle   Read handle to the file supplying the canonical data.
 * @param src_offset      Byte offset in the source file.
 * @param dest_offset     Byte offset in the destination file.
 * @param byte_count      Number of bytes (one cluster).
 * @return true on success, or error message on failure.
 */
std::expected<bool, std::wstring> dedup_cluster(
    HANDLE dest_handle, HANDLE source_handle,
    ULONGLONG src_offset, ULONGLONG dest_offset, ULONGLONG byte_count
) {
    DUPLICATE_EXTENTS_DATA dup;
    dup.FileHandle              = source_handle;
    dup.SourceFileOffset.QuadPart = src_offset;
    dup.TargetFileOffset.QuadPart = dest_offset;
    dup.ByteCount.QuadPart       = byte_count;

    DWORD returned = 0;
    if (!DeviceIoControl(dest_handle, FSCTL_DUPLICATE_EXTENTS_TO_FILE,
                         &dup, sizeof(dup), NULL, 0, &returned, NULL)) {
        return std::unexpected(L"FSCTL_DUPLICATE_EXTENTS_TO_FILE failed: " +
                               util::get_win32_error_message(GetLastError()));
    }
    return true;
}

// ============================================================================
// Phase 1: Inspect and Prepare
// ============================================================================

/**
 * @brief Validates arguments, runs the LCN/hash scan, and populates DedupContext.
 *
 * @param args    Parsed CLI arguments.
 * @param context Empty context to populate.
 * @param strategy Output: selected strategy instance.
 * @return true on success, or error string on failure.
 */
std::expected<bool, std::wstring> inspect_and_prepare(
    const util::CliArg& args,
    DedupContext& context,
    std::unique_ptr<IDedupStrategy>& strategy
) {
    if (args.positional.empty() || args.positional.size() > 2) {
        return std::unexpected(
            L"Usage: retool dedup <volume-root> | <file1> <file2> [--dry-run]");
    }

    const std::wstring& first_arg = args.positional[0];

    // Determine mode: volume-wide vs pair-wise
    auto is_vol_root = [](const std::wstring& p) {
        if (p.size() >= 2 && p[1] == L':') {
            if (p.size() == 2) return true;
            if (p.size() == 3 && (p[2] == L'\\' || p[2] == L'/')) return true;
        }
        if (p.size() >= 11 && p.substr(0, 11) == L"\\\\?\\Volume") return true;
        return false;
    };

    if (args.positional.size() == 1 && is_vol_root(first_arg)) {
        context.is_volume_wide = true;
        context.volume_root    = first_arg;
        strategy               = std::make_unique<VolumeWideDedupStrategy>();
    } else if (args.positional.size() == 2) {
        context.is_volume_wide = false;
        // Use the volume root of file 1 for the scan
        wchar_t vol[MAX_PATH];
        if (!GetVolumePathNameW(first_arg.c_str(), vol, MAX_PATH)) {
            return std::unexpected(L"Failed to get volume root: " +
                                   util::get_win32_error_message(GetLastError()));
        }
        context.volume_root = vol;
        strategy            = std::make_unique<PairwiseDedupStrategy>();
    } else {
        return std::unexpected(L"Provide a volume root (e.g. E:\\) or two file paths.");
    }

    // Verify destination is ReFS
    wchar_t fs_name[MAX_PATH] = {0};
    if (!GetVolumeInformationW(context.volume_root.c_str(),
                               NULL, 0, NULL, NULL, NULL,
                               fs_name, MAX_PATH)) {
        return std::unexpected(L"Failed to query volume filesystem: " +
                               util::get_win32_error_message(GetLastError()));
    }
    if (wcscmp(fs_name, L"ReFS") != 0) {
        return std::unexpected(L"Volume " + context.volume_root +
                               L" is not ReFS (filesystem: " +
                               std::wstring(fs_name) + L"). Dedup requires ReFS.");
    }

    // Get cluster size
    DWORD spc = 0, bps = 0, fc = 0, tc = 0;
    if (!GetDiskFreeSpaceW(context.volume_root.c_str(), &spc, &bps, &fc, &tc)) {
        return std::unexpected(L"Failed to get cluster size: " +
                               util::get_win32_error_message(GetLastError()));
    }
    context.cluster_size = spc * bps;

    // Run the scan (always kWithHash - we need content matching)
    context.out->message(output::Level::info, L"[dedup.inspect_and_prepare] Scanning " + context.volume_root + L"...");

    auto scan = inspect::build_lcn_index(context.volume_root, inspect::ScanMode::kWithHash, *context.out);
    if (!scan) return std::unexpected(scan.error());

    // For pair-wise mode: verify both files appear in the scan
    if (!context.is_volume_wide) {
        const std::wstring& p1 = args.positional[0];
        const std::wstring& p2 = args.positional[1];
        bool found1 = false, found2 = false;
        for (const auto& [lcn, entries] : scan->lcn_index) {
            for (const auto& e : entries) {
                const auto& ep = scan->resolve_path(e.file_index);
                if (ep == p1) found1 = true;
                if (ep == p2) found2 = true;
            }
            if (found1 && found2) break;
        }
        if (!found1) context.out->message(output::Level::warn, L"File not found in scan results: " + p1);
        if (!found2) context.out->message(output::Level::warn, L"File not found in scan results: " + p2);
    }

    context.scan = std::move(*scan);
    return true;
}

// ============================================================================
// Phase 2: Execute Operation
// ============================================================================

/**
 * @brief Executes all dedup candidates via FSCTL_DUPLICATE_EXTENTS_TO_FILE.
 *
 * @param context   Populated DedupContext with candidates.
 * @param args      CLI arguments (dry_run, strict).
 * @return true on success, or error string on strict failure.
 */
std::expected<bool, std::wstring> execute_operation(
    DedupContext& context,
    const util::CliArg& args
) {
    const auto& candidates = context.candidates;
    const ULONGLONG total  = candidates.size();

    if (total == 0) {
        context.out->message(output::Level::info, L"[dedup.execute_operation] No deduplication candidates found.");
        return true;
    }

    context.out->message(output::Level::info, L"[dedup.execute_operation] Processing " +
                        std::to_wstring(total) + L" dedup candidates...");

    // Cache of open read/write file handles to avoid reopening on every cluster
    std::unordered_map<std::wstring, HANDLE> write_handles;
    std::unordered_map<std::wstring, HANDLE> read_handles;

    auto cleanup_handles = [&]() {
        for (auto& [path, h] : write_handles) if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        for (auto& [path, h] : read_handles)  if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    };

    auto get_write_handle = [&](const std::wstring& path) -> HANDLE {
        auto it = write_handles.find(path);
        if (it != write_handles.end()) return it->second;
        HANDLE h = CreateFileW(path.c_str(),
                               GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ,
                               NULL, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS, NULL);
        write_handles[path] = h;
        return h;
    };

    auto get_read_handle = [&](const std::wstring& path) -> HANDLE {
        auto it = read_handles.find(path);
        if (it != read_handles.end()) return it->second;
        HANDLE h = CreateFileW(path.c_str(),
                               GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS, NULL);
        read_handles[path] = h;
        return h;
    };

    ULONGLONG processed = 0;
    for (const auto& cand : candidates) {
        if (copy::g_cancel_requested) {
            cleanup_handles();
            return std::unexpected(L"Dedup cancelled by user.");
        }

        processed++;
        context.out->progress(cand.duplicate_path, processed, total);

        if (args.dry_run) {
            context.stats.clusters_deduped++;
            context.stats.bytes_reclaimed += cand.cluster_size;
            continue;
        }

        HANDLE src = get_read_handle(cand.canonical_path);
        if (src == INVALID_HANDLE_VALUE) {
            std::wstring err = L"Failed to open canonical file: " + cand.canonical_path;
            context.stats.errors.push_back(err);
            if (args.strict) { cleanup_handles(); return std::unexpected(err); }
            continue;
        }

        HANDLE dst = get_write_handle(cand.duplicate_path);
        if (dst == INVALID_HANDLE_VALUE) {
            std::wstring err = L"Failed to open duplicate file for writing: " + cand.duplicate_path;
            context.stats.errors.push_back(err);
            if (args.strict) { cleanup_handles(); return std::unexpected(err); }
            continue;
        }

        auto result = dedup_cluster(dst, src,
                                    cand.canonical_offset, cand.duplicate_offset,
                                    cand.cluster_size);
        if (!result) {
            context.stats.errors.push_back(cand.duplicate_path + L": " + result.error());
            if (args.strict) { cleanup_handles(); return std::unexpected(result.error()); }
            continue;
        }

        context.stats.clusters_deduped++;
        context.stats.bytes_reclaimed += cand.cluster_size;
    }

    // Track files processed
    std::unordered_map<std::wstring, bool> seen_files;
    for (const auto& cand : candidates) {
        seen_files[cand.duplicate_path] = true;
    }
    context.stats.files_processed = seen_files.size();

    cleanup_handles();
    return true;
}

// ============================================================================
// Phase 3: Finalize and Report
// ============================================================================

/**
 * @brief Emits a summary of deduplication results.
 *
 * @param context  Completed DedupContext.
 * @param dry_run  If true, labels the output as a simulation.
 * @return Exit code: 0 on success, 2 if any errors occurred.
 */
int finalize_and_report(const DedupContext& context, bool dry_run) {
    auto& out = *context.out;
    const auto& stats = context.stats;

    std::wstring section_name = dry_run
        ? L"retool dedup summary (dry-run)"
        : L"retool dedup summary";

    out.begin_section(section_name);
    out.field(L"Files Processed",    std::to_wstring(stats.files_processed));
    out.field(L"Clusters Deduped",   std::to_wstring(stats.clusters_deduped));
    out.field(L"Space Reclaimed", util::format_size(stats.bytes_reclaimed) +
              L" (" + std::to_wstring(stats.bytes_reclaimed) + L" bytes)");

    for (const auto& err : stats.errors) {
        out.message(output::Level::error, err);
    }

    out.end_section();

    return stats.errors.empty() ? 0 : 2;
}

// ============================================================================
// Entry Point
// ============================================================================

std::expected<int, std::wstring> execute_dedup(const util::CliArg& args, output::IOutput& out) {
    DedupContext context;
    context.out = &out;

    std::unique_ptr<IDedupStrategy> strategy;

    // Phase 1: Inspection
    auto prepare_ok = inspect_and_prepare(args, context, strategy);
    if (!prepare_ok) return std::unexpected(prepare_ok.error());

    // Build candidates
    auto candidates = strategy->build_candidates(context.scan, context);
    if (!candidates) return std::unexpected(candidates.error());
    context.candidates = std::move(*candidates);

    out.message(output::Level::info, L"[dedup.execute_dedup] Found " +
               std::to_wstring(context.candidates.size()) + L" dedup candidate clusters.");

    // Phase 2: Operation
    auto op_ok = execute_operation(context, args);
    if (!op_ok && args.strict) return std::unexpected(op_ok.error());

    // Phase 3: Finalization
    return finalize_and_report(context, args.dry_run);
}

} // namespace dedup
