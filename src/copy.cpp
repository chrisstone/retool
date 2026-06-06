#include <algorithm>
#include <atomic>
#include <expected>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <windows.h>

#include "copy.h"
#include "inspect.h"
#include "output.h"
#include "util.h"

namespace copy {

std::atomic<bool> g_cancel_requested{false};
BOOL g_cancel_requested_bool = FALSE;

// ============================================================================
// Data Structures
// ============================================================================

/// @brief Accumulated statistics for a copy operation.
struct CopyStats {
    ULONGLONG total_files = 0;
    ULONGLONG cloned_files = 0;
    ULONGLONG fallback_files = 0;
    ULONGLONG total_bytes = 0;
    std::vector<std::wstring> errors;
};

/// @brief RAII wrapper for Win32 HANDLE lifetime management.
struct ScopedHandle {
    HANDLE handle = INVALID_HANDLE_VALUE;

    explicit ScopedHandle(HANDLE h = INVALID_HANDLE_VALUE) : handle(h) {}
    ~ScopedHandle() { close(); }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other) noexcept : handle(other.handle) {
        other.handle = INVALID_HANDLE_VALUE;
    }

    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) {
            close();
            handle = other.handle;
            other.handle = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    void close() {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
        }
    }

    explicit operator bool() const { return handle != INVALID_HANDLE_VALUE; }
    HANDLE get() const { return handle; }

    HANDLE release() {
        HANDLE h = handle;
        handle = INVALID_HANDLE_VALUE;
        return h;
    }
};

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

/// @brief Shared state for the entire copy pipeline.
struct CopyContext {
    // -- Populated during Phase 1: Inspection --
    std::wstring src_path;              ///< Resolved absolute source path.
    std::wstring dest_path;             ///< Resolved absolute destination path.
    bool is_directory = false;          ///< True if source is a directory.

    std::wstring src_volume_root;       ///< Source volume root (e.g., L"E:\\").
    std::wstring dest_volume_root;      ///< Destination volume root (e.g., L"F:\\").
    bool same_volume = false;           ///< True if src and dest are on the same volume.
    bool dest_is_refs = false;          ///< True if destination is ReFS.
    DWORD src_cluster_size = 0;         ///< Source volume cluster size in bytes.
    DWORD dest_cluster_size = 0;        ///< Destination volume cluster size in bytes.

    std::unique_ptr<ICopyStrategy> strategy;  ///< Selected copy strategy.
    output::IOutput* out = nullptr;           ///< Non-owning pointer to the output interface.

    // -- Populated during Phase 2: Operation --
    /// Interned destination paths: maps index -> path string. Avoids storing
    /// a full wstring copy per lcn_map entry (saves ~5 GB at 31M entries).
    std::vector<std::wstring> dest_path_table;

    /// Map: source LCN -> {dest_path_index, dest_offset_in_bytes}
    std::unordered_map<LONGLONG, std::pair<uint32_t, ULONGLONG>> lcn_map;

    /// Destination LCN index seeded by --scan-dest pre-scan; maps dest LCN -> file+offset.
    inspect::LcnIndex dest_lcn_index;

    /// Interned file paths from the destination scan (resolves BlockEntry::file_index).
    std::vector<std::wstring> dest_scan_file_table;

    /// Content hash index seeded by --scan-dest pre-scan (kWithHash); maps SHA-256 -> dest LCNs.
    inspect::HashIndex hash_index;

    /// Reusable I/O buffer for copy_bytes_physical (allocated once, 4 MB).
    std::vector<BYTE> io_buffer;

    CopyStats stats;
    HandleLruCache handle_cache;

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
 * @brief Checks if the given volume root path is formatted with the ReFS filesystem.
 *
 * @param volume_root The root path of the volume (e.g. L"C:\\").
 * @return true If the volume is ReFS.
 * @return false If the volume is not ReFS or information could not be retrieved.
 */
bool is_refs_volume(const std::wstring& volume_root) {
    wchar_t fs_name[MAX_PATH] = {0};
    if (GetVolumeInformationW(volume_root.c_str(), NULL, 0, NULL, NULL, NULL, fs_name, MAX_PATH)) {
        return (wcscmp(fs_name, L"ReFS") == 0);
    }
    return false;
}

/**
 * @brief Retrieves the cluster allocation size for the given volume.
 *
 * @param volume_root The root path of the volume.
 * @return DWORD The cluster size in bytes, or 0 if retrieval fails.
 */
DWORD get_cluster_size(const std::wstring& volume_root) {
    DWORD sectors_per_cluster = 0, bytes_per_sector = 0, free_clusters = 0, total_clusters = 0;
    if (GetDiskFreeSpaceW(volume_root.c_str(), &sectors_per_cluster, &bytes_per_sector, &free_clusters, &total_clusters)) {
        return sectors_per_cluster * bytes_per_sector;
    }
    return 0;
}

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

