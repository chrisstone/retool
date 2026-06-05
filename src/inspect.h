/**
 * @file inspect.h
 * @brief Header for block layout inspection and cross-file sharing analysis.
 */

#pragma once

#include <expected>
#include <string>

#include "output.h"
#include "util.h"

namespace inspect {

/**
 * @brief Executes the inspect subcommand using the parsed command line arguments.
 *
 * Inspects the block layout (VCN to LCN extents) of a single file or compares sharing
 * patterns across a set of input files to estimate deduplication savings.
 *
 * @param args The parsed CLI arguments containing files, input-list, and strict flags.
 * @param out  The output interface for formatted results.
 * @return std::expected<int, std::wstring> Exit code on success, or an error message on failure.
 */
std::expected<int, std::wstring> execute_inspect(const util::CliArg& args, output::IOutput& out);

} // namespace inspect
