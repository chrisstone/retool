/**
 * @file volume.h
 * @brief Header for volume metadata and block statistics retrieval.
 */

#pragma once

#include <expected>
#include <string>

#include "util.h"

namespace volume {

/**
 * @brief Executes the volume subcommand using the parsed command line arguments.
 * 
 * Retrieves filesystem info, cluster size, and cluster capacity/usage information for the specified volume.
 * 
 * @param args The parsed CLI arguments containing the target volume path.
 * @return std::expected<int, std::wstring> Exit code on success, or an error message on failure.
 */
std::expected<int, std::wstring> execute_volume(const util::CliArg& args);

} // namespace volume
