/**
 * @file dedup.cpp
 * @brief In-place file deduplication engine for ReFS volumes.
 *
 * Implements a three-phase pipeline (Inspect → Operate → Finalize).
 * Strategies determine which files to process; the operation is identical
 * regardless of whether the input was a volume, directory, or file pair.
 *
 * Per-file protocol:
 *   1. Count dedup candidates. Skip the file if none exist.
 *   2. Rename origin to "<name>.old".
 *   3. Create a new file at the original path. Fill every cluster via
 *      FSCTL_DUPLICATE_EXTENTS_TO_FILE using the following source priority:
 *        a. Files already rebuilt in this run (maximises chain deduplication).
 *        b. Any other non-origin file with matching content.
 *        c. The origin .old file (for unique or unmatched clusters).
 *   4. On success: delete .old. Mark the file as processed.
 *      On failure: delete the partial new file, restore .old.
 */

#include <algorithm>
#include <expected>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
    ULONGLONG files_rebuilt    = 0;
    ULONGLONG clusters_deduped = 0;
    ULONGLONG bytes_reclaimed  = 0;
    std::vector<std::wstring> errors;
};

/// @brief Plan to rebuild one file. Strategies produce these; the operation is uniform.
struct FileRebuildPlan {
    uint32_t     file_index;       ///< Index into ScanResult::file_table.
    std::wstring original_path;    ///< Path where the rebuilt file will land.
    std::wstring old_path;         ///< Backup during rebuild (original_path + L".old").
    ULONGLONG    file_size;        ///< Exact byte size of the file.
    /// @brief File's clusters sorted ascending by file_offset: {file_offset, lcn}.
    std::vector<std::pair<ULONGLONG, LONGLONG>> ordered_clusters;
};

/// @brief Forward declaration so IDedupStrategy can reference DedupContext by reference.
struct DedupContext;

// ============================================================================
// Strategy Interface
// ============================================================================

/// @brief Abstract interface for determining which files to rebuild.
struct IDedupStrategy {
    virtual ~IDedupStrategy() = default;

    /**
     * @brief Returns the list of files to rebuild from the scan result.
     *
     * Strategies are responsible only for selection and ordering of files;
     * the rebuild operation itself is uniform across all modes.
     *
     * @param context  Shared context with scan data, precomputed maps, and target_paths.
     * @return Vector of FileRebuildPlan on success, or error string on failure.
     */
    virtual std::expected<std::vector<FileRebuildPlan>, std::wstring>
        build_plans(DedupContext& context) = 0;
};

/// @brief Shared state threaded through the dedup pipeline.
struct DedupContext {
    // Phase 1: set during prepare()
    std::wstring              volume_root;
    DWORD                     cluster_size   = 0;
    bool                      is_volume_wide = false;
    inspect::ScanResult       scan;
    output::IOutput*          out            = nullptr;
    std::vector<std::wstring> target_paths;  ///< Pair-wise: [fileRef, fileOp].
    std::unique_ptr<IDedupStrategy> strategy;
    util::CliArg              args_snapshot;

    // Precomputed from scan by execute().
    std::unordered_map<LONGLONG, std::string>                                  lcn_hash;
    std::unordered_map<uint32_t, std::vector<std::pair<ULONGLONG, LONGLONG>>>  file_clusters;

    // Phase 2: populated by strategy; updated during operation.
    std::vector<FileRebuildPlan>     plans;
    std::unordered_set<std::wstring> processed; ///< Paths of files successfully rebuilt so far.
    DedupStats stats;

    DedupContext() = default;
    DedupContext(const DedupContext&) = delete;
    DedupContext& operator=(const DedupContext&) = delete;
};

// ============================================================================
// Plan Building Helper
// ============================================================================

/**
 * @brief Builds a FileRebuildPlan for a single file.
 *
 * Retrieves the exact file size and populates ordered_clusters from
 * context.file_clusters. Source selection is deferred to rebuild_file().
 *
 * @param file_index  Index into ScanResult::file_table.
 * @param path        Absolute path of the file to rebuild.
 * @param context     DedupContext with precomputed file_clusters.
 * @return Populated FileRebuildPlan, or error string on failure.
 */
