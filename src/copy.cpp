#include <algorithm>
#include <expected>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include <windows.h>

#include "copy.h"
#include "util.h"

namespace copy {

struct CopyStats {
    ULONGLONG total_files = 0;
    ULONGLONG cloned_files = 0;
    ULONGLONG fallback_files = 0;
    ULONGLONG total_bytes = 0;
    std::vector<std::wstring> errors;
};

struct HandleCache {
    std::wstring path;
    HANDLE handle = INVALID_HANDLE_VALUE;

    void close() {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
        }
        path.clear();
    }

    HANDLE get(const std::wstring& filepath) {
        if (path == filepath && handle != INVALID_HANDLE_VALUE) {
            return handle;
        }
        close();
        handle = CreateFileW(
            filepath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS,
            NULL
        );
        if (handle != INVALID_HANDLE_VALUE) {
            path = filepath;
        }
        return handle;
    }
};

struct CopyContext {
    std::wstring src_volume_root;
    std::wstring dest_volume_root;
    bool same_volume = false;
    bool dest_is_refs = false;
    DWORD src_cluster_size = 0;
    DWORD dest_cluster_size = 0;
    // Map: source LCN -> {dest_file_path, dest_offset_in_bytes}
    std::unordered_map<LONGLONG, std::pair<std::wstring, ULONGLONG>> lcn_map;
    CopyStats stats;
    HandleCache handle_cache;

    ~CopyContext() {
        handle_cache.close();
    }
};

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
    wchar_t buffer[MAX_PATH];
    DWORD length = GetFullPathNameW(path.c_str(), MAX_PATH, buffer, NULL);
    if (length > 0) {
        return std::wstring(buffer, length);
    }
    return path;
}

/**
 * @brief Copies a single file from source to destination, preserving deduplication if possible.
 * 
 * Inspects block allocations, matches duplicates against the context map, duplicates extents via 
 * FSCTL_DUPLICATE_EXTENTS_TO_FILE, and falls back to standard CopyFileExW if operations fail.
 * 
 * @param src The source file path.
 * @param dest The destination file path.
 * @param args The command line arguments.
 * @param context The shared copy tracking context.
 * @return std::expected<bool, std::wstring> True on success, or an error message on failure.
 */
