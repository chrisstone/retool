#include <algorithm>
#include <cwctype>
#include <expected>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <aclapi.h>
#include <windows.h>

#pragma comment(lib, "Advapi32.lib")

#include "copy.h"
#include "inspect.h"
#include "output.h"
#include "util.h"

namespace copy {


// ============================================================================
// Data Structures
// ============================================================================

/// @brief A single source→destination file mapping produced during the gather phase.
struct FilePair {
    std::wstring src;
    std::wstring dest;
};

/// @brief Accumulated statistics for a copy operation.
struct CopyStats {
    ULONGLONG total_files    = 0;
    ULONGLONG cloned_files   = 0;
    ULONGLONG fallback_files = 0;
    ULONGLONG skipped_files  = 0;
    ULONGLONG total_bytes    = 0;
    std::vector<std::wstring> errors;
};

/// @brief Win32 HANDLE RAII wrapper - shared definition, see util::ScopedHandle.
using ScopedHandle = util::ScopedHandle;

/// @brief LRU cache for previously-opened destination file handles.
///
/// Caches up to kMaxSlots open HANDLEs, evicting the least-recently-used
/// on capacity overflow. Dramatically reduces CreateFileW/CloseHandle
/// syscall thrashing when cross-volume dedup references many distinct files.
struct HandleLruCache {
    static constexpr size_t kMaxSlots = 16;

    HandleLruCache() = default;
    ~HandleLruCache() { close_all(); }

    HandleLruCache(const HandleLruCache&) = delete;
    HandleLruCache& operator=(const HandleLruCache&) = delete;
    HandleLruCache(HandleLruCache&&) = default;
    HandleLruCache& operator=(HandleLruCache&&) = default;

    void close_all() {
        for (auto& [path, handle] : cache_) {
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        }
        cache_.clear();
        lru_.clear();
    }

    HANDLE get(const std::wstring& filepath) {
        auto it = cache_.find(filepath);
        if (it != cache_.end()) {
            // Move to front of LRU
            lru_.remove(filepath);
            lru_.push_front(filepath);
            return it->second;
        }

        // Evict if at capacity
        if (cache_.size() >= kMaxSlots) {
            const auto& victim = lru_.back();
            auto v_it = cache_.find(victim);
            if (v_it != cache_.end()) {
                if (v_it->second != INVALID_HANDLE_VALUE) CloseHandle(v_it->second);
                cache_.erase(v_it);
            }
            lru_.pop_back();
        }

        HANDLE h = CreateFileW(
            filepath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS,
            NULL
        );
        cache_[filepath] = h;
        lru_.push_front(filepath);
        return h;
    }

private:
    std::unordered_map<std::wstring, HANDLE> cache_;
    std::list<std::wstring> lru_;
};

// Forward declarations
struct ICopyStrategy;

/**
 * @brief Incrementally-built source-LCN -> destination-location map for live copy execution.
 *
 * Unlike inspect::LcnIntervalIndex (built once via a batch sweep over a complete
 * claim list gathered up front), this supports insertion one physically-contiguous
 * run at a time as the copy progresses. Every run passed to insert_run() was just
 * written or hash-matched-and-cloned to arithmetically contiguous destination byte
 * offsets within a single destination file - the caller guarantees that per call.
 * This class never infers or assumes destination contiguity across separate
 * insert_run() calls, even when their source LCN ranges are numerically adjacent:
 * two source LCNs that are contiguous may have been recorded by two unrelated
 * earlier insertions (different files, unrelated destination positions), and a
 * lookup here only ever returns what a single insert_run() call actually claimed.
 */
class CopySourceLcnMap {
public:
    /// @brief Records [src_lcn_start, src_lcn_start + run_length) mapping into
    /// dest_path_index, starting at dest_offset_start at src_lcn_start.
    void insert_run(LONGLONG src_lcn_start, ULONGLONG run_length,
                     uint32_t dest_path_index, ULONGLONG dest_offset_start) {
        if (run_length == 0) return;
        runs_[src_lcn_start] = {src_lcn_start + static_cast<LONGLONG>(run_length),
                                dest_path_index, dest_offset_start};
    }

    /// @brief Destination location for src_lcn, and how many further clusters
    /// (starting at src_lcn) stay within that same previously-inserted run.
    struct Lookup {
        uint32_t  dest_path_index;
        ULONGLONG dest_offset;             ///< Destination byte offset AT src_lcn.
        ULONGLONG run_clusters_remaining;  ///< Clusters from src_lcn through this run's end.
    };

    /// @brief Looks up src_lcn. cluster_size is needed to compute the offset
    /// within the run (this class stores no cluster size of its own).
    std::optional<Lookup> find(LONGLONG src_lcn, DWORD cluster_size) const {
        auto it = runs_.upper_bound(src_lcn);
        if (it == runs_.begin()) return std::nullopt;
        --it;
        if (src_lcn >= it->second.end_lcn) return std::nullopt;

        ULONGLONG delta = static_cast<ULONGLONG>(src_lcn - it->first);
        Lookup result;
        result.dest_path_index        = it->second.dest_path_index;
        result.dest_offset            = it->second.dest_offset_start + delta * cluster_size;
        result.run_clusters_remaining = static_cast<ULONGLONG>(it->second.end_lcn - src_lcn);
        return result;
    }

    /// @brief Cheaper existence check when the destination location isn't needed yet.
    bool contains(LONGLONG src_lcn) const {
        auto it = runs_.upper_bound(src_lcn);
        if (it == runs_.begin()) return false;
        --it;
        return src_lcn < it->second.end_lcn;
    }

private:
    struct Run {
        LONGLONG  end_lcn;
        uint32_t  dest_path_index;
        ULONGLONG dest_offset_start;
    };
    std::map<LONGLONG, Run> runs_;
};

/// @brief Shared state for the entire copy pipeline.
struct CopyContext {
    // -- Populated during Phase 1: prepare() --
    std::wstring src_path;              ///< Resolved absolute source path.
    std::wstring dest_path;             ///< Resolved absolute destination path.
    bool is_directory = false;          ///< True if source is a directory.

    std::wstring src_volume_root;       ///< Source volume root (e.g., L"E:\\").
    std::wstring dest_volume_root;      ///< Destination volume root (e.g., L"F:\\").
    bool same_volume    = false;        ///< True if src and dest are on the same volume.
    bool src_is_refs    = false;        ///< True if source volume is ReFS.
    bool dest_is_refs   = false;        ///< True if destination is ReFS.
    DWORD src_cluster_size  = 0;        ///< Source volume cluster size in bytes.
    DWORD dest_cluster_size = 0;        ///< Destination volume cluster size in bytes.

    std::unique_ptr<ICopyStrategy> strategy;  ///< Selected copy strategy.
    output::IOutput* out = nullptr;           ///< Non-owning pointer to the output interface.

    // -- Populated during Phase 2: gather() --
    std::vector<FilePair> file_pairs;           ///< All source→destination file mappings.
    ULONGLONG total_unique_src_lcns = 0;        ///< Unique source LCNs (overall progress total).

    /// Interned destination paths: maps index -> path string. Avoids storing
    /// a full wstring copy per lcn_map entry (saves ~5 GB at 31M entries).
    /// Reserved in gather() to avoid rehashing during execute().
    std::vector<std::wstring> dest_path_table;

    /// Incremental interval map: source LCN -> {dest_path_index, dest_offset}.
    /// Built up one contiguous run at a time as execute() proceeds - see
    /// CopySourceLcnMap for the contiguity guarantees this relies on.
    CopySourceLcnMap lcn_map;

    /// Destination first-occurrence LCN interval index seeded by --scan-dest.
    /// Built with ClaimOccurrence::kFirst - one representative owner per run -
    /// since copy only needs one location per dest LCN to clone from.
    inspect::LcnIntervalIndex dest_lcn_index;

    /// Interned file paths from the destination scan (resolves BlockEntry::file_index).
    std::vector<std::wstring> dest_scan_file_table;

    /// Content hash index seeded by --scan-dest when source is non-ReFS.
    /// Empty when both volumes are ReFS (LCN tracking handles dedup without hashing).
    inspect::HashIndex hash_index;

    /// Reusable I/O buffer for copy_bytes_physical (allocated once in prepare(), 4 MB).
    std::vector<BYTE> io_buffer;

    /// SHA-256 hasher for --scan-dest per-cluster content matching, opened lazily
    /// on first use in copy_new_run_with_hash_matching and reused for the rest of
    /// the copy operation - avoids reopening the BCrypt provider every cluster.
    std::optional<inspect::Sha256Hasher> hasher;

    // -- Updated during Phase 3: execute() --
    ULONGLONG processed_lcns = 0;  ///< Unique source LCNs copied so far (for progress_overall).

    CopyStats stats;
    HandleLruCache handle_cache;

    util::CliArg args_snapshot;     ///< Copy of the parsed CLI args captured during prepare().

    CopyContext() = default;
    ~CopyContext() { handle_cache.close_all(); }

    CopyContext(const CopyContext&) = delete;
    CopyContext& operator=(const CopyContext&) = delete;
    CopyContext(CopyContext&&) = default;
    CopyContext& operator=(CopyContext&&) = default;

    /// @brief Looks up or inserts a destination path, returning its interned index.
    uint32_t intern_path(const std::wstring& path) {
        for (uint32_t i = 0; i < static_cast<uint32_t>(dest_path_table.size()); ++i) {
            if (dest_path_table[i] == path) return i;
        }
        dest_path_table.push_back(path);
        return static_cast<uint32_t>(dest_path_table.size() - 1);
    }