static std::expected<FileRebuildPlan, std::wstring>
build_file_plan(uint32_t file_index, const std::wstring& path, const DedupContext& context) {
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr)) {
        return std::unexpected(L"Failed to get file attributes for: " + path +
                               L" - " + util::get_win32_error_message(GetLastError()));
    }

    auto cluster_it = context.file_clusters.find(file_index);
    if (cluster_it == context.file_clusters.end()) {
        return std::unexpected(L"No cluster data found for: " + path);
    }

    FileRebuildPlan plan;
    plan.file_index       = file_index;
    plan.original_path    = path;
    plan.old_path         = path + L".old";
    plan.file_size        = (static_cast<ULONGLONG>(attr.nFileSizeHigh) << 32) | attr.nFileSizeLow;
    plan.ordered_clusters = cluster_it->second; // Already sorted ascending by file_offset.
    return plan;
}

// ============================================================================
// Strategy: Volume-Wide Deduplication
// ============================================================================

/**
 * @brief Selects every file on the volume that has at least one cluster whose
 *        content hash matches a cluster in a different file.
 */
struct VolumeWideDedupStrategy : IDedupStrategy {
    std::expected<std::vector<FileRebuildPlan>, std::wstring>
    build_plans(DedupContext& context) override {
        const auto& scan = context.scan;

        // Identify files that have at least one hash-duplicate in a different file.
        std::unordered_set<uint32_t> needs_rebuild;
        for (const auto& [digest, lcns] : scan.hash_index) {
            if (lcns.size() < 2) continue;
            for (size_t i = 0; i < lcns.size(); ++i) {
                const auto* entry = inspect::lcn_interval_find(scan.lcn_index, lcns[i]);
                if (!entry) continue;
                uint32_t fi = entry->file_index;
                if (needs_rebuild.count(fi)) continue;
                for (size_t j = 0; j < lcns.size(); ++j) {
                    if (j == i) continue;
                    const auto* other = inspect::lcn_interval_find(scan.lcn_index, lcns[j]);
                    if (!other) continue;
                    if (other->file_index != fi) {
                        needs_rebuild.insert(fi);
                        break;
                    }
                }
            }
        }

        std::vector<FileRebuildPlan> plans;
        for (uint32_t fi : needs_rebuild) {
            const std::wstring& path = scan.resolve_path(fi);
            auto plan_res = build_file_plan(fi, path, context);
            if (!plan_res) {
                context.out->message(output::Level::warn,
                    L"Could not build dedup plan for " + path + L": " + plan_res.error());
                continue;
            }
            plans.push_back(std::move(*plan_res));
        }
        return plans;
    }
};

// ============================================================================
// Strategy: Pair-Wise Deduplication
// ============================================================================

/**
 * @brief Pair-wise deduplication of two files on the same volume.
 *
 * Roles:
 *   fileRef (target_paths[0]) - reference file: never modified; serves only as
 *                               a cluster source for matching content.
 *   fileOp  (target_paths[1]) - operand file:   the file that is rebuilt.
 *                               It is renamed to "<name>.old", a new file is
 *                               constructed at the original path (cloning as
 *                               many clusters as possible from fileRef and other
 *                               already-processed files), and the .old copy is
 *                               deleted on success.
 */
struct PairwiseDedupStrategy : IDedupStrategy {
    std::expected<std::vector<FileRebuildPlan>, std::wstring>
    build_plans(DedupContext& context) override {
        if (context.target_paths.size() != 2) {
            return std::unexpected(L"Pair-wise dedup requires exactly 2 file paths.");
        }

        const std::wstring& origin_path = context.target_paths[1];

        uint32_t origin_fi = UINT32_MAX;
        for (uint32_t i = 0;
             i < static_cast<uint32_t>(context.scan.file_table.size()); ++i) {
            if (context.scan.file_table[i] == origin_path) { origin_fi = i; break; }
        }
        if (origin_fi == UINT32_MAX) {
            return std::unexpected(L"Origin file not found in scan: " + origin_path);
        }

        auto plan_res = build_file_plan(origin_fi, origin_path, context);
        if (!plan_res) return std::unexpected(plan_res.error());

        std::vector<FileRebuildPlan> plans;
        plans.push_back(std::move(*plan_res));
        return plans;
    }
};

// ============================================================================
// Cluster-Level Operation
// ============================================================================

/// @brief Sets a file's logical size - thin wrapper over util::set_file_eof for call-site clarity.
static std::expected<bool, std::wstring> set_file_size(HANDLE handle, ULONGLONG size) {
    return util::set_file_eof(handle, size);
}

// ============================================================================
// Dynamic Source Selection
// ============================================================================

