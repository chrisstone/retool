/**
 * @file volume.h
 * @brief Header for volume metadata and block statistics retrieval.
 */

#pragma once

#include <expected>
#include <string>

#include "output.h"
#include "util.h"

namespace volume {

/// @brief Validated context produced by prepare() and consumed by execute().
struct VolumeContext {
    std::wstring input_path;  ///< Volume or file path to query.
};

/**
 * @brief Phase 1 — Validates arguments and constructs a VolumeContext.
 * @return Populated context on success, or error string on failure.
 */
std::expected<VolumeContext, std::wstring> prepare(const util::CliArg& args);

/**
 * @brief Phase 2 — Retrieves volume statistics and emits results.
 * @param ctx  Context produced by prepare().
 * @param out  Output interface.
 * @return Exit code on success, or error string on failure.
 */
std::expected<int, std::wstring> execute(VolumeContext& ctx, output::IOutput& out);

/**
 * @brief Phase 3 — Releases any resources held by the context.
 * No-op for VolumeContext; provided for API consistency.
 */
void cleanup(VolumeContext& ctx) noexcept;

} // namespace volume
