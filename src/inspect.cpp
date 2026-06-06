/**
 * @file inspect.cpp
 * @brief Block layout inspection, volume scan engine, and cross-file sharing analysis.
 *
 * Provides three modes of operation:
 *  - Single-file: dump VCN/LCN extent map.
 *  - Multi-file: cross-file block sharing report.
 *  - Volume scan: full LCN index with optional SHA-256 content hashing.
 */

#include <algorithm>
#include <array>
#include <expected>
#include <fstream>
#include <iomanip>
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

/// @brief Logical-to-physical extent entry for a single file.
struct VcnExtent {
    LONGLONG  start_vcn;
    LONGLONG  next_vcn;
    LONGLONG  lcn;
    ULONGLONG cluster_count;
};

/// @brief All physical-layout information for a single inspected file.
struct FileInspectResult {
    std::wstring path;
    std::wstring volume_root;
    DWORD        cluster_size = 0;
    ULONGLONG    file_size    = 0;
    std::vector<VcnExtent> extents;
    std::wstring error;
};

// ============================================================================
// Internal Helpers
// ============================================================================

/**
 * @brief Reads a list of file paths from a newline-delimited text file.
 *
 * Skips empty lines and lines starting with '#'. Strips BOM and trailing spaces.
 *
 * @param path The path to the text file list.
 * @return List of paths, or error string.
 */
std::expected<std::vector<std::wstring>, std::wstring> read_file_list(const std::wstring& path) {
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        return std::unexpected(L"Failed to open input file list: " + path);
    }

    std::vector<std::wstring> paths;
    std::string line;
    while (std::getline(file, line)) {
        if (line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line = line.substr(3);
        }

        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
               isspace(static_cast<unsigned char>(line.back())))) {
            line.pop_back();
        }

        size_t start = 0;
        while (start < line.size() && isspace(static_cast<unsigned char>(line[start]))) {
            start++;
        }

        if (start >= line.size() || line[start] == '#') continue;

        paths.push_back(util::to_wstring(line.substr(start)));
    }
    return paths;
}

/**
 * @brief Inspects the virtual and physical extents of a single file.
 *
 * @param path The target file path.
 * @return FileInspectResult containing extents, size, and any error.
 */
FileInspectResult inspect_file(const std::wstring& path) {
    FileInspectResult res;
    res.path = path;

    wchar_t volume_root[MAX_PATH];
    if (!GetVolumePathNameW(path.c_str(), volume_root, MAX_PATH)) {
        res.error = L"GetVolumePathNameW failed: " + util::get_win32_error_message(GetLastError());
        return res;
    }
    res.volume_root = volume_root;

    DWORD sectors_per_cluster = 0, bytes_per_sector = 0, free_clusters = 0, total_clusters = 0;
    if (!GetDiskFreeSpaceW(volume_root, &sectors_per_cluster, &bytes_per_sector, &free_clusters, &total_clusters)) {
        res.error = L"GetDiskFreeSpaceW failed: " + util::get_win32_error_message(GetLastError());
        return res;
    }
    res.cluster_size = sectors_per_cluster * bytes_per_sector;

    HANDLE file_handle = CreateFileW(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL
    );

    if (file_handle == INVALID_HANDLE_VALUE) {
        res.error = L"CreateFileW failed: " + util::get_win32_error_message(GetLastError());
        return res;
    }

    LARGE_INTEGER fs_size;
    if (!GetFileSizeEx(file_handle, &fs_size)) {
        res.error = L"GetFileSizeEx failed: " + util::get_win32_error_message(GetLastError());
        CloseHandle(file_handle);
        return res;
    }
    res.file_size = fs_size.QuadPart;

    STARTING_VCN_INPUT_BUFFER input = {0};
    input.StartingVcn.QuadPart = 0;

    const DWORD buf_size = sizeof(RETRIEVAL_POINTERS_BUFFER) +
                           sizeof(RETRIEVAL_POINTERS_BUFFER::Extents[0]) * 16;
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
            res.error = L"FSCTL_GET_RETRIEVAL_POINTERS failed: " + util::get_win32_error_message(err);
            CloseHandle(file_handle);
            return res;
        }

        if (ok) done = true;

        current_vcn = output_buf->StartingVcn.QuadPart;
        for (DWORD i = 0; i < output_buf->ExtentCount; ++i) {
            VcnExtent extent;
            extent.start_vcn    = current_vcn;
            extent.next_vcn     = output_buf->Extents[i].NextVcn.QuadPart;
            extent.lcn          = output_buf->Extents[i].Lcn.QuadPart;
            extent.cluster_count = extent.next_vcn - extent.start_vcn;
            res.extents.push_back(extent);
            current_vcn = extent.next_vcn;
        }

        input.StartingVcn.QuadPart = current_vcn;
    }

    CloseHandle(file_handle);
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

