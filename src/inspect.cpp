#include <algorithm>
#include <expected>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <windows.h>

#include "inspect.h"
#include "output.h"
#include "util.h"

namespace inspect {

struct VcnExtent {
    LONGLONG start_vcn;
    LONGLONG next_vcn;
    LONGLONG lcn;
    ULONGLONG cluster_count;
};

struct FileInspectResult {
    std::wstring path;
    std::wstring volume_root;
    DWORD cluster_size = 0;
    ULONGLONG file_size = 0;
    std::vector<VcnExtent> extents;
    std::wstring error;
};



/**
 * @brief Reads a list of file paths from a newline-delimited text file.
 * 
 * Skips empty lines and lines starting with '#'. Strips BOM and trailing spaces.
 * 
 * @param path The path to the text file list.
 * @return std::expected<std::vector<std::wstring>, std::wstring> List of paths, or error string.
 */
std::expected<std::vector<std::wstring>, std::wstring> read_file_list(const std::wstring& path) {
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        return std::unexpected(L"Failed to open input file list: " + path);
    }

    std::vector<std::wstring> paths;
    std::string line;
    while (std::getline(file, line)) {
        // Strip BOM if present
        if (line.size() >= 3 && 
            static_cast<unsigned char>(line[0]) == 0xEF && 
            static_cast<unsigned char>(line[1]) == 0xBB && 
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line = line.substr(3);
        }

        // Strip trailing \r and whitespace
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || isspace(static_cast<unsigned char>(line.back())))) {
            line.pop_back();
        }
        // Strip leading whitespace
        size_t start = 0;
        while (start < line.size() && isspace(static_cast<unsigned char>(line[start]))) {
            start++;
        }

        if (start >= line.size() || line[start] == '#') {
            continue; // Skip comments and empty lines
        }

        std::string cleaned = line.substr(start);
        paths.push_back(util::to_wstring(cleaned));
    }
    return paths;
}

/**
 * @brief Inspects the virtual and physical extents of a single file on ReFS.
 * 
 * Queries the filesystem type, cluster size, file size, and queries extents iteratively using
 * FSCTL_GET_RETRIEVAL_POINTERS.
 * 
 * @param path The path of the target file to inspect.
 * @return FileInspectResult A structure containing paths, extents, size, and error context.
 */
