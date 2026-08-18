/**
 * @file copy.h
 * @brief Header for ReFS deduplication-preserving copy operations.
 */

#pragma once

#include <expected>
#include <string>

#include "output.h"
#include "util.h"

namespace copy {

/// @brief Opaque context produced by prepare() and consumed by gather() and execute().
/// Full definition is internal to copy.cpp; main.cpp only holds a pointer.
struct CopyContext;

/**
 * @brief Phase 1 - Validates arguments and resolves paths, volumes, and copy strategy.
 *
 * Accepts exactly two kPath specifiers: source and destination. Rejects kVolume
 * specifiers and more than two specifiers.
 *
 * No filesystem writes are performed during this phase.
 *
 * @param args Parsed CLI arguments.
 * @return Heap-allocated CopyContext on success, or error string on failure.
 *         The caller is responsible for calling cleanup() on the returned pointer.
 */
std::expected<CopyContext*, std::wstring> prepare(const util::CliArg& args);

/**
 * @brief Phase 2 - Enumerates source files, collects LCNs, and pre-scans destination.
 *
 * Populates the context with all source→destination file mappings. For cross-volume
 * ReFS copies, queries retrieval pointers for all source files to count unique LCNs,
 * which drives the overall progress bar in execute(). When --scan-dest is active,
 * pre-scans the destination volume here. All significant memory allocation occurs
 * in this phase; execute() allocates no new large data structures.
 *
 * @param ctx  Context produced by prepare().
 * @param out  Output interface for progress and status messages.
 * @return true on success, or error string on failure.
 */
std::expected<bool, std::wstring> gather(CopyContext& ctx, output::IOutput& out);

/**
 * @brief Phase 3 - Copies source file(s) or directories to the destination.
 *
 * Iterates the file pairs built by gather(). Uses FSCTL_DUPLICATE_EXTENTS_TO_FILE
 * when copying within the same ReFS volume, or cross-volume deduplication mapping
 * when copying to a separate ReFS volume, preserving block allocation sharing.
 * Reports per-file byte progress and (for cross-volume ReFS) overall unique-LCN progress.
 *
 * @param ctx  Context produced by prepare() and populated by gather().
 * @param out  Output interface for formatted results and progress.
 * @return Exit code on success, or error string on failure.
 */
std::expected<int, std::wstring> execute(CopyContext& ctx, output::IOutput& out);

/**
 * @brief Phase 4 - Releases all resources held by the context and deletes it.
 *
 * Must be called even if gather() or execute() failed or was cancelled.
 *
 * @param ctx  Context produced by prepare(). Deleted by this call.
 */
void cleanup(CopyContext* ctx) noexcept;

} // namespace copy