    /// @brief Resolves an interned path index back to the path string.
    const std::wstring& resolve_path(uint32_t index) const {
        return dest_path_table[index];
    }
};

/// @brief Abstract interface for a single-file copy operation.
struct ICopyStrategy {
    virtual ~ICopyStrategy() = default;

    /**
     * @brief Copy a single file from src to dest.
     * @param src   Source file path.
     * @param dest  Destination file path.
     * @param args  CLI arguments.
     * @param context  Shared copy tracking context.
     * @return true on success, or error message on failure.
     */
    virtual std::expected<bool, std::wstring> copy_file(
        const std::wstring& src,
        const std::wstring& dest,
        const util::CliArg& args,
        CopyContext& context
    ) = 0;
};

// ============================================================================
// Pure Helper Functions
// ============================================================================

/**
 * @brief Converts a relative path into a fully qualified absolute path name.
 *
 * @param path The relative or absolute path.
 * @return std::wstring The fully qualified absolute path.
 */
std::wstring get_absolute_path(const std::wstring& path) {
    // First call with 0 buffer to get required length (includes null terminator).
    DWORD length = GetFullPathNameW(path.c_str(), 0, NULL, NULL);
    if (length == 0) return path;
    std::wstring buffer(length, L'\0');
    DWORD written = GetFullPathNameW(path.c_str(), length, buffer.data(), NULL);
    if (written > 0 && written < length) {
        buffer.resize(written);
        return buffer;
    }
    return path;
}

// ============================================================================
// Decomposed I/O Helpers
// ============================================================================

/**
 * @brief Opens a source file for reading with backup semantics.
 *
 * @param path The source file path.
 * @return ScopedHandle on success, or error message on failure.
 */
std::expected<ScopedHandle, std::wstring> open_source_file(const std::wstring& path) {
    ScopedHandle h(CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        NULL
    ));
    if (!h) {
        return std::unexpected(L"Failed to open source file: " + util::get_win32_error_message(GetLastError()));
    }
    return h;
}

/**
 * @brief Creates a destination file and sets the sparse attribute.
 *
 * The sparse attribute allows logical sizing (SetEndOfFile) without allocating
 * physical disk space, preventing disk-full failures for deduplicated files.
 *
 * @param path The destination file path.
 * @return ScopedHandle on success, or error message on failure.
 */
std::expected<ScopedHandle, std::wstring> create_dest_file(const std::wstring& path) {
    ScopedHandle h(CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        CREATE_ALWAYS,
        FILE_FLAG_BACKUP_SEMANTICS,
        NULL
    ));
    if (!h) {
        return std::unexpected(L"Failed to create destination file: " + util::get_win32_error_message(GetLastError()));
    }

    FILE_SET_SPARSE_BUFFER sparse_buffer;
    sparse_buffer.SetSparse = TRUE;
    DWORD temp_returned = 0;
    if (!DeviceIoControl(h.get(), FSCTL_SET_SPARSE, &sparse_buffer, sizeof(sparse_buffer), NULL, 0, &temp_returned, NULL)) {
        return std::unexpected(L"Failed to set sparse attribute: " + util::get_win32_error_message(GetLastError()));
    }

    return h;
}

/// @brief Tracks destination file sizing state for incremental growth.
struct DestSizeTracker {
    bool incremental = false;   ///< True if pre-sizing failed and we grow on demand.
    ULONGLONG current_eof = 0;  ///< Current logical end-of-file position.
};

/**
 * @brief Pre-sizes the destination file to the source file size.
 *
 * If pre-sizing fails (e.g. disk full), falls back to incremental mode where
 * the file is grown on demand as blocks are written.
 *
 * @param dest_handle  Handle to the destination file.
 * @param src_size     Source file size in bytes.
 * @return DestSizeTracker indicating sizing mode and current EOF.
 */
std::expected<DestSizeTracker, std::wstring> pre_size_dest_file(HANDLE dest_handle, LONGLONG src_size) {
    DestSizeTracker tracker;

    if (src_size <= 0) {
        return tracker;
    }

    if (util::set_file_eof(dest_handle, static_cast<ULONGLONG>(src_size))) {
        tracker.current_eof = src_size;
    } else {
        tracker.incremental = true;
    }

    // No seek-reset needed: unlike SetFilePointerEx + SetEndOfFile, set_file_eof
    // (SetFileInformationByHandle) never moves the handle's file pointer.
    return tracker;
}

/**
 * @brief Ensures the destination file is at least the required size.
 *
 * Only grows the file if in incremental sizing mode and the required size
 * exceeds the current EOF.
 *
 * @param dest_handle  Handle to the destination file.
 * @param tracker      Mutable sizing tracker.
 * @param required_size  Minimum required file size in bytes.
 * @return true on success, or error message on failure.
 */
std::expected<bool, std::wstring> ensure_dest_size(HANDLE dest_handle, DestSizeTracker& tracker, ULONGLONG required_size) {
    if (!tracker.incremental || required_size <= tracker.current_eof) {
        return true;
    }

    auto eof_ok = util::set_file_eof(dest_handle, required_size);
    if (!eof_ok) {
        return std::unexpected(L"Incremental sizing failed: " + eof_ok.error());
    }
    tracker.current_eof = required_size;
    return true;
}

/// @brief Extent type shared with inspect.h - the single place the
/// FSCTL_GET_RETRIEVAL_POINTERS walk is implemented.
using Extent = inspect::Extent;

/**
 * @brief Queries all retrieval pointers (extents) for a file.
 *
 * Thin wrapper over inspect::collect_extents() for call-site compatibility.
 *
 * @param file_handle  Handle to the file.
 * @return Vector of extents on success, or error message on failure.
 */
std::expected<std::vector<Extent>, std::wstring> query_retrieval_pointers(HANDLE file_handle) {
    return inspect::collect_extents(file_handle);
}

/**
 * @brief Clones an extent from a source file to a destination file on the same volume.
 *
 * @param dest_handle     Handle to the destination file.
 * @param src_handle      Handle to the source file.
 * @param src_offset      Byte offset in the source file.
 * @param dest_offset     Byte offset in the destination file.
 * @param byte_count      Number of bytes to clone.
 * @return true on success, or error message on failure.
 */
std::expected<bool, std::wstring> clone_extent_same_volume(
    HANDLE dest_handle, HANDLE src_handle,
    ULONGLONG src_offset, ULONGLONG dest_offset, ULONGLONG byte_count
) {
    return util::duplicate_extents(dest_handle, src_handle, src_offset, dest_offset, byte_count);
}

/**
 * @brief Clones an extent from a previously-copied destination file to the current destination.
 *
 * Used in cross-volume copies to preserve deduplication by cloning blocks that
 * already exist on the destination volume.
 *
 * @param dest_handle          Handle to the current destination file.
 * @param prev_dest_handle     Handle to the previously-copied file on the same volume.
 * @param prev_dest_offset     Byte offset in the previously-copied file.
 * @param dest_offset          Byte offset in the current destination file.
 * @param byte_count           Number of bytes to clone.
 * @return true on success, or error message on failure.
 */
std::expected<bool, std::wstring> clone_extent_from_dest(
    HANDLE dest_handle, HANDLE prev_dest_handle,
    ULONGLONG prev_dest_offset, ULONGLONG dest_offset, ULONGLONG byte_count
) {
    return util::duplicate_extents(dest_handle, prev_dest_handle, prev_dest_offset, dest_offset, byte_count);
}

/**
 * @brief Physically copies bytes from source to destination in 64 KB chunks.
 *
 * Optionally reports per-file progress via the IOutput interface.
 *
 * @param src_handle   Handle to the source file.
 * @param dest_handle  Handle to the destination file.
 * @param src_offset   Byte offset to start reading from in the source.
 * @param dest_offset  Byte offset to start writing to in the destination.
 * @param byte_count   Total bytes to copy in this call.
 * @param out          Optional output interface for progress reporting.
 * @param filename     Display filename for progress (required if out is set).
 * @param file_offset  Cumulative bytes already transferred for this file.
 * @param file_total   Total file size in bytes.
 * @return true on success, or error message on failure.
 */