// ============================================================================
// Volume Enumeration
// ============================================================================

/**
 * @brief Recursively enumerates all non-system files under a directory.
 *
 * @param dir       Directory to scan.
 * @param out_files Accumulates absolute file paths.
 * @param errors    Accumulates non-fatal error messages.
 */
void enumerate_files_recursive(
    const std::wstring& dir,
    std::vector<std::wstring>& out_files,
    std::vector<std::wstring>& errors
) {
    std::wstring search = dir + L"*";
    WIN32_FIND_DATAW find_data;
    HANDLE find_handle = FindFirstFileW(search.c_str(), &find_data);

    if (find_handle == INVALID_HANDLE_VALUE) {
        errors.push_back(L"FindFirstFileW failed for: " + dir +
                         L" (" + util::get_win32_error_message(GetLastError()) + L")");
        return;
    }

    do {
        std::wstring name = find_data.cFileName;
        if (name == L"." || name == L"..") continue;

        DWORD attrs = find_data.dwFileAttributes;

        // Skip system files, junctions, and reparse points
        if (attrs & FILE_ATTRIBUTE_SYSTEM) continue;
        if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) continue;

        std::wstring full_path = dir + name;

        if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
            enumerate_files_recursive(full_path + L"\\", out_files, errors);
        } else {
            out_files.push_back(full_path);
        }
    } while (FindNextFileW(find_handle, &find_data));

    FindClose(find_handle);
}

// ============================================================================
// Scan Engine
// ============================================================================

