/**
 * @file test_lcn_interval.cpp
 * @brief Synthetic-data tests for the LCN interval index (inspect::build_lcn_interval_index
 *        and friends). Exercises pure sweep-line/lookup logic against hand-built claim
 *        lists - no real files, volumes, or ReFS hardware required.
 *
 * Not wired into a test framework; this is a small standalone executable that asserts
 * and prints PASS/FAIL per case, returning nonzero on any failure.
 */

#include <cstdio>
#include <cstdlib>

#include <windows.h>

#include "inspect.h"

// inspect.cpp checks this for cancellation in code paths these tests don't
// exercise; normally defined in main.cpp, which isn't linked into this target.
std::atomic<bool> util::g_cancel_requested{false};

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
    if (cond) {
        std::printf("  PASS: %s\n", what);
    } else {
        std::printf("  FAIL: %s\n", what);
        ++g_failures;
    }
}

constexpr DWORD kClusterSize = 4096;

using inspect::BlockEntry;
using inspect::ClaimOccurrence;
using inspect::LcnClaim;
using inspect::LcnInterval;
using inspect::LcnIntervalIndex;
using inspect::build_lcn_interval_index;
using inspect::lcn_interval_find;
using inspect::lcn_interval_find_all;
using inspect::lcn_interval_for_each_overlapping;

/// @brief file 0 owns [1,5), file 1 owns [4,7) - the staggered overlap case
/// discussed for the sweep-line design: not aligned to either file's extent
/// boundaries, so the resulting sub-ranges must reflect the true overlap.
void test_staggered_partial_overlap() {
    std::printf("test_staggered_partial_overlap\n");

    std::vector<LcnClaim> claims = {
        {0, 1, 5, 0},
        {1, 4, 7, 0},
    };
    LcnIntervalIndex idx = build_lcn_interval_index(claims, kClusterSize, ClaimOccurrence::kAll);

    check(idx.size() == 3, "produces exactly 3 disjoint sub-intervals");

    // [1,4) owned by file 0 only
    const auto* owners_1 = lcn_interval_find_all(idx, 1);
    check(owners_1 != nullptr && owners_1->size() == 1 && (*owners_1)[0].file_index == 0,
          "LCN 1 owned by file 0 only");
    const auto* owners_3 = lcn_interval_find_all(idx, 3);
    check(owners_3 != nullptr && owners_3->size() == 1 && (*owners_3)[0].file_index == 0,
          "LCN 3 (last cluster before overlap) owned by file 0 only");

    // [4,5) owned by both
    const auto* owners_4 = lcn_interval_find_all(idx, 4);
    check(owners_4 != nullptr && owners_4->size() == 2, "LCN 4 (the shared cluster) owned by both files");

    // [5,7) owned by file 1 only
    const auto* owners_5 = lcn_interval_find_all(idx, 5);
    check(owners_5 != nullptr && owners_5->size() == 1 && (*owners_5)[0].file_index == 1,
          "LCN 5 (first cluster after overlap) owned by file 1 only");
    const auto* owners_6 = lcn_interval_find_all(idx, 6);
    check(owners_6 != nullptr && owners_6->size() == 1 && (*owners_6)[0].file_index == 1,
          "LCN 6 (last cluster of file 1's range) owned by file 1 only");

    // Boundaries: LCN 0 and LCN 7 are outside every claim.
    check(lcn_interval_find(idx, 0) == nullptr, "LCN 0 (before every claim) is unclaimed");
    check(lcn_interval_find(idx, 7) == nullptr, "LCN 7 (end_lcn, exclusive) is unclaimed");
}

/// @brief Three files all overlapping the same range - find_all must report all three.
void test_three_way_overlap() {
    std::printf("test_three_way_overlap\n");

    std::vector<LcnClaim> claims = {
        {0, 10, 20, 0},
        {1, 10, 20, 0},
        {2, 10, 20, 0},
    };
    LcnIntervalIndex idx = build_lcn_interval_index(claims, kClusterSize, ClaimOccurrence::kAll);

    check(idx.size() == 1, "three identical ranges merge into a single interval");
    const auto* owners = lcn_interval_find_all(idx, 15);
    check(owners != nullptr && owners->size() == 3, "midpoint LCN reports all three owners");
}

/// @brief No overlap at all - every claim stays its own interval, single-owner.
void test_no_overlap() {
    std::printf("test_no_overlap\n");

    std::vector<LcnClaim> claims = {
        {0, 0, 5, 0},
        {1, 10, 15, 0},
        {2, 100, 200, 0},
    };
    LcnIntervalIndex idx = build_lcn_interval_index(claims, kClusterSize, ClaimOccurrence::kAll);

    check(idx.size() == 3, "three disjoint claims produce three intervals");
    for (const auto& iv : idx) {
        check(iv.owners.size() == 1, "each disjoint interval has exactly one owner");
    }
    check(lcn_interval_find(idx, 7) == nullptr, "gap between claims is unclaimed");
    check(lcn_interval_find(idx, 50) == nullptr, "gap between claims is unclaimed (second gap)");
}