FileInspectResult inspect_file(const std::wstring& path) {
    FileInspectResult res;
    res.path = path;

    // Get volume path root
    wchar_t volume_root[MAX_PATH];
    if (!GetVolumePathNameW(path.c_str(), volume_root, MAX_PATH)) {
        res.error = L"GetVolumePathNameW failed: " + util::get_win32_error_message(GetLastError());
        return res;
    }
    res.volume_root = volume_root;

    // Get cluster size
    DWORD sectors_per_cluster = 0, bytes_per_sector = 0, free_clusters = 0, total_clusters = 0;
    if (!GetDiskFreeSpaceW(volume_root, &sectors_per_cluster, &bytes_per_sector, &free_clusters, &total_clusters)) {
        res.error = L"GetDiskFreeSpaceW failed: " + util::get_win32_error_message(GetLastError());
        return res;
    }
    res.cluster_size = sectors_per_cluster * bytes_per_sector;

    // Open file
    HANDLE file_handle = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        NULL
    );

    if (file_handle == INVALID_HANDLE_VALUE) {
        res.error = L"CreateFileW failed: " + util::get_win32_error_message(GetLastError());
        return res;
    }

    // Get file size
    LARGE_INTEGER fs_size;
    if (!GetFileSizeEx(file_handle, &fs_size)) {
        res.error = L"GetFileSizeEx failed: " + util::get_win32_error_message(GetLastError());
        CloseHandle(file_handle);
        return res;
    }
    res.file_size = fs_size.QuadPart;

    // Retrieve extents
    STARTING_VCN_INPUT_BUFFER input = {0};
    input.StartingVcn.QuadPart = 0;

    // Allocate buffer for retrieval pointers
    const DWORD buf_size = sizeof(RETRIEVAL_POINTERS_BUFFER) + sizeof(RETRIEVAL_POINTERS_BUFFER::Extents[0]) * 16;
    std::vector<BYTE> buffer(buf_size);
    PRETRIEVAL_POINTERS_BUFFER output = reinterpret_cast<PRETRIEVAL_POINTERS_BUFFER>(buffer.data());

    DWORD bytes_returned = 0;
    bool done = false;
    LONGLONG current_vcn = 0;

    while (!done) {
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
            if (err == ERROR_HANDLE_EOF) {
                break; // End of file / sparse file
            }
            res.error = L"FSCTL_GET_RETRIEVAL_POINTERS failed: " + util::get_win32_error_message(err);
            CloseHandle(file_handle);
            return res;
        }

        if (ok) {
            done = true;
        }

        LONGLONG start_vcn = output->StartingVcn.QuadPart;
        current_vcn = start_vcn;

        for (DWORD i = 0; i < output->ExtentCount; ++i) {
            LONGLONG next_vcn = output->Extents[i].NextVcn.QuadPart;
            LONGLONG lcn = output->Extents[i].Lcn.QuadPart;

            VcnExtent extent;
            extent.start_vcn = current_vcn;
            extent.next_vcn = next_vcn;
            extent.lcn = lcn;
            extent.cluster_count = next_vcn - current_vcn;

            res.extents.push_back(extent);
            current_vcn = next_vcn;
        }

        input.StartingVcn.QuadPart = current_vcn;
    }

    CloseHandle(file_handle);
    return res;
}

/**
 * @brief Outputs the single-file inspect report via IOutput.
 *
 * @param res  The inspect result for a single file.
 * @param out  The output interface.
 */
