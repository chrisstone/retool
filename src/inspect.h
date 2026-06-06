/**
 * @file inspect.h
 * @brief Header for block layout inspection, volume scan engine, and cross-file sharing analysis.
 */

#pragma once

#include <expected>
#include <string>
#include <unordered_map>
#include <vector>

#include "output.h"
#include "util.h"

namespace inspect {

// ============================================================================
// Scan Engine Types
// ============================================================================

/// @brief Controls what the volume scan engine computes per cluster.
enum class ScanMode {
    kLcnOnly,   ///< Build LCN->file map without reading disk content (fast).
    kWithHash,  ///< Also SHA-256 hash each cluster to enable content-based matching.
};

/// @brief A record associating a physical cluster (LCN) with a file and byte offset.
struct BlockEntry {
    std::wstring file_path;   ///< Absolute path of the owning file.
    ULONGLONG    file_offset; ///< Byte offset of this cluster within the file.
};

/// @brief Maps each LCN to the list of files that physically share that cluster.
/// Clusters shared by two or more files will have more than one BlockEntry.
using LcnIndex = std::unordered_map<LONGLONG, std::vector<BlockEntry>>;

/// @brief Maps a SHA-256 hex digest to the list of LCNs that contain identical content.
/// Only populated when ScanMode::kWithHash is used.
using HashIndex = std::unordered_map<std::string, std::vector<LONGLONG>>;

/// @brief Result produced by build_lcn_index.
struct ScanResult {
    LcnIndex  lcn_index;           ///< Primary LCN-to-file mapping.
    HashIndex hash_index;          ///< Content hash index (kWithHash only).
    DWORD     cluster_size = 0;    ///< Volume cluster size in bytes.
    std::wstring volume_root;      ///< Volume root that was scanned.
    ULONGLONG files_scanned = 0;   ///< Total files successfully enumerated.
    ULONGLONG clusters_indexed = 0;///< Total clusters entered into the index.
    std::vector<std::wstring> errors; ///< Non-fatal per-file errors encountered.
};

// ============================================================================
// Public API
// ============================================================================

/**
 * @brief Enumerates all files on a volume and builds an LCN (and optionally hash) index.
 *
 * Walks the entire volume tree starting from @p volume_root using FindFirstFileW.
 * For each file, queries physical extents via FSCTL_GET_RETRIEVAL_POINTERS.
 * In kWithHash mode, also reads and SHA-256 hashes every cluster-aligned block.
 *
 * Progress and non-fatal errors are reported through @p status_out.
 *
 * @param volume_root  Volume root path (e.g. L"E:\\").
 * @param mode         Scan mode (LCN-only or with content hashing).
 * @param status_out   Output interface for status and warning messages.
 * @return ScanResult on success, or error string on fatal failure.
 */
std::expected<ScanResult, std::wstring> build_lcn_index(
    const std::wstring& volume_root,
    ScanMode mode,
    output::IOutput& status_out
);

/**
 * @brief Executes the inspect subcommand using the parsed command line arguments.
 *
 * Supports three modes based on positional arguments:
 *  - Single file path: block layout dump.
 *  - Multiple file paths: cross-file deduplication sharing report.
 *  - Single volume root (e.g. "E:\\"): full volume deduplication scan report.
 *
 * @param args The parsed CLI arguments.
 * @param out  The output interface for formatted results.
 * @return Exit code on success, or error string on failure.
 */
std::expected<int, std::wstring> execute_inspect(const util::CliArg& args, output::IOutput& out);

/**
 * @brief Computes a SHA-256 hex digest of a data buffer.
 *
 * Self-contained: opens and closes its own BCrypt algorithm provider per call.
 * For bulk hashing (thousands of calls), prefer caching the algorithm handle
 * externally and calling the internal hash_sha256() directly.
 *
 * @param data      Pointer to the data to hash.
 * @param data_len  Length of the data in bytes.
 * @return 64-character lowercase hex string on success, or empty string on failure.
 */
std::string compute_sha256(const BYTE* data, DWORD data_len);

} // namespace inspect