std::expected<bool, std::wstring> copy_bytes_physical(
    HANDLE src_handle, HANDLE dest_handle,
    ULONGLONG src_offset, ULONGLONG dest_offset, ULONGLONG byte_count,
    std::vector<BYTE>& io_buffer,
    output::IOutput* out = nullptr, const std::wstring& filename = L"",
    ULONGLONG file_offset = 0, ULONGLONG file_total = 0
) {
    // Seek source
    LARGE_INTEGER li_src;
    li_src.QuadPart = src_offset;
    if (!SetFilePointerEx(src_handle, li_src, NULL, FILE_BEGIN)) {
        return std::unexpected(L"SetFilePointerEx failed on source: " + util::get_win32_error_message(GetLastError()));
    }

    // Seek destination
    LARGE_INTEGER li_dest;
    li_dest.QuadPart = dest_offset;
    if (!SetFilePointerEx(dest_handle, li_dest, NULL, FILE_BEGIN)) {
        return std::unexpected(L"SetFilePointerEx failed on destination: " + util::get_win32_error_message(GetLastError()));
    }

    ULONGLONG copied = 0;

    while (copied < byte_count) {
        if (util::g_cancel_requested) {
            return std::unexpected(L"Copy cancelled by user.");
        }
        DWORD to_read = static_cast<DWORD>(std::min<ULONGLONG>(io_buffer.size(), byte_count - copied));
        DWORD read = 0;
        if (!ReadFile(src_handle, io_buffer.data(), to_read, &read, NULL) || read == 0) {
            DWORD err = GetLastError();
            return std::unexpected(L"ReadFile failed on source (to_read="
                + std::to_wstring(to_read) + L", read=" + std::to_wstring(read)
                + L", copied=" + std::to_wstring(copied)
                + L", byte_count=" + std::to_wstring(byte_count)
                + L", src_offset=" + std::to_wstring(src_offset)
                + L"): " + util::get_win32_error_message(err));
        }

        DWORD written = 0;
        if (!WriteFile(dest_handle, io_buffer.data(), read, &written, NULL) || written != read) {
            return std::unexpected(L"WriteFile failed on destination: " + util::get_win32_error_message(GetLastError()));
        }

        copied += read;

        // Report progress
        if (out && file_total > 0) {
            out->progress(filename, file_offset + copied, file_total);
        }
    }

    return true;
}

/**
 * @brief Sets the final destination file size.
 *
 * Truncates or extends the destination to the exact source file size.
 * File metadata (attributes, timestamps, security, owner) is applied
 * separately by copy_file_metadata() after the data copy succeeds.
 *
 * @param dest_handle  Handle to the destination file.
 * @param src_size     The exact file size to set on the destination.
 * @return true on success, or error message on failure.
 */
std::expected<bool, std::wstring> finalize_dest_file(HANDLE dest_handle, LONGLONG src_size) {
    auto eof_ok = util::set_file_eof(dest_handle, static_cast<ULONGLONG>(src_size));
    if (!eof_ok) {
        return std::unexpected(L"Failed to finalize destination file size: " + eof_ok.error());
    }
    return true;
}

// ============================================================================
// Copy Attempt Fallback
// ============================================================================

/**
 * @brief Fallback to a standard byte-by-byte copy after a clone/dedup failure.
 *
 * Deletes the partially-written destination, warns the user, then performs a
 * manual read/write copy. File metadata (attributes, timestamps, security,
 * owner) is NOT applied here; it is handled by copy_file_metadata() in
 * retry_copy_file() after this function returns.
 *
 * @param src             Source file path.
 * @param dest            Destination file path.
 * @param original_error  Error from the failed clone attempt.
 * @param context         Shared copy context.
 * @return true on success, or error string on failure.
 */
std::expected<bool, std::wstring> attempt_fallback(
    const std::wstring& src,
    const std::wstring& dest,
    const std::wstring& original_error,
    CopyContext& context
) {
    DeleteFileW(dest.c_str());
    if (util::g_cancel_requested) return std::unexpected(L"Copy cancelled by user.");

    context.out->message(output::Level::warn,
        L"Deduplication-preserving copy failed (" + original_error +
        L"). Falling back to standard copy for: " + src);

    auto src_result = open_source_file(src);
    if (!src_result) {
        return std::unexpected(L"Dedup copy failed (" + original_error +
            L"), and fallback open failed: " + src_result.error());
    }
    ScopedHandle src_h = std::move(*src_result);

    auto dest_result = create_dest_file(dest);
    if (!dest_result) {
        return std::unexpected(L"Dedup copy failed (" + original_error +
            L"), and fallback create failed: " + dest_result.error());
    }
    ScopedHandle dest_h = std::move(*dest_result);

    LARGE_INTEGER src_size = {0};
    if (!GetFileSizeEx(src_h.get(), &src_size)) {
        return std::unexpected(L"Fallback: failed to get source size: " +
                               util::get_win32_error_message(GetLastError()));
    }

    auto tracker_result = pre_size_dest_file(dest_h.get(), src_size.QuadPart);
    if (!tracker_result) {
        return std::unexpected(L"Fallback: pre-size failed: " + tracker_result.error());
    }

    if (src_size.QuadPart > 0) {
        auto copy_ok = copy_bytes_physical(
            src_h.get(), dest_h.get(),
            0, 0, static_cast<ULONGLONG>(src_size.QuadPart),
            context.io_buffer, context.out, src,
            0, static_cast<ULONGLONG>(src_size.QuadPart)
        );
        if (!copy_ok) {
            dest_h.close();
            src_h.close();
            if (util::g_cancel_requested) {
                DeleteFileW(dest.c_str());
                return std::unexpected(L"Copy cancelled by user.");
            }
            return std::unexpected(L"Dedup copy failed (" + original_error +
                L"), and fallback copy failed: " + copy_ok.error());
        }
    }

    auto final_ok = finalize_dest_file(dest_h.get(), src_size.QuadPart);
    if (!final_ok) {
        return std::unexpected(L"Fallback: finalize failed: " + final_ok.error());
    }

    context.stats.fallback_files++;
    context.stats.total_files++;
    context.stats.total_bytes += static_cast<ULONGLONG>(src_size.QuadPart);
    return true;
}

// ============================================================================
// Copy Strategies
// ============================================================================

/**
 * @brief Fallback strategy using standard byte-by-byte file copy.
 *
 * Used when the destination is non-ReFS or cluster sizes are mismatched,
 * making block cloning impossible. File metadata is NOT applied here; it
 * is handled by copy_file_metadata() in retry_copy_file().
 */
struct FallbackCopyStrategy : ICopyStrategy {
    std::expected<bool, std::wstring> copy_file(
        const std::wstring& src,
        const std::wstring& dest,
        const util::CliArg& args,
        CopyContext& context
    ) override {
        if (args.dry_run) {
            context.out->message(output::Level::info,
                L"[DRY-RUN] Would copy with standard fallback: " + src + L" -> " + dest);
            context.stats.fallback_files++;
            context.stats.total_files++;
            return true;
        }

        if (!context.dest_is_refs) {
            context.out->message(output::Level::warn,
                L"Destination volume is non-ReFS. Using standard copy for: " + src);
        } else {
            context.out->message(output::Level::warn,
                L"Destination volume cluster size mismatch ("
                + std::to_wstring(context.dest_cluster_size) + L" vs "
                + std::to_wstring(context.src_cluster_size)
                + L"). Using standard copy for: " + src);
        }

        auto src_result = open_source_file(src);
        if (!src_result) return std::unexpected(src_result.error());
        ScopedHandle src_handle = std::move(*src_result);

        auto dest_result = create_dest_file(dest);
        if (!dest_result) return std::unexpected(dest_result.error());
        ScopedHandle dest_handle = std::move(*dest_result);

        LARGE_INTEGER src_size = {0};
        if (!GetFileSizeEx(src_handle.get(), &src_size)) {
            return std::unexpected(L"Failed to get source file size: " +
                                   util::get_win32_error_message(GetLastError()));
        }

        auto tracker_result = pre_size_dest_file(dest_handle.get(), src_size.QuadPart);
        if (!tracker_result) return std::unexpected(tracker_result.error());

        if (src_size.QuadPart > 0) {
            auto copy_ok = copy_bytes_physical(
                src_handle.get(), dest_handle.get(),
                0, 0, static_cast<ULONGLONG>(src_size.QuadPart),
                context.io_buffer, context.out, src,
                0, static_cast<ULONGLONG>(src_size.QuadPart)
            );
            if (!copy_ok) {
                dest_handle.close();
                src_handle.close();
                if (util::g_cancel_requested) {
                    DeleteFileW(dest.c_str());
                    return std::unexpected(L"Copy cancelled by user.");
                }
                return std::unexpected(copy_ok.error());
            }
        }

        auto final_ok = finalize_dest_file(dest_handle.get(), src_size.QuadPart);
        if (!final_ok) return std::unexpected(final_ok.error());

        context.stats.fallback_files++;
        context.stats.total_files++;
        context.stats.total_bytes += static_cast<ULONGLONG>(src_size.QuadPart);
        return true;
    }
};

/**
 * @brief Same-volume strategy using FSCTL_DUPLICATE_EXTENTS_TO_FILE.
 *
 * Clones entire extents directly from the source file handle, preserving
 * block sharing on the same volume.
 */
