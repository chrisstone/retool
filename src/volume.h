#pragma once

#include <expected>
#include <string>

#include "util.h"

namespace volume {

std::expected<int, std::wstring> run(const util::CliArg& args);

} // namespace volume
