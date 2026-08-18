#include <expected>
#include <string>

#include <windows.h>

#include "output.h"
#include "util.h"
#include "volume.h"

namespace volume {

// ============================================================================
// Phase 1: Prepare
// ============================================================================

/**
 * @brief Validates arguments and constructs a VolumeContext.
 *
 * @param args CLI arguments; expects exactly one file_spec containing a volume or file path.
 * @return Populated VolumeContext on success, or error string on failure.
 */
std::expected<VolumeContext, std::wstring> prepare(const util::CliArg& args) {
    if (args.file_specs.empty()) {
        return std::unexpected(L"Error: Missing volume path argument. Usage: retool volume <drive-letter or path>");
    }
    return VolumeContext{args.file_specs[0].path};
}

// ============================================================================
// Phase 2: Execute
// ============================================================================

/**
 * @brief Resolves the volume path, queries filesystem information, and outputs statistics.
 *
 * @param ctx  Context produced by prepare().
 * @param out  Output interface for formatted results.
 * @return Exit code on success, or error string on failure.
 */
std::expected<int, std::wstring> execute(VolumeContext& ctx, output::IOutput& out) {
    // Resolve volume root, cluster size, and filesystem name in one call.
    auto vol_info = util::resolve_volume_info(ctx.input_path);
    if (!vol_info) return std::unexpected(vol_info.error());
    const std::wstring& volume_root = vol_info->volume_root;
    DWORD cluster_size = vol_info->cluster_size;

    // Unlike copy/dedup (which only need a ReFS yes/no), this command's whole
    // purpose is reporting filesystem info, so an unresolved fs_name is a hard
    // error here rather than treated as "assume not ReFS".
    if (vol_info->fs_name.empty()) {
        return std::unexpected(L"Failed to query volume information for " + volume_root +
                               L": filesystem name could not be determined.");
    }
    const std::wstring& fs_name_str = vol_info->fs_name;

    // Get 64-bit safe sizes
    ULARGE_INTEGER free_bytes_avail = {0};
    ULARGE_INTEGER total_bytes = {0};
    ULARGE_INTEGER total_free_bytes = {0};
    if (!GetDiskFreeSpaceExW(volume_root.c_str(), &free_bytes_avail, &total_bytes, &total_free_bytes)) {
        DWORD error = GetLastError();
        return std::unexpected(L"Failed to query extended disk free space: " + util::get_win32_error_message(error));
    }

    ULONGLONG total_space = total_bytes.QuadPart;
    ULONGLONG free_space = total_free_bytes.QuadPart;
    ULONGLONG used_space = total_space > free_space ? (total_space - free_space) : 0;
    ULONGLONG total_clusters = cluster_size > 0 ? (total_space / cluster_size) : 0;

    // Output via IOutput
    out.begin_section(L"Volume Information");

    out.field(L"Volume",         volume_root);
    out.field(L"File System",    fs_name_str);

    if (fs_name_str != L"ReFS") {
        out.message(output::Level::warn, L"This is not a ReFS filesystem. Some features (cloning) will not work.");
    }

    out.field(L"Cluster Size",   std::to_wstring(cluster_size) + L" bytes");
    out.field(L"Total Clusters", std::to_wstring(total_clusters));

    out.field(L"Total Space",    util::format_size_detailed(total_space));
    out.field(L"Free Space",     util::format_size_detailed(free_space));
    out.field(L"Used Space",     util::format_size_detailed(used_space));

    out.end_section();

    return 0;
}

// ============================================================================
// Phase 3: Cleanup
// ============================================================================

/// @brief No resources to release for VolumeContext; provided for API consistency.
void cleanup(VolumeContext& /*ctx*/) noexcept {}

} // namespace volume
