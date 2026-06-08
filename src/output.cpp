/**
 * @file output.cpp
 * @brief Implementations of CliOutput and JsonOutput for formatted program output.
 */

#include <algorithm>
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
    if (!flushed_) {
        flush();
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
        // Overwrite with exactly as many spaces as the last line, then CR
        size_t clear_len = last_line_len_ > 0 ? last_line_len_ : 80;
        std::string blank(clear_len + 2, ' ');
        blank[0] = '\r';
        blank[clear_len + 1] = '\r';
        WriteConsoleA(console, blank.c_str(), static_cast<DWORD>(blank.size()), &written, NULL);
        progress_active_ = false;
        last_line_len_ = 0;
        last_progress_file_.clear();
    }
}

void CliOutput::message(Level level, const std::wstring& text) {
    clear_progress_line();
    if (level == Level::error) {
        out() << L"ERROR: " << text << std::endl;
    } else if (level == Level::warn) {
        out() << L"WARNING: " << text << std::endl;
    } else {
        out() << text << std::endl;
    }
}

void CliOutput::field(const std::wstring& name, const std::wstring& value) {
    out() << std::left << std::setw(15) << name << L" " << value << L"\n";
}

void CliOutput::begin_section(const std::wstring& name) {
    clear_progress_line();
    out() << L"\n--- " << name << L" ---\n";
    section_depth_++;
}

void CliOutput::end_section() {
    out() << std::endl;
    if (section_depth_ > 0) {
        section_depth_--;
        if (section_depth_ == 0) {
            flush();
        }
    }
}

void CliOutput::begin_table(const std::vector<std::wstring>& columns) {
    table_columns_ = columns;
    table_rows_.clear();
}

void CliOutput::table_row(const std::vector<std::wstring>& values) {
    table_rows_.push_back(values);
}

void CliOutput::end_table() {
    if (table_columns_.empty()) return;

    const size_t ncols = table_columns_.size();

    // ── Compute column widths: max of header and each row's cell ─────────────
    // Cap each non-last column at kMaxColWidth to prevent runaway filenames.
    constexpr size_t kMaxColWidth = 40;
    std::vector<size_t> widths(ncols, 0);
    for (size_t c = 0; c < ncols; ++c) {
        widths[c] = table_columns_[c].size();
    }
    for (const auto& row : table_rows_) {
        for (size_t c = 0; c < row.size() && c < ncols; ++c) {
            widths[c] = (std::max)(widths[c], row[c].size());
        }
    }
    for (size_t c = 0; c + 1 < ncols; ++c) {
        widths[c] = (std::min)(widths[c], kMaxColWidth);
    }

    // ── Print header ─────────────────────────────────────────────────────────
    for (size_t c = 0; c < ncols; ++c) {
        bool last = (c == ncols - 1);
        out() << std::left << std::setw(last ? 0 : static_cast<int>(widths[c] + 2))
              << table_columns_[c];
    }
    out() << L"\n";

    // ── Print divider (one dash-run per column) ───────────────────────────────
    for (size_t c = 0; c < ncols; ++c) {
        bool last = (c == ncols - 1);
        std::wstring div(widths[c], L'-');
        out() << std::left << std::setw(last ? 0 : static_cast<int>(widths[c] + 2)) << div;
    }
    out() << L"\n";

    // ── Print rows (truncate cells exceeding max width with ellipsis) ─────────
    for (const auto& row : table_rows_) {
        for (size_t c = 0; c < ncols; ++c) {
            bool last = (c == ncols - 1);
            const std::wstring& raw = (c < row.size()) ? row[c] : L"";
            std::wstring cell = raw;
            if (!last && cell.size() > kMaxColWidth) {
                cell = cell.substr(0, kMaxColWidth - 3) + L"...";
            }
            out() << std::left << std::setw(last ? 0 : static_cast<int>(widths[c] + 2)) << cell;
        }
        out() << L"\n";
    }

    table_columns_.clear();
    table_rows_.clear();
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
    flushed_ = true;
}

void CliOutput::graceful_teardown() {
    flush();
}

