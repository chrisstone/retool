#pragma once

#include <expected>
#include <string>

#include "util.h"

namespace inspect {

std::expected<int, std::wstring> run(const util::CliArg& args);

} // namespace inspect
