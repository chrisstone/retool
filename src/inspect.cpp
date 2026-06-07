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

/// @brief Fragmentation statistics computed from a file's extent list.
struct FragStat {
    ULONGLONG fragment_count      = 0; ///< Number of non-sparse extents.
    ULONGLONG total_clusters      = 0; ///< Total non-sparse clusters.
    ULONGLONG min_extent_clusters = 0; ///< Smallest non-sparse extent (clusters).
    ULONGLONG max_extent_clusters = 0; ///< Largest non-sparse extent (clusters).
    double    avg_extent_clusters = 0; ///< Mean extent size (clusters).
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
    bool first_line = true;
    while (std::getline(file, line)) {
        // Strip UTF-8 BOM only from the first line of the file
        if (first_line) {
            first_line = false;
            if (line.size() >= 3 &&
                static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF) {
                line = line.substr(3);
            }
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

std::string compute_sha256(const BYTE* data, DWORD data_len) {
    BcryptAlgHandle alg;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg.handle, BCRYPT_SHA256_ALGORITHM, NULL, 0))) {
        return {};
    }
    return hash_sha256(alg.handle, data, data_len);
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

    // Phase C: Inspect each file, build index.
    // Each file is opened once: extents are queried and (optionally) cluster data
    // is hashed through the same handle, avoiding redundant CreateFileW/CloseHandle.
    const ULONGLONG kProgressInterval = 500;
    std::vector<BYTE> read_buffer;

    if (mode == ScanMode::kWithHash && result.cluster_size > 0) {
        read_buffer.resize(result.cluster_size);
    }

    const DWORD rp_buf_size = sizeof(RETRIEVAL_POINTERS_BUFFER) +
                              sizeof(RETRIEVAL_POINTERS_BUFFER::Extents[0]) * 16;
    std::vector<BYTE> rp_buffer(rp_buf_size);

