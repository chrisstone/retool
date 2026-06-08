#include <expected>
#include <fcntl.h>
#include <io.h>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include <windows.h>

#include "copy.h"
#include "dedup.h"
#include "inspect.h"
#include "output.h"
#include "util.h"
#include "version.h"
#include "volume.h"

constexpr std::wstring_view APP_VERSION = RETOOL_VERSION;

/**
 * @brief Prints the general usage instructions and available subcommands to standard output.
 */
void print_general_help() {
    std::wcout << L"retool - ReFS block-level inspection and copy utility\n\n"
               << L"Usage:\n"
               << L"  retool <command> [options] [arguments]\n\n"
               << L"Commands:\n"
               << L"  inspect (i)   Inspect block layout and deduplication stats\n"
               << L"  copy (cp)     Copy files preserving deduplication\n"
               << L"  dedup (dd)    Deduplicate files in-place on a ReFS volume\n"
               << L"  volume (vol)  Show volume-level information and stats\n"
               << L"  help (h)      Show this help information\n"
               << L"  version       Show version information\n\n"
               << L"Global Options:\n"
               << L"  -j            Output in JSON format\n"
               << L"  -q            Suppress all output (quiet mode)\n"
               << L"  -o <file>     Redirect output to a file\n\n"
               << L"Use 'retool help <command>' for command-specific options.\n"
               << L"Use 'retool help all' to print help for every command." << std::endl;
}

/// @brief Prints a separator line between help blocks in 'help all' mode.
static void print_help_separator() {
    std::wcout << L"\n------------------------------------------------------------\n\n";
}

/**
 * @brief Prints command-specific usage instructions for the given command name.
 *
 * @param cmd The command name or alias (e.g. "inspect", "copy", "volume", "all").
 */
void print_command_help(const std::wstring& cmd) {

    // ── Global options footer appended to every command help ─────────────────
    auto print_global = []() {
        std::wcout << L"\nGlobal Options:\n"
                   << L"  -j            Output in JSON format\n"
                   << L"  -q            Suppress all output\n"
                   << L"  -o <file>     Redirect output to a file\n";
    };

    if (cmd == L"inspect" || cmd == L"i") {
        std::wcout << L"Usage: retool inspect <file|dir|glob> [file2 ...] [options]\n"
                   << L"       retool inspect <volume-root> [options]\n\n"
                   << L"Inputs:\n"
                   << L"  <file>          Single file  - 5-field summary; add -e for extent table\n"
                   << L"  <file1> <file2> Multiple files - per-file cluster sharing report\n"
                   << L"  <dir>           Directory - recursively enumerates all files\n"
                   << L"  <glob>          Glob pattern e.g. E:\\Data\\*.vbk (non-recursive)\n"
                   << L"  <volume-root>   Volume scan e.g. E:\\ - full LCN index\n\n"
                   << L"Options:\n"
                   << L"  -e            Extended mode: extent table (single-file) or sharing\n"
                   << L"                matrix (multi-file); per-file cluster table always shown\n"
                   << L"  -r            Fragmentation report (fragment count, min/max/avg extent)\n"
                   << L"  -s            Strict: abort on first error (default: best-effort)\n"
                   << L"  -i <file>     Read file paths from a newline-delimited file\n";
        print_global();
    } else if (cmd == L"copy" || cmd == L"cp") {
        std::wcout << L"Usage: retool copy <src> <dest> [options]\n\n"
                   << L"Options:\n"
                   << L"  -r            Recursive directory copy\n"
                   << L"  -n            Dry run: simulate without writing\n"
                   << L"  -d            Pre-scan destination volume to seed dedup index\n"
                   << L"  -s            Strict: abort on first error (default: best-effort)\n";
        print_global();
    } else if (cmd == L"dedup" || cmd == L"dd") {
        std::wcout << L"Usage: retool dedup <volume-root> [options]\n"
                   << L"       retool dedup <file1> <file2> [options]\n\n"
                   << L"Modes:\n"
                   << L"  <volume-root>       Volume-wide deduplication scan (e.g. E:\\\\)\n"
                   << L"  <file1> <file2>     Pair-wise deduplication of two files\n\n"
                   << L"Options:\n"
                   << L"  -n            Dry run: simulate without writing\n"
                   << L"  -s            Strict: abort on first error\n";
        print_global();
    } else if (cmd == L"volume" || cmd == L"vol") {
        std::wcout << L"Usage: retool volume <drive-letter or path> [options]\n\n"
                   << L"Options:\n";
        print_global();
    } else if (cmd == L"all") {
        // Print every command's help separated by dividers
        const std::wstring cmds[] = {L"inspect", L"copy", L"dedup", L"volume"};
        for (const auto& c : cmds) {
            print_command_help(c);
            print_help_separator();
        }
        return;
    } else {
        print_general_help();
    }
}

static output::IOutput* g_active_output = nullptr;
static bool g_is_copy_or_dedup = false;

/**
 * @brief Windows console control handler callback to process Ctrl+C and Ctrl+Break.
 * Sets the copy cancellation flags to signal active copying operations to stop and clean up,
 * and gracefully tears down the output interface.
 */
BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
        copy::g_cancel_requested = true;
        copy::g_cancel_requested_bool = TRUE;
        if (g_active_output) {
            g_active_output->graceful_teardown();
        }
        if (ctrlType == CTRL_CLOSE_EVENT) {
            ExitProcess(0xC000013A);
        }
        if (!g_is_copy_or_dedup) {
            ExitProcess(0xC000013A);
        }
        return TRUE; // Consume event to let copy/dedup terminate gracefully on the main thread
    }
    return FALSE;
}

/**
 * @brief Application entry point.
 *
 * Parses command line arguments, handles help and version flags, performs privilege
 * checks, checks OS-level ReFS support, and dispatches the specified subcommand.
 *
 * @param argc The number of command line arguments.
 * @param argv The array of wide command line arguments.
 * @return int 0 on success, 1 on syntax or privilege error, 2 on operational error.
 */
int wmain(int argc, wchar_t* argv[]) {
    // Configure stdout/stderr encoding based on the attached handle type.
    //
    // When stdout is a real console (interactive terminal) _O_U16TEXT lets
    // std::wcout emit UTF-16 via WriteConsoleW, which is the only reliable way
    // to display Unicode in cmd/conhost.
    //
    // When stdout is a pipe or file (redirected, PowerShell capture, etc.) we
    // switch to _O_U8TEXT so that the byte stream is valid UTF-8 - the de-facto
    // encoding expected by modern tools and PowerShell's default decoding.
    auto pick_mode = [](FILE* f) -> int {
        HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(f)));
        if (h != INVALID_HANDLE_VALUE &&
            GetFileType(h) == FILE_TYPE_CHAR) {
            // Attached to a character device (console)
            return _O_U16TEXT;
        }
        return _O_U8TEXT;
    };
    _setmode(_fileno(stdout), pick_mode(stdout));
    _setmode(_fileno(stderr), pick_mode(stderr));

    // Parse arguments
    auto args_res = util::parse_arguments(argc, argv);
    if (!args_res) {
        std::wcerr << args_res.error() << std::endl;
        print_general_help();
        return 1;
    }

    const util::CliArg& args = *args_res;

    // Handle empty command, help, or version
    if (args.command.empty() || args.command == L"help" || args.command == L"h" || args.command == L"-h" || args.command == L"--help") {
        if (args.command == L"help" && !args.positional.empty()) {
            print_command_help(args.positional[0]);
        } else {
            print_general_help();
        }
        return 0;
    }

    if (args.command == L"version" || args.command == L"--version") {
        std::wcout << L"retool version " << APP_VERSION << std::endl;
        return 0;
    }

    // Verify Administrator privileges for functional commands
    auto elevation_res = util::is_elevated();
    if (!elevation_res) {
        std::wcerr << L"ERROR: Failed to check privilege status. Details: "
                   << util::get_win32_error_message(elevation_res.error()) << std::endl;
        return 1;
    }

    if (!*elevation_res) {
        std::wcerr << L"ERROR: retool requires Administrator privileges.\n"
                   << L"       Please re-run from an elevated command prompt." << std::endl;
        return 1;
    }

    // Check if ReFS is supported by the OS (do not block execution, only warn)
    auto refs_supported = util::is_refs_supported();
    if (!refs_supported) {
        std::wcerr << L"WARNING: Failed to verify ReFS OS support status. Details: "
                   << util::get_win32_error_message(refs_supported.error()) << L"\n" << std::endl;
    } else if (!*refs_supported) {
        std::wcerr << L"WARNING: ReFS driver/service does not appear to be present or registered on this operating system.\n"
                   << L"         Low-level ReFS commands and ioctls will likely fail.\n" << std::endl;
    }

    // Construct the appropriate output interface
    std::unique_ptr<output::IOutput> out;
    if (args.quiet) {
        out = std::make_unique<output::QuietOutput>();
    } else if (args.json) {
        out = std::make_unique<output::JsonOutput>(args.command, args.output_file);

    } else {
        out = std::make_unique<output::CliOutput>(args.output_file);
    }

    g_active_output = out.get();
    g_is_copy_or_dedup = (args.command == L"copy"  || args.command == L"cp" ||
                          args.command == L"dedup" || args.command == L"dd");

    // Register console control handler for all commands (handles teardown on Ctrl+C)
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    // Dispatch subcommands
    std::expected<int, std::wstring> run_res;
    if (args.command == L"inspect" || args.command == L"i") {
        run_res = inspect::execute_inspect(args, *out);
    } else if (args.command == L"copy" || args.command == L"cp") {
        run_res = copy::execute_copy(args, *out);
    } else if (args.command == L"dedup" || args.command == L"dd") {
        run_res = dedup::execute_dedup(args, *out);
    } else if (args.command == L"volume" || args.command == L"vol") {
        run_res = volume::execute_volume(args, *out);
    } else {
        std::wcerr << L"ERROR: Unknown command '" << args.command << L"'.\n" << std::endl;
        print_general_help();
        SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
        g_active_output = nullptr;
        return 1;
    }

    SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
    g_active_output = nullptr;

    if (!run_res) {
        std::wcerr << L"ERROR: " << run_res.error() << std::endl;
        return 2;
    }

    return *run_res;
}