std::expected<bool, std::wstring> copy_file_preserving_dedup(
    const std::wstring& src,
    const std::wstring& dest,
    const util::CliArg& args,
    CopyContext& context
) {
    if (args.dry_run) {
        if (context.same_volume) {
            std::wcout << L"[DRY-RUN] Would clone (same volume): " << src << L" -> " << dest << std::endl;
            context.stats.cloned_files++;
        } else if (context.dest_is_refs && context.src_cluster_size == context.dest_cluster_size) {
            std::wcout << L"[DRY-RUN] Would copy preserving deduplication (cross volume ReFS): " << src << L" -> " << dest << std::endl;
            context.stats.cloned_files++;
        } else {
            std::wcout << L"[DRY-RUN] Would copy with standard fallback: " << src << L" -> " << dest << std::endl;
            context.stats.fallback_files++;
        }
        context.stats.total_files++;
        return true;
    }

    // Branch A: Fallback to standard copy (Non-ReFS target or mismatched cluster size)
    if (!context.same_volume && (!context.dest_is_refs || context.src_cluster_size != context.dest_cluster_size)) {
        if (!context.dest_is_refs) {
            std::wcout << L"WARNING: Destination volume is non-ReFS. Falling back to standard copy for: " << src << std::endl;
        } else {
            std::wcout << L"WARNING: Destination volume cluster size mismatch (" << context.dest_cluster_size 
                       << L" vs " << context.src_cluster_size << L"). Falling back to standard copy for: " << src << std::endl;
        }
        if (!CopyFileExW(src.c_str(), dest.c_str(), NULL, NULL, NULL, COPY_FILE_ALLOW_DECRYPTED_DESTINATION)) {
            DWORD error = GetLastError();
            return std::unexpected(L"Fallback CopyFileExW failed: " + util::get_win32_error_message(error));
        }
        context.stats.fallback_files++;
        context.stats.total_files++;
        return true;
    }

    // Branch B: Same-volume block clone or Cross-volume ReFS block clone
    HANDLE src_handle = CreateFileW(
        src.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        NULL
    );
    if (src_handle == INVALID_HANDLE_VALUE) {
        return std::unexpected(L"Failed to open source file: " + util::get_win32_error_message(GetLastError()));
    }

    HANDLE dest_handle = CreateFileW(
        dest.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        CREATE_ALWAYS,
        FILE_FLAG_BACKUP_SEMANTICS,
        NULL
    );
    if (dest_handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        CloseHandle(src_handle);
        return std::unexpected(L"Failed to create destination file: " + util::get_win32_error_message(error));
    }

    // Mark the destination file as sparse. This allows logical sizing (SetEndOfFile)
    // to succeed without allocating physical disk space, preventing disk-full failures
    // for files that will be deduplicated/cloned.
    FILE_SET_SPARSE_BUFFER sparse_buffer;
    sparse_buffer.SetSparse = TRUE;
    DWORD temp_returned = 0;
    bool sparse_ok = DeviceIoControl(
        dest_handle,
        FSCTL_SET_SPARSE,
        &sparse_buffer,
        sizeof(sparse_buffer),
        NULL, 0,
        &temp_returned,
        NULL
    );

    bool copy_failed = false;
    std::wstring copy_error_msg;

    if (!sparse_ok) {
        copy_failed = true;
        copy_error_msg = L"Failed to set sparse attribute: " + util::get_win32_error_message(GetLastError());
    }

    LARGE_INTEGER src_size = {0};
    if (!copy_failed) {
        if (!GetFileSizeEx(src_handle, &src_size)) {
            DWORD error = GetLastError();
            CloseHandle(src_handle);
            CloseHandle(dest_handle);
            return std::unexpected(L"Failed to get source file size: " + util::get_win32_error_message(error));
        }
    }

    bool incremental_sizing = false;
    ULONGLONG current_dest_eof = 0;

    // Pre-size the destination file to source size so that block cloning works (cannot clone beyond EOF)
    if (!copy_failed && src_size.QuadPart > 0) {
        if (!SetFilePointerEx(dest_handle, src_size, NULL, FILE_BEGIN) || !SetEndOfFile(dest_handle)) {
            // Pre-sizing failed (e.g., disk full). Fall back to incremental block-by-block sizing.
            incremental_sizing = true;
        } else {
            current_dest_eof = src_size.QuadPart;
        }

        // Seek back to beginning
        LARGE_INTEGER zero_offset;
        zero_offset.QuadPart = 0;
        if (!SetFilePointerEx(dest_handle, zero_offset, NULL, FILE_BEGIN)) {
            DWORD error = GetLastError();
            CloseHandle(src_handle);
            CloseHandle(dest_handle);
            return std::unexpected(L"Failed to reset destination file pointer: " + util::get_win32_error_message(error));
        }
    }

    auto ensure_dest_size = [&](ULONGLONG required_size) -> bool {
        if (incremental_sizing && required_size > current_dest_eof) {
            LARGE_INTEGER li;
            li.QuadPart = required_size;
            if (!SetFilePointerEx(dest_handle, li, NULL, FILE_BEGIN) || !SetEndOfFile(dest_handle)) {
                return false;
            }
            current_dest_eof = required_size;
        }
        return true;
    };

    STARTING_VCN_INPUT_BUFFER input = {0};
    input.StartingVcn.QuadPart = 0;

    const DWORD buf_size = sizeof(RETRIEVAL_POINTERS_BUFFER) + sizeof(RETRIEVAL_POINTERS_BUFFER::Extents[0]) * 16;
    std::vector<BYTE> buffer(buf_size);
    PRETRIEVAL_POINTERS_BUFFER output = reinterpret_cast<PRETRIEVAL_POINTERS_BUFFER>(buffer.data());

    DWORD bytes_returned = 0;
    bool done = false;
    LONGLONG current_vcn = 0;

    while (!done && !copy_failed) {
        BOOL ok = DeviceIoControl(
            src_handle,
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
            if (err == ERROR_HANDLE_EOF) {
                break;
            }
            copy_failed = true;
            copy_error_msg = L"FSCTL_GET_RETRIEVAL_POINTERS failed: " + util::get_win32_error_message(err);
            break;
        }

        if (ok) {
            done = true;
        }

        LONGLONG start_vcn = output->StartingVcn.QuadPart;
        current_vcn = start_vcn;

        for (DWORD i = 0; i < output->ExtentCount; ++i) {
            LONGLONG next_vcn = output->Extents[i].NextVcn.QuadPart;
            LONGLONG lcn = output->Extents[i].Lcn.QuadPart;

            ULONGLONG extent_cluster_count = next_vcn - current_vcn;

            if (lcn == (LONGLONG)-1) {
                // Sparse extent: let SetEndOfFile handle sizing
                current_vcn = next_vcn;
                continue;
            }

            // Process extent clusters
            ULONGLONG c_offset = 0;
            while (c_offset < extent_cluster_count) {
                LONGLONG current_lcn = lcn + c_offset;
                ULONGLONG src_offset = (current_vcn + c_offset) * context.src_cluster_size;

                if (context.same_volume) {
                    ULONGLONG required_size = src_offset + extent_cluster_count * context.src_cluster_size;
                    if (!ensure_dest_size(required_size)) {
                        copy_failed = true;
                        copy_error_msg = L"Incremental sizing failed: " + util::get_win32_error_message(GetLastError());
                        break;
                    }

                    // Direct same-volume clone
                    DUPLICATE_EXTENTS_DATA dup_data;
                    dup_data.FileHandle = src_handle;
                    dup_data.SourceFileOffset.QuadPart = src_offset;
                    dup_data.TargetFileOffset.QuadPart = src_offset;
                    dup_data.ByteCount.QuadPart = extent_cluster_count * context.src_cluster_size;

                    DWORD dup_returned = 0;
                    BOOL dup_ok = DeviceIoControl(
                        dest_handle,
                        FSCTL_DUPLICATE_EXTENTS_TO_FILE,
                        &dup_data,
                        sizeof(dup_data),
                        NULL, 0,
                        &dup_returned,
                        NULL
                    );

                    if (!dup_ok) {
                        copy_failed = true;
                        copy_error_msg = L"FSCTL_DUPLICATE_EXTENTS_TO_FILE failed: " + util::get_win32_error_message(GetLastError());
                    }
                    // Since it cloned the whole extent, we are done with this extent
                    break;
                } else {
                    // Cross-volume ReFS copy: check LCN map
                    auto map_it = context.lcn_map.find(current_lcn);
                    if (map_it != context.lcn_map.end()) {
                        // duplicate LCN: find contiguous run length
                        const auto& master = map_it->second;
                        ULONGLONG run_clusters = 1;
                        while (c_offset + run_clusters < extent_cluster_count) {
                            LONGLONG next_lcn = lcn + c_offset + run_clusters;
                            auto next_map_it = context.lcn_map.find(next_lcn);
                            if (next_map_it == context.lcn_map.end()) {
                                break;
                            }
                            const auto& next_master = next_map_it->second;
                            if (next_master.first != master.first || 
                                next_master.second != master.second + run_clusters * context.src_cluster_size) {
                                break;
                            }
                            run_clusters++;
                        }

                        ULONGLONG required_size = src_offset + run_clusters * context.src_cluster_size;
                        if (!ensure_dest_size(required_size)) {
                            copy_failed = true;
                            copy_error_msg = L"Incremental sizing failed: " + util::get_win32_error_message(GetLastError());
                            break;
                        }

                        // Clone duplicate run from already copied destination file
                        HANDLE prev_dest_handle = context.handle_cache.get(master.first);
                        if (prev_dest_handle == INVALID_HANDLE_VALUE) {
                            copy_failed = true;
                            copy_error_msg = L"Failed to open previously copied destination file: " + master.first;
                            break;
                        }

                        DUPLICATE_EXTENTS_DATA dup_data;
                        dup_data.FileHandle = prev_dest_handle;
                        dup_data.SourceFileOffset.QuadPart = master.second;
                        dup_data.TargetFileOffset.QuadPart = src_offset;
                        dup_data.ByteCount.QuadPart = run_clusters * context.src_cluster_size;

                        DWORD dup_returned = 0;
                        BOOL dup_ok = DeviceIoControl(
                            dest_handle,
                            FSCTL_DUPLICATE_EXTENTS_TO_FILE,
                            &dup_data,
                            sizeof(dup_data),
                            NULL, 0,
                            &dup_returned,
                            NULL
                        );

                        if (!dup_ok) {
                            DWORD dup_err = GetLastError();
                            std::wcout << L"ERROR: FSCTL_DUPLICATE_EXTENTS_TO_FILE failed on target volume. SourceOffset=" 
                                       << dup_data.SourceFileOffset.QuadPart 
                                       << L", TargetOffset=" << dup_data.TargetFileOffset.QuadPart 
                                       << L", ByteCount=" << dup_data.ByteCount.QuadPart 
                                       << L", err=" << dup_err << std::endl;
                            copy_failed = true;
                            copy_error_msg = L"FSCTL_DUPLICATE_EXTENTS_TO_FILE failed on target volume: " + util::get_win32_error_message(dup_err);
                            break;
                        }

                        c_offset += run_clusters;
                    } else {
                        // New LCN: find contiguous run length
                        ULONGLONG run_clusters = 1;
                        while (c_offset + run_clusters < extent_cluster_count) {
                            LONGLONG next_lcn = lcn + c_offset + run_clusters;
                            if (context.lcn_map.contains(next_lcn)) {
                                break;
                            }
                            run_clusters++;
                        }

                        ULONGLONG bytes_to_copy = run_clusters * context.src_cluster_size;
                        ULONGLONG bytes_to_read = bytes_to_copy;
                        if ((ULONGLONG)src_size.QuadPart > src_offset) {
                            if (src_offset + bytes_to_read > (ULONGLONG)src_size.QuadPart) {
                                bytes_to_read = (ULONGLONG)src_size.QuadPart - src_offset;
                            }
                        } else {
                            bytes_to_read = 0;
                        }

                        ULONGLONG required_size = src_offset + bytes_to_read;
                        if (!ensure_dest_size(required_size)) {
                            copy_failed = true;
                            copy_error_msg = L"Incremental sizing failed: " + util::get_win32_error_message(GetLastError());
                            break;
                        }

                        // Seek source
                        LARGE_INTEGER li_src;
                        li_src.QuadPart = src_offset;
                        if (!SetFilePointerEx(src_handle, li_src, NULL, FILE_BEGIN)) {
                            copy_failed = true;
                            copy_error_msg = L"SetFilePointerEx failed on source: " + util::get_win32_error_message(GetLastError());
                            break;
                        }

                        // Seek target
                        LARGE_INTEGER li_dest;
                        li_dest.QuadPart = src_offset;
                        if (!SetFilePointerEx(dest_handle, li_dest, NULL, FILE_BEGIN)) {
                            copy_failed = true;
                            copy_error_msg = L"SetFilePointerEx failed on destination: " + util::get_win32_error_message(GetLastError());
                            break;
                        }

                        // Copy bytes physically
                        std::vector<BYTE> io_buffer(64 * 1024); // 64 KB chunk copy
                        ULONGLONG copied = 0;
                        bool write_ok = true;

                        while (copied < bytes_to_read) {
                            DWORD to_read = static_cast<DWORD>(std::min<ULONGLONG>(io_buffer.size(), bytes_to_read - copied));
                            DWORD read = 0;
                            if (!ReadFile(src_handle, io_buffer.data(), to_read, &read, NULL) || read == 0) {
                                DWORD read_err = GetLastError();
                                std::wcout << L"ERROR: ReadFile EOF/failure. to_read=" << to_read 
                                           << L", read=" << read 
                                           << L", copied=" << copied 
                                           << L", bytes_to_read=" << bytes_to_read 
                                           << L", src_offset=" << src_offset 
                                           << L", file_size=" << src_size.QuadPart 
                                           << L", err=" << read_err << std::endl;
                                copy_failed = true;
                                copy_error_msg = L"ReadFile failed on source: " + util::get_win32_error_message(read_err);
                                write_ok = false;
                                break;
                            }

                            DWORD written = 0;
                            if (!WriteFile(dest_handle, io_buffer.data(), read, &written, NULL) || written != read) {
                                copy_failed = true;
                                copy_error_msg = L"WriteFile failed on destination: " + util::get_win32_error_message(GetLastError());
                                write_ok = false;
                                break;
                            }

                            copied += read;
                        }

                        if (!write_ok) break;

                        // Map LCNs to destination
                        for (ULONGLONG k = 0; k < run_clusters; ++k) {
                            context.lcn_map[lcn + c_offset + k] = {dest, src_offset + k * context.src_cluster_size};
                        }

                        c_offset += run_clusters;
                    }
                }
            }

            if (copy_failed) break;
            current_vcn = next_vcn;
        }

        if (copy_failed) break;
        input.StartingVcn.QuadPart = current_vcn;
    }

    if (copy_failed) {
        CloseHandle(dest_handle);
        DeleteFileW(dest.c_str());

        std::wcout << L"WARNING: Deduplication-preserving copy failed (" << copy_error_msg 
                   << L"). Falling back to standard copy for: " << src << std::endl;

        // Standard copy fallback
        if (!CopyFileExW(src.c_str(), dest.c_str(), NULL, NULL, NULL, COPY_FILE_ALLOW_DECRYPTED_DESTINATION)) {
            DWORD error = GetLastError();
            CloseHandle(src_handle);
            return std::unexpected(L"Dedup copy failed (" + copy_error_msg + L"), and fallback copy failed: " + util::get_win32_error_message(error));
        }

        context.stats.fallback_files++;
    } else {
        // Truncate/extend destination file to exact size
        LARGE_INTEGER dist_offset;
        dist_offset.QuadPart = src_size.QuadPart;
        if (!SetFilePointerEx(dest_handle, dist_offset, NULL, FILE_BEGIN)) {
            DWORD error = GetLastError();
            CloseHandle(src_handle);
            CloseHandle(dest_handle);
            return std::unexpected(L"Failed to set file pointer: " + util::get_win32_error_message(error));
        }
        if (!SetEndOfFile(dest_handle)) {
            DWORD error = GetLastError();
            CloseHandle(src_handle);
            CloseHandle(dest_handle);
            return std::unexpected(L"SetEndOfFile failed: " + util::get_win32_error_message(error));
        }

        // Copy file times/attributes
        FILE_BASIC_INFO basic_info;
        if (GetFileInformationByHandleEx(src_handle, FileBasicInfo, &basic_info, sizeof(basic_info))) {
            SetFileInformationByHandle(dest_handle, FileBasicInfo, &basic_info, sizeof(basic_info));
        }

        CloseHandle(dest_handle);
        context.stats.cloned_files++;
    }

    CloseHandle(src_handle);
    context.stats.total_files++;
    context.stats.total_bytes += src_size.QuadPart;
    return true;
}