    for (const auto& path : file_paths) {
        // Open the file once for both extent query and optional hashing
        HANDLE file_handle = CreateFileW(
            path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL
        );
        if (file_handle == INVALID_HANDLE_VALUE) {
            result.errors.push_back(path + L": " + util::get_win32_error_message(GetLastError()));
            continue;
        }

        // Get file size
        LARGE_INTEGER fs_size;
        if (!GetFileSizeEx(file_handle, &fs_size)) {
            result.errors.push_back(path + L": GetFileSizeEx failed: " +
                                    util::get_win32_error_message(GetLastError()));
            CloseHandle(file_handle);
            continue;
        }
        ULONGLONG file_size = fs_size.QuadPart;

        // Query retrieval pointers (extents)
        auto rp_out = reinterpret_cast<PRETRIEVAL_POINTERS_BUFFER>(rp_buffer.data());
        STARTING_VCN_INPUT_BUFFER input = {0};
        input.StartingVcn.QuadPart = 0;

        std::vector<VcnExtent> extents;
        bool done = false;
        LONGLONG current_vcn = 0;

        while (!done) {
            DWORD bytes_returned = 0;
            BOOL ok = DeviceIoControl(
                file_handle, FSCTL_GET_RETRIEVAL_POINTERS,
                &input, sizeof(input), rp_out, rp_buf_size,
                &bytes_returned, NULL
            );

            DWORD err = GetLastError();
            if (!ok && err != ERROR_MORE_DATA) {
                if (err == ERROR_HANDLE_EOF) break;
                result.errors.push_back(path + L": FSCTL_GET_RETRIEVAL_POINTERS: " +
                                        util::get_win32_error_message(err));
                break;
            }
            if (ok) done = true;

            current_vcn = rp_out->StartingVcn.QuadPart;
            for (DWORD i = 0; i < rp_out->ExtentCount; ++i) {
                VcnExtent ext;
                ext.start_vcn    = current_vcn;
                ext.next_vcn     = rp_out->Extents[i].NextVcn.QuadPart;
                ext.lcn          = rp_out->Extents[i].Lcn.QuadPart;
                ext.cluster_count = ext.next_vcn - ext.start_vcn;
                extents.push_back(ext);
                current_vcn = ext.next_vcn;
            }
            input.StartingVcn.QuadPart = current_vcn;
        }

        // Intern the file path
        uint32_t file_idx = static_cast<uint32_t>(result.file_table.size());
        result.file_table.push_back(path);

        result.files_scanned++;

        // Periodic progress update
        if (result.files_scanned % kProgressInterval == 0) {
            status_out.status(L"[build_lcn_index] Indexed " +
                              std::to_wstring(result.files_scanned) + L" files...");
        }

        // Build index entries and optionally hash each cluster
        for (const auto& ext : extents) {
            if (ext.lcn == (LONGLONG)-1) continue; // Skip sparse

            for (ULONGLONG c = 0; c < ext.cluster_count; ++c) {
                LONGLONG    lcn         = ext.lcn + static_cast<LONGLONG>(c);
                ULONGLONG   file_offset = (ext.start_vcn + c) * result.cluster_size;

                BlockEntry entry;
                entry.file_index  = file_idx;
                entry.file_offset = file_offset;

                result.lcn_index[lcn].push_back(entry);
                result.clusters_indexed++;

                // Hash this cluster if requested (reuses the already-open handle)
                if (mode == ScanMode::kWithHash) {
                    LARGE_INTEGER li;
                    li.QuadPart = static_cast<LONGLONG>(file_offset);
                    if (!SetFilePointerEx(file_handle, li, NULL, FILE_BEGIN)) continue;

                    DWORD bytes_read = 0;
                    DWORD to_read = static_cast<DWORD>(
                        std::min<ULONGLONG>(result.cluster_size,
                            file_size > file_offset
                                ? file_size - file_offset
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

        CloseHandle(file_handle);
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
 *
 * The output is wrapped in a named section so that JSON output produces
 * a clean nested object rather than polluting the root.
 *
 * @param res          The inspection result for the file.
 * @param show_extents If true, emit the full VCN/LCN extent table.
 *                     If false, emit only the summary fields.
 */
void output_single_file(const FileInspectResult& res, output::IOutput& out, bool show_extents) {
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
        if (ext.lcn == (LONGLONG)-1) continue; // Skip sparse

        stat.fragment_count++;
        stat.total_clusters += ext.cluster_count;

        if (stat.fragment_count == 1) {
            stat.min_extent_clusters = ext.cluster_count;
            stat.max_extent_clusters = ext.cluster_count;
        } else {
            if (ext.cluster_count < stat.min_extent_clusters)
                stat.min_extent_clusters = ext.cluster_count;
            if (ext.cluster_count > stat.max_extent_clusters)
                stat.max_extent_clusters = ext.cluster_count;
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
 * per-file unique/shared cluster breakdown table only — no cross-file sharing
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
    bool show_extended
) {
    // ── Build LCN → file index ────────────────────────────────────────────────
    std::unordered_map<LONGLONG, std::vector<size_t>> lcn_to_files;
    ULONGLONG total_file_bytes    = 0;
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

    // ── Summary header (always shown) ────────────────────────────────────────
    out.begin_section(L"inspect multi-file report");
    out.field(L"Volume",         common_volume);
    out.field(L"Cluster Size",   std::to_wstring(common_cluster_size) + L" bytes");
    out.field(L"Files Analyzed", std::to_wstring(results.size()));
    out.field(L"Total Size",     util::format_size(total_file_bytes) +
              L" (" + std::to_wstring(total_file_bytes) + L" bytes)");
    out.field(L"Shared Blocks",  util::format_size(shared_bytes) +
              L" (" + std::to_wstring(shared_bytes) + L" bytes)");
    out.field(L"Saved Space",    util::format_size(saved_bytes) +
              L" (" + std::to_wstring(saved_bytes) + L" bytes)");
    {
        std::wostringstream ss;
        ss << std::fixed << std::setprecision(2) << savings_pct << L"%";
        out.field(L"Dedup Savings", ss.str());
    }
    out.end_section();

    // ── Per-file unique / shared cluster breakdown (always shown) ────────────
    out.begin_section(L"Per-File Cluster Breakdown");
    out.begin_table({L"File", L"Total Clusters", L"Unique Clusters",
                     L"Shared Clusters", L"Unique Bytes", L"Shared Bytes"});

    ULONGLONG grand_total = 0, grand_unique = 0, grand_shared = 0;

    for (size_t f_idx = 0; f_idx < results.size(); ++f_idx) {
        const auto& res = results[f_idx];
        if (!res.error.empty()) continue;

        std::wstring path = res.path;
        size_t last_slash2 = path.find_last_of(L"\\/ ");
        std::wstring name2 = (last_slash2 != std::wstring::npos)
            ? path.substr(last_slash2 + 1) : path;
        if (name2.size() > 24) name2 = name2.substr(0, 21) + L"...";

        std::unordered_set<LONGLONG> file_lcn_set;
        for (const auto& ext : res.extents) {
            if (ext.lcn == (LONGLONG)-1) continue;
            for (ULONGLONG off = 0; off < ext.cluster_count; ++off) {
                file_lcn_set.insert(ext.lcn + static_cast<LONGLONG>(off));
            }
        }
        ULONGLONG total_c  = file_lcn_set.size();
        ULONGLONG unique_c = 0;
        ULONGLONG shared_c = 0;

        for (LONGLONG lcn : file_lcn_set) {
            auto it = lcn_to_files.find(lcn);
            if (it != lcn_to_files.end() && it->second.size() >= 2) {
                ++shared_c;
            } else {
                ++unique_c;
            }
        }

        grand_total  += total_c;
        grand_unique += unique_c;
        grand_shared += shared_c;

        out.table_row({
            name2,
            std::to_wstring(total_c),
            std::to_wstring(unique_c),
            std::to_wstring(shared_c),
            util::format_size(unique_c * common_cluster_size),
            util::format_size(shared_c * common_cluster_size)
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

        out.begin_section(L"Sharing Matrix");
        out.begin_table(columns);
        for (size_t i = 0; i < results.size(); ++i) {
            std::wstring path = results[i].path;
            size_t last_slash = path.find_last_of(L"\\/ ");
            std::wstring name = (last_slash != std::wstring::npos) ? path.substr(last_slash + 1) : path;
            if (name.size() > 18) name = name.substr(0, 15) + L"...";

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
    out.field(L"Shared Blocks", util::format_size(shared_bytes) +
              L" (" + std::to_wstring(shared_clusters) + L" clusters)");
    out.field(L"Dedup Savings", util::format_size(saved_bytes) + L" saved");
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
    // Drive letter: "E:\" or "E:/"
    if (path.size() >= 2 && path[1] == L':') {
        if (path.size() == 2) return true; // "E:" without trailing slash
        if (path.size() == 3 && (path[2] == L'\\' || path[2] == L'/')) return true;
    }
    // Volume GUID path: "\\?\Volume{...}\"
    if (path.size() >= 11 && path.substr(0, 11) == L"\\\\?\\Volume") {
        return true;
    }
    return false;
}

/**
 * @brief Expands a single positional argument into a list of file paths.
 *
 * Handles four cases:
 *  - Volume root (e.g. "E:\") → returned as-is in a single-element vector
 *    (caller decides volume-scan vs. file mode).
 *  - Plain directory (no wildcard, is a directory) → recursively enumerates
 *    all files under it using enumerate_files_recursive.
 *  - Glob pattern (contains '*' or '?') → expands non-recursively with
 *    FindFirstFileW in the directory portion of the pattern.
 *  - Ordinary file path → returned as-is in a single-element vector.
 *
 * @param arg    The raw positional argument string.
 * @param errors Accumulates non-fatal error messages.
 * @return Flat list of matching file paths.
 */
std::vector<std::wstring> expand_path_glob(
    const std::wstring& arg,
    std::vector<std::wstring>& errors
) {
    // Volume root: pass through unchanged so caller can handle volume scan
    if (is_volume_root(arg)) {
        return {arg};
    }

    bool has_wildcard = (arg.find(L'*') != std::wstring::npos ||
                         arg.find(L'?') != std::wstring::npos);

    // Plain directory (no wildcard): recursive enumeration
    if (!has_wildcard) {
        DWORD attrs = GetFileAttributesW(arg.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            std::wstring dir = arg;
            if (dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
            std::vector<std::wstring> files;
            enumerate_files_recursive(dir, files, errors);
            return files;
        }
        // Ordinary file
        return {arg};
    }

    // Glob pattern: extract directory portion, enumerate with FindFirstFileW
    std::wstring dir_part;
    size_t last_sep = arg.find_last_of(L"\\/");
    if (last_sep != std::wstring::npos) {
        dir_part = arg.substr(0, last_sep + 1);
    } else {
        // No directory prefix — use current directory
        wchar_t cwd[MAX_PATH];
        if (GetCurrentDirectoryW(MAX_PATH, cwd)) {
            dir_part = std::wstring(cwd) + L'\\';
        }
    }

    std::vector<std::wstring> files;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(arg.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND) {
            errors.push_back(L"Glob expansion failed for '" + arg + L"': " +
                             util::get_win32_error_message(err));
        }
        return files;
    }
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM)    continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        files.push_back(dir_part + name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return files;
}

// ============================================================================
// Entry Point
// ============================================================================

std::expected<int, std::wstring> execute_inspect(const util::CliArg& args, output::IOutput& out) {
    // ── Collect raw targets from positional args and optional -i file list ───
    std::vector<std::wstring> raw_targets = args.positional;

    if (!args.input_file.empty()) {
        auto file_list_res = read_file_list(args.input_file);
        if (!file_list_res) return std::unexpected(file_list_res.error());
        raw_targets.insert(raw_targets.end(),
                           file_list_res->begin(), file_list_res->end());
    }

    if (raw_targets.empty()) {
        return std::unexpected(L"Error: No target files, directory, or volume specified for inspection.");
    }

    // ── Expand directories and glob patterns ─────────────────────────────────
    std::vector<std::wstring> target_paths;
    std::vector<std::wstring> expand_errors;

    for (const auto& raw : raw_targets) {
        auto expanded = expand_path_glob(raw, expand_errors);
        target_paths.insert(target_paths.end(), expanded.begin(), expanded.end());
    }

    for (const auto& err : expand_errors) {
        out.warn(err);
    }

    if (target_paths.empty()) {
        return std::unexpected(L"Error: No files matched the specified path(s).");
    }

    // ── Volume scan mode: single volume-root argument ─────────────────────────
    if (target_paths.size() == 1 && is_volume_root(target_paths[0])) {
        ScanMode mode = ScanMode::kLcnOnly;
        auto scan_res = build_lcn_index(target_paths[0], mode, out);
        if (!scan_res) return std::unexpected(scan_res.error());
        output_scan_report(*scan_res, out);
        return 0;
    }

    // ── Single or multi-file mode ─────────────────────────────────────────────
    std::vector<FileInspectResult> results;
    std::vector<std::wstring> errors;

    if (target_paths.size() == 1) {
        // Single-file: emit a brief status then inspect
        const std::wstring& path = target_paths[0];
        std::wstring name = path;
        size_t last_sep = name.find_last_of(L"\\/ ");
        if (last_sep != std::wstring::npos) name = name.substr(last_sep + 1);
        out.status(L"Inspecting: " + name);
    } else {
        // Multi-file: announce the count up-front, then log each file
        out.status(L"Inspecting " + std::to_wstring(target_paths.size()) + L" files...");
    }

    const ULONGLONG total_files = static_cast<ULONGLONG>(target_paths.size());

    for (ULONGLONG f_idx = 0; f_idx < total_files; ++f_idx) {
        const std::wstring& path = target_paths[static_cast<size_t>(f_idx)];

        // Drive the progress bar for multi-file mode.
        // "scanning" is the fixed label shown in the progress display.
        if (total_files > 1) {
            out.progress(L"scanning", f_idx, total_files);
        }

        auto res = inspect_file(path);
        if (!res.error.empty()) {
            errors.push_back(path + L": " + res.error);
            if (args.strict) {
                return std::unexpected(L"Strict Mode: Failed to inspect " + path +
                                       L". Error: " + res.error);
            }
        }
        results.push_back(std::move(res));
    }

    // Complete the progress bar (clears the line before output begins)
    if (total_files > 1) {
        out.progress(L"scanning", total_files, total_files);
    }

    if (results.size() == 1) {
        const auto& res = results[0];
        if (!res.error.empty()) return std::unexpected(res.error);
        output_single_file(res, out, args.show_extents);
        if (args.recursive) {
            auto stat = compute_frag_stat(res);
            output_frag_report(res, stat, out);
        }
    } else {
        output_multi_file(results, errors, out, args.show_extents);
        if (args.recursive) {
            for (const auto& res : results) {
                if (!res.error.empty()) continue;
                auto stat = compute_frag_stat(res);
                output_frag_report(res, stat, out);
            }
        }
    }

    return 0;
}

} // namespace inspect