struct SameVolumeCopyStrategy : ICopyStrategy {
    std::expected<bool, std::wstring> copy_file(
        const std::wstring& src,
        const std::wstring& dest,
        const util::CliArg& args,
        CopyContext& context
    ) override {
        if (args.dry_run) {
            context.out->message(output::Level::info, L"[DRY-RUN] Would clone (same volume): " + src + L" -> " + dest);
            context.stats.cloned_files++;
            context.stats.total_files++;
            return true;
        }

        // Open files
        auto src_result = open_source_file(src);
        if (!src_result) return std::unexpected(src_result.error());
        ScopedHandle src_handle = std::move(*src_result);

        auto dest_result = create_dest_file(dest);
        if (!dest_result) return std::unexpected(dest_result.error());
        ScopedHandle dest_handle = std::move(*dest_result);

        // Get source file size
        LARGE_INTEGER src_size = {0};
        if (!GetFileSizeEx(src_handle.get(), &src_size)) {
            return std::unexpected(L"Failed to get source file size: " + util::get_win32_error_message(GetLastError()));
        }

        // Pre-size destination
        auto tracker_result = pre_size_dest_file(dest_handle.get(), src_size.QuadPart);
        if (!tracker_result) return std::unexpected(tracker_result.error());
        DestSizeTracker tracker = *tracker_result;

        // Query extents
        auto extents_result = query_retrieval_pointers(src_handle.get());
        if (!extents_result) {
            dest_handle.close();
            src_handle.close();
            if (util::g_cancel_requested) {
                DeleteFileW(dest.c_str());
                return std::unexpected(L"Copy cancelled by user.");
            }
            return attempt_fallback(src, dest, extents_result.error(), context);
        }

        // Clone each extent
        ULONGLONG bytes_cloned = 0;
        for (const auto& ext : *extents_result) {
            if (ext.is_sparse()) continue;

            if (util::g_cancel_requested) {
                dest_handle.close();
                src_handle.close();
                DeleteFileW(dest.c_str());
                return std::unexpected(L"Copy cancelled by user.");
            }

            ULONGLONG src_offset = ext.start_vcn * context.src_cluster_size;
            ULONGLONG byte_count = ext.cluster_count() * context.src_cluster_size;
            ULONGLONG required_size = src_offset + byte_count;

            auto size_ok = ensure_dest_size(dest_handle.get(), tracker, required_size);
            if (!size_ok) {
                dest_handle.close();
                src_handle.close();
                if (util::g_cancel_requested) {
                    DeleteFileW(dest.c_str());
                    return std::unexpected(L"Copy cancelled by user.");
                }
                return attempt_fallback(src, dest, size_ok.error(), context);
            }

            auto clone_ok = clone_extent_same_volume(dest_handle.get(), src_handle.get(), src_offset, src_offset, byte_count);
            if (!clone_ok) {
                dest_handle.close();
                src_handle.close();
                if (util::g_cancel_requested) {
                    DeleteFileW(dest.c_str());
                    return std::unexpected(L"Copy cancelled by user.");
                }
                return attempt_fallback(src, dest, clone_ok.error(), context);
            }

            bytes_cloned += byte_count;
            context.out->progress(src, bytes_cloned, static_cast<ULONGLONG>(src_size.QuadPart));
        }

        // A non-empty source with nothing cloned means every extent was sparse, or
        // there were no extents at all - both happen for resident files (their
        // content lives in file-record metadata, invisible to FSCTL_GET_RETRIEVAL_
        // POINTERS) as well as for genuinely all-sparse files. The two can't be told
        // apart from the extent list alone, so fall back to a standard read/write
        // copy rather than silently finalizing a correctly-sized but zero-content
        // file - the fallback is correct either way, since ReadFile transparently
        // returns zeros for sparse holes.
        if (bytes_cloned == 0 && src_size.QuadPart > 0) {
            dest_handle.close();
            src_handle.close();
            if (util::g_cancel_requested) {
                DeleteFileW(dest.c_str());
                return std::unexpected(L"Copy cancelled by user.");
            }
            return attempt_fallback(src, dest,
                L"No clonable extents found for non-empty source (resident or fully-sparse file)",
                context);
        }

        // Finalize
        auto final_ok = finalize_dest_file(dest_handle.get(), src_size.QuadPart);
        if (!final_ok) {
            dest_handle.close();
            src_handle.close();
            if (util::g_cancel_requested) {
                DeleteFileW(dest.c_str());
                return std::unexpected(L"Copy cancelled by user.");
            }
            return std::unexpected(final_ok.error());
        }

        context.stats.cloned_files++;
        context.stats.total_files++;
        context.stats.total_bytes += src_size.QuadPart;
        return true;
    }
};

/**
 * @brief Cross-volume ReFS strategy preserving deduplication via LCN mapping.
 *
 * For each extent: new LCNs are physically copied from source, duplicate LCNs
 * are cloned from previously-copied destination files via handle cache.
 */
struct CrossVolumeRefsCopyStrategy : ICopyStrategy {
    std::expected<bool, std::wstring> copy_file(
        const std::wstring& src,
        const std::wstring& dest,
        const util::CliArg& args,
        CopyContext& context
    ) override {
        if (args.dry_run) {
            context.out->message(output::Level::info, L"[DRY-RUN] Would copy preserving deduplication (cross volume ReFS): "
                + src + L" -> " + dest);
            context.stats.cloned_files++;
            context.stats.total_files++;
            return true;
        }

        // Open files
        auto src_result = open_source_file(src);
        if (!src_result) return std::unexpected(src_result.error());
        ScopedHandle src_handle = std::move(*src_result);

        auto dest_result = create_dest_file(dest);
        if (!dest_result) return std::unexpected(dest_result.error());
        ScopedHandle dest_handle = std::move(*dest_result);

        // Get source file size
        LARGE_INTEGER src_size = {0};
        if (!GetFileSizeEx(src_handle.get(), &src_size)) {
            return std::unexpected(L"Failed to get source file size: " + util::get_win32_error_message(GetLastError()));
        }

        // Pre-size destination
        auto tracker_result = pre_size_dest_file(dest_handle.get(), src_size.QuadPart);
        if (!tracker_result) return std::unexpected(tracker_result.error());
        DestSizeTracker tracker = *tracker_result;

        // Query extents
        auto extents_result = query_retrieval_pointers(src_handle.get());
        if (!extents_result) {
            dest_handle.close();
            src_handle.close();
            if (util::g_cancel_requested) {
                DeleteFileW(dest.c_str());
                return std::unexpected(L"Copy cancelled by user.");
            }
            return attempt_fallback(src, dest, extents_result.error(), context);
        }

        // Process each extent cluster-by-cluster
        ULONGLONG bytes_processed = 0;
        for (const auto& ext : *extents_result) {
            if (ext.is_sparse()) continue;

            if (util::g_cancel_requested) {
                dest_handle.close();
                src_handle.close();
                DeleteFileW(dest.c_str());
                return std::unexpected(L"Copy cancelled by user.");
            }

            auto result = process_cross_volume_extent(
                ext, src_handle.get(), dest_handle.get(),
                src_size.QuadPart, src, dest, tracker, context, bytes_processed
            );
            if (!result) {
                dest_handle.close();
                src_handle.close();
                if (util::g_cancel_requested) {
                    DeleteFileW(dest.c_str());
                    return std::unexpected(L"Copy cancelled by user.");
                }
                return attempt_fallback(src, dest, result.error(), context);
            }
        }

        // Same reasoning as SameVolumeCopyStrategy: a non-empty source with nothing
        // processed means the extent list was empty or entirely sparse, which is
        // indistinguishable between a resident file and a genuinely all-sparse one
        // from the extent list alone. Fall back to a standard copy rather than
        // silently finalizing a correctly-sized but zero-content file.
        if (bytes_processed == 0 && src_size.QuadPart > 0) {
            dest_handle.close();
            src_handle.close();
            if (util::g_cancel_requested) {
                DeleteFileW(dest.c_str());
                return std::unexpected(L"Copy cancelled by user.");
            }
            return attempt_fallback(src, dest,
                L"No clonable extents found for non-empty source (resident or fully-sparse file)",
                context);
        }

        // Finalize
        auto final_ok = finalize_dest_file(dest_handle.get(), src_size.QuadPart);
        if (!final_ok) {
            dest_handle.close();
            src_handle.close();
            if (util::g_cancel_requested) {
                DeleteFileW(dest.c_str());
                return std::unexpected(L"Copy cancelled by user.");
            }
            return std::unexpected(final_ok.error());
        }

        context.stats.cloned_files++;
        context.stats.total_files++;
        context.stats.total_bytes += src_size.QuadPart;
        return true;
    }

private:
    /**
     * @brief Processes a single extent for cross-volume copy.
     *
     * Iterates clusters within the extent. Duplicate LCNs are cloned from
     * previously-copied destination files; new LCNs are physically copied
     * from the source.
     */
    std::expected<bool, std::wstring> process_cross_volume_extent(
        const Extent& ext, HANDLE src_handle, HANDLE dest_handle,
        LONGLONG src_file_size, const std::wstring& src_path,
        const std::wstring& dest_path,
        DestSizeTracker& tracker, CopyContext& context,
        ULONGLONG& bytes_processed
    ) {
        ULONGLONG c_offset = 0;
        while (c_offset < ext.cluster_count()) {
            if (util::g_cancel_requested) {
                return std::unexpected(L"Copy cancelled by user.");
            }
            LONGLONG current_lcn = ext.lcn + c_offset;
            ULONGLONG src_offset = (ext.start_vcn + c_offset) * context.src_cluster_size;

            auto lookup = context.lcn_map.find(current_lcn, context.src_cluster_size);
            if (lookup) {
                // Duplicate LCN: clone from previously-copied destination
                ULONGLONG pre_offset = c_offset;
                auto result = clone_duplicate_run(
                    ext, c_offset, *lookup, src_offset,
                    dest_handle, tracker, context
                );
                if (!result) return std::unexpected(result.error());
                bytes_processed += (c_offset - pre_offset) * context.src_cluster_size;
            } else {
                // New LCN: physically copy from source
                ULONGLONG pre_offset = c_offset;
                auto result = copy_new_run(
                    ext, c_offset, src_offset, src_file_size,
                    src_handle, dest_handle, src_path, dest_path, tracker, context
                );
                if (!result) return std::unexpected(result.error());
                bytes_processed += (c_offset - pre_offset) * context.src_cluster_size;
            }

            // Report per-file byte progress
            context.out->progress(src_path, bytes_processed, static_cast<ULONGLONG>(src_file_size));
            // Report overall unique-LCN progress (secondary bar; throttled internally)
            if (context.total_unique_src_lcns > 0) {
                context.out->progress_overall(L"clusters", context.processed_lcns,
                                               context.total_unique_src_lcns, L"", true);
            }
        }
        return true;
    }

