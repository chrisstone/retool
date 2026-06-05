/**
 * @file copy.h
 * @brief Header for ReFS deduplication-preserving copy operations.
 */

#pragma once

#include <atomic>
#include <expected>
#include <string>

#include "output.h"
#include "util.h"

namespace copy {

extern std::atomic<bool> g_cancel_requested;
extern BOOL g_cancel_requested_bool;

/**
 * @brief Executes the copy subcommand using the parsed command line arguments.
 * 
 * Walks through source file(s) or directories and replicates them to the target path.
 * Uses FSCTL_DUPLICATE_EXTENTS_TO_FILE when copying within the same volume, or cross-volume
 * deduplication mapping when copying to a separate ReFS volume, preserving block allocation sharing.
 * 
 * @param args The parsed CLI arguments containing path, dry-run, recursive, and strict flags.
 * @param out  The output interface for formatted results and progress.
 * @return std::expected<int, std::wstring> Exit code on success, or an error message on failure.
 */
std::expected<int, std::wstring> execute_copy(const util::CliArg& args, output::IOutput& out);

} // namespace copy
