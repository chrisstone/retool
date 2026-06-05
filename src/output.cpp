/**
 * @file output.cpp
 * @brief Implementations of CliOutput and JsonOutput for formatted program output.
 */

#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include <windows.h>

#include "output.h"
#include "util.h"

namespace output {

// ============================================================================
// CliOutput Implementation
// ============================================================================

CliOutput::CliOutput(const std::wstring& output_file) {
    if (!output_file.empty()) {
        file_out_.open(output_file, std::ios::out | std::ios::binary);
        if (file_out_.is_open()) {
            use_file_ = true;
        }
    }
}

CliOutput::~CliOutput() {
    if (progress_active_) {
        // Clean up any lingering progress line
        HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD written = 0;
        WriteConsoleA(console, "\n", 1, &written, NULL);
    }
}

std::wostream& CliOutput::out() {
    if (use_file_) return file_out_;
    return std::wcout;
}

void CliOutput::clear_progress_line() {
    if (progress_active_) {
        // Clear the progress line on console (progress always goes to console)
        HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD written = 0;
        // Overwrite with spaces and return to start
        char blank[82];
        std::memset(blank, ' ', 80);
        blank[0] = '\r';
        blank[81] = '\r';
        WriteConsoleA(console, blank, 82, &written, NULL);
        progress_active_ = false;
        last_line_len_ = 0;
        last_progress_file_.clear();
    }
}

void CliOutput::status(const std::wstring& message) {
    clear_progress_line();
    out() << message << std::endl;
}

void CliOutput::warn(const std::wstring& message) {
    clear_progress_line();
    out() << L"WARNING: " << message << std::endl;
}

void CliOutput::error(const std::wstring& message) {
    clear_progress_line();
    out() << L"ERROR: " << message << std::endl;
}

void CliOutput::field(const std::wstring& name, const std::wstring& value) {
    out() << std::left << std::setw(15) << name << L" " << value << L"\n";
}

void CliOutput::begin_section(const std::wstring& name) {
    out() << L"\n--- " << name << L" ---\n";
}

void CliOutput::end_section() {
    out() << std::endl;
}

void CliOutput::begin_table(const std::vector<std::wstring>& columns) {
    table_columns_ = columns;
    for (const auto& col : columns) {
        out() << std::left << std::setw(15) << col;
    }
    out() << L"\n";
}

void CliOutput::table_row(const std::vector<std::wstring>& values) {
    for (size_t i = 0; i < values.size(); ++i) {
        if (i == values.size() - 1) {
            out() << values[i];
        } else {
            out() << std::left << std::setw(15) << values[i];
        }
    }
    out() << L"\n";
}

void CliOutput::end_table() {
    table_columns_.clear();
}

/**
 * @brief Renders a 20-character progress bar using ASCII block characters.
 *
 * Each of the 20 slots covers 5 percentage points. Within each slot,
 * the sub-percentage maps to one of 5 visual states:
 *   - ' '  (0x20)  = 0%
 *   - '░'  (0xB0)  = 1-24%
 *   - '▒'  (0xB1)  = 25-49%
 *   - '▓'  (0xB2)  = 50-74%
 *   - '█'  (0xDB)  = 75-100%
 *
 * @param percent Completion percentage (0.0 to 100.0).
 * @return std::string The 20-character bar (narrow chars for console output).
 */
std::string CliOutput::render_progress_bar(double percent) const {
    if (percent < 0.0) percent = 0.0;
    if (percent > 100.0) percent = 100.0;

    std::string bar(20, ' ');

    for (int slot = 0; slot < 20; ++slot) {
        double slot_start = slot * 5.0;
        double slot_end = slot_start + 5.0;

        if (percent >= slot_end) {
            bar[slot] = static_cast<char>(0xDB); // █
        } else if (percent <= slot_start) {
            bar[slot] = ' ';
        } else {
            double sub = (percent - slot_start) / 5.0;
            if (sub < 0.01) {
                bar[slot] = ' ';
            } else if (sub < 0.25) {
                bar[slot] = static_cast<char>(0xB0); // ░
            } else if (sub < 0.50) {
                bar[slot] = static_cast<char>(0xB1); // ▒
            } else if (sub < 0.75) {
                bar[slot] = static_cast<char>(0xB2); // ▓
            } else {
                bar[slot] = static_cast<char>(0xDB); // █
            }
        }
    }

    return bar;
}

void CliOutput::progress(const std::wstring& filename, ULONGLONG current, ULONGLONG total) {
    if (total == 0) return;

    // Ignore duplicate progress calls for an already completed file
    if (filename == last_progress_file_ && !progress_active_) {
        return;
    }

    auto now = std::chrono::steady_clock::now();

    // Initialize timing on first call or new file
    if (!progress_active_ || filename != last_progress_file_) {
        progress_start_time_ = now;
        last_progress_time_ = {};
        last_progress_file_ = filename;
        progress_active_ = true;
    }

    bool is_complete = (current >= total);

    // Throttle updates to 1 per second (unless complete)
    if (!is_complete) {
        auto elapsed_since_last = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_progress_time_);
        if (elapsed_since_last.count() < 1000 && last_progress_time_.time_since_epoch().count() > 0) {
            return;
        }
    }

    last_progress_time_ = now;

    double percent = (static_cast<double>(current) / total) * 100.0;
    std::string bar = render_progress_bar(percent);

    // Calculate throughput (MBps)
    double elapsed_sec = std::chrono::duration<double>(now - progress_start_time_).count();
    ULONGLONG mbps = 0;
    if (elapsed_sec > 0.01) {
        mbps = static_cast<ULONGLONG>(current / (elapsed_sec * 1024.0 * 1024.0));
    }