    /**
     * @brief Clones a contiguous run of duplicate LCNs from a previously-copied destination file.
     *
     * The run length comes directly from the interval the lookup already resolved
     * (CopySourceLcnMap never merges separate insertions, so this is exactly the
     * span one earlier insert_run() call actually claimed) - capped by how much
     * of the current source extent remains, since the two are independent.
     *
     * @param c_offset Updated in-place to advance past the cloned run.
     */
    std::expected<bool, std::wstring> clone_duplicate_run(
        const Extent& ext, ULONGLONG& c_offset,
        const CopySourceLcnMap::Lookup& master,
        ULONGLONG src_offset, HANDLE dest_handle,
        DestSizeTracker& tracker, CopyContext& context
    ) {
        ULONGLONG ext_remaining = ext.cluster_count() - c_offset;
        ULONGLONG run_clusters = (std::min)(master.run_clusters_remaining, ext_remaining);

        ULONGLONG byte_count = run_clusters * context.src_cluster_size;
        auto size_ok = ensure_dest_size(dest_handle, tracker, src_offset + byte_count);
        if (!size_ok) return std::unexpected(size_ok.error());

        // Resolve interned path and get handle to the previously-copied destination file
        const std::wstring& master_path = context.resolve_path(master.dest_path_index);
        HANDLE prev_dest_handle = context.handle_cache.get(master_path);
        if (prev_dest_handle == INVALID_HANDLE_VALUE) {
            return std::unexpected(L"Failed to open previously copied destination file: " + master_path);
        }

        auto clone_ok = clone_extent_from_dest(dest_handle, prev_dest_handle, master.dest_offset, src_offset, byte_count);
        if (!clone_ok) return std::unexpected(clone_ok.error());

        c_offset += run_clusters;
        return true;
    }

    /**
     * @brief Physically copies a contiguous run of new (unmapped) LCNs from source.
     *
     * When --scan-dest was used (hash_index is populated), reads each cluster,
     * hashes it, and checks for a content match on the destination volume.
     * Matching clusters are cloned instead of physically written.
     * When hash_index is empty, copies the entire run in bulk for speed.
     *
     * @param c_offset Updated in-place to advance past the copied run.
     */
    std::expected<bool, std::wstring> copy_new_run(
        const Extent& ext, ULONGLONG& c_offset,
        ULONGLONG src_offset, LONGLONG src_file_size,
        HANDLE src_handle, HANDLE dest_handle,
        const std::wstring& src_path, const std::wstring& dest_path,
        DestSizeTracker& tracker, CopyContext& context
    ) {
        // Find contiguous run of new LCNs
        ULONGLONG run_clusters = 1;
        while (c_offset + run_clusters < ext.cluster_count()) {
            LONGLONG next_lcn = ext.lcn + c_offset + run_clusters;
            if (context.lcn_map.contains(next_lcn)) break;
            run_clusters++;
        }

        // Calculate bytes to copy (clamped to file size)
        ULONGLONG bytes_to_copy = run_clusters * context.src_cluster_size;
        ULONGLONG bytes_to_read = bytes_to_copy;
        if (static_cast<ULONGLONG>(src_file_size) > src_offset) {
            if (src_offset + bytes_to_read > static_cast<ULONGLONG>(src_file_size)) {
                bytes_to_read = static_cast<ULONGLONG>(src_file_size) - src_offset;
            }
        } else {
            bytes_to_read = 0;
        }

        auto size_ok = ensure_dest_size(dest_handle, tracker, src_offset + bytes_to_read);
        if (!size_ok) return std::unexpected(size_ok.error());

        // If --scan-dest populated hash_index, try per-cluster hash matching
        if (!context.hash_index.empty() && bytes_to_read > 0) {
            auto result = copy_new_run_with_hash_matching(
                ext, c_offset, run_clusters, src_offset, src_file_size,
                src_handle, dest_handle, src_path, dest_path, context
            );
            if (!result) return std::unexpected(result.error());
        } else {
            // No hash index: bulk copy the entire run
            if (bytes_to_read > 0) {
                auto copy_ok = copy_bytes_physical(
                    src_handle, dest_handle, src_offset, src_offset, bytes_to_read,
                    context.io_buffer, context.out, src_path,
                    src_offset, static_cast<ULONGLONG>(src_file_size)
                );
                if (!copy_ok) return std::unexpected(copy_ok.error());
            }

            // Record this run for future dedup (using interned path index). The
            // whole run lands at contiguous destination offsets by construction
            // (a straight physical copy mirrors source position), so it's one
            // interval, not one insertion per cluster.
            uint32_t path_idx = context.intern_path(dest_path);
            context.lcn_map.insert_run(ext.lcn + static_cast<LONGLONG>(c_offset), run_clusters, path_idx, src_offset);
            context.processed_lcns += run_clusters;

            c_offset += run_clusters;
        }

        return true;
    }

    /**
     * @brief Per-cluster hash-matching copy for --scan-dest mode.
     *
     * Reads each cluster from the source, computes SHA-256, and checks
     * the destination hash_index. If a match is found, the cluster is
     * cloned from the existing destination file. Otherwise it is
     * physically written.
     *
     * @param c_offset Updated in-place to advance past the processed run.
     */
    std::expected<bool, std::wstring> copy_new_run_with_hash_matching(
        const Extent& ext, ULONGLONG& c_offset, ULONGLONG run_clusters,
        ULONGLONG src_offset, LONGLONG src_file_size,
        HANDLE src_handle, HANDLE dest_handle,
        const std::wstring& src_path, const std::wstring& dest_path,
        CopyContext& context
    ) {
        DWORD cluster_size = context.src_cluster_size;
        uint32_t path_idx = context.intern_path(dest_path);

        // Open the BCrypt provider once and reuse it for the rest of the copy
        // operation, instead of paying BCryptOpenAlgorithmProvider/Close per cluster.
        if (!context.hasher) {
            auto hasher_res = inspect::make_sha256_hasher();
            if (!hasher_res) return std::unexpected(hasher_res.error());
            context.hasher = std::move(*hasher_res);
        }

        // Seek source to beginning of this run
        LARGE_INTEGER li_src;
        li_src.QuadPart = src_offset;
        if (!SetFilePointerEx(src_handle, li_src, NULL, FILE_BEGIN)) {
            return std::unexpected(L"SetFilePointerEx failed: " + util::get_win32_error_message(GetLastError()));
        }

        for (ULONGLONG k = 0; k < run_clusters; ++k) {
            if (util::g_cancel_requested) {
                return std::unexpected(L"Copy cancelled by user.");
            }

            ULONGLONG cluster_offset = src_offset + k * cluster_size;
            ULONGLONG bytes_remaining = 0;
            if (static_cast<ULONGLONG>(src_file_size) > cluster_offset) {
                bytes_remaining = static_cast<ULONGLONG>(src_file_size) - cluster_offset;
            }
            DWORD to_read = static_cast<DWORD>(std::min<ULONGLONG>(cluster_size, bytes_remaining));

            if (to_read == 0) {
                // Past EOF - nothing to hash or write, but still part of this
                // run's LCN range; covered by the single insert_run() below.
                continue;
            }

            // Read source cluster into io_buffer
            DWORD bytes_read = 0;
            if (!ReadFile(src_handle, context.io_buffer.data(), to_read, &bytes_read, NULL) || bytes_read == 0) {
                return std::unexpected(L"ReadFile failed during hash matching: " + util::get_win32_error_message(GetLastError()));
            }

            // Hash the cluster
            std::string digest = context.hasher->hash(context.io_buffer.data(), bytes_read);
            bool cloned = false;

            if (!digest.empty()) {
                auto hash_it = context.hash_index.find(digest);
                if (hash_it != context.hash_index.end() && !hash_it->second.empty()) {
                    // Found a content match on the destination volume.
                    // Resolve the first matching destination LCN to a file+offset.
                    // lcn_interval_find_at() corrects file_offset for dest_lcn's exact
                    // position within its interval - lcn_interval_find() would return
                    // the offset at the interval's start_lcn regardless of dest_lcn,
                    // which is wrong for any cluster past a multi-cluster file's first.
                    LONGLONG dest_lcn = hash_it->second[0];
                    auto block = inspect::lcn_interval_find_at(context.dest_lcn_index, dest_lcn, cluster_size);
                    if (block) {
                        const std::wstring& block_path = context.dest_scan_file_table[block->file_index];
                        HANDLE match_handle = context.handle_cache.get(block_path);
                        if (match_handle != INVALID_HANDLE_VALUE) {
                            auto clone_ok = clone_extent_from_dest(
                                dest_handle, match_handle,
                                block->file_offset, cluster_offset, cluster_size
                            );
                            if (clone_ok) {
                                cloned = true;
                            }
                            // If clone fails, fall through to physical write
                        }
                    }
                }
            }

            if (!cloned) {
                // No match or clone failed - write physically
                LARGE_INTEGER li_dest;
                li_dest.QuadPart = cluster_offset;
                if (!SetFilePointerEx(dest_handle, li_dest, NULL, FILE_BEGIN)) {
                    return std::unexpected(L"SetFilePointerEx failed on dest: " + util::get_win32_error_message(GetLastError()));
                }
                DWORD written = 0;
                if (!WriteFile(dest_handle, context.io_buffer.data(), bytes_read, &written, NULL) || written != bytes_read) {
                    return std::unexpected(L"WriteFile failed: " + util::get_win32_error_message(GetLastError()));
                }
            }

            context.processed_lcns++;

            // Report progress
            context.out->progress(src_path, cluster_offset + bytes_read, static_cast<ULONGLONG>(src_file_size));
        }

        // Every cluster in this run lands at a contiguous destination offset
        // within dest_path (mirroring its source position), regardless of
        // whether any individual cluster was hash-matched-and-cloned or
        // physically written - so the whole run is recorded as one interval,
        // not per cluster. This is distinct from (and unaffected by) whichever
        // *other* file a hash match above may have cloned bytes from.
        context.lcn_map.insert_run(ext.lcn + static_cast<LONGLONG>(c_offset), run_clusters, path_idx, src_offset);

        c_offset += run_clusters;
        return true;
    }
};