// ============================================================================
// JsonOutput Implementation
// ============================================================================

/// @brief Helper to convert wide string to narrow UTF-8 for JSON keys/values.
static std::string to_narrow(const std::wstring& ws) {
    return util::to_string(ws);
}

// ── Static name mappings ──────────────────────────────────────────────────────

#include <unordered_map>

/**
 * @brief Maps human-readable display field/column names to camelCase JSON keys.
 *
 * All field names emitted by all commands are listed here.  Unknown names fall
 * through to the auto-camelCase converter in to_json_key().
 */
static const std::unordered_map<std::string, std::string> kFieldNameMap = {
    // Common
    {"Volume",           "volume"},
    {"Cluster Size",     "clusterSize"},
    {"File System",      "fileSystem"},
    // Single-file inspect
    {"File",             "filePath"},
    {"File Size",        "fileSizeBytes"},
    {"Fragments",        "fragments"},
    // Multi-file inspect summary
    {"Files Analyzed",   "filesAnalyzed"},
    {"Total Size",       "totalSizeBytes"},
    {"Shared Blocks",    "sharedBlocksBytes"},
    {"Saved Space",      "savedSpaceBytes"},
    {"Dedup Savings",    "dedupSavingsPct"},
    // Per-file breakdown table columns
    {"Total Clusters",   "totalClusters"},
    {"Unique Clusters",  "uniqueClusters"},
    {"Shared Clusters",  "sharedClusters"},
    {"Unique Bytes",     "uniqueBytes"},
    {"Shared Bytes",     "sharedBytes"},
    // Extent table columns
    {"Extent",           "extentIndex"},
    {"VCN",              "vcn"},
    {"LCN",              "lcn"},
    {"Clusters",         "clusters"},
    {"Bytes",            "bytes"},
    {"Cumulative",       "cumulativeBytes"},
    // Volume command
    {"Total Space",      "totalSpaceBytes"},
    {"Free Space",       "freeSpaceBytes"},
    {"Used Space",       "usedSpaceBytes"},
    // Fragmentation report
    {"Frag Score",       "fragScore"},
    {"Smallest Extent",  "smallestExtentClusters"},
    {"Largest Extent",   "largestExtentClusters"},
    {"Avg Extent",       "avgExtentClusters"},
    // Volume scan report
    {"Files Scanned",    "filesScanned"},
    {"Clusters Indexed", "clustersIndexed"},
    {"Content Groups",   "contentGroups"},
    {"Savings %",        "savingsPct"},
    // Dedup
    {"Files Processed",  "filesProcessed"},
    {"Clusters Deduped", "clustersDeduplicated"},
    {"Space Reclaimed",  "spaceReclaimedBytes"},
    // Copy
    {"Total Files",      "totalFiles"},
    {"Cloned Files",     "clonedFiles"},
    {"Fallback Copies",  "fallbackCopies"},
    {"Total Bytes",      "totalBytes"},
};

/**
 * @brief Converts a display field name to its camelCase JSON key.
 *
 * Looks up kFieldNameMap first; falls back to auto-camelCase conversion
 * (space-separated title → camelCase) for names not listed.
 */
