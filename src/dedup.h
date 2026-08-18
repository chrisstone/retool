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

/// @brief Opaque context produced by prepare() and consumed by execute().
/// Full definition is internal to dedup.cpp; main.cpp only holds a pointer.
struct DedupContext;

/**
 * @brief Phase 1 - Validates arguments, scans the volume, and builds the dedup plan.
 *
 * Accepts either:
 *  - A single kVolume specifier (e.g. E:) for volume-wide deduplication.
 *  - Two kPath specifiers for pair-wise deduplication of fileRef and fileOp.
 *
 * This phase performs the volume scan (FSCTL_GET_RETRIEVAL_POINTERS + SHA-256 hash)
 * and constructs the rebuild plans. It is the most time-consuming phase.
 *
 * @param args Parsed CLI arguments.
 * @return Heap-allocated DedupContext on success, or error string on failure.
 *         The caller is responsible for calling cleanup() on the returned pointer.
 */
std::expected<DedupContext*, std::wstring> prepare(const util::CliArg& args);

/**
 * @brief Phase 2 - Executes the deduplication rebuild plans.
 *
 * Issues FSCTL_DUPLICATE_EXTENTS_TO_FILE for each cluster that can be shared.
 * Respects dry-run mode and Ctrl+C cancellation.
 *
 * @param ctx  Context produced by prepare().
 * @param out  Output interface for status, progress, and summary results.
 * @return Exit code on success, or error string on failure.
 */
std::expected<int, std::wstring> execute(DedupContext& ctx, output::IOutput& out);

/**
 * @brief Phase 3 - Releases all resources held by the context and deletes it.
 *
 * Must be called even if execute() failed or was cancelled.
 *
 * @param ctx  Context produced by prepare(). Set to nullptr after deletion.
 */
void cleanup(DedupContext* ctx) noexcept;

} // namespace dedup
