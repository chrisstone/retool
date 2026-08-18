#include "util.h"

#include <cwctype>
#include <expected>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>

namespace util {

bool is_volume_specifier(const std::wstring& path) {
    // Drive letter volume: "X:" only — no trailing separator.
    // "X:\" is the root directory of the volume, which is a kPath.
    if (path.size() == 2 && std::iswalpha(path[0]) && path[1] == L':') {
        return true;
    }
    // Win32 device paths: \\.\xxx  (covers \\.\C:, \\.\PhysicalDriveN, \\.\Volume{GUID})
    if (path.size() >= 4 && path[0] == L'\\' && path[1] == L'\\' &&
                             path[2] == L'.'  && path[3] == L'\\') {
        return true;
    }
    // Volume GUID path: \\?\Volume{...}
    if (path.size() >= 11 &&
        path[0] == L'\\' && path[1] == L'\\' &&
        path[2] == L'?'  && path[3] == L'\\' &&
        path.substr(4, 7) == L"Volume{") {
        return true;
    }
    return false;
}

/**
 * @brief Ensures a volume specifier has a trailing backslash for Win32 APIs.
 *
 * "X:" becomes "X:\\"; device/GUID paths without a trailing separator get one
 * appended. Paths that already end with '\\' or '/' are returned unchanged.
 */
std::wstring normalize_volume_root(const std::wstring& spec) {
    if (spec.empty()) return spec;
    wchar_t last = spec.back();
    if (last == L'\\' || last == L'/') return spec;
    return spec + L'\\';
}

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

std::expected<CliArg, std::wstring> parse_arguments(int argc, wchar_t *argv[]) {
	CliArg args;

	if (argc < 2) {
		return args; // No command specified
	}

	args.command = argv[1];

    // Pending -i paths are buffered and appended after all positional args so
    // that ordering (positional first, then -i contents) is always consistent.
    std::vector<std::wstring> deferred_i_paths;

	for (int i = 2; i < argc; ++i) {
		std::wstring arg = argv[i];

		if (arg == L"-s") {
			args.strict = true;
		} else if (arg == L"-n") {
			args.dry_run = true;
		} else if (arg == L"-j") {
			if (args.quiet)
				return std::unexpected(L"Error: -q option cannot be used with -j option.");
			args.json = true;
		} else if (arg == L"-q") {
			if (args.json)
				return std::unexpected(L"Error: -q option cannot be used with -j option.");
			args.quiet = true;
		} else if (arg == L"-r") {
			args.recursive = true;
		} else if (arg == L"-e") {
			args.show_extents = true;
		} else if (arg == L"-d") {
			args.scan_dest = true;
		} else if (arg == L"-xs") {
			args.skip_existing = true;
		} else if (arg == L"-t") {
			if (i + 1 < argc) {
				wchar_t *end = nullptr;
				long val = wcstol(argv[++i], &end, 10);
				if (end == argv[i] || *end != L'\0' || val < 0) {
					return std::unexpected(L"Error: -t requires a non-negative integer (retry count).");
				}
				args.retry_count = static_cast<int32_t>(val);
			} else {
				return std::unexpected(L"Error: -t option requires a retry count argument.");
			}
		} else if (arg == L"-w") {
			if (i + 1 < argc) {
				wchar_t *end = nullptr;
				long val = wcstol(argv[++i], &end, 10);
				if (end == argv[i] || *end != L'\0' || val < 0) {
					return std::unexpected(L"Error: -w requires a non-negative integer (wait seconds).");
				}
				args.retry_wait = static_cast<int32_t>(val);
			} else {
				return std::unexpected(L"Error: -w option requires a wait-time argument.");
			}
		} else if (arg.starts_with(L"-ca:")) {
			// Must be checked before -c: to avoid prefix collision.
			std::wstring val = arg.substr(4);
			std::wstring upper;
			for (wchar_t c : val) {
				wchar_t u = static_cast<wchar_t>(towupper(c));
				if (u != L'R' && u != L'A' && u != L'S' && u != L'H') {
					return std::unexpected(L"Error: -ca: accepts only R, A, S, H (got '" + std::wstring(1, c) + L"').");
				}
				upper += u;
			}
			if (upper.empty()) {
				return std::unexpected(L"Error: -ca: requires at least one attribute letter (R, A, S, H).");
			}
			args.copy_attr_mask = upper;
		} else if (arg.starts_with(L"-c:")) {
			std::wstring val = arg.substr(3);
			std::wstring upper;
			for (wchar_t c : val) {
				wchar_t u = static_cast<wchar_t>(towupper(c));
				if (u != L'D' && u != L'A' && u != L'T' && u != L'S' && u != L'O') {
					return std::unexpected(L"Error: -c: accepts only D, A, T, S, O (got '" + std::wstring(1, c) +
										   L"').");
				}
				upper += u;
			}
			if (upper.empty()) {
				return std::unexpected(L"Error: -c: requires at least one component letter (D, A, T, S, O).");
			}
			args.copy_components = upper;
        } else if (arg == L"-i") {
            // Read file list and defer; -i paths are always kPath and are
            // appended after all positional args regardless of -i position.
            if (i + 1 >= argc) {
                return std::unexpected(L"Error: -i option requires a file path argument.");
            }
            std::wstring list_path = argv[++i];
            std::wifstream ifs(list_path);
            if (!ifs) {
                return std::unexpected(L"Error: cannot open -i file list '" + list_path + L"'.");
            }
            std::wstring line;
            while (std::getline(ifs, line)) {
                // Strip CR (getline on Windows text files may leave it)
                if (!line.empty() && line.back() == L'\r') line.pop_back();
                // Skip blank lines and # comments
                if (line.empty() || line[0] == L'#') continue;
                deferred_i_paths.push_back(line);
            }
		} else if (arg == L"-o") {
			if (args.quiet)
				return std::unexpected(L"Error: -o option cannot be used with -q option.");
			if (i + 1 < argc) {
				args.output_file = argv[++i];
			} else {
				return std::unexpected(L"Error: -o option requires a file path argument.");
			}
		} else if (arg.starts_with(L"-")) {
			return std::unexpected(L"Error: Unknown option '" + arg + L"'.");
		} else {
            // Positional argument: classify and validate the volume rule.
            FileSpecKind kind = is_volume_specifier(arg) ? FileSpecKind::kVolume : FileSpecKind::kPath;
            if (kind == FileSpecKind::kVolume && !args.file_specs.empty()) {
                return std::unexpected(
                    L"Error: volume specifier '" + arg +
                    L"' must be the first argument (only the first specifier may be a volume).");
            }
            args.file_specs.push_back({kind, arg});
		}
	}

    // Append deferred -i paths (always kPath; volumes not permitted here).
    for (const auto& p : deferred_i_paths) {
        args.file_specs.push_back({FileSpecKind::kPath, p});
    }

	return args;
}

std::wstring to_wstring(const std::string &str) {
	if (str.empty())
		return L"";
	int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
	std::wstring wstr_to(size_needed, 0);
	MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstr_to[0], size_needed);
	return wstr_to;
}