/// @brief Result of selecting a clone source for one cluster.
struct SourceInfo {
    std::wstring source_path;   ///< File to clone from.
    ULONGLONG    source_offset; ///< Byte offset within source_path.
    bool         is_dedup;      ///< true = not sourced from origin .old (reclaims space).
};

/**
 * @brief Selects the best available source for one cluster of the origin file.
 *
 * Priority order:
 *   1. Files already rebuilt in this run (same content, already optimally shared).
 *   2. Any other non-origin file with matching content hash.
 *   3. The origin's .old file (unique content, no space savings).
 *
 * For clusters whose LCN is already shared with another file (same physical block),
 * no other LCN appears in hash_index for that hash, so the fallback .old is used —
 * correctly reflecting zero space savings for already-deduped clusters.
 *
 * @param lcn           Physical cluster number of this cluster in the origin.
 * @param origin_fi     file_index of the origin file being rebuilt.
 * @param fallback_path Path to the origin's .old file.
 * @param fallback_off  Byte offset in the .old file (same as dest_offset).
 * @param scan          Scan result.
 * @param lcn_hash      Precomputed LCN → digest reverse map.
 * @param processed     Paths of files successfully rebuilt so far in this run.
 * @return SourceInfo describing where to clone this cluster from.
 */
static SourceInfo select_source(
    LONGLONG                                               lcn,
    uint32_t                                               origin_fi,
    const std::wstring&                                    fallback_path,
    ULONGLONG                                              fallback_off,
    const inspect::ScanResult&                             scan,
    const std::unordered_map<LONGLONG, std::string>&       lcn_hash,
    const std::unordered_set<std::wstring>&                processed,
    DWORD                                                  cluster_size
) {
    auto hash_it = lcn_hash.find(lcn);
    if (hash_it != lcn_hash.end()) {
        const std::string& digest = hash_it->second;
        auto group_it = scan.hash_index.find(digest);
        if (group_it != scan.hash_index.end()) {
            struct Candidate {
                std::wstring path;
                ULONGLONG    offset;
                bool         is_processed;
            };
            std::vector<Candidate> processed_cands;
            std::vector<Candidate> other_cands;

            for (LONGLONG other_lcn : group_it->second) {
                if (other_lcn == lcn) continue; // Skip: same physical block as origin.
                // lcn_interval_find_at() corrects file_offset for other_lcn's exact
                // position within its interval - lcn_interval_find() alone would return
                // the offset at the interval's start_lcn regardless of other_lcn, which
                // is wrong for any cluster past a multi-cluster file's first one.
                auto e = inspect::lcn_interval_find_at(scan.lcn_index, other_lcn, cluster_size);
                if (!e) continue;
                if (e->file_index == origin_fi) continue; // Skip: same file (different cluster).
                const std::wstring& p = scan.resolve_path(e->file_index);
                if (processed.count(p)) processed_cands.push_back({p, e->file_offset, true});
                else                    other_cands.push_back({p, e->file_offset, false});
            }

            // Priority 1: already-rebuilt files.
            if (!processed_cands.empty()) {
                return {processed_cands[0].path, processed_cands[0].offset, true};
            }
            // Priority 2: any other non-origin file.
            if (!other_cands.empty()) {
                return {other_cands[0].path, other_cands[0].offset, true};
            }
        }
    }

    // Priority 3: origin .old (no dedup savings).
    return {fallback_path, fallback_off, false};
}

// ============================================================================
// Phase 1: Prepare
// ============================================================================

/**
 * @brief Validates arguments, runs the LCN/hash scan, and returns a heap-allocated DedupContext.
 */
