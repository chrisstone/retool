/**
 * @file dedup.h
 * @brief Header for in-place file deduplication on ReFS volumes.
 */

#pragma once

#include <expected>
#include <string>

#include "output.h"
#include "util.h"

namespace dedup {

/**
 * @brief Executes the dedup subcommand using the parsed command line arguments.
 *
 * Supports two modes:
 *  - Volume-wide dedup: a single volume root argument (e.g. L"E:\\") scans
 *    all files on the volume and deduplicates clusters that share identical content.
 *  - Pair-wise dedup: two file paths are provided; clusters with matching content
 *    are deduplicated between them.
 *
 * Respects --dry-run to simulate without writing. Cancellation via Ctrl+C is
 * observed through the shared g_cancel_requested flag in copy.h.
 *
 * @param args Parsed CLI arguments (positional paths, dry_run, strict, json flags).
 * @param out  Output interface for status, progress, and summary results.
 * @return Exit code on success, or error string on failure.
 */
std::expected<int, std::wstring> execute_dedup(const util::CliArg& args, output::IOutput& out);

} // namespace dedup