std::expected<ScanResult, std::wstring> build_lcn_index(
    const std::wstring& volume_root,
    ScanMode mode,
    output::IOutput& status_out
) {
    ScanResult result;
    result.volume_root = volume_root;

    // Determine cluster size for the volume
    DWORD spc = 0, bps = 0, fc = 0, tc = 0;
    if (!GetDiskFreeSpaceW(volume_root.c_str(), &spc, &bps, &fc, &tc)) {
        return std::unexpected(L"Failed to get volume cluster size: " +
                               util::get_win32_error_message(GetLastError()));
    }
    result.cluster_size = spc * bps;

    // Phase A: Enumerate all file paths
    status_out.status(L"[build_lcn_index] Enumerating files on " + volume_root + L"...");
    std::vector<std::wstring> file_paths;
    enumerate_files_recursive(volume_root, file_paths, result.errors);

    // Phase B: Open BCrypt provider for hashing (if requested)
    BcryptAlgHandle alg;
    if (mode == ScanMode::kWithHash) {
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
                &alg.handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
            return std::unexpected(L"Failed to open BCrypt SHA-256 algorithm provider.");
        }
    }

    // Phase C: Inspect each file, build index
    const ULONGLONG kProgressInterval = 500;
    std::vector<BYTE> read_buffer;

    if (mode == ScanMode::kWithHash && result.cluster_size > 0) {
        read_buffer.resize(result.cluster_size);
    }

    for (const auto& path : file_paths) {
        auto file_result = inspect_file(path);

        if (!file_result.error.empty()) {
            result.errors.push_back(path + L": " + file_result.error);
            continue;
        }

        result.files_scanned++;

        // Periodic progress update
        if (result.files_scanned % kProgressInterval == 0) {
            status_out.status(L"[build_lcn_index] Indexed " +
                              std::to_wstring(result.files_scanned) + L" files...");
        }

        // Handle to read cluster data (only needed in kWithHash mode)
        HANDLE file_handle = INVALID_HANDLE_VALUE;
        if (mode == ScanMode::kWithHash) {
            file_handle = CreateFileW(
                path.c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL
            );
            if (file_handle == INVALID_HANDLE_VALUE) {
                result.errors.push_back(path + L": Failed to open for hashing: " +
                                        util::get_win32_error_message(GetLastError()));
                continue;
            }
        }

        for (const auto& ext : file_result.extents) {
            if (ext.lcn == (LONGLONG)-1) continue; // Skip sparse

            for (ULONGLONG c = 0; c < ext.cluster_count; ++c) {
                LONGLONG    lcn         = ext.lcn + static_cast<LONGLONG>(c);
                ULONGLONG   file_offset = (ext.start_vcn + c) * result.cluster_size;

                BlockEntry entry;
                entry.file_path   = path;
                entry.file_offset = file_offset;

                result.lcn_index[lcn].push_back(entry);
                result.clusters_indexed++;

                // Hash this cluster if requested
                if (mode == ScanMode::kWithHash && file_handle != INVALID_HANDLE_VALUE) {
                    LARGE_INTEGER li;
                    li.QuadPart = static_cast<LONGLONG>(file_offset);
                    if (!SetFilePointerEx(file_handle, li, NULL, FILE_BEGIN)) continue;

                    DWORD bytes_read = 0;
                    DWORD to_read = static_cast<DWORD>(
                        std::min<ULONGLONG>(result.cluster_size,
                            file_result.file_size > file_offset
                                ? file_result.file_size - file_offset
                                : 0));

                    if (to_read == 0) continue;

                    if (ReadFile(file_handle, read_buffer.data(), to_read, &bytes_read, NULL) && bytes_read > 0) {
                        std::string digest = hash_sha256(alg.handle, read_buffer.data(), bytes_read);
                        if (!digest.empty()) {
                            result.hash_index[digest].push_back(lcn);
                        }
                    }
                }
            }
        }

        if (file_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(file_handle);
        }
    }

    status_out.status(L"[build_lcn_index] Complete: " +
                      std::to_wstring(result.files_scanned) + L" files, " +
                      std::to_wstring(result.clusters_indexed) + L" clusters indexed.");

    return result;
}

// ============================================================================
// Output Helpers
// ============================================================================

/**
 * @brief Outputs the single-file inspect report via IOutput.
 */
void output_single_file(const FileInspectResult& res, output::IOutput& out) {
    out.field(L"File",         res.path);
    out.field(L"Volume",       res.volume_root);
    out.field(L"Cluster Size", std::to_wstring(res.cluster_size) + L" bytes");
    out.field(L"File Size",    std::to_wstring(res.file_size) + L" bytes");
    out.field(L"Fragments",    std::to_wstring(res.extents.size()));

    out.begin_table({L"Extent #", L"VCN", L"LCN", L"Clusters", L"Bytes", L"Cumulative"});

    ULONGLONG cumulative_bytes = 0;
    for (size_t i = 0; i < res.extents.size(); ++i) {
        const auto& ext = res.extents[i];
        ULONGLONG bytes = ext.cluster_count * res.cluster_size;
        cumulative_bytes += bytes;

        std::wstring lcn_str = ext.lcn == (LONGLONG)-1 ? L"SPARSE" : std::to_wstring(ext.lcn);

        out.table_row({
            std::to_wstring(i),
            std::to_wstring(ext.start_vcn),
            lcn_str,
            std::to_wstring(ext.cluster_count),
            std::to_wstring(bytes),
            std::to_wstring(cumulative_bytes)
        });
    }

    out.end_table();
}