std::string to_string(const std::wstring &wstr) {
	if (wstr.empty())
		return "";
	int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
	std::string str_to(size_needed, 0);
	WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &str_to[0], size_needed, NULL, NULL);
	return str_to;
}

std::expected<bool, DWORD> is_refs_supported() {
	// 1. Check Registry Service Key
	HKEY hkey = nullptr;
	LSTATUS status =
		RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\ReFS", 0, KEY_READ, &hkey);

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
	DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
								  NULL,
								  error_code,
								  MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
								  buffer,
								  1024,
								  NULL);

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

void enumerate_files_recursive(
    const std::wstring& dir,
    std::vector<std::wstring>& out_files,
    std::vector<std::wstring>& errors
) {
    std::wstring search = dir + L"*";
    WIN32_FIND_DATAW find_data;
    HANDLE find_handle = FindFirstFileW(search.c_str(), &find_data);

    if (find_handle == INVALID_HANDLE_VALUE) {
        errors.push_back(L"FindFirstFileW failed for: " + dir +
                         L" (" + get_win32_error_message(GetLastError()) + L")");
        return;
    }

    do {
        std::wstring name = find_data.cFileName;
        if (name == L"." || name == L"..") continue;

        DWORD attrs = find_data.dwFileAttributes;
        if (attrs & FILE_ATTRIBUTE_SYSTEM) continue;
        if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) continue;

        std::wstring full_path = dir + name;

        if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
            enumerate_files_recursive(full_path + L"\\", out_files, errors);
        } else {
            out_files.push_back(full_path);
        }
    } while (FindNextFileW(find_handle, &find_data));

    FindClose(find_handle);
}