/**
 * @brief Recursively copies a directory tree from source to destination.
 * 
 * Replicates the directory structure and invokes copy_file_preserving_dedup on files.
 * Ignores system-attributed files and folders.
 * 
 * @param src_dir The source directory path.
 * @param dest_dir The destination directory path.
 * @param args The command line arguments.
 * @param context The shared copy tracking context.
 * @return std::expected<bool, std::wstring> True on success, or an error message on failure.
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
        std::wstring name = find_data.cFileName;
        if (name == L"." || name == L"..") {
            continue;
        }

        std::wstring src_item = src_dir + L"\\" + name;
        std::wstring dest_item = dest_dir + L"\\" + name;

        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) {
            continue;
        }

        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (args.recursive) {
                auto res = copy_directory_recursive(src_item, dest_item, args, context);
                if (!res) {
                    context.stats.errors.push_back(src_item + L": " + res.error());
                    if (args.strict) {
                        FindClose(find_handle);
                        return std::unexpected(res.error());
                    }
                }
            }
        } else {
            auto res = copy_file_preserving_dedup(src_item, dest_item, args, context);
            if (!res) {
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

/**
 * @brief Executes the copy subcommand.
 * 
 * Resolves source/destination bounds, establishes mapping contexts, and coordinates
 * recursive/single file copying. Prints summary statistics at completion.
 * 
 * @param args CLI arguments containing positional source and destination paths.
 * @return std::expected<int, std::wstring> Exit code on success, or error string on failure.
 */