/**
 * @brief Outputs the multi-file inspect report via IOutput.
 */
void output_multi_file(
    const std::vector<FileInspectResult>& results,
    const std::vector<std::wstring>& errors,
    output::IOutput& out
) {
    std::unordered_map<LONGLONG, std::vector<size_t>> lcn_to_files;
    ULONGLONG total_file_bytes   = 0;
    DWORD     common_cluster_size = 0;
    std::wstring common_volume;

    for (size_t f_idx = 0; f_idx < results.size(); ++f_idx) {
        const auto& res = results[f_idx];
        if (!res.error.empty()) continue;

        if (common_cluster_size == 0) {
            common_cluster_size = res.cluster_size;
            common_volume = res.volume_root;
        }

        total_file_bytes += res.file_size;

        std::unordered_set<LONGLONG> unique_lcns;
        for (const auto& ext : res.extents) {
            if (ext.lcn == (LONGLONG)-1) continue;
            for (ULONGLONG offset = 0; offset < ext.cluster_count; ++offset) {
                unique_lcns.insert(ext.lcn + static_cast<LONGLONG>(offset));
            }
        }

        for (LONGLONG lcn : unique_lcns) {
            lcn_to_files[lcn].push_back(f_idx);
        }
    }

    ULONGLONG shared_clusters = 0;
    ULONGLONG saved_clusters  = 0;
    for (const auto& [lcn, files] : lcn_to_files) {
        if (files.size() >= 2) {
            shared_clusters++;
            saved_clusters += (files.size() - 1);
        }
    }

    ULONGLONG shared_bytes = shared_clusters * common_cluster_size;
    ULONGLONG saved_bytes  = saved_clusters  * common_cluster_size;
    double savings_pct = total_file_bytes > 0
        ? (static_cast<double>(saved_bytes) / total_file_bytes) * 100.0
        : 0.0;

    out.begin_section(L"retool inspect multi-file report");
    out.field(L"Volume",         common_volume);
    out.field(L"Cluster Size",   std::to_wstring(common_cluster_size) + L" bytes");
    out.field(L"Files Analyzed", std::to_wstring(results.size()));

    {
        std::wostringstream ss;
        ss << std::fixed << std::setprecision(2) << total_file_bytes / (1024.0 * 1024.0)
           << L" MB (" << total_file_bytes << L" bytes)";
        out.field(L"Total Sizes", ss.str());
    }
    {
        std::wostringstream ss;
        ss << std::fixed << std::setprecision(2) << shared_bytes / (1024.0 * 1024.0)
           << L" MB (" << shared_bytes << L" bytes)";
        out.field(L"Shared Blocks", ss.str());
    }
    {
        std::wostringstream ss;
        ss << std::fixed << std::setprecision(2) << saved_bytes / (1024.0 * 1024.0)
           << L" MB (" << saved_bytes << L" bytes)";
        out.field(L"Saved Space", ss.str());
    }
    {
        std::wostringstream ss;
        ss << std::fixed << std::setprecision(2) << savings_pct << L"%";
        out.field(L"Dedup Savings", ss.str());
    }

    // Sharing matrix
    std::vector<std::vector<ULONGLONG>> shared_matrix(
        results.size(), std::vector<ULONGLONG>(results.size(), 0));
    for (const auto& [lcn, files] : lcn_to_files) {
        if (files.size() < 2) continue;
        for (size_t i = 0; i < files.size(); ++i) {
            for (size_t j = i + 1; j < files.size(); ++j) {
                shared_matrix[files[i]][files[j]]++;
                shared_matrix[files[j]][files[i]]++;
            }
        }
    }

    std::vector<std::wstring> columns;
    columns.push_back(L"File");
    for (size_t j = 0; j < results.size(); ++j) {
        columns.push_back(L"F" + std::to_wstring(j));
    }

    out.begin_table(columns);
    for (size_t i = 0; i < results.size(); ++i) {
        std::wstring path = results[i].path;
        size_t last_slash = path.find_last_of(L"\\/");
        std::wstring name = (last_slash != std::wstring::npos) ? path.substr(last_slash + 1) : path;
        if (name.size() > 18) name = name.substr(0, 15) + L"...";

        std::vector<std::wstring> row;
        row.push_back(name);
        for (size_t j = 0; j < results.size(); ++j) {
            if (i == j) {
                row.push_back(L"-");
            } else {
                std::wostringstream ss;
                ss << std::fixed << std::setprecision(1)
                   << (shared_matrix[i][j] * common_cluster_size) / (1024.0 * 1024.0);
                row.push_back(ss.str());
            }
        }
        out.table_row(row);
    }

    out.end_table();
    out.end_section();

    for (const auto& err : errors) {
        out.error(err);
    }
}