std::expected<DedupContext*, std::wstring> prepare(const util::CliArg& args) {
    auto ctx = std::make_unique<DedupContext>();
    ctx->args_snapshot = args;

    if (args.file_specs.empty() || args.file_specs.size() > 2) {
        return std::unexpected(
            L"Usage: retool dedup <volume> | <fileRef> <fileOp> [-n]");
    }

    ctx->target_paths.clear();
    for (const auto& s : args.file_specs) ctx->target_paths.push_back(s.path);
    const std::wstring& first = args.file_specs[0].path;

    if (args.file_specs.size() == 1 && args.file_specs[0].kind == util::FileSpecKind::kVolume) {
        ctx->is_volume_wide = true;
        ctx->strategy       = std::make_unique<VolumeWideDedupStrategy>();
    } else if (args.file_specs.size() == 2) {
        ctx->is_volume_wide = false;
        ctx->strategy       = std::make_unique<PairwiseDedupStrategy>();
    } else {
        return std::unexpected(L"Provide a volume specifier (e.g. E:) or two file paths.");
    }

    // Resolve volume root, cluster size, and filesystem name once for this target.
    auto vol_info = util::resolve_volume_info(first);
    if (!vol_info) return std::unexpected(L"Failed to resolve volume: " + vol_info.error());
    ctx->volume_root  = vol_info->volume_root;
    ctx->cluster_size = vol_info->cluster_size;

    // Verify ReFS. An empty fs_name means the filesystem query itself failed
    // (see util::VolumeInfo::fs_name) - treated the same as "not ReFS" here,
    // since dedup requires ReFS either way.
    if (vol_info->fs_name != L"ReFS") {
        return std::unexpected(L"Volume " + ctx->volume_root +
                               L" is not ReFS (filesystem: " +
                               (vol_info->fs_name.empty() ? L"unknown" : vol_info->fs_name) +
                               L"). Dedup requires ReFS.");
    }

    // Output is not yet available in prepare(); defer progress messages to execute().
    // We create a temporary silent outputter to satisfy build_lcn_index_for_files.
    output::QuietOutput silent;

    auto scan = inspect::build_lcn_index_for_files(ctx->volume_root,
                                                    inspect::ScanMode::kWithHash, silent);
    if (!scan) return std::unexpected(scan.error());
    ctx->scan = std::move(*scan);

    // Pair-wise: confirm both files appear in the scan.
    if (!ctx->is_volume_wide) {
        const std::wstring& p1 = args.file_specs[0].path;
        const std::wstring& p2 = args.file_specs[1].path;
        bool found1 = false, found2 = false;
        for (const auto& path : ctx->scan.file_table) {
            if (path == p1) found1 = true;
            if (path == p2) found2 = true;
            if (found1 && found2) break;
        }
        if (!found1) silent.message(output::Level::warn,
            L"File not found in scan results: " + p1);
        if (!found2) silent.message(output::Level::warn,
            L"File not found in scan results: " + p2);
    }

    return ctx.release();
}

// ============================================================================
// Phase 2: Execute Operation
// ============================================================================

/**
 * @brief Rebuilds one file: rename → create → clone-all-clusters → delete.
 *
 * Source selection is performed dynamically per-cluster using select_source(),
 * which consults context.processed to prefer already-rebuilt files.
 *
 * On failure: deletes the partial new file and restores the .old file.
 * On success: marks the file as processed in context.processed.
 *
 * @param plan     Rebuild plan for this file.
 * @param context  Shared context (scan, lcn_hash, processed, stats, out).
 * @param args     CLI arguments (dry_run, strict).
 * @return true on success, error string on failure.
 */