/// @brief ClaimOccurrence::kFirst must keep only the lowest file_index per run,
/// matching the original try_emplace "first scanned wins" semantics.
void test_first_occurrence_tiebreak() {
    std::printf("test_first_occurrence_tiebreak\n");

    // File 2 scanned "first" in this claim list is irrelevant - file_index
    // itself encodes scan order, and the lowest file_index must win regardless
    // of the order claims are passed in.
    std::vector<LcnClaim> claims = {
        {2, 0, 10, 2000},
        {0, 0, 10, 0},
        {1, 0, 10, 1000},
    };
    LcnIntervalIndex idx = build_lcn_interval_index(claims, kClusterSize, ClaimOccurrence::kFirst);

    check(idx.size() == 1, "fully overlapping claims collapse to one interval");
    const auto* owners = lcn_interval_find_all(idx, 5);
    check(owners != nullptr && owners->size() == 1, "kFirst keeps exactly one owner");
    check(owners != nullptr && (*owners)[0].file_index == 0, "kFirst keeps the lowest file_index (earliest scanned)");
}

/// @brief Owner file_offset must track the correct byte position within a run,
/// including the offset at a sub-range's start after a claim is split by overlap.
void test_offset_arithmetic() {
    std::printf("test_offset_arithmetic\n");

    // File 0 claims LCN [100,110) starting at file byte offset 0 (so LCN 100 -> offset 0,
    // LCN 105 -> offset 5*4096). File 1 claims LCN [105,108) at file byte offset 0 (its own
    // file, so LCN 105 -> offset 0 for file 1).
    std::vector<LcnClaim> claims = {
        {0, 100, 110, 0},
        {1, 105, 108, 0},
    };
    LcnIntervalIndex idx = build_lcn_interval_index(claims, kClusterSize, ClaimOccurrence::kAll);

    const BlockEntry* f0_at_105 = nullptr;
    const auto* owners_105 = lcn_interval_find_all(idx, 105);
    check(owners_105 != nullptr && owners_105->size() == 2, "LCN 105 has two owners");
    if (owners_105) {
        for (const auto& o : *owners_105) {
            if (o.file_index == 0) f0_at_105 = &o;
        }
    }
    check(f0_at_105 != nullptr && f0_at_105->file_offset == 5ULL * kClusterSize,
          "file 0's offset at LCN 105 is correctly advanced 5 clusters from its claim start");
}

/// @brief lcn_interval_for_each_overlapping must return exactly the intervals
/// touching a query range - including partial overlap at both edges - and none
/// outside it.
void test_for_each_overlapping() {
    std::printf("test_for_each_overlapping\n");

    std::vector<LcnClaim> claims = {
        {0, 0, 5, 0},
        {1, 5, 10, 0},
        {2, 10, 15, 0},
        {3, 20, 25, 0},
    };
    LcnIntervalIndex idx = build_lcn_interval_index(claims, kClusterSize, ClaimOccurrence::kAll);
    check(idx.size() == 4, "four disjoint claims produce four intervals");

    // Query [3, 12): should touch intervals [0,5), [5,10), [10,15) but not [20,25).
    std::vector<LONGLONG> touched;
    lcn_interval_for_each_overlapping(idx, 3, 12, [&](const LcnInterval& iv) {
        touched.push_back(iv.start_lcn);
    });
    check(touched.size() == 3, "query [3,12) touches exactly 3 intervals");
    check(!touched.empty() && touched[0] == 0, "first touched interval starts at 0");
    check(touched.size() >= 3 && touched[2] == 10, "third touched interval starts at 10");

    // Query exactly matching a gap-adjacent boundary: [15, 20) touches nothing (the gap).
    std::vector<LONGLONG> gap_touched;
    lcn_interval_for_each_overlapping(idx, 15, 20, [&](const LcnInterval&) {
        gap_touched.push_back(0);
    });
    check(gap_touched.empty(), "query over an unclaimed gap touches no intervals");

    // Query touching only the last interval's tail: [23, 30).
    std::vector<LONGLONG> tail_touched;
    lcn_interval_for_each_overlapping(idx, 23, 30, [&](const LcnInterval& iv) {
        tail_touched.push_back(iv.start_lcn);
    });
    check(tail_touched.size() == 1 && tail_touched[0] == 20, "query touching only the tail finds just that interval");
}

/// @brief Empty input must not crash and must behave as "nothing claimed".
void test_empty_index() {
    std::printf("test_empty_index\n");

    LcnIntervalIndex idx = build_lcn_interval_index({}, kClusterSize, ClaimOccurrence::kAll);
    check(idx.empty(), "empty claim list produces an empty index");
    check(lcn_interval_find(idx, 0) == nullptr, "find() on empty index returns nullptr");
    check(lcn_interval_find_all(idx, 0) == nullptr, "find_all() on empty index returns nullptr");

    int overlap_calls = 0;
    lcn_interval_for_each_overlapping(idx, 0, 1000, [&](const LcnInterval&) { ++overlap_calls; });
    check(overlap_calls == 0, "for_each_overlapping() on empty index invokes nothing");
}

} // namespace

int main() {
    test_staggered_partial_overlap();
    test_three_way_overlap();
    test_no_overlap();
    test_first_occurrence_tiebreak();
    test_offset_arithmetic();
    test_for_each_overlapping();
    test_empty_index();

    if (g_failures == 0) {
        std::printf("\nAll tests passed.\n");
        return 0;
    }
    std::printf("\n%d assertion(s) FAILED.\n", g_failures);
    return 1;
}