    // Extract just the filename (no directory path)
    std::wstring display_name = filename;
    size_t last_slash = filename.find_last_of(L"\\/");
    if (last_slash != std::wstring::npos) {
        display_name = filename.substr(last_slash + 1);
    }

    // Truncate long filenames for display
    const size_t kMaxNameLen = 30;
    if (display_name.size() > kMaxNameLen) {
        display_name = display_name.substr(0, kMaxNameLen - 3) + L"...";
    }

    // Render: [XXXXXXXXXXXXXXXXXXXX] filename Y MBps
    std::string narrow_name = util::to_string(display_name);

    char line[256];
    int len = std::snprintf(line, sizeof(line), "\r[%s] %-30s %llu MBps",
        bar.c_str(), narrow_name.c_str(), mbps);

    if (len > 0) {
        size_t current_len = static_cast<size_t>(len);
        std::string out_line(line);
        if (current_len < last_line_len_) {
            out_line.append(last_line_len_ - current_len, ' ');
        }
        last_line_len_ = current_len;

        // Write directly via console handle for code page control
        // Progress always goes to console, never to file
        HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD written = 0;
        WriteConsoleA(console, out_line.c_str(), static_cast<DWORD>(out_line.size()), &written, NULL);

        if (is_complete) {
            WriteConsoleA(console, "\n", 1, &written, NULL);
            progress_active_ = false;
            last_line_len_ = 0;
        }
    }
}

void CliOutput::flush() {
    if (progress_active_) {
        HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD written = 0;
        WriteConsoleA(console, "\n", 1, &written, NULL);
        progress_active_ = false;
        last_line_len_ = 0;
    }
    last_progress_file_.clear();
    out().flush();
}

// ============================================================================
// JsonOutput Implementation
// ============================================================================

/// @brief Helper to convert wide string to narrow UTF-8 for JSON keys/values.
static std::string to_narrow(const std::wstring& ws) {
    return util::to_string(ws);
}

JsonOutput::JsonOutput(const std::wstring& output_file)
    : output_file_(output_file) {
}

nlohmann::ordered_json& JsonOutput::current() {
    if (!section_stack_.empty()) {
        return *section_stack_.back();
    }
    return root_;
}

void JsonOutput::status(const std::wstring& message) {
    current()["messages"].push_back(to_narrow(message));
}

void JsonOutput::warn(const std::wstring& message) {
    current()["warnings"].push_back(to_narrow(message));
}

void JsonOutput::error(const std::wstring& message) {
    current()["errors"].push_back(to_narrow(message));
}

void JsonOutput::field(const std::wstring& name, const std::wstring& value) {
    std::string key = to_narrow(name);
    std::string val = to_narrow(value);

    // Attempt to parse value as number for cleaner JSON
    try {
        size_t pos = 0;
        long long int_val = std::stoll(val, &pos);
        if (pos == val.size()) {
            current()[key] = int_val;
            return;
        }
    } catch (...) {}

    try {
        size_t pos = 0;
        double dbl_val = std::stod(val, &pos);
        if (pos == val.size()) {
            current()[key] = dbl_val;
            return;
        }
    } catch (...) {}

    current()[key] = val;
}

void JsonOutput::begin_section(const std::wstring& name) {
    std::string key = to_narrow(name);
    current()[key] = nlohmann::ordered_json::object();
    section_stack_.push_back(&current()[key]);
}

void JsonOutput::end_section() {
    if (!section_stack_.empty()) {
        section_stack_.pop_back();
    }
}

void JsonOutput::begin_table(const std::vector<std::wstring>& columns) {
    table_columns_.clear();
    for (const auto& col : columns) {
        table_columns_.push_back(col);
    }

    current_table_key_ = "rows";
    current()[current_table_key_] = nlohmann::ordered_json::array();
}

void JsonOutput::table_row(const std::vector<std::wstring>& values) {
    nlohmann::ordered_json row = nlohmann::ordered_json::object();
    for (size_t i = 0; i < values.size() && i < table_columns_.size(); ++i) {
        std::string key = to_narrow(table_columns_[i]);
        std::string val = to_narrow(values[i]);

        // Attempt numeric parsing
        try {
            size_t pos = 0;
            long long int_val = std::stoll(val, &pos);
            if (pos == val.size()) {
                row[key] = int_val;
                continue;
            }
        } catch (...) {}

        try {
            size_t pos = 0;
            double dbl_val = std::stod(val, &pos);
            if (pos == val.size()) {
                row[key] = dbl_val;
                continue;
            }
        } catch (...) {}

        row[key] = val;
    }
    current()[current_table_key_].push_back(row);
}

void JsonOutput::end_table() {
    table_columns_.clear();
    current_table_key_.clear();
}

void JsonOutput::progress(const std::wstring&, ULONGLONG, ULONGLONG) {
    // JSON does not output progress information
}

void JsonOutput::flush() {
    std::string json_str = root_.dump(2);

    if (!output_file_.empty()) {
        // Write to file
        std::ofstream file(output_file_, std::ios::out | std::ios::binary);
        if (file.is_open()) {
            file.write(json_str.c_str(), json_str.size());
            file.put('\n');
        }
    } else {
        // Write to stdout
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD written = 0;
        WriteFile(out, json_str.c_str(), static_cast<DWORD>(json_str.size()), &written, NULL);
        WriteFile(out, "\n", 1, &written, NULL);
    }
}

} // namespace output
