#pragma once

#include <expected>
#include <string>
#include <vector>

#include <windows.h>

namespace util {

struct CliArg {
    std::wstring command;
    std::vector<std::wstring> positional;
    bool strict = false;
    bool dry_run = false;
    bool recursive = false;
    std::wstring input_file;
    std::wstring output_file;
};

// Check if current process has Administrator privileges
std::expected<bool, DWORD> is_elevated();

// Check if ReFS is supported by the OS (registry service key + driver binary presence)
std::expected<bool, DWORD> is_refs_supported();

// Parse command line arguments
std::expected<CliArg, std::wstring> parse_arguments(int argc, wchar_t* argv[]);

// Utility to convert narrow string to wide string
std::wstring to_wstring(const std::string& str);

// Utility to convert wide string to narrow string
std::string to_string(const std::wstring& wstr);

// Utility to format Win32 error code as a message string
std::wstring get_win32_error_message(DWORD error_code);

} // namespace util
