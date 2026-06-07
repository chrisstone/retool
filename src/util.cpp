#include <expected>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include <windows.h>

#include "util.h"

namespace util {

std::expected<bool, DWORD> is_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return std::unexpected(GetLastError());
    }

    TOKEN_ELEVATION elevation;
    DWORD size = sizeof(elevation);
    if (!GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)) {
        DWORD error = GetLastError();
        CloseHandle(token);
        return std::unexpected(error);
    }

    CloseHandle(token);
    return elevation.TokenIsElevated != 0;
}

std::expected<CliArg, std::wstring> parse_arguments(int argc, wchar_t* argv[]) {
    CliArg args;

    if (argc < 2) {
        return args; // No command specified
    }

    args.command = argv[1];

    for (int i = 2; i < argc; ++i) {
        std::wstring arg = argv[i];

        if (arg == L"--strict") {
            args.strict = true;
        } else if (arg == L"--dry-run") {
            args.dry_run = true;
        } else if (arg == L"--json") {
            args.json = true;
        } else if (arg == L"-q") {
            args.quiet = true;
        } else if (arg == L"-r") {
            args.recursive = true;
        } else if (arg == L"-i") {
            if (i + 1 < argc) {
                args.input_file = argv[++i];
            } else {
                return std::unexpected(L"Error: -i option requires a file path argument.");
            }
        } else if (arg == L"-o") {
            if (i + 1 < argc) {
                args.output_file = argv[++i];
            } else {
                return std::unexpected(L"Error: -o option requires a file path argument.");
            }
        } else if (arg == L"--scan-dest") {
            args.scan_dest = true;
        } else if (arg == L"-e") {
            args.show_extents = true;
        } else if (arg.starts_with(L"-")) {
            return std::unexpected(L"Error: Unknown option '" + arg + L"'.");
        } else {
            args.positional.push_back(arg);
        }
    }

    return args;
}

std::wstring to_wstring(const std::string& str) {
    if (str.empty()) return L"";
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstr_to(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstr_to[0], size_needed);
    return wstr_to;
}

std::string to_string(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string str_to(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &str_to[0], size_needed, NULL, NULL);
    return str_to;
}

std::expected<bool, DWORD> is_refs_supported() {
    // 1. Check Registry Service Key
    HKEY hkey = nullptr;
    LSTATUS status = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Services\\ReFS",
        0,
        KEY_READ,
        &hkey
    );
    
    if (status == ERROR_FILE_NOT_FOUND) {
        return false;
    } else if (status != ERROR_SUCCESS) {
        return std::unexpected(static_cast<DWORD>(status));
    }
    RegCloseKey(hkey);

    // 2. Check File path to driver binary
    wchar_t system_path[MAX_PATH];
    if (!GetSystemDirectoryW(system_path, MAX_PATH)) {
        return std::unexpected(GetLastError());
    }
    std::wstring driver_path = std::wstring(system_path) + L"\\drivers\\refs.sys";
    
    DWORD attrib = GetFileAttributesW(driver_path.c_str());
    bool file_exists = (attrib != INVALID_FILE_ATTRIBUTES && !(attrib & FILE_ATTRIBUTE_DIRECTORY));
    
    return file_exists;
}

std::wstring get_win32_error_message(DWORD error_code) {
    wchar_t buffer[1024];
    DWORD length = FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        error_code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        buffer,
        1024,
        NULL
    );

    if (length > 0) {
        // Strip trailing newlines
        std::wstring msg(buffer, length);
        while (!msg.empty() && (msg.back() == L'\r' || msg.back() == L'\n')) {
            msg.pop_back();
        }
        return msg;
    }

    return L"Unknown Win32 Error (" + std::to_wstring(error_code) + L")";
}

std::wstring format_size(ULONGLONG bytes) {
    constexpr ULONGLONG kKB = 1024ULL;
    constexpr ULONGLONG kMB = 1024ULL * 1024ULL;
    constexpr ULONGLONG kGB = 1024ULL * 1024ULL * 1024ULL;
    constexpr ULONGLONG kTB = 1024ULL * 1024ULL * 1024ULL * 1024ULL;

    std::wostringstream ss;
    ss << std::fixed << std::setprecision(2);

    if (bytes >= kTB) {
        ss << static_cast<double>(bytes) / static_cast<double>(kTB) << L" TB";
    } else if (bytes >= kGB) {
        ss << static_cast<double>(bytes) / static_cast<double>(kGB) << L" GB";
    } else if (bytes >= kMB) {
        ss << static_cast<double>(bytes) / static_cast<double>(kMB) << L" MB";
    } else if (bytes >= kKB) {
        ss << static_cast<double>(bytes) / static_cast<double>(kKB) << L" KB";
    } else {
        ss << bytes << L" bytes";
    }

    return ss.str();
}

} // namespace util