/**
 * @brief Phase 1 - Validates arguments, queries volumes, and selects copy strategy.
 *
 * No filesystem writes are performed. Returns a heap-allocated CopyContext ready
 * for execute(). The caller is responsible for calling cleanup().
 *
 * @param args  CLI arguments with file_specs[0]=source, file_specs[1]=destination.
 * @return Heap-allocated CopyContext on success, or error string on failure.
 */
std::expected<CopyContext*, std::wstring> prepare(const util::CliArg& args) {
    // copy only operates on kPath specifiers (files, directories, globs).
    for (const auto& spec : args.file_specs) {
        if (spec.kind == util::FileSpecKind::kVolume) {
            return std::unexpected(
                L"Error: copy does not accept volume specifiers ('" + spec.path +
                L"'). Use a full path such as E:\\ or E:\\folder\\.");
        }
    }

    if (args.file_specs.size() < 2) {
        return std::unexpected(L"Error: Missing source or destination path. Usage: retool copy <src> <dest> [options]");
    }

    // When more than two specifiers are given, the last must be a directory
    // (multi-source copy: retool copy src1 src2 ... dest_dir).
    // Currently only single-source is implemented; reject multi-source for now.
    if (args.file_specs.size() > 2) {
        return std::unexpected(
            L"Error: Multiple source files are not yet supported. "
            L"Usage: retool copy <src> <dest> [options]");
    }

    auto ctx = std::make_unique<CopyContext>();
    ctx->args_snapshot = args;
    ctx->src_path  = get_absolute_path(args.file_specs[0].path);
    ctx->dest_path = get_absolute_path(args.file_specs[1].path);

    // Validate source
    DWORD src_attr = GetFileAttributesW(ctx->src_path.c_str());
    if (src_attr == INVALID_FILE_ATTRIBUTES) {
        return std::unexpected(L"Source path does not exist: " + ctx->src_path);
    }

    ctx->is_directory = (src_attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (ctx->is_directory && !args.recursive) {
        return std::unexpected(L"Error: Source is a directory but -r (recursive) was not specified.");
    }

    // Resolve volume roots and cluster sizes - once per volume, not per file.
    auto src_vol = util::resolve_volume_info(ctx->src_path);
    if (!src_vol) return std::unexpected(L"Failed to resolve source volume: " + src_vol.error());
    auto dest_vol = util::resolve_volume_info(ctx->dest_path);
    if (!dest_vol) return std::unexpected(L"Failed to resolve destination volume: " + dest_vol.error());

    ctx->src_volume_root   = src_vol->volume_root;
    ctx->dest_volume_root  = dest_vol->volume_root;
    ctx->same_volume       = (ctx->src_volume_root == ctx->dest_volume_root);
    ctx->src_is_refs       = (src_vol->fs_name == L"ReFS");
    ctx->dest_is_refs      = (dest_vol->fs_name == L"ReFS");
    ctx->src_cluster_size  = src_vol->cluster_size;
    ctx->dest_cluster_size = dest_vol->cluster_size;

    // Allocate reusable I/O buffer (4 MB)
    ctx->io_buffer.resize(4 * 1024 * 1024);

    // Select strategy
    if (ctx->same_volume) {
        ctx->strategy = std::make_unique<SameVolumeCopyStrategy>();
    } else if (ctx->dest_is_refs && ctx->src_cluster_size == ctx->dest_cluster_size) {
        ctx->strategy = std::make_unique<CrossVolumeRefsCopyStrategy>();
    } else {
        ctx->strategy = std::make_unique<FallbackCopyStrategy>();
    }

    return ctx.release();
}

/**
 * @brief Pre-scans the destination volume and seeds the LCN/hash indexes.
 *
 * Called when --scan-dest is set and the strategy is CrossVolumeRefsCopyStrategy.
 * Populates context.lcn_map and context.hash_index so that blocks already present
 * on the destination can be cloned instead of physically copied.
 *
 * @param context The CopyContext with dest_volume_root and out set.
 * @return true on success, or error string on failure.
 */
std::expected<bool, std::wstring> seed_from_dest_scan(CopyContext& context) {
    // When both volumes are ReFS, the within-operation lcn_map handles dedup tracking
    // without content hashing. Use kLcnOnly to avoid the SHA-256 overhead and the
    // HashIndex memory cost.
    const bool both_refs = context.src_is_refs && context.dest_is_refs;
    const inspect::ScanMode scan_mode = both_refs
        ? inspect::ScanMode::kLcnOnly
        : inspect::ScanMode::kWithHash;

    context.out->message(output::Level::info,
        L"[seed_from_dest_scan] Pre-scanning destination volume " + context.dest_volume_root +
        L" (" + (both_refs ? L"LCN-only, no hash" : L"with content hash") + L")...");

    auto scan = inspect::build_dest_lcn_index(
        util::FileSpecifier{util::FileSpecKind::kVolume, context.dest_volume_root},
        scan_mode, *context.out);
    if (!scan) return std::unexpected(scan.error());

    context.dest_lcn_index      = std::move(scan->lcn_index);
    context.dest_scan_file_table = std::move(scan->file_table);
    context.hash_index           = std::move(scan->hash_index);

    context.out->message(output::Level::info,
        L"[seed_from_dest_scan] Done. " +
        std::to_wstring(context.dest_lcn_index.size()) + L" dest LCN runs indexed" +
        (context.hash_index.empty() ? L"." : L", " +
         std::to_wstring(context.hash_index.size()) + L" hash groups."));
    return true;
}

// ============================================================================
// Metadata Helpers and Predicates
// ============================================================================

/// @brief Returns true if the given component letter is in args.copy_components.
bool has_component(const util::CliArg& args, wchar_t c) {
    return args.copy_components.find(static_cast<wchar_t>(towupper(c))) != std::wstring::npos;
}

/// @brief Returns true if the given RASH attribute letter is in args.copy_attr_mask.
bool has_attr(const util::CliArg& args, wchar_t c) {
    return args.copy_attr_mask.find(static_cast<wchar_t>(towupper(c))) != std::wstring::npos;
}

/// @brief Builds the Win32 attribute bitmask (FILE_ATTRIBUTE_*) from args.copy_attr_mask.
DWORD build_attr_mask(const util::CliArg& args) {
    DWORD mask = 0;
    if (has_attr(args, L'R')) mask |= FILE_ATTRIBUTE_READONLY;
    if (has_attr(args, L'A')) mask |= FILE_ATTRIBUTE_ARCHIVE;
    if (has_attr(args, L'S')) mask |= FILE_ATTRIBUTE_SYSTEM;
    if (has_attr(args, L'H')) mask |= FILE_ATTRIBUTE_HIDDEN;
    return mask;
}

/**
 * @brief Returns true if the destination file should be skipped.
 *
 * A file is skippable when -xs is active and the destination already has
 * the same size and last-write timestamp as the source.
 *
 * @param src   Absolute source file path.
 * @param dest  Absolute destination file path.
 * @return true if the file should be skipped; false if it should be copied.
 */
bool should_skip_file(const std::wstring& src, const std::wstring& dest) {
    WIN32_FILE_ATTRIBUTE_DATA dest_info{};
    if (!GetFileAttributesExW(dest.c_str(), GetFileExInfoStandard, &dest_info)) return false;

    WIN32_FILE_ATTRIBUTE_DATA src_info{};
    if (!GetFileAttributesExW(src.c_str(), GetFileExInfoStandard, &src_info)) return false;

    if (dest_info.nFileSizeHigh != src_info.nFileSizeHigh ||
        dest_info.nFileSizeLow  != src_info.nFileSizeLow)  return false;

    return CompareFileTime(&dest_info.ftLastWriteTime, &src_info.ftLastWriteTime) == 0;
}

/**
 * @brief Copies timestamps and/or file attribute bits from source to destination.
 *
 * Reads FILE_BASIC_INFO from the source and applies it selectively:
 *   - T component: copies creation, access, write, and change timestamps.
 *     Time fields set to 0 in FILE_BASIC_INFO mean "no change".
 *   - A component: merges the attribute bits selected by -ca: (RASH subset)
 *     with the current destination attributes; only the masked bits are copied.
 *     FileAttributes of 0 means "no change".
 *
 * @param src   Source file path.
 * @param dest  Destination file path.
 * @param args  CLI arguments (copy_components, copy_attr_mask).
 * @return true on success, or error string on failure.
 */
std::expected<bool, std::wstring> copy_timestamps_and_attrs(
    const std::wstring& src,
    const std::wstring& dest,
    const util::CliArg& args
) {
    bool do_attrs = has_component(args, L'A');
    bool do_times = has_component(args, L'T');
    if (!do_attrs && !do_times) return true;

    ScopedHandle src_h(CreateFileW(
        src.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL
    ));
    if (!src_h) {
        return std::unexpected(L"Failed to open source for attribute query: " +
                               util::get_win32_error_message(GetLastError()));
    }

    FILE_BASIC_INFO src_info{};
    if (!GetFileInformationByHandleEx(src_h.get(), FileBasicInfo, &src_info, sizeof(src_info))) {
        return std::unexpected(L"GetFileInformationByHandleEx failed on source: " +
                               util::get_win32_error_message(GetLastError()));
    }
    src_h.close();

    ScopedHandle dest_h(CreateFileW(
        dest.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL
    ));
    if (!dest_h) {
        return std::unexpected(L"Failed to open destination for attribute write: " +
                               util::get_win32_error_message(GetLastError()));
    }

    // Zero fields = "no change" for times; FileAttributes = 0 = "no change".
    FILE_BASIC_INFO out_info{};

    if (do_times) {
        out_info.CreationTime   = src_info.CreationTime;
        out_info.LastAccessTime = src_info.LastAccessTime;
        out_info.LastWriteTime  = src_info.LastWriteTime;
        out_info.ChangeTime     = src_info.ChangeTime;
    }

    if (do_attrs) {
        DWORD attr_mask      = build_attr_mask(args);
        DWORD cur_dest_attrs = GetFileAttributesW(dest.c_str());
        if (cur_dest_attrs == INVALID_FILE_ATTRIBUTES) cur_dest_attrs = FILE_ATTRIBUTE_NORMAL;
        DWORD new_attrs = (cur_dest_attrs & ~attr_mask) | (src_info.FileAttributes & attr_mask);
        out_info.FileAttributes = (new_attrs == 0) ? FILE_ATTRIBUTE_NORMAL : new_attrs;
    }

    if (!SetFileInformationByHandle(dest_h.get(), FileBasicInfo, &out_info, sizeof(out_info))) {
        return std::unexpected(L"SetFileInformationByHandle failed: " +
                               util::get_win32_error_message(GetLastError()));
    }

    return true;
}

/**
 * @brief Copies security (DACL) and/or owner/group from source to destination.
 *
 * Uses GetNamedSecurityInfoW / SetNamedSecurityInfoW. Failures are warnings,
 * not errors - the data copy already succeeded, and security propagation may
 * fail legitimately (e.g. setting owner requires SeRestorePrivilege).
 *
 * Components copied:
 *   - S: DACL (discretionary ACL - standard file permissions).
 *   - O: owner SID and primary group SID.
 *
 * @param src   Source file path.
 * @param dest  Destination file path.
 * @param args  CLI arguments (copy_components).
 * @param out   Output interface for warning messages.
 */
void copy_security_info(
    const std::wstring& src,
    const std::wstring& dest,
    const util::CliArg& args,
    output::IOutput& out
) {
    bool do_security = has_component(args, L'S');
    bool do_owner    = has_component(args, L'O');
    if (!do_security && !do_owner) return;

    SECURITY_INFORMATION si = 0;
    if (do_security) si |= DACL_SECURITY_INFORMATION;
    if (do_owner)    si |= OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION;

    PSID                 owner_sid = nullptr;
    PSID                 group_sid = nullptr;
    PACL                 dacl      = nullptr;
    PSECURITY_DESCRIPTOR sd        = nullptr;

    DWORD err = GetNamedSecurityInfoW(
        src.c_str(), SE_FILE_OBJECT, si,
        do_owner    ? &owner_sid : nullptr,
        do_owner    ? &group_sid : nullptr,
        do_security ? &dacl      : nullptr,
        nullptr, &sd
    );

    if (err != ERROR_SUCCESS) {
        out.message(output::Level::warn,
            L"[copy.copy_security_info] Failed to read security from '" + src +
            L"': " + util::get_win32_error_message(err));
        return;
    }

    err = SetNamedSecurityInfoW(
        const_cast<LPWSTR>(dest.c_str()), SE_FILE_OBJECT, si,
        do_owner    ? owner_sid : nullptr,
        do_owner    ? group_sid : nullptr,
        do_security ? dacl      : nullptr,
        nullptr
    );

    LocalFree(sd);

    if (err != ERROR_SUCCESS) {
        out.message(output::Level::warn,
            L"[copy.copy_security_info] Failed to apply security to '" + dest +
            L"': " + util::get_win32_error_message(err));
    }
}

/**
 * @brief Applies post-copy metadata to the destination file.
 *
 * Applies each component selected by -c:X and -ca:X:
 *   A - file attribute bits (subset from -ca:)
 *   T - timestamps (creation, access, write, change)
 *   S - DACL (security / ACLs)
 *   O - owner SID and primary group
 *
 * All failures are emitted as warnings; the data copy already succeeded.
 *
 * @param src   Source file path (metadata is read from here).
 * @param dest  Destination file path (metadata is applied here).
 * @param args  CLI arguments.
 * @param out   Output interface for warning messages.
 */
void copy_file_metadata(
    const std::wstring& src,
    const std::wstring& dest,
    const util::CliArg& args,
    output::IOutput& out
) {
    auto ta_ok = copy_timestamps_and_attrs(src, dest, args);
    if (!ta_ok) {
        out.message(output::Level::warn,
            L"[copy.copy_file_metadata] Attribute/timestamp copy failed for '" +
            dest + L"': " + ta_ok.error());
    }
    copy_security_info(src, dest, args, out);
}

// ============================================================================
// Retry-Aware Copy Wrapper
// ============================================================================

/**
 * @brief Copies a single file with skip-if-exists, retry-on-failure, and
 *        selective metadata application.
 *
 * Execution order:
 *   1. If -xs is set and the destination is up-to-date (same size and
 *      last-write time), skip the file and return immediately.
 *   2. If 'D' is in -c:X, run the copy strategy with the retry loop.
 *   3. Apply file metadata (A, T, S, O) from -c:X unless dry-run.
 *
 * @param src      Absolute source file path.
 * @param dest     Absolute destination file path.
 * @param args     CLI arguments.
 * @param context  Shared copy context.
 * @return true on success, or the last error string on final failure.
 */
std::expected<bool, std::wstring> retry_copy_file(
    const std::wstring& src,
    const std::wstring& dest,
    const util::CliArg& args,
    CopyContext& context
) {
    // ── Skip-if-exists check ──────────────────────────────────────────────────
    if (args.skip_existing && should_skip_file(src, dest)) {
        context.out->message(output::Level::info,
            L"[copy.retry_copy_file] Skipping (destination up-to-date): " + dest);
        context.stats.skipped_files++;
        context.stats.total_files++;
        return true;
    }

    // ── Data copy (strategy with retry loop) ─────────────────────────────────
    if (has_component(args, L'D')) {
        int32_t attempts_remaining = args.retry_count;
        int32_t attempt_num        = 0;
        std::wstring last_error;

        while (true) {
            ++attempt_num;
            auto result = context.strategy->copy_file(src, dest, args, context);
            if (result) break; // Data copy succeeded

            if (util::g_cancel_requested) return result;

            last_error = result.error();

            if (attempts_remaining <= 0) {
                if (args.retry_count == 0) {
                    context.out->message(output::Level::error,
                        L"[copy.retry_copy_file] Copy failed (no retry configured): "
                        + src + L" -> " + dest + L"\n  Error: " + last_error);
                } else {
                    context.out->message(output::Level::error,
                        L"[copy.retry_copy_file] Copy failed after all retries exhausted: "
                        + src + L" -> " + dest + L"\n  Final error: " + last_error);
                }
                return std::unexpected(last_error);
            }

            context.out->message(output::Level::warn,
                L"[copy.retry_copy_file] Copy attempt " + std::to_wstring(attempt_num)
                + L" of " + std::to_wstring(args.retry_count + 1)
                + L" failed for: " + src + L" -> " + dest
                + L"\n  Error: " + last_error
                + L"\n  Retries remaining: " + std::to_wstring(attempts_remaining));

            if (!args.dry_run) DeleteFileW(dest.c_str());

            if (args.retry_wait > 0) {
                context.out->message(output::Level::warn,
                    L"[copy.retry_copy_file] Waiting " + std::to_wstring(args.retry_wait)
                    + L" second(s) before retry "
                    + std::to_wstring(attempt_num + 1)
                    + L" of " + std::to_wstring(args.retry_count + 1) + L"...");
                Sleep(static_cast<DWORD>(args.retry_wait) * 1000UL);
            } else {
                context.out->message(output::Level::warn,
                    L"[copy.retry_copy_file] Retrying immediately (attempt "
                    + std::to_wstring(attempt_num + 1)
                    + L" of " + std::to_wstring(args.retry_count + 1) + L")...");
            }

            --attempts_remaining;
        }
    } else {
        // D not in components: metadata-only mode.
        // The destination must already exist (we have no data to write).
        if (!args.dry_run) {
            if (GetFileAttributesW(dest.c_str()) == INVALID_FILE_ATTRIBUTES) {
                context.out->message(output::Level::warn,
                    L"[copy.retry_copy_file] 'D' not in copy components and destination does "
                    L"not exist: " + dest + L". Skipping.");
                return true;
            }
        }
        context.stats.total_files++;
    }

    // ── Metadata copy (A, T, S, O) ────────────────────────────────────────────
    if (!args.dry_run) {
        copy_file_metadata(src, dest, args, *context.out);
    }

    return true;
}

// ============================================================================
// Phase 2: Gather - Directory Enumeration and LCN Collection
// ============================================================================

/**
 * @brief Creates a directory and all required parent directories (mkdir -p semantics).
 *
 * Silently succeeds if the directory already exists. Used in execute() to
 * lazily create destination directory structure before copying each file.
 */
static void create_directory_recursive(const std::wstring& path) {
    if (path.size() <= 3) return; // Drive root (e.g. "C:\")
    if (CreateDirectoryW(path.c_str(), NULL) || GetLastError() == ERROR_ALREADY_EXISTS) return;

    // Create parent, then retry this directory
    size_t last_sep = path.find_last_of(L"\\/");
    if (last_sep != std::wstring::npos && last_sep > 2) {
        create_directory_recursive(path.substr(0, last_sep));
        CreateDirectoryW(path.c_str(), NULL);
    }
}

/**
 * @brief Queries retrieval pointers for all source files and counts unique source LCNs.
 *
 * Only called for cross-volume ReFS copies where the lcn_map dedup mechanism is active.
 * The resulting count is stored in ctx.total_unique_src_lcns for use as the overall
 * progress bar denominator. Also reserves dest_path_table capacity to minimize
 * hash-table rehashing during execute().
 *
 * Claims accumulate per extent, not per cluster, so this stays cheap even at
 * 31M+ clusters - only fragment count drives memory here, not data volume.
 *
 * @return Unique source LCN count, or error string on unexpected failure.
 */
std::expected<ULONGLONG, std::wstring> gather_src_lcns(CopyContext& ctx, output::IOutput& out) {
    const ULONGLONG total_files = static_cast<ULONGLONG>(ctx.file_pairs.size());
    ULONGLONG files_done = 0;

    out.message(output::Level::info,
        L"[gather] Collecting source LCN map (" + std::to_wstring(total_files) + L" files)...");

    std::vector<inspect::LcnClaim> claims;

    for (const auto& pair : ctx.file_pairs) {
        if (util::g_cancel_requested) break;

        out.progress(L"scanning sources", files_done, total_files);

        auto h_result = open_source_file(pair.src);
        if (!h_result) {
            ctx.stats.errors.push_back(pair.src + L": " + h_result.error());
            ++files_done;
            continue;
        }
        ScopedHandle h = std::move(*h_result);

        auto extents_res = inspect::collect_extents(h.get());
        if (!extents_res) {
            ctx.stats.errors.push_back(pair.src + L": " + extents_res.error());
            ++files_done;
            continue;
        }

        for (const auto& ext : *extents_res) {
            if (ext.is_sparse()) continue;
            claims.push_back({0, ext.lcn, ext.lcn + static_cast<LONGLONG>(ext.cluster_count()), 0});
        }

        ++files_done;
    }

    out.progress(L"scanning sources", total_files, total_files);

    // File identity doesn't matter for a pure distinct-LCN count, so every claim
    // shares file_index 0; the interval index just needs to know which LCNs are
    // covered by *some* source file, not which one specifically.
    auto interval_index = inspect::build_lcn_interval_index(
        std::move(claims), ctx.src_cluster_size, inspect::ClaimOccurrence::kFirst);
    ULONGLONG unique_lcns = 0;
    for (const auto& iv : interval_index) {
        unique_lcns += static_cast<ULONGLONG>(iv.end_lcn - iv.start_lcn);
    }

    ctx.dest_path_table.reserve(ctx.file_pairs.size());

    out.message(output::Level::info,
        L"[gather] Source scan complete: " + std::to_wstring(unique_lcns) +
        L" unique LCNs across " + std::to_wstring(files_done) + L" files.");

    return unique_lcns;
}

/**
 * @brief Phase 2 - Enumerate files, collect source LCNs, and optionally pre-scan destination.
 *
 * Populates ctx.file_pairs from the source tree. For cross-volume ReFS copies,
 * runs gather_src_lcns() to count unique source LCNs for the overall progress bar
 * and to pre-reserve lcn_map capacity. Runs seed_from_dest_scan() when --scan-dest
 * is active, moving all large allocations out of execute().
 */
std::expected<bool, std::wstring> gather(CopyContext& ctx, output::IOutput& out) {
    ctx.out = &out;
    const util::CliArg& args = ctx.args_snapshot;

    // Build the flat file-pair list (pure discovery, no filesystem writes)
    if (ctx.is_directory) {
        out.message(output::Level::info, L"[gather] Enumerating source files...");

        // util::enumerate_files_recursive requires a trailing separator and is
        // unconditionally recursive - both fine here, since prepare() already
        // rejected a directory source without -r.
        std::wstring src_dir = ctx.src_path + L"\\";
        std::vector<std::wstring> src_files;
        util::enumerate_files_recursive(src_dir, src_files, ctx.stats.errors);

        for (const auto& src_file : src_files) {
            std::wstring relative = src_file.substr(src_dir.size());
            ctx.file_pairs.push_back({src_file, ctx.dest_path + L"\\" + relative});
        }
    } else {
        ctx.file_pairs.push_back({ctx.src_path, ctx.dest_path});
    }

    out.message(output::Level::info,
        L"[gather] Found " + std::to_wstring(ctx.file_pairs.size()) + L" file(s) to copy.");

    // Collect source LCNs for the overall progress bar (cross-volume ReFS only).
    // This is the condition that selects CrossVolumeRefsCopyStrategy in prepare().
    const bool do_lcn_gather = ctx.src_is_refs && !ctx.same_volume &&
                               ctx.dest_is_refs && ctx.src_cluster_size == ctx.dest_cluster_size;
    if (do_lcn_gather && !ctx.file_pairs.empty()) {
        auto lcn_count = gather_src_lcns(ctx, out);
        if (lcn_count) {
            ctx.total_unique_src_lcns = *lcn_count;
        }
        // Non-fatal: if this fails the overall bar simply won't appear
    }

    // Pre-scan destination for dedup seeding (moved from execute() to keep all
    // large allocations in the gather phase)
    if (args.scan_dest && !ctx.same_volume && ctx.dest_is_refs) {
        auto seed_ok = seed_from_dest_scan(ctx);
        if (!seed_ok) {
            out.message(output::Level::warn,
                L"Destination pre-scan failed: " + seed_ok.error() +
                L". Continuing without scan-dest seeding.");
        }
    }

    return true;
}

// ============================================================================
// Phase 3: Finalization - Summary & Cleanup
// ============================================================================

/**
 * @brief Phase 3: Prints copy summary statistics and returns exit code.
 *
 * @param context The fully-processed CopyContext with accumulated stats.
 * @return Exit code (0 = success, 2 = errors occurred).
 */
int finalize_and_report(CopyContext& context) {
    const auto& stats = context.stats;
    auto& out = *context.out;

    out.begin_section(L"retool copy summary");
    out.field(L"Total Files",     std::to_wstring(stats.total_files));
    out.field(L"Cloned Files",    std::to_wstring(stats.cloned_files));
    out.field(L"Fallback Copies", std::to_wstring(stats.fallback_files));
    out.field(L"Skipped Files",   std::to_wstring(stats.skipped_files));
    out.field(L"Total Bytes",     util::format_size_detailed(stats.total_bytes));

    for (const auto& err : stats.errors) {
        out.message(output::Level::error, err);
    }

    out.end_section();

    return stats.errors.empty() ? 0 : 2;
}

// ============================================================================
// Pipeline Entry Point
// ============================================================================

/**
 * @brief Phase 3 - Copies source file(s) or directories to the destination.
 *
 * Iterates ctx.file_pairs populated by gather(). Destination directories are
 * created lazily (just-in-time before each file), so the gather phase remains
 * read-only. No new large data structures are allocated here; lcn_map and
 * dest_path_table grow within their pre-reserved capacity from gather().
 *
 * @param ctx  Context produced by prepare() and populated by gather().
 * @param out  Output interface for formatted results and progress.
 * @return Exit code on success, or error string on failure.
 */
std::expected<int, std::wstring> execute(CopyContext& ctx, output::IOutput& out) {
    ctx.out = &out;
    const util::CliArg& args = ctx.args_snapshot;

    for (const auto& pair : ctx.file_pairs) {
        if (util::g_cancel_requested) break;

        // Lazily create the destination directory tree before each file
        if (!args.dry_run) {
            size_t sep = pair.dest.find_last_of(L"\\/");
            if (sep != std::wstring::npos) {
                create_directory_recursive(pair.dest.substr(0, sep));
            }
        }

        auto result = retry_copy_file(pair.src, pair.dest, args, ctx);
        if (!result) {
            if (util::g_cancel_requested) break;
            ctx.stats.errors.push_back(pair.src + L": " + result.error());
            if (args.strict) return std::unexpected(result.error());
        }
    }

    return finalize_and_report(ctx);
}

/**
 * @brief Phase 3 - Releases all resources held by the context and deletes it.
 */
void cleanup(CopyContext* ctx) noexcept {
    delete ctx;
}

} // namespace copy
