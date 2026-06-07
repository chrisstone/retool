/**
 * @file output.h
 * @brief Polymorphic output abstraction for CLI, JSON, and silent modes.
 *
 * Provides IOutput as the base interface with three implementations:
 * - NoOutput:   All methods are no-ops (-q mode).
 * - CliOutput:  Human-readable formatted output to the console.
 * - JsonOutput: Machine-readable JSON output via nlohmann/json.
 */

#pragma once

#include <chrono>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <windows.h>

namespace output {

/**
 * @brief Abstract interface for all program output.
 *
 * Commands receive an IOutput reference and call its methods instead of
 * writing to std::wcout directly. This decouples content generation from
 * presentation format.
 */
struct IOutput {
    virtual ~IOutput() = default;

    /// @brief Emit an informational status message (e.g., dry-run actions).
    virtual void status(const std::wstring& message) = 0;

    /// @brief Emit a warning message.
    virtual void warn(const std::wstring& message) = 0;

    /// @brief Emit an error message.
    virtual void error(const std::wstring& message) = 0;

    /// @brief Emit a named key-value field (e.g., "Cluster Size" = "4096 bytes").
    virtual void field(const std::wstring& name, const std::wstring& value) = 0;

    /// @brief Begin a named output section (groups fields and tables).
    virtual void begin_section(const std::wstring& name) = 0;

    /// @brief End the current output section.
    virtual void end_section() = 0;

    /// @brief Begin a table with the given column headers.
    virtual void begin_table(const std::vector<std::wstring>& columns) = 0;

    /// @brief Add a row to the current table.
    virtual void table_row(const std::vector<std::wstring>& values) = 0;

    /// @brief End the current table.
    virtual void end_table() = 0;

    /**
     * @brief Report file transfer progress (copy command only).
     *
     * CLI renders a progress bar; JSON ignores this entirely.
     *
     * @param filename  Display name of the file being transferred.
     * @param current   Bytes transferred so far for this file.
     * @param total     Total bytes for this file.
     */
    virtual void progress(const std::wstring& filename, ULONGLONG current, ULONGLONG total) = 0;

    /// @brief Finalize output. JSON serializes here; CLI may print a trailing newline.
    virtual void flush() = 0;

    /**
     * @brief Override the command name recorded in JSON output.
     *
     * Called by command modules to specialise the envelope's "command" field
     * (e.g., "inspect_multi", "inspect_scan"). Default is a no-op.
     */
    virtual void set_command(const std::wstring&) {}
};

// ============================================================================
// Implementations
// ============================================================================

/**
 * @brief Silent output — all methods are no-ops.
 *
 * Selected via the -q CLI flag.
 */
struct NoOutput : IOutput {
    void status(const std::wstring&) override {}
    void warn(const std::wstring&) override {}
    void error(const std::wstring&) override {}
    void field(const std::wstring&, const std::wstring&) override {}
    void begin_section(const std::wstring&) override {}
    void end_section() override {}
    void begin_table(const std::vector<std::wstring>&) override {}
    void table_row(const std::vector<std::wstring>&) override {}
    void end_table() override {}
    void progress(const std::wstring&, ULONGLONG, ULONGLONG) override {}
    void flush() override {}
    void set_command(const std::wstring&) override {}
};

/**
 * @brief Human-readable CLI output with progress bar support.
 *
 * Formats fields as aligned key-value pairs, tables as column-aligned text,
 * and renders an ASCII block-character progress bar throttled to 1 update/sec.
 *
 * @param output_file  Optional file path. If non-empty, data output goes to
 *                     this file instead of stdout. Progress always goes to console.
 */
struct CliOutput : IOutput {
    explicit CliOutput(const std::wstring& output_file = L"");
    ~CliOutput() override;

    void status(const std::wstring& message) override;
    void warn(const std::wstring& message) override;
    void error(const std::wstring& message) override;
    void field(const std::wstring& name, const std::wstring& value) override;
    void begin_section(const std::wstring& name) override;
    void end_section() override;
    void begin_table(const std::vector<std::wstring>& columns) override;
    void table_row(const std::vector<std::wstring>& values) override;
    void end_table() override;
    void progress(const std::wstring& filename, ULONGLONG current, ULONGLONG total) override;
    void flush() override;

    /// @brief No-op: CLI output has no command envelope.
    void set_command(const std::wstring&) override {}


private:
    /// @brief Renders the 20-character progress bar string from a percentage.
    std::string render_progress_bar(double percent) const;

    /// @brief Returns the active output stream (file or wcout).
    std::wostream& out();

    /// @brief Clears the progress line if one is active.
    void clear_progress_line();

    std::wofstream file_out_;                                              ///< File output stream (if -o specified).
    bool use_file_ = false;                                                ///< True if outputting to file.
    std::wstring last_progress_file_;                                      ///< Last file reported for progress.
    std::chrono::steady_clock::time_point last_progress_time_{};           ///< Timestamp of last progress render.
    std::chrono::steady_clock::time_point progress_start_time_{};          ///< Start time for throughput calc.
    bool progress_active_ = false;                                         ///< True if a progress line is on screen.
    size_t last_line_len_ = 0;                                             ///< Length of the last rendered progress line.
    std::vector<std::wstring> table_columns_;                              ///< Current table column headers.
    std::vector<std::vector<std::wstring>> table_rows_;                    ///< Buffered rows; flushed by end_table.
};

/**
 * @brief Machine-readable JSON output via nlohmann/json.
 *
 * Accumulates all output into a JSON object. progress() is a no-op.
 * flush() serializes and prints the complete JSON to stdout or a file.
 *
 * @param output_file  Optional file path. If non-empty, JSON is written to
 *                     this file instead of stdout.
 */
struct JsonOutput : IOutput {
    /**
     * @param command     The command name written into the JSON envelope
     *                    (e.g., L"inspect", L"copy", L"dedup", L"volume").
     * @param output_file Optional file path. If non-empty, JSON is written to
     *                    this file instead of stdout.
     */
    explicit JsonOutput(const std::wstring& command,
                        const std::wstring& output_file = L"");

    void status(const std::wstring& message) override;
    void warn(const std::wstring& message) override;
    void error(const std::wstring& message) override;
    void field(const std::wstring& name, const std::wstring& value) override;
    void begin_section(const std::wstring& name) override;
    void end_section() override;
    void begin_table(const std::vector<std::wstring>& columns) override;
    void table_row(const std::vector<std::wstring>& values) override;
    void end_table() override;
    void progress(const std::wstring& filename, ULONGLONG current, ULONGLONG total) override;
    void flush() override;

    /// @brief Override the command field in the JSON envelope at runtime.
    void set_command(const std::wstring& command) override;

private:
    nlohmann::ordered_json root_;                                ///< Top-level JSON envelope.
    std::vector<std::string> section_keys_;                      ///< Key path for nested sections within data.
    std::vector<std::wstring> table_columns_;                    ///< Current table column headers.
    std::string current_table_key_;                              ///< JSON key for the current table array.
    std::string current_section_name_;                           ///< Display name of the innermost active section.
    std::wstring output_file_;                                   ///< Optional output file path.
    std::string command_;                                        ///< Command name for the envelope.
    int section_depth_ = 0;                                      ///< Nesting depth of begin_section calls.
    bool has_error_    = false;                                  ///< True if error() was called.

    /// @brief Returns the currently active JSON object (data or nested sub-object).
    nlohmann::ordered_json& current();
};

} // namespace output
