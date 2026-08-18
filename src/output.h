/**
 * @file output.h
 * @brief Polymorphic output abstraction for CLI, JSON, and quiet modes.
 *
 * Provides IOutput as the base interface with three implementations:
 * - QuietOutput: All methods are no-ops (-q mode).
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
 * @brief Message level for program output.
 */
enum class Level {
    info,
    warn,
    error
};

/**
 * @brief Abstract interface for all program program output.
 *
 * Commands receive an IOutput reference and call its methods instead of
 * writing to std::wcout directly. This decouples content generation from
 * presentation format.
 */
struct IOutput {
    virtual ~IOutput() = default;

    /// @brief Emit a message at the specified level.
    virtual void message(Level level, const std::wstring& text) = 0;

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
     * @brief Report per-item progress (file transfer, scan, etc.).
     *
     * CLI renders a progress bar; JSON and quiet are no-ops.
     *
     * @param displayName  Label shown on the bar (filename, operation name, etc.).
     * @param current      Units completed so far.
     * @param total        Total units.
     * @param rateUnit     Unit label for the throughput display (e.g. L"MBps"). Empty = no rate shown.
     * @param showCounts   If true, shows "current/total" alongside the bar.
     */
    virtual void progress(const std::wstring& displayName, ULONGLONG current, ULONGLONG total,
                          const std::wstring& rateUnit = L"MBps", bool showCounts = false) = 0;

    /**
     * @brief Report overall/secondary progress packed beside the primary bar.
     *
     * When both progress() and progress_overall() are active, CliOutput renders
     * them side-by-side on one line. Default no-op for JSON and quiet.
     *
     * @param displayName  Label for the overall bar (e.g. L"files").
     * @param current      Overall units completed.
     * @param total        Overall total units.
     * @param rateUnit     Unit label. Empty = no rate shown (typical for count-based bars).
     * @param showCounts   If true, shows "current/total" alongside the bar.
     */
    virtual void progress_overall(const std::wstring& /*displayName*/, ULONGLONG /*current*/, ULONGLONG /*total*/,
                                   const std::wstring& /*rateUnit*/ = L"", bool /*showCounts*/ = true) {}

    /**
     * @brief Gracefully tear down the output interface.
     *
     * Flushes any buffered data and closes open file streams. Used during normal
     * program exit and signal handling (e.g. Ctrl+C).
     */
    virtual void graceful_teardown() = 0;

    /**
     * @brief Begin a section that must be nested as a sub-object in JSON output.
     *
     * Delegated to begin_section() now that sections support automatic nesting.
     *
     * @param name Section/sub-object name (e.g., L"Fragmentation Report").
     */
    virtual void begin_nested_section(const std::wstring& name) { begin_section(name); }

    /// @brief End a section started by begin_nested_section().
    virtual void end_nested_section() { end_section(); }

private:
    /// @brief Finalize output. JSON serializes here; CLI may print a trailing newline.
    virtual void flush() = 0;
};

// ============================================================================
// Implementations
// ============================================================================

/**
 * @brief Quiet output - all methods are no-ops.
 *
 * Selected via the -q CLI flag.
 */
struct QuietOutput : IOutput {
    void message(Level, const std::wstring&) override {}
    void field(const std::wstring&, const std::wstring&) override {}
    void begin_section(const std::wstring&) override {}
    void end_section() override {}
    void begin_table(const std::vector<std::wstring>&) override {}
    void table_row(const std::vector<std::wstring>&) override {}
    void end_table() override {}
    void progress(const std::wstring&, ULONGLONG, ULONGLONG, const std::wstring&, bool) override {}
    void progress_overall(const std::wstring&, ULONGLONG, ULONGLONG, const std::wstring&, bool) override {}
    void graceful_teardown() override {}

private:
    void flush() override {}
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

    void message(Level level, const std::wstring& text) override;
    void field(const std::wstring& name, const std::wstring& value) override;
    void begin_section(const std::wstring& name) override;
    void end_section() override;
    void begin_table(const std::vector<std::wstring>& columns) override;
    void table_row(const std::vector<std::wstring>& values) override;
    void end_table() override;
    void progress(const std::wstring& displayName, ULONGLONG current, ULONGLONG total,
                  const std::wstring& rateUnit, bool showCounts) override;
    void progress_overall(const std::wstring& displayName, ULONGLONG current, ULONGLONG total,
                          const std::wstring& rateUnit, bool showCounts) override;
    void graceful_teardown() override;

private:
    struct ProgressState {
        std::wstring displayName;
        std::wstring rateUnit;
        bool showCounts = false;
        ULONGLONG current = 0;
        ULONGLONG total = 0;
        bool active = false;
        std::chrono::steady_clock::time_point startTime{};
        std::chrono::steady_clock::time_point lastTime{};
    };

    /// @brief Renders an N-character progress bar string from a percentage.
    std::string render_progress_bar(double percent, int width = 20) const;

    /// @brief Returns the active output stream (file or wcout).
    std::wostream& out();

    /// @brief Clears the in-progress line from the console and deactivates all bars.
    void clear_progress_line();

    /// @brief Updates a bar's state; returns true if a render should follow.
    bool update_bar(ProgressState& state, const std::wstring& displayName,
                    ULONGLONG current, ULONGLONG total,
                    const std::wstring& rateUnit, bool showCounts,
                    std::chrono::steady_clock::time_point now);

    /// @brief Renders the current composite or single progress line to the console.
    void render_progress_line(std::chrono::steady_clock::time_point now);

    void flush() override;

    std::wofstream file_out_;                                              ///< File output stream (if -o specified).
    bool use_file_ = false;                                                ///< True if outputting to file.
    ProgressState primary_;                                                ///< Per-item progress bar state.
    ProgressState secondary_;                                              ///< Overall progress bar state (packed beside primary).
    size_t last_line_len_ = 0;                                             ///< Length of the last rendered progress line.
    std::vector<std::wstring> table_columns_;                              ///< Current table column headers.
    std::vector<std::vector<std::wstring>> table_rows_;                    ///< Buffered rows; flushed by end_table.
    int section_depth_ = 0;                                                ///< Active section nesting depth.
    bool flushed_ = false;                                                 ///< True if flush() has run.
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
    ~JsonOutput() override;

    void message(Level level, const std::wstring& text) override;
    void field(const std::wstring& name, const std::wstring& value) override;
    void begin_section(const std::wstring& name) override;
    void end_section() override;
    void begin_table(const std::vector<std::wstring>& columns) override;
    void table_row(const std::vector<std::wstring>& values) override;
    void end_table() override;
    void progress(const std::wstring& displayName, ULONGLONG current, ULONGLONG total,
                  const std::wstring& rateUnit, bool showCounts) override;
    void graceful_teardown() override;

private:
    void flush() override;

    nlohmann::ordered_json root_;                                ///< Top-level JSON envelope.
    std::vector<std::string> section_keys_;                      ///< Key path for nested sections within data.
    std::vector<std::wstring> table_columns_;                    ///< Current table column headers.
    std::string current_table_key_;                              ///< JSON key for the current table array.
    std::string current_section_name_;                           ///< Display name of the innermost active section.
    std::wstring output_file_;                                   ///< Optional output file path.
    std::string command_;                                        ///< Command name for the envelope.
    int section_depth_ = 0;                                      ///< Nesting depth of begin_section calls.
    bool has_error_    = false;                                  ///< True if error() was called.
    bool flushed_ = false;                                       ///< True if flush() has run.

    /// @brief Returns the currently active JSON object (data or nested sub-object).
    nlohmann::ordered_json& current();
};

} // namespace output