std::expected<VolumeInfo, std::wstring> resolve_volume_info(const std::wstring& path) {
    VolumeInfo info;

    if (is_volume_specifier(path)) {
        info.volume_root = normalize_volume_root(path);
    } else {
        // Strip any wildcard segment so GetVolumePathNameW gets a real path.
        std::wstring base = path;
        size_t wild = base.find_first_of(L"*?");
        if (wild != std::wstring::npos) {
            size_t sep = base.find_last_of(L"\\/", wild);
            base = (sep != std::wstring::npos) ? base.substr(0, sep) : L".";
        }
        wchar_t vol[MAX_PATH];
        if (!GetVolumePathNameW(base.c_str(), vol, MAX_PATH)) {
            return std::unexpected(L"Failed to determine volume root for '" + path +
                                   L"': " + get_win32_error_message(GetLastError()));
        }
        info.volume_root = vol;
    }

    DWORD spc = 0, bps = 0, fc = 0, tc = 0;
    if (!GetDiskFreeSpaceW(info.volume_root.c_str(), &spc, &bps, &fc, &tc)) {
        return std::unexpected(L"Failed to get cluster size for volume '" + info.volume_root +
                               L"': " + get_win32_error_message(GetLastError()));
    }
    info.cluster_size = spc * bps;

    // Filesystem name query is non-fatal: most callers only need cluster_size,
    // and shouldn't fail because of this. See VolumeInfo::fs_name.
    wchar_t fs_name[MAX_PATH] = {0};
    if (GetVolumeInformationW(info.volume_root.c_str(), NULL, 0, NULL, NULL, NULL, fs_name, MAX_PATH)) {
        info.fs_name = fs_name;
    }

    return info;
}

std::expected<ResolvedTarget, std::wstring> resolve_target(
    const FileSpecifier& spec,
    std::vector<std::wstring>& errors
) {
    auto vol_info = resolve_volume_info(spec.path);
    if (!vol_info) return std::unexpected(vol_info.error());

    ResolvedTarget target;
    target.volume_root  = vol_info->volume_root;
    target.cluster_size = vol_info->cluster_size;
    target.files        = expand_file_specifier(spec, errors);
    return target;
}

std::vector<std::wstring> expand_file_specifier(
    const FileSpecifier& spec,
    std::vector<std::wstring>& errors
) {
    std::vector<std::wstring> files;

    if (spec.kind == FileSpecKind::kVolume) {
        std::wstring root = normalize_volume_root(spec.path);
        enumerate_files_recursive(root, files, errors);
        return files;
    }

    const std::wstring& path = spec.path;
    bool has_wildcard = path.find(L'*') != std::wstring::npos ||
                        path.find(L'?') != std::wstring::npos;

    if (!has_wildcard) {
        DWORD attrs = GetFileAttributesW(path.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            std::wstring dir = path;
            if (dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
            enumerate_files_recursive(dir, files, errors);
        } else {
            files.push_back(path);
        }
        return files;
    }

    // Glob: extract directory portion, expand non-recursively via FindFirstFileW
    std::wstring dir_part;
    size_t last_sep = path.find_last_of(L"\\/");
    if (last_sep != std::wstring::npos) {
        dir_part = path.substr(0, last_sep + 1);
    } else {
        wchar_t cwd[MAX_PATH];
        if (GetCurrentDirectoryW(MAX_PATH, cwd)) {
            dir_part = std::wstring(cwd) + L'\\';
        }
    }

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(path.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND) {
            errors.push_back(L"Glob expansion failed for '" + path + L"': " +
                             get_win32_error_message(err));
        }
        return files;
    }
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)    continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM)       continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        files.push_back(dir_part + name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return files;
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

std::wstring format_size_detailed(ULONGLONG bytes) {
    return format_size(bytes) + L" (" + std::to_wstring(bytes) + L" bytes)";
}

std::expected<bool, std::wstring> duplicate_extents(
    HANDLE dest_handle,
    HANDLE src_handle,
    ULONGLONG src_offset,
    ULONGLONG dest_offset,
    ULONGLONG byte_count
) {
    DUPLICATE_EXTENTS_DATA dup_data;
    dup_data.FileHandle = src_handle;
    dup_data.SourceFileOffset.QuadPart = static_cast<LONGLONG>(src_offset);
    dup_data.TargetFileOffset.QuadPart = static_cast<LONGLONG>(dest_offset);
    dup_data.ByteCount.QuadPart        = static_cast<LONGLONG>(byte_count);

    DWORD returned = 0;
    if (!DeviceIoControl(dest_handle, FSCTL_DUPLICATE_EXTENTS_TO_FILE,
                         &dup_data, sizeof(dup_data), NULL, 0, &returned, NULL)) {
        DWORD err = GetLastError();
        return std::unexpected(L"FSCTL_DUPLICATE_EXTENTS_TO_FILE failed (SourceOffset="
            + std::to_wstring(src_offset) + L", TargetOffset=" + std::to_wstring(dest_offset)
            + L", ByteCount=" + std::to_wstring(byte_count) + L"): " +
            get_win32_error_message(err));
    }
    return true;
}

std::expected<bool, std::wstring> set_file_eof(HANDLE handle, ULONGLONG size) {
    FILE_END_OF_FILE_INFO eof_info;
    eof_info.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
    if (!SetFileInformationByHandle(handle, FileEndOfFileInfo, &eof_info, sizeof(eof_info))) {
        return std::unexpected(get_win32_error_message(GetLastError()));
    }
    return true;
}

} // namespace util
