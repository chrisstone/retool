#pragma once

#include <atomic>
#include <expected>
#include <string>
#include <vector>

#include <windows.h>

namespace util {

/// @brief Discriminates the kind of a FileSpecifier entry.
enum class FileSpecKind {
    kVolume,  ///< Volume root: drive letter (X: or X:\), \\.\ device path, or \\?\Volume GUID.
    kPath,    ///< File path, directory path, or glob pattern.
};

/// @brief A single input specifier from the command line or a -i file list.
struct FileSpecifier {
    FileSpecKind kind;
    std::wstring path;  ///< Raw specifier as given (directories/globs not yet expanded).
};

struct CliArg {
    std::wstring command;
    std::vector<FileSpecifier> file_specs;        ///< Typed input specifiers (positional args + -i contents).
    bool     strict           = false;            ///< Abort on first error (-s).
    bool     dry_run          = false;            ///< Simulate without writing (-n).
    bool     recursive        = false;            ///< Recursive directory operation (-r).
    bool     json             = false;            ///< JSON output mode (-j).
    bool     quiet            = false;            ///< Suppress all output (-q).
    bool     scan_dest        = false;            ///< Pre-scan destination volume to seed dedup index (-d).
    bool     show_extents     = false;            ///< Extended mode: extent table / sharing matrix (-e).
    bool     skip_existing    = false;            ///< Skip file if dest exists with same size+date (-xs).
    int32_t  retry_count      = 3;               ///< Retries per file on copy failure (-t X; 0 = no retry).
    int32_t  retry_wait       = 30;              ///< Seconds to wait between retries (-w X; 0 = immediate).
    std::wstring copy_components = L"DATSO";     ///< What to copy per file (-c:DATSO).
    std::wstring copy_attr_mask  = L"RASH";      ///< File attribute bits to copy (-ca:RASH).
    std::wstring output_file;                    ///< Redirect output to this file (-o).
};

/// @brief RAII wrapper for Win32 HANDLE lifetime management.
///
/// Move-only; closes the handle on destruction, move-assignment (over the
/// previous handle), or explicit close(). release() relinquishes ownership
/// without closing, for callers that need to hand the raw HANDLE elsewhere.
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

// Check if current process has Administrator privileges
std::expected<bool, DWORD> is_elevated();

// Check if ReFS is supported by the OS (registry service key + driver binary presence)
std::expected<bool, DWORD> is_refs_supported();

/**
 * @brief Returns true if path is a volume specifier.
 *
 * Recognised forms:
 *  - X:       drive letter without trailing separator
 *  - X:\      drive letter with one trailing backslash or forward slash
 *  - \\.\... any Win32 device path (covers \\.\C:, \\.\PhysicalDriveN, \\.\Volume{GUID})
 *  - \\?\Volume{GUID}  volume GUID path
 */
bool is_volume_specifier(const std::wstring& path);

/**
 * @brief Appends a trailing backslash to a volume specifier if absent.
 *
 * Win32 APIs such as GetVolumeInformationW and GetDiskFreeSpaceW require the
 * volume root to end with a backslash (e.g. "E:\\"). This helper normalises a
 * bare drive letter ("E:") or a device path that lacks the trailing separator.
 */
std::wstring normalize_volume_root(const std::wstring& spec);

// Parse command line arguments
std::expected<CliArg, std::wstring> parse_arguments(int argc, wchar_t* argv[]);

// Utility to convert narrow string to wide string
std::wstring to_wstring(const std::string& str);

// Utility to convert wide string to narrow string
std::string to_string(const std::wstring& wstr);

// Utility to format Win32 error code as a message string
std::wstring get_win32_error_message(DWORD error_code);

// Utility to format a byte count as a human-readable size string (e.g. "4.00 MB")
std::wstring format_size(ULONGLONG bytes);

/// @brief Formats a byte count as "<format_size(bytes)> (<bytes> bytes)", e.g.
/// "4.00 MB (4194304 bytes)". The composite form repeated across most reporting
/// sections that show both a human-readable size and the exact byte count.
std::wstring format_size_detailed(ULONGLONG bytes);

/**
 * @brief Clones a byte range from src_handle into dest_handle via FSCTL_DUPLICATE_EXTENTS_TO_FILE.
 *
 * Shared IOCTL boilerplate for block cloning - used both by copy (same-volume clone,
 * cross-volume clone-from-previously-copied-dest) and dedup (cluster rebuild). All
 * three previously issued the identical IOCTL shape with only error-message detail
 * differing.
 *
 * @param dest_handle  Handle to the file receiving the cloned extent.
 * @param src_handle   Handle to the file supplying the extent's data.
 * @param src_offset   Byte offset within src_handle to clone from.
 * @param dest_offset  Byte offset within dest_handle to clone into.
 * @param byte_count   Number of bytes to clone (must be cluster-aligned per FSCTL semantics).
 * @return true on success, or error string (including the offsets involved) on failure.
 */
std::expected<bool, std::wstring> duplicate_extents(
    HANDLE dest_handle,
    HANDLE src_handle,
    ULONGLONG src_offset,
    ULONGLONG dest_offset,
    ULONGLONG byte_count
);

/**
 * @brief Sets a file's logical end-of-file via SetFileInformationByHandle(FileEndOfFileInfo).
 *
 * Preferred over SetFilePointerEx + SetEndOfFile on modern Windows since it doesn't
 * disturb the file's current seek pointer. Shared low-level primitive - callers with
 * their own sizing semantics (e.g. copy.cpp's incremental-growth-on-demand fallback
 * and its pointer-reset-after-resize behavior) keep that logic in their own wrappers
 * and call this only for the actual Win32 call.
 *
 * @param handle  Handle to the file (must have been opened with write access).
 * @param size    The exact size, in bytes, to set.
 * @return true on success, or error string on failure.
 */
std::expected<bool, std::wstring> set_file_eof(HANDLE handle, ULONGLONG size);

/**
 * @brief Recursively enumerates all non-system, non-reparse-point files under a directory.
 *
 * @p dir must end with a backslash ('\\').
 * Skips items with FILE_ATTRIBUTE_SYSTEM or FILE_ATTRIBUTE_REPARSE_POINT.
 *
 * @param dir       Directory to scan (must end with '\\').
 * @param out_files Accumulates absolute file paths.
 * @param errors    Accumulates non-fatal error messages.
 */
void enumerate_files_recursive(
    const std::wstring& dir,
    std::vector<std::wstring>& out_files,
    std::vector<std::wstring>& errors
);

/// @brief Volume root, cluster size, and filesystem name resolved for a target specifier.
struct VolumeInfo {
    std::wstring volume_root;
    DWORD        cluster_size = 0;
    /// @brief Filesystem name (e.g. L"ReFS", L"NTFS"), or empty if the
    /// GetVolumeInformationW query failed. That query is non-fatal here -
    /// callers that only need cluster_size (the common case) shouldn't fail
    /// because of it. Callers that need fs_name for its own sake should treat
    /// an empty string as "could not be determined" and decide accordingly.
    std::wstring fs_name;
};

/**
 * @brief Resolves the volume root, cluster size, and filesystem name for a raw path or specifier.
 *
 * Strips any wildcard segment before querying, so glob patterns (e.g. "D:\\data\\*.dat")
 * resolve against their containing directory rather than failing outright. Intended to
 * be called once per volume/specifier - never once per file - since cluster size and
 * filesystem name are volume-level properties.
 *
 * @param path  A volume specifier, directory, file path, or glob pattern.
 * @return VolumeInfo on success, or error string on failure. Only the cluster-size
 *         query is fatal; a failed filesystem-name query leaves VolumeInfo::fs_name empty.
 */
std::expected<VolumeInfo, std::wstring> resolve_volume_info(const std::wstring& path);

/// @brief Volume info plus the enumerated files for a target specifier.
struct ResolvedTarget {
    std::wstring volume_root;
    DWORD        cluster_size = 0;
    std::vector<std::wstring> files;
};

/**
 * @brief Resolves a FileSpecifier to its volume info and enumerated file list in one call.
 *
 * Combines resolve_volume_info() with expand_file_specifier(). This is the standard
 * way for a command to turn an input specifier into "what volume, what cluster size,
 * what files" - the volume/cluster-size lookup happens once per specifier, not once
 * per enumerated file.
 *
 * @param spec    The specifier to resolve.
 * @param errors  Accumulates non-fatal file-enumeration errors.
 * @return ResolvedTarget on success, or error string on failure.
 */
std::expected<ResolvedTarget, std::wstring> resolve_target(
    const FileSpecifier& spec,
    std::vector<std::wstring>& errors
);

/**
 * @brief Expands a FileSpecifier to a flat list of absolute file paths.
 *
 * Handles all four cases:
 *  - kVolume: full recursive enumeration from the normalized volume root.
 *  - kPath + directory: recursive enumeration from the directory.
 *  - kPath + glob pattern ('*' or '?'): non-recursive expansion via FindFirstFileW.
 *  - kPath + plain file: single-element list containing the path.
 *
 * @param spec    The specifier to expand.
 * @param errors  Accumulates non-fatal errors (access failures, etc.).
 * @return Flat list of matching absolute file paths.
 */
std::vector<std::wstring> expand_file_specifier(
    const FileSpecifier& spec,
    std::vector<std::wstring>& errors
);

/// @brief Set to true by the Ctrl+C handler to signal active operations to stop.
extern std::atomic<bool> g_cancel_requested;

} // namespace util