static std::expected<bool, std::wstring> rebuild_file(
    const FileRebuildPlan& plan,
    DedupContext&          context,
    const util::CliArg&    args
) {
    auto& out = *context.out;
    const ULONGLONG cs = context.cluster_size;

    // -- Pre-check: count dedup candidates before touching the filesystem ---------
    ULONGLONG dedup_clusters = 0;
    for (const auto& [file_offset, lcn] : plan.ordered_clusters) {
        auto src = select_source(lcn, plan.file_index, plan.old_path, file_offset,
                                  context.scan, context.lcn_hash, context.processed,
                                  context.cluster_size);
        if (src.is_dedup) dedup_clusters++;
    }

    if (dedup_clusters == 0) {
        out.message(output::Level::info,
            L"[dedup.rebuild_file] Skipping (no dedup candidates): " + plan.original_path);
        return true;
    }

    // -- Dry-run: accumulate stats without modifying the filesystem ---------------
    if (args.dry_run) {
        context.stats.clusters_deduped += dedup_clusters;
        context.stats.bytes_reclaimed  += dedup_clusters * cs;
        context.stats.files_rebuilt++;
        // Simulate the file as processed so subsequent dry-run files use it as Priority 1.
        context.processed.insert(plan.original_path);
        return true;
    }

    // -- Step 1: Rename origin → .old --------------------------------------------
    if (!MoveFileW(plan.original_path.c_str(), plan.old_path.c_str())) {
        return std::unexpected(
            L"Failed to rename " + plan.original_path + L" to " + plan.old_path +
            L": " + util::get_win32_error_message(GetLastError()));
    }

    auto rollback = [&](util::ScopedHandle* new_h = nullptr) {
        if (new_h) new_h->close();
        DeleteFileW(plan.original_path.c_str());
        if (!MoveFileW(plan.old_path.c_str(), plan.original_path.c_str())) {
            out.message(output::Level::error,
                L"CRITICAL: Failed to restore " + plan.old_path +
                L" -> " + plan.original_path + L". Manual recovery required.");
        }
    };

    // -- Step 2: Create new file at original path --------------------------------
    util::ScopedHandle new_h(CreateFileW(
        plan.original_path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0, NULL, CREATE_NEW,
        FILE_FLAG_BACKUP_SEMANTICS, NULL));
    if (!new_h) {
        std::wstring err = L"Failed to create " + plan.original_path + L": " +
                           util::get_win32_error_message(GetLastError());
        rollback();
        return std::unexpected(err);
    }

    // -- Step 3: Pre-size to cluster boundary (trimmed after cloning) ------------
    ULONGLONG full_size = ((plan.file_size + cs - 1) / cs) * cs;
    if (auto r = set_file_size(new_h.get(), full_size); !r) {
        std::wstring err = L"Failed to pre-size " + plan.original_path + L": " + r.error();
        rollback(&new_h);
        return std::unexpected(err);
    }

    // -- Step 4: Open .old for read (Priority 3 source) --------------------------
    util::ScopedHandle old_h(CreateFileW(
        plan.old_path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, NULL));
    if (!old_h) {
        std::wstring err = L"Failed to open backup " + plan.old_path + L": " +
                           util::get_win32_error_message(GetLastError());
        rollback(&new_h);
        return std::unexpected(err);
    }

    // -- Step 5: Clone all clusters into the new file ----------------------------
    std::unordered_map<std::wstring, HANDLE> read_cache;
    auto close_cache = [&]() {
        for (auto& [p, h] : read_cache) if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        read_cache.clear();
    };
    auto get_handle = [&](const std::wstring& path) -> HANDLE {
        if (path == plan.old_path) return old_h.get();
        auto it = read_cache.find(path);
        if (it != read_cache.end()) return it->second;
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS, NULL);
        read_cache[path] = h;
        return h;
    };

    const ULONGLONG total = plan.ordered_clusters.size();
    ULONGLONG processed_n = 0;
    bool      failed      = false;
    std::wstring fail_err;

    for (const auto& [file_offset, lcn] : plan.ordered_clusters) {
        if (util::g_cancel_requested) {
            fail_err = L"Dedup cancelled by user.";
            failed   = true;
            break;
        }

        processed_n++;
        out.progress(plan.original_path, processed_n, total);

        auto src = select_source(lcn, plan.file_index, plan.old_path, file_offset,
                                  context.scan, context.lcn_hash, context.processed,
                                  context.cluster_size);

        HANDLE src_h = get_handle(src.source_path);
        if (src_h == INVALID_HANDLE_VALUE) {
            fail_err = L"Failed to open source file " + src.source_path + L": " +
                       util::get_win32_error_message(GetLastError());
            failed = true;
            break;
        }

        auto res = util::duplicate_extents(new_h.get(), src_h,
                                            src.source_offset, file_offset, cs);
        if (!res) {
            fail_err = plan.original_path + L" (offset " +
                       std::to_wstring(file_offset) + L"): " + res.error();
            failed = true;
            break;
        }
    }

    close_cache();

    if (failed) {
        rollback(&new_h);
        old_h.close();
        return std::unexpected(fail_err);
    }

    // -- Step 6: Trim to exact file size -----------------------------------------
    if (auto r = set_file_size(new_h.get(), plan.file_size); !r) {
        std::wstring err = L"Failed to trim " + plan.original_path + L": " + r.error();
        rollback(&new_h);
        old_h.close();
        return std::unexpected(err);
    }

    new_h.close();
    old_h.close();

    // -- Step 7: Delete .old -----------------------------------------------------
    if (!DeleteFileW(plan.old_path.c_str())) {
        out.message(output::Level::warn,
            L"Rebuild succeeded but failed to delete backup: " + plan.old_path +
            L" - " + util::get_win32_error_message(GetLastError()));
    }

    context.stats.clusters_deduped += dedup_clusters;
    context.stats.bytes_reclaimed  += dedup_clusters * cs;
    context.stats.files_rebuilt++;
    context.processed.insert(plan.original_path); // Available as Priority 1 for subsequent files.
    return true;
}