std::expected<int, std::wstring> execute_copy(const util::CliArg& args) {
    if (args.positional.size() < 2) {
        return std::unexpected(L"Error: Missing source or destination path. Usage: retool copy <src> <dest> [options]");
    }

    // Resolve absolute paths
    std::wstring src = get_absolute_path(args.positional[0]);
    std::wstring dest = get_absolute_path(args.positional[1]);

    DWORD src_attr = GetFileAttributesW(src.c_str());
    if (src_attr == INVALID_FILE_ATTRIBUTES) {
        return std::unexpected(L"Source path does not exist: " + src);
    }

    bool is_dir = (src_attr & FILE_ATTRIBUTE_DIRECTORY) != 0;

    // Get volume roots
    wchar_t src_volume[MAX_PATH];
    wchar_t dest_volume[MAX_PATH];
    if (!GetVolumePathNameW(src.c_str(), src_volume, MAX_PATH)) {
        return std::unexpected(L"Failed to get source volume root: " + util::get_win32_error_message(GetLastError()));
    }
    if (!GetVolumePathNameW(dest.c_str(), dest_volume, MAX_PATH)) {
        return std::unexpected(L"Failed to get destination volume root: " + util::get_win32_error_message(GetLastError()));
    }

    CopyContext context;
    context.src_volume_root = src_volume;
    context.dest_volume_root = dest_volume;
    context.same_volume = (wcscmp(src_volume, dest_volume) == 0);
    context.dest_is_refs = is_refs_volume(dest_volume);
    context.src_cluster_size = get_cluster_size(src_volume);
    context.dest_cluster_size = get_cluster_size(dest_volume);

    if (is_dir) {
        if (!args.recursive) {
            return std::unexpected(L"Error: Source is a directory but -r (recursive) was not specified.");
        }
        auto res = copy_directory_recursive(src, dest, args, context);
        if (!res && args.strict) {
            return std::unexpected(res.error());
        }
    } else {
        auto res = copy_file_preserving_dedup(src, dest, args, context);
        if (!res) {
            return std::unexpected(res.error());
        }
    }

    const auto& stats = context.stats;

    // Print summary stats
    std::wcout << L"\n--- retool copy summary ---\n"
               << L"Total Files:     " << stats.total_files << L"\n"
               << L"Cloned Files:    " << stats.cloned_files << L" (preserves dedup/blocks)\n"
               << L"Fallback Copies: " << stats.fallback_files << L" (standard copies)\n"
               << L"Total Bytes:     " << stats.total_bytes << L" bytes" << std::endl;

    if (!stats.errors.empty()) {
        std::wcout << L"\nErrors:\n";
        for (const auto& err : stats.errors) {
            std::wcout << L"  - " << err << L"\n";
        }
    }

    if (!stats.errors.empty()) {
        return 2; // Operational error exit code
    }

    return 0;
}

} // namespace copy