    LARGE_INTEGER li;
    li.QuadPart = src_size;
    if (SetFilePointerEx(dest_handle, li, NULL, FILE_BEGIN) && SetEndOfFile(dest_handle)) {
        tracker.current_eof = src_size;
    } else {
        tracker.incremental = true;
    }

    // Seek back to beginning regardless
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    if (!SetFilePointerEx(dest_handle, zero, NULL, FILE_BEGIN)) {
        return std::unexpected(L"Failed to reset destination file pointer: " + util::get_win32_error_message(GetLastError()));
    }

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

    LARGE_INTEGER li;
    li.QuadPart = required_size;
    if (!SetFilePointerEx(dest_handle, li, NULL, FILE_BEGIN) || !SetEndOfFile(dest_handle)) {
        return std::unexpected(L"Incremental sizing failed: " + util::get_win32_error_message(GetLastError()));
    }
    tracker.current_eof = required_size;
    return true;
}

/// @brief A single VCN extent with its LCN mapping.
struct Extent {
    LONGLONG vcn = 0;       ///< Starting VCN of this extent.
    LONGLONG next_vcn = 0;  ///< VCN of the next extent (or EOF).
    LONGLONG lcn = 0;       ///< Starting LCN (-1 for sparse).

    ULONGLONG cluster_count() const { return next_vcn - vcn; }
    bool is_sparse() const { return lcn == (LONGLONG)-1; }
};

/**
 * @brief Queries all retrieval pointers (extents) for a file.
 *
 * Iterates FSCTL_GET_RETRIEVAL_POINTERS until all extents are retrieved.
 *
 * @param file_handle  Handle to the file.
 * @return Vector of extents on success, or error message on failure.
 */