/**
 * @brief Processes each FileRebuildPlan sequentially via rebuild_file().
 *
 * @param context  Populated DedupContext with plans.
 * @param args     CLI arguments (dry_run, strict).
 * @return true on success, error string on strict failure.
 */
static std::expected<bool, std::wstring> execute_operation(
    DedupContext&       context,
    const util::CliArg& args
) {
    if (context.plans.empty()) {
        context.out->message(output::Level::info,
            L"[dedup.execute_operation] No files with duplicate clusters found.");
        return true;
    }

    context.out->message(output::Level::info,
        L"[dedup.execute_operation] Processing " +
        std::to_wstring(context.plans.size()) + L" file(s)...");

    for (const auto& plan : context.plans) {
        if (util::g_cancel_requested) {
            return std::unexpected(L"Dedup cancelled by user.");
        }
        auto res = rebuild_file(plan, context, args);
        if (!res) {
            context.stats.errors.push_back(res.error());
            if (args.strict) return std::unexpected(res.error());
        }
    }

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
static int finalize_and_report(const DedupContext& context, bool dry_run) {
    auto& out        = *context.out;
    const auto& stats = context.stats;

    std::wstring section_name = dry_run
        ? L"retool dedup summary (dry-run)"
        : L"retool dedup summary";

    out.begin_section(section_name);
    out.field(L"Files Processed",  std::to_wstring(stats.files_rebuilt));
    out.field(L"Clusters Deduped", std::to_wstring(stats.clusters_deduped));
    out.field(L"Space Reclaimed",  util::format_size_detailed(stats.bytes_reclaimed));
    for (const auto& err : stats.errors) {
        out.message(output::Level::error, err);
    }
    out.end_section();
    return stats.errors.empty() ? 0 : 2;
}

// ============================================================================
// Phase 2: Execute
// ============================================================================

std::expected<int, std::wstring> execute(DedupContext& ctx, output::IOutput& out) {
    ctx.out = &out;

    out.message(output::Level::info,
        L"[dedup.execute] Scanning " + ctx.volume_root + L"...");

    // Precompute auxiliary maps shared by all strategies and rebuild_file().
    // file_clusters expands the interval index back to one entry per cluster:
    // rebuild_file() clones cluster-by-cluster regardless of how the index is
    // stored, so this reconstruction is unavoidable here (same complexity the
    // original per-cluster index carried - no regression, just deferred).
    for (const auto& [digest, lcns] : ctx.scan.hash_index) {
        for (LONGLONG lcn : lcns) ctx.lcn_hash[lcn] = digest;
    }
    for (const auto& iv : ctx.scan.lcn_index) {
        for (const auto& owner : iv.owners) {
            for (LONGLONG lcn = iv.start_lcn; lcn < iv.end_lcn; ++lcn) {
                ULONGLONG offset = owner.file_offset +
                    static_cast<ULONGLONG>(lcn - iv.start_lcn) * ctx.cluster_size;
                ctx.file_clusters[owner.file_index].emplace_back(offset, lcn);
            }
        }
    }
    for (auto& [fi, clusters] : ctx.file_clusters) {
        std::sort(clusters.begin(), clusters.end());
    }

    // Build per-file rebuild plans.
    auto plans = ctx.strategy->build_plans(ctx);
    if (!plans) return std::unexpected(plans.error());
    ctx.plans = std::move(*plans);

    if (ctx.plans.empty()) {
        out.message(output::Level::info,
            L"[dedup.execute] No duplicate clusters found. Nothing to do.");
        return finalize_and_report(ctx, ctx.args_snapshot.dry_run);
    }

    out.message(output::Level::info,
        L"[dedup.execute] " + std::to_wstring(ctx.plans.size()) +
        L" file(s) identified for rebuild.");

    // Phase 2: Operation.
    auto op_ok = execute_operation(ctx, ctx.args_snapshot);
    if (!op_ok && ctx.args_snapshot.strict) return std::unexpected(op_ok.error());

    // Phase 3: Finalization.
    return finalize_and_report(ctx, ctx.args_snapshot.dry_run);
}

// ============================================================================
// Phase 3: Cleanup
// ============================================================================

void cleanup(DedupContext* ctx) noexcept {
    delete ctx;
}

} // namespace dedup
