#pragma once

#include <expected>
#include <string>

#include "util.h"

namespace copy {

std::expected<int, std::wstring> run(const util::CliArg& args);

} // namespace copy