static std::string to_json_key(const std::string& name) {
    auto it = kFieldNameMap.find(name);
    if (it != kFieldNameMap.end()) return it->second;
    // Auto-convert: "Some Field" -> "someField"
    std::string key;
    bool next_upper = false;
    for (size_t i = 0; i < name.size(); ++i) {
        char c = name[i];
        if (c == ' ' || c == '_' || c == '-') { next_upper = true; continue; }
        if (key.empty()) {
            key += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (next_upper) {
            key += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            next_upper = false;
        } else {
            key += c;
        }
    }
    return key.empty() ? name : key;
}

/**
 * @brief Determines the JSON array key for a table given its first column
 *        and the enclosing section name.
 *
 * Uses an explicit map for known tables; falls back to auto-camelCase + "s".
 */
static std::string get_table_array_key(const std::string& first_col,
                                        const std::string& section_name) {
    // Sharing matrix and per-file breakdown both have "File" as first column;
    // distinguish by section name.
    if (first_col == "File" &&
        section_name.find("Sharing") != std::string::npos) {
        return "sharingMatrix";
    }
    static const std::unordered_map<std::string, std::string> kArrayKeys = {
        {"File",   "files"},
        {"Extent", "extents"},
    };
    auto it = kArrayKeys.find(first_col);
    if (it != kArrayKeys.end()) return it->second;
    // Fallback: auto-camelCase the first column name + pluralise.
    return to_json_key(first_col) + "s";
}

// ── Value normalisation ───────────────────────────────────────────────────────

/**
 * @brief Normalise a display string value to its most precise JSON type.
 *
 * Applied rules (in priority order):
 *  1. "human_label (N bytes)"    -> integer N
 *  2. "N bytes"                  -> integer N
 *  3. "human_label (N clusters)" -> integer N
 *  4. "N clusters (...)"         -> integer or double N
 *  5. "N%"                       -> double  N   (percentage, strip %)
 *  6. Drive root "X:\"           -> string "X:" (strip trailing separator)
 *  7. Pure integer               -> integer
 *  8. Pure double                -> double
 *  9. Anything else              -> string as-is
 */
static nlohmann::ordered_json normalise_json_value(const std::string& val) {
    // ── Pattern: "human_label (N bytes)" ─────────────────────────────────────
    const std::string bytes_suffix = " bytes)";
    if (val.size() > bytes_suffix.size() &&
        val.compare(val.size() - bytes_suffix.size(), bytes_suffix.size(), bytes_suffix) == 0) {
        size_t open = val.rfind('(', val.size() - bytes_suffix.size());
        if (open != std::string::npos) {
            std::string num = val.substr(open + 1, val.size() - bytes_suffix.size() - open - 1);
            try { size_t pos = 0; long long bval = std::stoll(num, &pos);
                  if (pos == num.size()) return bval; } catch (...) {}
        }
    }

    // ── Pattern: "N bytes" ───────────────────────────────────────────────────
    const std::string plain_bytes = " bytes";
    if (val.size() > plain_bytes.size() &&
        val.compare(val.size() - plain_bytes.size(), plain_bytes.size(), plain_bytes) == 0) {
        std::string num = val.substr(0, val.size() - plain_bytes.size());
        try { size_t pos = 0; long long bval = std::stoll(num, &pos);
              if (pos == num.size()) return bval; } catch (...) {}
    }

    // ── Pattern: "human_label (N clusters)" ──────────────────────────────────
    const std::string clust_suffix = " clusters)";
    if (val.size() > clust_suffix.size() &&
        val.compare(val.size() - clust_suffix.size(), clust_suffix.size(), clust_suffix) == 0) {
        size_t open = val.rfind('(', val.size() - clust_suffix.size());
        if (open != std::string::npos) {
            std::string num = val.substr(open + 1, val.size() - clust_suffix.size() - open - 1);
            try { size_t pos = 0; long long cval = std::stoll(num, &pos);
                  if (pos == num.size()) return cval; } catch (...) {}
        }
    }

    // ── Pattern: "N clusters (...)" - integer or fractional cluster count ─────
    {
        const std::string clust_tok = " clusters";
        size_t sp = val.find(clust_tok);
        if (sp != std::string::npos && sp > 0) {
            std::string num = val.substr(0, sp);
            try { size_t pos = 0; long long cval = std::stoll(num, &pos);
                  if (pos == num.size()) return cval; } catch (...) {}
            try { size_t pos = 0; double cval = std::stod(num, &pos);
                  if (pos == num.size()) return cval; } catch (...) {}
        }
    }

    // ── Pattern: "N%" -> double N (percentage) ───────────────────────────────
    if (!val.empty() && val.back() == '%') {
        std::string num = val.substr(0, val.size() - 1);
        try { size_t pos = 0; double pval = std::stod(num, &pos);
              if (pos == num.size()) return pval; } catch (...) {}
    }

    // ── Drive root: strip trailing \ or / (e.g. "A:\" -> "A:") ──────────────
    if (!val.empty() && (val.back() == '\\' || val.back() == '/')) {
        return val.substr(0, val.size() - 1);
    }

    // ── Pure integer ─────────────────────────────────────────────────────────
    try { size_t pos = 0; long long ival = std::stoll(val, &pos);
          if (pos == val.size()) return ival; } catch (...) {}

    // ── Pure double ──────────────────────────────────────────────────────────
    try { size_t pos = 0; double dval = std::stod(val, &pos);
          if (pos == val.size()) return dval; } catch (...) {}

    return val;
}

// ── JsonOutput methods ────────────────────────────────────────────────────────

JsonOutput::JsonOutput(const std::wstring& command, const std::wstring& output_file)
    : command_(to_narrow(command)), output_file_(output_file) {
    root_["command"]  = command_;
    root_["status"]   = "success";
    root_["warnings"] = nlohmann::ordered_json::array();
    root_["errors"]   = nlohmann::ordered_json::array();
    root_["data"]     = nlohmann::ordered_json::object();
}

JsonOutput::~JsonOutput() {
    if (!flushed_) {
        flush();
    }
}

nlohmann::ordered_json& JsonOutput::current() {
    nlohmann::ordered_json* node = &root_["data"];
    for (const auto& key : section_keys_) {
        node = &(*node)[key];
    }
    return *node;
}

void JsonOutput::message(Level level, const std::wstring& text) {
    if (level == Level::error) {
        has_error_ = true;
        root_["errors"].push_back(to_narrow(text));
    }
}

void JsonOutput::field(const std::wstring& name, const std::wstring& value) {
    current()[to_json_key(to_narrow(name))] = normalise_json_value(to_narrow(value));
}

void JsonOutput::begin_section(const std::wstring& name) {
    current_section_name_ = to_narrow(name);

    if (section_depth_ == 0) {
        // Top-level section: enter the data context without nesting.
        // All field() and begin_table() calls write flat into root_["data"].
        // This eliminates the dynamic root-key problem entirely.
        section_depth_++;
        return;
    }

    // Nested section: create a camelCase sub-object within the current context.
    std::string key = to_json_key(current_section_name_);
    current()[key] = nlohmann::ordered_json::object();
    section_keys_.push_back(key);
    section_depth_++;
}

void JsonOutput::end_section() {
    if (section_depth_ > 0) {
        --section_depth_;
        if (!section_keys_.empty()) {
            section_keys_.pop_back();
        }
        if (section_depth_ == 0) {
            flush();
        }
    }
}

void JsonOutput::begin_table(const std::vector<std::wstring>& columns) {
    table_columns_.clear();
    for (const auto& col : columns) {
        table_columns_.push_back(col);
    }
    std::string first = columns.empty() ? "" : to_narrow(columns[0]);
    current_table_key_ = get_table_array_key(first, current_section_name_);
    current()[current_table_key_] = nlohmann::ordered_json::array();
}

void JsonOutput::table_row(const std::vector<std::wstring>& values) {
    nlohmann::ordered_json row = nlohmann::ordered_json::object();
    for (size_t i = 0; i < values.size() && i < table_columns_.size(); ++i) {
        row[to_json_key(to_narrow(table_columns_[i]))] =
            normalise_json_value(to_narrow(values[i]));
    }
    current()[current_table_key_].push_back(row);
}

void JsonOutput::end_table() {
    table_columns_.clear();
    current_table_key_.clear();
}

void JsonOutput::progress(const std::wstring&, ULONGLONG, ULONGLONG) {
    // JSON does not output progress information.
}

void JsonOutput::flush() {
    root_["status"] = has_error_ ? "error" : "success";
    std::string json_str = root_.dump(2);

    if (!output_file_.empty()) {
        std::ofstream file(output_file_, std::ios::out | std::ios::binary);
        if (file.is_open()) {
            file.write(json_str.c_str(), json_str.size());
            file.put('\n');
        }
    } else {
        std::wcout << util::to_wstring(json_str) << std::endl;
    }
    flushed_ = true;
}

void JsonOutput::graceful_teardown() {
    flush();
}

} // namespace output