std::expected<std::vector<Extent>, std::wstring> query_retrieval_pointers(HANDLE file_handle) {
    std::vector<Extent> extents;

    STARTING_VCN_INPUT_BUFFER input = {0};
    input.StartingVcn.QuadPart = 0;

    const DWORD buf_size = sizeof(RETRIEVAL_POINTERS_BUFFER) + sizeof(RETRIEVAL_POINTERS_BUFFER::Extents[0]) * 16;
    std::vector<BYTE> buffer(buf_size);
    auto output = reinterpret_cast<PRETRIEVAL_POINTERS_BUFFER>(buffer.data());

    bool done = false;
    while (!done) {
        DWORD bytes_returned = 0;
        BOOL ok = DeviceIoControl(
            file_handle,
            FSCTL_GET_RETRIEVAL_POINTERS,
            &input,
            sizeof(input),
            output,
            buf_size,
            &bytes_returned,
            NULL
        );

        DWORD err = GetLastError();
        if (!ok && err != ERROR_MORE_DATA) {
            if (err == ERROR_HANDLE_EOF) break;
            return std::unexpected(L"FSCTL_GET_RETRIEVAL_POINTERS failed: " + util::get_win32_error_message(err));
        }

        if (ok) done = true;

        LONGLONG current_vcn = output->StartingVcn.QuadPart;
        for (DWORD i = 0; i < output->ExtentCount; ++i) {
            Extent ext;
            ext.vcn = current_vcn;
            ext.next_vcn = output->Extents[i].NextVcn.QuadPart;
            ext.lcn = output->Extents[i].Lcn.QuadPart;
            extents.push_back(ext);
            current_vcn = ext.next_vcn;
        }

        input.StartingVcn.QuadPart = extents.empty() ? 0 : extents.back().next_vcn;
    }

    return extents;
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
    DUPLICATE_EXTENTS_DATA dup_data;
    dup_data.FileHandle = src_handle;
    dup_data.SourceFileOffset.QuadPart = src_offset;
    dup_data.TargetFileOffset.QuadPart = dest_offset;
    dup_data.ByteCount.QuadPart = byte_count;

    DWORD dup_returned = 0;
    if (!DeviceIoControl(dest_handle, FSCTL_DUPLICATE_EXTENTS_TO_FILE, &dup_data, sizeof(dup_data), NULL, 0, &dup_returned, NULL)) {
        return std::unexpected(L"FSCTL_DUPLICATE_EXTENTS_TO_FILE failed: " + util::get_win32_error_message(GetLastError()));
    }
    return true;
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
    DUPLICATE_EXTENTS_DATA dup_data;
    dup_data.FileHandle = prev_dest_handle;
    dup_data.SourceFileOffset.QuadPart = prev_dest_offset;
    dup_data.TargetFileOffset.QuadPart = dest_offset;
    dup_data.ByteCount.QuadPart = byte_count;

    DWORD dup_returned = 0;
    if (!DeviceIoControl(dest_handle, FSCTL_DUPLICATE_EXTENTS_TO_FILE, &dup_data, sizeof(dup_data), NULL, 0, &dup_returned, NULL)) {
        DWORD err = GetLastError();
        return std::unexpected(L"FSCTL_DUPLICATE_EXTENTS_TO_FILE failed on target volume (SourceOffset="
            + std::to_wstring(dup_data.SourceFileOffset.QuadPart)
            + L", TargetOffset=" + std::to_wstring(dup_data.TargetFileOffset.QuadPart)
            + L", ByteCount=" + std::to_wstring(dup_data.ByteCount.QuadPart)
            + L"): " + util::get_win32_error_message(err));
    }
    return true;
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
        if (g_cancel_requested) {
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
 * @brief Sets the final destination file size and copies timestamps/attributes.
 *
 * @param dest_handle  Handle to the destination file.
 * @param src_handle   Handle to the source file (for reading attributes).
 * @param src_size     The exact file size to set on the destination.
 * @return true on success, or error message on failure.
 */
std::expected<bool, std::wstring> finalize_dest_file(HANDLE dest_handle, HANDLE src_handle, LONGLONG src_size) {
    // Truncate/extend destination to exact source size
    LARGE_INTEGER li;
    li.QuadPart = src_size;
    if (!SetFilePointerEx(dest_handle, li, NULL, FILE_BEGIN)) {
        return std::unexpected(L"Failed to set file pointer: " + util::get_win32_error_message(GetLastError()));
    }
    if (!SetEndOfFile(dest_handle)) {
        return std::unexpected(L"SetEndOfFile failed: " + util::get_win32_error_message(GetLastError()));
    }

    // Copy file times and attributes
    FILE_BASIC_INFO basic_info;
    if (GetFileInformationByHandleEx(src_handle, FileBasicInfo, &basic_info, sizeof(basic_info))) {
        SetFileInformationByHandle(dest_handle, FileBasicInfo, &basic_info, sizeof(basic_info));
    }

    return true;
}

// ============================================================================
// Copy Strategies
// ============================================================================

/**
 * @brief Fallback strategy using standard CopyFileExW.
 *
 * Used when the destination is non-ReFS or cluster sizes are mismatched,
 * making block cloning impossible.
 */
struct FallbackCopyStrategy : ICopyStrategy {
    std::expected<bool, std::wstring> copy_file(
        const std::wstring& src,
        const std::wstring& dest,
        const util::CliArg& args,
        CopyContext& context
    ) override {
        if (args.dry_run) {
            context.out->status(L"[DRY-RUN] Would copy with standard fallback: " + src + L" -> " + dest);
            context.stats.fallback_files++;
            context.stats.total_files++;
            return true;
        }

        if (!context.dest_is_refs) {
            context.out->warn(L"Destination volume is non-ReFS. Falling back to standard copy for: " + src);
        } else {
            context.out->warn(L"Destination volume cluster size mismatch ("
                + std::to_wstring(context.dest_cluster_size) + L" vs " + std::to_wstring(context.src_cluster_size)
                + L"). Falling back to standard copy for: " + src);
        }

        // Use progress callback for CopyFileExW
        ProgressContext prog_ctx{src, context.out};
        if (!CopyFileExW(src.c_str(), dest.c_str(), &copy_progress_callback, &prog_ctx, &g_cancel_requested_bool, COPY_FILE_ALLOW_DECRYPTED_DESTINATION)) {
            DWORD error = GetLastError();
            return std::unexpected(L"Fallback CopyFileExW failed: " + util::get_win32_error_message(error));
        }

        // Report completion
        if (prog_ctx.total_size > 0) {
            context.out->progress(src, prog_ctx.total_size, prog_ctx.total_size);
        }

        context.stats.fallback_files++;
        context.stats.total_files++;
        context.stats.total_bytes += prog_ctx.total_size;
        return true;
    }

private:
    /// @brief Context passed to CopyFileExW progress callback.
    struct ProgressContext {
        std::wstring filename;
        output::IOutput* out;
        ULONGLONG total_size = 0;
    };

    /// @brief CopyFileExW progress routine that forwards to IOutput::progress.
    static DWORD CALLBACK copy_progress_callback(
        LARGE_INTEGER TotalFileSize,
        LARGE_INTEGER TotalBytesTransferred,
        LARGE_INTEGER /*StreamSize*/,
        LARGE_INTEGER /*StreamBytesTransferred*/,
        DWORD /*dwStreamNumber*/,
        DWORD /*dwCallbackReason*/,
        HANDLE /*hSourceFile*/,
        HANDLE /*hDestinationFile*/,
        LPVOID lpData
    ) {
        auto* ctx = static_cast<ProgressContext*>(lpData);
        ctx->total_size = TotalFileSize.QuadPart;
        if (g_cancel_requested) {
            return PROGRESS_CANCEL;
        }
        if (ctx->out) {
            ctx->out->progress(ctx->filename,
                static_cast<ULONGLONG>(TotalBytesTransferred.QuadPart),
                static_cast<ULONGLONG>(TotalFileSize.QuadPart));
        }
        return PROGRESS_CONTINUE;
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
            context.out->status(L"[DRY-RUN] Would clone (same volume): " + src + L" -> " + dest);
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
            if (g_cancel_requested) {
                DeleteFileW(dest.c_str());
                return std::unexpected(L"Copy cancelled by user.");
            }
            return attempt_fallback(src, dest, extents_result.error(), context);
        }

        // Clone each extent
        ULONGLONG bytes_cloned = 0;
        for (const auto& ext : *extents_result) {
            if (ext.is_sparse()) continue;

            if (g_cancel_requested) {
                dest_handle.close();
                src_handle.close();
                DeleteFileW(dest.c_str());
                return std::unexpected(L"Copy cancelled by user.");
            }

            ULONGLONG src_offset = ext.vcn * context.src_cluster_size;
            ULONGLONG byte_count = ext.cluster_count() * context.src_cluster_size;
            ULONGLONG required_size = src_offset + byte_count;

            auto size_ok = ensure_dest_size(dest_handle.get(), tracker, required_size);
            if (!size_ok) {
                dest_handle.close();
                src_handle.close();
                if (g_cancel_requested) {
                    DeleteFileW(dest.c_str());
                    return std::unexpected(L"Copy cancelled by user.");
                }
                return attempt_fallback(src, dest, size_ok.error(), context);
            }

            auto clone_ok = clone_extent_same_volume(dest_handle.get(), src_handle.get(), src_offset, src_offset, byte_count);
            if (!clone_ok) {
                dest_handle.close();
                src_handle.close();
                if (g_cancel_requested) {
                    DeleteFileW(dest.c_str());
                    return std::unexpected(L"Copy cancelled by user.");
                }
                return attempt_fallback(src, dest, clone_ok.error(), context);
            }

            bytes_cloned += byte_count;
            context.out->progress(src, bytes_cloned, static_cast<ULONGLONG>(src_size.QuadPart));
        }

        // Finalize
        auto final_ok = finalize_dest_file(dest_handle.get(), src_handle.get(), src_size.QuadPart);
        if (!final_ok) {
            dest_handle.close();
            src_handle.close();
            if (g_cancel_requested) {
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
     * @brief Attempts a standard copy fallback after a clone failure.
     *
     * Deletes the partially-written destination file and falls back to CopyFileExW.
     */
    std::expected<bool, std::wstring> attempt_fallback(
        const std::wstring& src, const std::wstring& dest,
        const std::wstring& original_error, CopyContext& context
    ) {
        DeleteFileW(dest.c_str());
        if (g_cancel_requested) {
            return std::unexpected(L"Copy cancelled by user.");
        }
        context.out->warn(L"Deduplication-preserving copy failed (" + original_error
            + L"). Falling back to standard copy for: " + src);

        if (!CopyFileExW(src.c_str(), dest.c_str(), NULL, NULL, &g_cancel_requested_bool, COPY_FILE_ALLOW_DECRYPTED_DESTINATION)) {
            DWORD error = GetLastError();
            return std::unexpected(L"Dedup copy failed (" + original_error + L"), and fallback copy failed: " + util::get_win32_error_message(error));
        }

        // Query file size for stats tracking
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExW(src.c_str(), GetFileExInfoStandard, &fad)) {
            ULONGLONG sz = (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
            context.stats.total_bytes += sz;
        }

        context.stats.fallback_files++;
        context.stats.total_files++;
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
            context.out->status(L"[DRY-RUN] Would copy preserving deduplication (cross volume ReFS): "
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
            if (g_cancel_requested) {
                DeleteFileW(dest.c_str());
                return std::unexpected(L"Copy cancelled by user.");
            }
            return attempt_fallback(src, dest, extents_result.error(), context);
        }

        // Process each extent cluster-by-cluster
        ULONGLONG bytes_processed = 0;
        for (const auto& ext : *extents_result) {
            if (ext.is_sparse()) continue;

            if (g_cancel_requested) {
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
                if (g_cancel_requested) {
                    DeleteFileW(dest.c_str());
                    return std::unexpected(L"Copy cancelled by user.");
                }
                return attempt_fallback(src, dest, result.error(), context);
            }
        }

        // Finalize
        auto final_ok = finalize_dest_file(dest_handle.get(), src_handle.get(), src_size.QuadPart);
        if (!final_ok) {
            dest_handle.close();
            src_handle.close();
            if (g_cancel_requested) {
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
            if (g_cancel_requested) {
                return std::unexpected(L"Copy cancelled by user.");
            }
            LONGLONG current_lcn = ext.lcn + c_offset;
            ULONGLONG src_offset = (ext.vcn + c_offset) * context.src_cluster_size;

            auto map_it = context.lcn_map.find(current_lcn);
            if (map_it != context.lcn_map.end()) {
                // Duplicate LCN: clone from previously-copied destination
                ULONGLONG pre_offset = c_offset;
                auto result = clone_duplicate_run(
                    ext, c_offset, map_it->second, src_offset,
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

            // Report progress
            context.out->progress(src_path, bytes_processed, static_cast<ULONGLONG>(src_file_size));
        }
        return true;
    }

    /**
     * @brief Clones a contiguous run of duplicate LCNs from a previously-copied destination file.
     *
     * Scans forward from c_offset to find how many consecutive LCNs map to the
     * same contiguous region, then clones the entire run in one ioctl.
     *
     * @param c_offset Updated in-place to advance past the cloned run.
     */
    std::expected<bool, std::wstring> clone_duplicate_run(
        const Extent& ext, ULONGLONG& c_offset,
        const std::pair<uint32_t, ULONGLONG>& master,
        ULONGLONG src_offset, HANDLE dest_handle,
        DestSizeTracker& tracker, CopyContext& context
    ) {
        // Find contiguous run length
        ULONGLONG run_clusters = 1;
        while (c_offset + run_clusters < ext.cluster_count()) {
            LONGLONG next_lcn = ext.lcn + c_offset + run_clusters;
            auto next_it = context.lcn_map.find(next_lcn);
            if (next_it == context.lcn_map.end()) break;

            const auto& next_master = next_it->second;
            if (next_master.first != master.first ||
                next_master.second != master.second + run_clusters * context.src_cluster_size) {
                break;
            }
            run_clusters++;
        }

        ULONGLONG byte_count = run_clusters * context.src_cluster_size;
        auto size_ok = ensure_dest_size(dest_handle, tracker, src_offset + byte_count);
        if (!size_ok) return std::unexpected(size_ok.error());

        // Resolve interned path and get handle to the previously-copied destination file
        const std::wstring& master_path = context.resolve_path(master.first);
        HANDLE prev_dest_handle = context.handle_cache.get(master_path);
        if (prev_dest_handle == INVALID_HANDLE_VALUE) {
            return std::unexpected(L"Failed to open previously copied destination file: " + master_path);
        }

        auto clone_ok = clone_extent_from_dest(dest_handle, prev_dest_handle, master.second, src_offset, byte_count);
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

            // Record LCNs in the map for future dedup (using interned path index)
            uint32_t path_idx = context.intern_path(dest_path);
            for (ULONGLONG k = 0; k < run_clusters; ++k) {
                context.lcn_map[ext.lcn + c_offset + k] = {path_idx, src_offset + k * context.src_cluster_size};
            }

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

        // Seek source to beginning of this run
        LARGE_INTEGER li_src;
        li_src.QuadPart = src_offset;
        if (!SetFilePointerEx(src_handle, li_src, NULL, FILE_BEGIN)) {
            return std::unexpected(L"SetFilePointerEx failed: " + util::get_win32_error_message(GetLastError()));
        }

        for (ULONGLONG k = 0; k < run_clusters; ++k) {
            if (g_cancel_requested) {
                return std::unexpected(L"Copy cancelled by user.");
            }

            ULONGLONG cluster_offset = src_offset + k * cluster_size;
            ULONGLONG bytes_remaining = 0;
            if (static_cast<ULONGLONG>(src_file_size) > cluster_offset) {
                bytes_remaining = static_cast<ULONGLONG>(src_file_size) - cluster_offset;
            }
            DWORD to_read = static_cast<DWORD>(std::min<ULONGLONG>(cluster_size, bytes_remaining));

            if (to_read == 0) {
                // Past EOF — record in map and skip
                context.lcn_map[ext.lcn + c_offset + k] = {path_idx, cluster_offset};
                continue;
            }

            // Read source cluster into io_buffer
            DWORD bytes_read = 0;
            if (!ReadFile(src_handle, context.io_buffer.data(), to_read, &bytes_read, NULL) || bytes_read == 0) {
                return std::unexpected(L"ReadFile failed during hash matching: " + util::get_win32_error_message(GetLastError()));
            }

            // Hash the cluster
            std::string digest = inspect::compute_sha256(context.io_buffer.data(), bytes_read);
            bool cloned = false;

            if (!digest.empty()) {
                auto hash_it = context.hash_index.find(digest);
                if (hash_it != context.hash_index.end() && !hash_it->second.empty()) {
                    // Found a content match on the destination volume.
                    // Resolve the first matching destination LCN to a file+offset.
                    LONGLONG dest_lcn = hash_it->second[0];
                    auto lcn_it = context.dest_lcn_index.find(dest_lcn);
                    if (lcn_it != context.dest_lcn_index.end() && !lcn_it->second.empty()) {
                        const auto& block = lcn_it->second[0];
                        const std::wstring& block_path = context.dest_scan_file_table[block.file_index];
                        HANDLE match_handle = context.handle_cache.get(block_path);
                        if (match_handle != INVALID_HANDLE_VALUE) {
                            auto clone_ok = clone_extent_from_dest(
                                dest_handle, match_handle,
                                block.file_offset, cluster_offset, cluster_size
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
                // No match or clone failed — write physically
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

            // Record in lcn_map for future cross-file dedup within this copy operation
            context.lcn_map[ext.lcn + c_offset + k] = {path_idx, cluster_offset};

            // Report progress
            context.out->progress(src_path, cluster_offset + bytes_read, static_cast<ULONGLONG>(src_file_size));
        }

        c_offset += run_clusters;
        return true;
    }

    /**
     * @brief Attempts a standard copy fallback after a clone/copy failure.
     */
    std::expected<bool, std::wstring> attempt_fallback(
        const std::wstring& src, const std::wstring& dest,
        const std::wstring& original_error,
        CopyContext& context
    ) {
        DeleteFileW(dest.c_str());
        if (g_cancel_requested) {
            return std::unexpected(L"Copy cancelled by user.");
        }
        context.out->warn(L"Deduplication-preserving copy failed (" + original_error
            + L"). Falling back to standard copy for: " + src);

        if (!CopyFileExW(src.c_str(), dest.c_str(), NULL, NULL, &g_cancel_requested_bool, COPY_FILE_ALLOW_DECRYPTED_DESTINATION)) {
            DWORD error = GetLastError();
            return std::unexpected(L"Dedup copy failed (" + original_error + L"), and fallback copy failed: " + util::get_win32_error_message(error));
        }

        // Query file size for stats tracking
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExW(src.c_str(), GetFileExInfoStandard, &fad)) {
            ULONGLONG sz = (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
            context.stats.total_bytes += sz;
        }

        context.stats.fallback_files++;
        context.stats.total_files++;
        return true;
    }
};

// ============================================================================
// Phase 1: Inspection — Validate & Decorate Context
// ============================================================================

/**
 * @brief Phase 1: Validates inputs and populates the CopyContext.
 *
 * Resolves absolute paths, verifies source existence, queries volume
 * information (filesystem type, cluster size), and selects the appropriate
 * copy strategy based on volume topology.
 *
 * @param args  CLI arguments with positional source/dest paths.
 * @return Fully decorated CopyContext on success, or error string.
 */
std::expected<CopyContext, std::wstring> inspect_and_prepare(const util::CliArg& args) {
    if (args.positional.size() < 2) {
        return std::unexpected(L"Error: Missing source or destination path. Usage: retool copy <src> <dest> [options]");
    }

    CopyContext context;
    context.src_path = get_absolute_path(args.positional[0]);
    context.dest_path = get_absolute_path(args.positional[1]);

    // Validate source
    DWORD src_attr = GetFileAttributesW(context.src_path.c_str());
    if (src_attr == INVALID_FILE_ATTRIBUTES) {
        return std::unexpected(L"Source path does not exist: " + context.src_path);
    }

    context.is_directory = (src_attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (context.is_directory && !args.recursive) {
        return std::unexpected(L"Error: Source is a directory but -r (recursive) was not specified.");
    }

    // Resolve volume roots
    wchar_t src_volume[MAX_PATH];
    wchar_t dest_volume[MAX_PATH];
    if (!GetVolumePathNameW(context.src_path.c_str(), src_volume, MAX_PATH)) {
        return std::unexpected(L"Failed to get source volume root: " + util::get_win32_error_message(GetLastError()));
    }
    if (!GetVolumePathNameW(context.dest_path.c_str(), dest_volume, MAX_PATH)) {
        return std::unexpected(L"Failed to get destination volume root: " + util::get_win32_error_message(GetLastError()));
    }

    context.src_volume_root = src_volume;
    context.dest_volume_root = dest_volume;
    context.same_volume = (wcscmp(src_volume, dest_volume) == 0);
    context.dest_is_refs = is_refs_volume(dest_volume);
    context.src_cluster_size = get_cluster_size(src_volume);
    context.dest_cluster_size = get_cluster_size(dest_volume);

    // Allocate reusable I/O buffer (4 MB)
    context.io_buffer.resize(4 * 1024 * 1024);

    // Select strategy
    if (context.same_volume) {
        context.strategy = std::make_unique<SameVolumeCopyStrategy>();
    } else if (context.dest_is_refs && context.src_cluster_size == context.dest_cluster_size) {
        context.strategy = std::make_unique<CrossVolumeRefsCopyStrategy>();
    } else {
        context.strategy = std::make_unique<FallbackCopyStrategy>();
    }

    return std::move(context);
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
    context.out->status(L"[seed_from_dest_scan] Pre-scanning destination volume " +
                        context.dest_volume_root + L" for deduplication...");

    auto scan = inspect::build_lcn_index(
        context.dest_volume_root, inspect::ScanMode::kWithHash, *context.out);
    if (!scan) return std::unexpected(scan.error());

    // Store the destination LcnIndex and file_table so hash matches can be
    // resolved to destination file+offset for cloning.
    context.dest_lcn_index = std::move(scan->lcn_index);
    context.dest_scan_file_table = std::move(scan->file_table);
    context.hash_index = std::move(scan->hash_index);

    ULONGLONG hash_groups = 0;
    for (const auto& [digest, lcns] : context.hash_index) {
        if (lcns.size() >= 1) hash_groups++;
    }

    context.out->status(L"[seed_from_dest_scan] Done. " +
                        std::to_wstring(hash_groups) + L" unique hashes indexed from destination.");
    return true;
}

// ============================================================================
// Phase 2: Operation — Directory Recursion
// ============================================================================

/**
 * @brief Recursively copies a directory tree from source to destination.
 *
 * Replicates the directory structure and delegates file copying to the
 * strategy selected during Phase 1. Ignores system-attributed files and folders.
 *
 * @param src_dir   The source directory path.
 * @param dest_dir  The destination directory path.
 * @param args      The command line arguments.
 * @param context   The shared copy tracking context.
 * @return true on success, or error message on failure.
 */
std::expected<bool, std::wstring> copy_directory_recursive(
    const std::wstring& src_dir,
    const std::wstring& dest_dir,
    const util::CliArg& args,
    CopyContext& context
) {
    if (!args.dry_run) {
        if (!CreateDirectoryW(dest_dir.c_str(), NULL)) {
            DWORD error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS) {
                return std::unexpected(L"Failed to create directory " + dest_dir + L": " + util::get_win32_error_message(error));
            }
        }
    }

    std::wstring search_path = src_dir + L"\\*";
    WIN32_FIND_DATAW find_data;
    HANDLE find_handle = FindFirstFileW(search_path.c_str(), &find_data);

    if (find_handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        return std::unexpected(L"Failed to scan directory " + src_dir + L": " + util::get_win32_error_message(error));
    }

    do {
        if (g_cancel_requested) {
            FindClose(find_handle);
            return std::unexpected(L"Copy cancelled by user.");
        }

        std::wstring name = find_data.cFileName;
        if (name == L"." || name == L"..") continue;

        std::wstring src_item = src_dir + L"\\" + name;
        std::wstring dest_item = dest_dir + L"\\" + name;

        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) continue;

        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (args.recursive) {
                auto res = copy_directory_recursive(src_item, dest_item, args, context);
                if (!res) {
                    if (g_cancel_requested) {
                        FindClose(find_handle);
                        return std::unexpected(res.error());
                    }
                    context.stats.errors.push_back(src_item + L": " + res.error());
                    if (args.strict) {
                        FindClose(find_handle);
                        return std::unexpected(res.error());
                    }
                }
            }
        } else {
            auto res = context.strategy->copy_file(src_item, dest_item, args, context);
            if (!res) {
                if (g_cancel_requested) {
                    FindClose(find_handle);
                    return std::unexpected(res.error());
                }
                context.stats.errors.push_back(src_item + L": " + res.error());
                if (args.strict) {
                    FindClose(find_handle);
                    return std::unexpected(res.error());
                }
            }
        }

    } while (FindNextFileW(find_handle, &find_data));

    FindClose(find_handle);
    return true;
}

// ============================================================================
// Phase 3: Finalization — Summary & Cleanup
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
    out.field(L"Total Bytes",     std::to_wstring(stats.total_bytes));

    for (const auto& err : stats.errors) {
        out.error(err);
    }

    out.end_section();

    return stats.errors.empty() ? 0 : 2;
}

// ============================================================================
// Pipeline Entry Point
// ============================================================================

/**
 * @brief Executes the copy subcommand.
 *
 * Orchestrates a three-phase pipeline:
 *   1. Inspection — validate inputs, query volumes, select strategy.
 *   2. Operation — recursively copy files using the selected strategy.
 *   3. Finalization — print summary statistics, return exit code.
 *
 * @param args CLI arguments containing positional source and destination paths.
 * @return Exit code on success, or error string on failure.
 */
std::expected<int, std::wstring> execute_copy(const util::CliArg& args, output::IOutput& out) {
    // Phase 1: Inspection
    auto context = inspect_and_prepare(args);
    if (!context) return std::unexpected(context.error());

    context->out = &out;

    // Phase 1b: Optionally pre-scan destination to seed deduplication indexes
    if (args.scan_dest && !context->same_volume && context->dest_is_refs) {
        auto seed_ok = seed_from_dest_scan(*context);
        if (!seed_ok) {
            context->out->warn(L"Destination pre-scan failed: " + seed_ok.error() +
                               L". Continuing without scan-dest seeding.");
        }
    }

    // Phase 2: Operation
    if (context->is_directory) {
        auto result = copy_directory_recursive(context->src_path, context->dest_path, args, *context);
        if (!result && args.strict) return std::unexpected(result.error());
    } else {
        auto result = context->strategy->copy_file(context->src_path, context->dest_path, args, *context);
        if (!result) return std::unexpected(result.error());
    }

    // Phase 3: Finalization
    return finalize_and_report(*context);
}

} // namespace copy
