#include <expected>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include "copy.h"
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
               << L"  volume (vol)  Show volume-level information and stats\n"
               << L"  help (h)      Show this help information\n"
               << L"  version       Show version information\n\n"
               << L"Global Options:\n"
               << L"  --json        Output in JSON format\n"
               << L"  -q            Suppress all output (quiet mode)\n"
               << L"  -o <file>     Redirect output to a file\n\n"
               << L"Use 'retool help <command>' for command-specific options." << std::endl;
}

/**
 * @brief Prints command-specific usage instructions for the given command name.
 * 
 * @param cmd The command name or alias (e.g. "inspect", "copy", "volume").
 */
void print_command_help(const std::wstring& cmd) {
    if (cmd == L"inspect" || cmd == L"i") {
        std::wcout << L"Usage: retool inspect <file1> [file2 ...] [options]\n\n"
                   << L"Options:\n"
                   << L"  -i <file>     Read file paths from a newline-delimited file\n"
                   << L"  -o <file>     Redirect output to a file\n"
                   << L"  --json        Output in JSON format\n"
                   << L"  -q            Suppress all output\n"
                   << L"  --strict      Abort on first error (default: best-effort)" << std::endl;
    } else if (cmd == L"copy" || cmd == L"cp") {
        std::wcout << L"Usage: retool copy <src> <dest> [options]\n\n"
                   << L"Options:\n"
                   << L"  -r            Recursive directory copy\n"
                   << L"  -o <file>     Redirect output to a file\n"
                   << L"  --json        Output in JSON format\n"
                   << L"  -q            Suppress all output\n"
                   << L"  --strict      Abort on first error (default: best-effort)\n"
                   << L"  --dry-run     Simulate the copy operation without writing data" << std::endl;
    } else if (cmd == L"volume" || cmd == L"vol") {
        std::wcout << L"Usage: retool volume <drive-letter or path> [options]\n\n"
                   << L"Options:\n"
                   << L"  -o <file>     Redirect output to a file\n"
                   << L"  --json        Output in JSON format\n"
                   << L"  -q            Suppress all output" << std::endl;
    } else {
        print_general_help();
    }
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
        out = std::make_unique<output::NoOutput>();
    } else if (args.json) {
        out = std::make_unique<output::JsonOutput>(args.output_file);
    } else {
        out = std::make_unique<output::CliOutput>(args.output_file);
    }

    // Dispatch subcommands
    std::expected<int, std::wstring> run_res;
    if (args.command == L"inspect" || args.command == L"i") {
        run_res = inspect::execute_inspect(args, *out);
    } else if (args.command == L"copy" || args.command == L"cp") {
        run_res = copy::execute_copy(args, *out);
    } else if (args.command == L"volume" || args.command == L"vol") {
        run_res = volume::execute_volume(args, *out);
    } else {
        std::wcerr << L"ERROR: Unknown command '" << args.command << L"'.\n" << std::endl;
        print_general_help();
        return 1;
    }

    // Finalize output (JSON serializes here)
    out->flush();

    if (!run_res) {
        std::wcerr << L"ERROR: " << run_res.error() << std::endl;
        return 2;
    }

    return *run_res;
}