/**
 * @brief Outputs a volume-wide deduplication scan report from a ScanResult.
 */
void output_scan_report(const ScanResult& scan, output::IOutput& out) {
    // Count shared clusters and compute savings
    ULONGLONG shared_clusters = 0;
    ULONGLONG saved_clusters  = 0;

    for (const auto& [lcn, entries] : scan.lcn_index) {
        if (entries.size() >= 2) {
            shared_clusters++;
            saved_clusters += (entries.size() - 1);
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
    {
        std::wostringstream ss;
        ss << std::fixed << std::setprecision(2) << shared_bytes / (1024.0 * 1024.0)
           << L" MB (" << shared_clusters << L" clusters)";
        out.field(L"Shared Blocks", ss.str());
    }
    {
        std::wostringstream ss;
        ss << std::fixed << std::setprecision(2) << saved_bytes / (1024.0 * 1024.0)
           << L" MB saved";
        out.field(L"Dedup Savings", ss.str());
    }
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
        out.error(err);
    }
}

// ============================================================================
// Helpers: Volume Root Detection
// ============================================================================

/**
 * @brief Returns true if the given path looks like a volume root (e.g. "E:\").
 */
bool is_volume_root(const std::wstring& path) {
    if (path.size() == 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) {
        return true;
    }
    return false;
}

// ============================================================================
// Entry Point
// ============================================================================

std::expected<int, std::wstring> execute_inspect(const util::CliArg& args, output::IOutput& out) {
    std::vector<std::wstring> target_paths = args.positional;

    if (!args.input_file.empty()) {
        auto file_list_res = read_file_list(args.input_file);
        if (!file_list_res) return std::unexpected(file_list_res.error());
        target_paths.insert(target_paths.end(), file_list_res->begin(), file_list_res->end());
    }

    if (target_paths.empty()) {
        return std::unexpected(L"Error: No target files or volume specified for inspection.");
    }

    // Volume scan mode: single argument that is a volume root
    if (target_paths.size() == 1 && is_volume_root(target_paths[0])) {
        ScanMode mode = ScanMode::kLcnOnly;
        // --hash flag (if we add it to CliArg in future); for now: detect via any future flag
        auto scan_res = build_lcn_index(target_paths[0], mode, out);
        if (!scan_res) return std::unexpected(scan_res.error());
        output_scan_report(*scan_res, out);
        return 0;
    }

    // Single or multi-file mode
    std::vector<FileInspectResult> results;
    std::vector<std::wstring> errors;

    for (const auto& path : target_paths) {
        auto res = inspect_file(path);
        if (!res.error.empty()) {
            errors.push_back(path + L": " + res.error);
            if (args.strict) {
                return std::unexpected(L"Strict Mode: Failed to inspect " + path +
                                       L". Error: " + res.error);
            }
        }
        results.push_back(res);
    }

    if (results.size() == 1) {
        const auto& res = results[0];
        if (!res.error.empty()) return std::unexpected(res.error);
        output_single_file(res, out);
    } else {
        output_multi_file(results, errors, out);
    }

    return 0;
}

} // namespace inspect
