#include <expected>
#include <iomanip>
#include <iostream>
#include <string>

#include <windows.h>

#include "util.h"
#include "volume.h"

namespace volume {

/**
 * @brief Executes the volume subcommand.
 * 
 * Resolves the volume path name, queries filesystem information, and prints statistics.
 * 
 * @param args CLI arguments containing positional drive/volume path.
 * @return std::expected<int, std::wstring> Exit code on success, or error string on failure.
 */
std::expected<int, std::wstring> execute_volume(const util::CliArg& args) {
    if (args.positional.empty()) {
        return std::unexpected(L"Error: Missing volume path argument. Usage: retool volume <drive-letter or path>");
    }

    std::wstring input_path = args.positional[0];
    
    // Resolve volume root path
    wchar_t volume_root[MAX_PATH];
    if (!GetVolumePathNameW(input_path.c_str(), volume_root, MAX_PATH)) {
        DWORD error = GetLastError();
        return std::unexpected(L"Failed to get volume path name: " + util::get_win32_error_message(error));
    }

    // Query filesystem type
    wchar_t fs_name[MAX_PATH] = {0};
    wchar_t vol_name[MAX_PATH] = {0};
    DWORD fs_flags = 0;
    if (!GetVolumeInformationW(
            volume_root,
            vol_name, MAX_PATH,
            NULL, NULL,
            &fs_flags,
            fs_name, MAX_PATH)) {
        DWORD error = GetLastError();
        return std::unexpected(L"Failed to query volume information for " + std::wstring(volume_root) + L": " + util::get_win32_error_message(error));
    }

    std::wstring fs_name_str(fs_name);

    // Get cluster size
    DWORD sectors_per_cluster = 0;
    DWORD bytes_per_sector = 0;
    DWORD free_clusters = 0;
    DWORD total_clusters_32 = 0;
    if (!GetDiskFreeSpaceW(volume_root, &sectors_per_cluster, &bytes_per_sector, &free_clusters, &total_clusters_32)) {
        DWORD error = GetLastError();
        return std::unexpected(L"Failed to query disk free space: " + util::get_win32_error_message(error));
    }

    DWORD cluster_size = sectors_per_cluster * bytes_per_sector;

    // Get 64-bit safe sizes
    ULARGE_INTEGER free_bytes_avail = {0};
    ULARGE_INTEGER total_bytes = {0};
    ULARGE_INTEGER total_free_bytes = {0};
    if (!GetDiskFreeSpaceExW(volume_root, &free_bytes_avail, &total_bytes, &total_free_bytes)) {
        DWORD error = GetLastError();
        return std::unexpected(L"Failed to query extended disk free space: " + util::get_win32_error_message(error));
    }

    ULONGLONG total_space = total_bytes.QuadPart;
    ULONGLONG free_space = total_free_bytes.QuadPart;
    ULONGLONG used_space = total_space > free_space ? (total_space - free_space) : 0;
    ULONGLONG total_clusters = cluster_size > 0 ? (total_space / cluster_size) : 0;

    // Plain text output
    std::wcout << L"Volume:        " << volume_root << L"\n"
               << L"File System:   " << fs_name_str << L"\n";
    
    if (fs_name_str != L"ReFS") {
        std::wcout << L"WARNING:       This is not a ReFS filesystem. Some features (cloning) will not work.\n";
    }

    double total_gb = static_cast<double>(total_space) / (1024.0 * 1024.0 * 1024.0);
    double free_gb = static_cast<double>(free_space) / (1024.0 * 1024.0 * 1024.0);
    double used_gb = static_cast<double>(used_space) / (1024.0 * 1024.0 * 1024.0);

    std::wcout << L"Cluster Size:  " << cluster_size << L" bytes\n"
               << L"Total Clusters: " << total_clusters << L"\n"
               << std::fixed << std::setprecision(2)
               << L"Total Space:   " << total_gb << L" GB (" << total_space << L" bytes)\n"
               << L"Free Space:    " << free_gb << L" GB (" << free_space << L" bytes)\n"
               << L"Used Space:    " << used_gb << L" GB (" << used_space << L" bytes)" << std::endl;

    return 0;
}

} // namespace volume