void output_single_file(const FileInspectResult& res, output::IOutput& out) {
    out.field(L"File",          res.path);
    out.field(L"Volume",        res.volume_root);
    out.field(L"Cluster Size",  std::to_wstring(res.cluster_size) + L" bytes");
    out.field(L"File Size",     std::to_wstring(res.file_size) + L" bytes");
    out.field(L"Fragments",     std::to_wstring(res.extents.size()));

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
 *
 * @param results  All inspect results.
 * @param errors   Accumulated error messages.
 * @param out      The output interface.
 */
void output_multi_file(
    const std::vector<FileInspectResult>& results,
    const std::vector<std::wstring>& errors,
    output::IOutput& out
) {
    // Calculate cross-file block sharing
    std::unordered_map<LONGLONG, std::vector<size_t>> lcn_to_files;
    ULONGLONG total_file_bytes = 0;
    DWORD common_cluster_size = 0;
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
            if (ext.lcn == (LONGLONG)-1) continue; // Skip sparse
            for (ULONGLONG offset = 0; offset < ext.cluster_count; ++offset) {
                unique_lcns.insert(ext.lcn + offset);
            }
        }

        for (LONGLONG lcn : unique_lcns) {
            lcn_to_files[lcn].push_back(f_idx);
        }
    }

    // Shared clusters calculation
    ULONGLONG shared_clusters = 0;
    ULONGLONG saved_clusters = 0;
    for (const auto& [lcn, files] : lcn_to_files) {
        if (files.size() >= 2) {
            shared_clusters++;
            saved_clusters += (files.size() - 1);
        }
    }

    ULONGLONG shared_bytes = shared_clusters * common_cluster_size;
    ULONGLONG saved_bytes = saved_clusters * common_cluster_size;

    double savings_pct = total_file_bytes > 0 
        ? (static_cast<double>(saved_bytes) / total_file_bytes) * 100.0 
        : 0.0;

    // Output summary fields
    out.begin_section(L"retool inspect multi-file report");

    out.field(L"Volume",         common_volume);
    out.field(L"Cluster Size",   std::to_wstring(common_cluster_size) + L" bytes");
    out.field(L"Files Analyzed", std::to_wstring(results.size()));

    {
        std::wostringstream ss;
        double total_mb = total_file_bytes / (1024.0 * 1024.0);
        ss << std::fixed << std::setprecision(2) << total_mb << L" MB (" << total_file_bytes << L" bytes)";
        out.field(L"Total Sizes", ss.str());
    }
    {
        std::wostringstream ss;
        double shared_mb = shared_bytes / (1024.0 * 1024.0);
        ss << std::fixed << std::setprecision(2) << shared_mb << L" MB (" << shared_bytes << L" bytes)";
        out.field(L"Shared Blocks", ss.str());
    }
    {
        std::wostringstream ss;
        double saved_mb = saved_bytes / (1024.0 * 1024.0);
        ss << std::fixed << std::setprecision(2) << saved_mb << L" MB (" << saved_bytes << L" bytes)";
        out.field(L"Saved Space", ss.str());
    }
    {
        std::wostringstream ss;
        ss << std::fixed << std::setprecision(2) << savings_pct << L"%";
        out.field(L"Dedup Savings", ss.str());
    }

    // Sharing matrix
    std::vector<std::vector<ULONGLONG>> shared_matrix(results.size(), std::vector<ULONGLONG>(results.size(), 0));
    for (const auto& [lcn, files] : lcn_to_files) {
        if (files.size() < 2) continue;
        for (size_t i = 0; i < files.size(); ++i) {
            for (size_t j = i + 1; j < files.size(); ++j) {
                shared_matrix[files[i]][files[j]]++;
                shared_matrix[files[j]][files[i]]++;
            }
        }
    }

    // Build table columns: "File", "F0", "F1", ...
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
                double mb = (shared_matrix[i][j] * common_cluster_size) / (1024.0 * 1024.0);
                ss << std::fixed << std::setprecision(1) << mb;
                row.push_back(ss.str());
            }
        }
        out.table_row(row);
    }

    out.end_table();
    out.end_section();

    // Output errors
    for (const auto& err : errors) {
        out.error(err);
    }
}

/**
 * @brief Executes the inspect subcommand.
 * 
 * Performs block layout query for a single file, or builds a block sharing/overlap index across
 * multiple files to report deduplication metrics.
 * 
 * @param args CLI arguments containing files, input-list, and output redirect.
 * @param out  Output interface for formatted results.
 * @return std::expected<int, std::wstring> Exit code on success, or error string on failure.
 */
std::expected<int, std::wstring> execute_inspect(const util::CliArg& args, output::IOutput& out) {
    std::vector<std::wstring> target_paths = args.positional;

    // Load file list if specified
    if (!args.input_file.empty()) {
        auto file_list_res = read_file_list(args.input_file);
        if (!file_list_res) {
            return std::unexpected(file_list_res.error());
        }
        target_paths.insert(target_paths.end(), file_list_res->begin(), file_list_res->end());
    }

    if (target_paths.empty()) {
        return std::unexpected(L"Error: No target files specified for inspection.");
    }

    std::vector<FileInspectResult> results;
    std::vector<std::wstring> errors;

    for (const auto& path : target_paths) {
        auto res = inspect_file(path);
        if (!res.error.empty()) {
            errors.push_back(path + L": " + res.error);
            if (args.strict) {
                return std::unexpected(L"Strict Mode: Failed to inspect " + path + L". Error: " + res.error);
            }
        }
        results.push_back(res);
    }

    // Single-file Mode
    if (results.size() == 1) {
        const auto& res = results[0];
        if (!res.error.empty()) {
            return std::unexpected(res.error);
        }
        output_single_file(res, out);
    }
    // Multi-file Mode
    else {
        output_multi_file(results, errors, out);
    }

    return 0;
}

} // namespace inspect
