/*
 * Copyright (C) 2025 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/trace_processor/core/dataframe/dataframe.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/regex.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/base/test/status_matchers.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/core/dataframe/dataframe_test_utils.h"
#include "src/trace_processor/core/dataframe/logical_plan.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/core/dataframe/typed_cursor.h"
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::dataframe {

inline std::string TrimSpacePerLine(const std::string& s) {
  std::string result;
  result.reserve(s.size());
  bool at_line_start = true;
  for (char c : s) {
    if (c == '\n') {
      at_line_start = true;
      result += c;
    } else if (at_line_start && std::isspace(c)) {
      // Skip whitespace at line start
      continue;
    } else {
      at_line_start = false;
      result += c;
    }
  }
  return result;
}

// Custom matcher that compares strings ignoring all whitespace
MATCHER_P(EqualsIgnoringWhitespace,
          expected_str,
          "equals (ignoring all whitespace)") {
  std::string stripped_expected =
      TrimSpacePerLine(base::TrimWhitespace(expected_str));
  std::string stripped_actual = TrimSpacePerLine(base::TrimWhitespace(arg));
  if (stripped_actual == stripped_expected) {
    return true;
  }
  *result_listener << "after removing all whitespace:\nExpected:\n"
                   << stripped_expected << "\nActual:\n"
                   << stripped_actual;
  return false;
}

TEST(DataframeTest, Insert) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"id", "col2", "col3", "col4"},
      CreateTypedColumnSpec(Id(), NonNull(), IdSorted()),
      CreateTypedColumnSpec(Uint32(), NonNull(), Unsorted()),
      CreateTypedColumnSpec(Int64(), DenseNull(), Unsorted()),
      CreateTypedColumnSpec(String(), SparseNull(), Unsorted()));
  StringPool pool;
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &pool);
  df.InsertUnchecked(kSpec, std::monostate(), 10u, std::make_optional(0l),
                     std::make_optional(pool.InternString("foo")));
  df.InsertUnchecked(kSpec, std::monostate(), 20u, std::nullopt, std::nullopt);

  VerifyData(
      df, 0b1111,
      Rows(Row(0u, 10u, int64_t(0l), "foo"), Row(1u, 20u, nullptr, nullptr)));
}

TEST(DataframeTest, GetCellAndSetCell) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"id", "col2", "col3", "col4"},
      CreateTypedColumnSpec(Id(), NonNull(), IdSorted()),
      CreateTypedColumnSpec(Uint32(), NonNull(), Unsorted()),
      CreateTypedColumnSpec(Int64(), DenseNull(), Unsorted()),
      CreateTypedColumnSpec(String(), SparseNullWithPopcountAlways(),
                            Unsorted()));
  StringPool pool;
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &pool);
  df.InsertUnchecked(kSpec, std::monostate(), 10u, std::make_optional(0l),
                     std::make_optional(pool.InternString("foo")));
  df.InsertUnchecked(kSpec, std::monostate(), 20u, std::nullopt, std::nullopt);

  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 0), 0u);
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 0), 10u);
  ASSERT_EQ(df.GetCellUnchecked<2>(kSpec, 0), 0l);
  ASSERT_EQ(df.GetCellUnchecked<3>(kSpec, 0), pool.InternString("foo"));

  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 1), 1u);
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 1), 20u);
  ASSERT_EQ(df.GetCellUnchecked<2>(kSpec, 1), std::nullopt);
  ASSERT_EQ(df.GetCellUnchecked<3>(kSpec, 1), std::nullopt);

  df.SetCellUnchecked<1>(kSpec, 0, 9u);
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 0), 9u);

  df.SetCellUnchecked<2>(kSpec, 0, std::nullopt);
  ASSERT_EQ(df.GetCellUnchecked<2>(kSpec, 0), std::nullopt);
}

TEST(DataframeTest, SetCellUncheckedInternal_SparseNullWithPopcount) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"col_sparse_uint32", "col_sparse_str"},
      CreateTypedColumnSpec(Uint32(), SparseNullWithPopcountAlways(),
                            Unsorted()),
      CreateTypedColumnSpec(String(), SparseNullWithPopcountUntilFinalization(),
                            Unsorted()));
  StringPool pool;
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &pool);

  // Initial values:
  // Row 0: {100, "aa"}
  // Row 1: {null, null}
  // Row 2: {200, "bb"}
  // Row 3: {null, "cc"}
  // Row 4: {300, null}
  df.InsertUnchecked(kSpec, std::make_optional(100u),
                     std::make_optional(pool.InternString("aa")));
  df.InsertUnchecked(kSpec, std::nullopt, std::nullopt);
  df.InsertUnchecked(kSpec, std::make_optional(200u),
                     std::make_optional(pool.InternString("bb")));
  df.InsertUnchecked(kSpec, std::nullopt,
                     std::make_optional(pool.InternString("cc")));
  df.InsertUnchecked(kSpec, std::make_optional(300u), std::nullopt);

  // Verify initial state
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 0), std::make_optional(100u));
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 0),
            std::make_optional(pool.InternString("aa")));
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 1), std::nullopt);
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 1), std::nullopt);
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 2), std::make_optional(200u));
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 2),
            std::make_optional(pool.InternString("bb")));
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 3), std::nullopt);
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 3),
            std::make_optional(pool.InternString("cc")));
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 4), std::make_optional(300u));
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 4), std::nullopt);

  // Test 1: Set existing non-null to new non-null
  // Row 0: {100, "aa"} -> {101, "new_aa"}
  df.SetCellUnchecked<0>(kSpec, 0, std::make_optional(101u));
  df.SetCellUnchecked<1>(kSpec, 0,
                         std::make_optional(pool.InternString("new_aa")));
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 0), std::make_optional(101u));
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 0),
            std::make_optional(pool.InternString("new_aa")));

  // Test 2: Set existing non-null to null (triggers memmove for deletion)
  // Row 2: {200, "bb"} -> {null, null}
  // Expected data after:
  // Row 0: {101, "new_aa"}
  // Row 1: {null, null}
  // Row 2: {null, null}
  // Row 3: {null, "cc"}
  // Row 4: {300, null}
  // Sparse Uint32 data: [101, 300]
  // Sparse String data: ["new_aa", "cc"]
  df.SetCellUnchecked<0>(kSpec, 2, std::nullopt);
  df.SetCellUnchecked<1>(kSpec, 2, std::nullopt);
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 2), std::nullopt);
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 2), std::nullopt);
  // Check surrounding values are not affected
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 0), std::make_optional(101u));
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 0),
            std::make_optional(pool.InternString("new_aa")));
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 4), std::make_optional(300u));
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 3),
            std::make_optional(pool.InternString("cc")));

  // Test 3: Set existing null to non-null (triggers memmove for insertion)
  // Row 1: {null, null} -> {150, "dd"}
  // Expected data after:
  // Row 0: {101, "new_aa"}
  // Row 1: {150, "dd"}
  // Row 2: {null, null}
  // Row 3: {null, "cc"}
  // Row 4: {300, null}
  // Sparse Uint32 data: [101, 150, 300]
  // Sparse String data: ["new_aa", "dd", "cc"]
  df.SetCellUnchecked<0>(kSpec, 1, std::make_optional(150u));
  df.SetCellUnchecked<1>(kSpec, 1, std::make_optional(pool.InternString("dd")));
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 1), std::make_optional(150u));
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 1),
            std::make_optional(pool.InternString("dd")));
  // Check surrounding values
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 0), std::make_optional(101u));
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 4), std::make_optional(300u));
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 0),
            std::make_optional(pool.InternString("new_aa")));
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 3),
            std::make_optional(pool.InternString("cc")));

  // Test 4: Set at the beginning - null to non-null
  // Row 0 was {101, "new_aa"}. Set to {50, "start"}
  // This is effectively an update, not an insertion in sparse terms if it was
  // already non-null. Let's make Row 0 null first to test insertion at
  // beginning. Row 0: {101, "new_aa"} -> {null, null} Sparse Uint32 data: [150,
  // 300] Sparse String data: ["dd", "cc"]
  df.SetCellUnchecked<0>(kSpec, 0, std::nullopt);
  df.SetCellUnchecked<1>(kSpec, 0, std::nullopt);
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 0), std::nullopt);
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 0), std::nullopt);

  // Now set Row 0 from null to non-null: {null, null} -> {50, "start"}
  // Expected data after:
  // Row 0: {50, "start"}
  // Row 1: {150, "dd"}
  // Row 2: {null, null}
  // Row 3: {null, "cc"}
  // Row 4: {300, null}
  // Sparse Uint32 data: [50, 150, 300]
  // Sparse String data: ["start", "dd", "cc"]
  df.SetCellUnchecked<0>(kSpec, 0, std::make_optional(50u));
  df.SetCellUnchecked<1>(kSpec, 0,
                         std::make_optional(pool.InternString("start")));
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 0), std::make_optional(50u));
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 0),
            std::make_optional(pool.InternString("start")));
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 1),
            std::make_optional(150u));  // Check next element
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 1),
            std::make_optional(pool.InternString("dd")));

  // Test 5: Set at the end - null to non-null
  // Row 4 was {300, null}. Let's make it {null, null} first.
  // Sparse Uint32 data: [50, 150]
  // Sparse String data: ["start", "dd", "cc"] (Row 4 string was already null)
  df.SetCellUnchecked<0>(kSpec, 4, std::nullopt);
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 4), std::nullopt);

  // Now set Row 4 from {null, null} -> {400, "end"}
  // Expected data after:
  // Row 0: {50, "start"}
  // Row 1: {150, "dd"}
  // Row 2: {null, null}
  // Row 3: {null, "cc"}
  // Row 4: {400, "end"}
  // Sparse Uint32 data: [50, 150, 400]
  // Sparse String data: ["start", "dd", "cc", "end"]
  df.SetCellUnchecked<0>(kSpec, 4, std::make_optional(400u));
  df.SetCellUnchecked<1>(kSpec, 4,
                         std::make_optional(pool.InternString("end")));
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 4), std::make_optional(400u));
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 4),
            std::make_optional(pool.InternString("end")));
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 1),
            std::make_optional(150u));  // Check previous element
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 3),
            std::make_optional(pool.InternString("cc")));

  // Test 6: Set at the end - non-null to null
  // Row 4: {400, "end"} -> {null, null}
  // Expected data after:
  // Row 0: {50, "start"}
  // Row 1: {150, "dd"}
  // Row 2: {null, null}
  // Row 3: {null, "cc"}
  // Row 4: {null, null}
  // Sparse Uint32 data: [50, 150]
  // Sparse String data: ["start", "dd", "cc"]
  df.SetCellUnchecked<0>(kSpec, 4, std::nullopt);
  df.SetCellUnchecked<1>(kSpec, 4, std::nullopt);
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 4), std::nullopt);
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 4), std::nullopt);
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 1), std::make_optional(150u));
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 3),
            std::make_optional(pool.InternString("cc")));

  // Test 7: Operations on an empty column (implicitly tested by starting with
  // empty and inserting) Create a new dataframe for this.
  Dataframe df_empty = Dataframe::CreateFromTypedSpec(kSpec, &pool);
  df_empty.InsertUnchecked(kSpec, std::nullopt,
                           std::nullopt);  // Row 0: {null, null}
  df_empty.SetCellUnchecked<0>(kSpec, 0, std::make_optional(1u));
  ASSERT_EQ(df_empty.GetCellUnchecked<0>(kSpec, 0), std::make_optional(1u));
  df_empty.SetCellUnchecked<0>(kSpec, 0, std::nullopt);
  ASSERT_EQ(df_empty.GetCellUnchecked<0>(kSpec, 0), std::nullopt);

  // Test 8: Full column (all non-null), then set to null
  Dataframe df_full = Dataframe::CreateFromTypedSpec(kSpec, &pool);
  df_full.InsertUnchecked(kSpec, std::make_optional(10u),
                          std::make_optional(pool.InternString("f1")));
  df_full.InsertUnchecked(kSpec, std::make_optional(20u),
                          std::make_optional(pool.InternString("f2")));
  df_full.SetCellUnchecked<0>(kSpec, 0, std::nullopt);
  ASSERT_EQ(df_full.GetCellUnchecked<0>(kSpec, 0), std::nullopt);
  ASSERT_EQ(df_full.GetCellUnchecked<0>(kSpec, 1), std::make_optional(20u));
  ASSERT_EQ(
      df_full.GetCellUnchecked<1>(kSpec, 0),
      std::make_optional(pool.InternString("f1")));  // String col unaffected
}

TEST(DataframeTest, InsertAndSet_WordBoundaryStress) {
  StringPool pool;
  constexpr auto kSpec = CreateTypedDataframeSpec(
      {"sparse_col_uint32"},
      CreateTypedColumnSpec(Uint32(), SparseNullWithPopcountAlways(),
                            Unsorted()));

  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &pool);
  std::vector<std::optional<uint32_t>> current_state;

  const uint32_t kMaxRows = 130;  // Crosses two 64-bit boundaries

  uint32_t critical_indices_arr[] = {0,   1,   62,  63,  64,          65,
                                     126, 127, 128, 129, kMaxRows - 1};
  std::set<uint32_t> critical_indices(
      critical_indices_arr,
      critical_indices_arr + (sizeof(critical_indices_arr) / sizeof(uint32_t)));

  auto verify_state = [&]() {
    ASSERT_EQ(df.row_count(), current_state.size()) << "Row count mismatch.";
    for (uint32_t i = 0; i < current_state.size(); ++i) {
      ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, i), current_state[i])
          << "Mismatch at index " << i;
    }
  };

  // Phase 1: Insertions (stressing InsertUncheckedInternal)
  for (uint32_t i = 0; i < kMaxRows; ++i) {
    std::optional<uint32_t> val_to_insert;
    if (i % 3 == 0) {  // Introduce some nulls
      val_to_insert = std::nullopt;
    } else {
      val_to_insert = std::make_optional(i * 10);
    }

    df.InsertUnchecked(kSpec, val_to_insert);
    current_state.push_back(val_to_insert);

    if (critical_indices.count(i)) {
      verify_state();
    }
  }
  verify_state();  // Final verification after all insertions

  // Phase 2: Set Operations (stressing SetCellUncheckedInternal)

  // Test 2.1: Non-null -> Null at critical indices
  // Setup: Ensure all elements are non-null to robustly test the transition.
  for (uint32_t i = 0; i < kMaxRows; ++i) {
    if (!current_state[i].has_value()) {
      current_state[i] =
          std::make_optional((i * 10) + 77);  // Arbitrary non-null
      df.SetCellUnchecked<0>(kSpec, i, current_state[i]);
    }
  }
  verify_state();  // Verify setup

  for (uint32_t idx : critical_indices) {
    // Precondition: current_state[idx] is non-null due to setup.
    current_state[idx] = std::nullopt;
    df.SetCellUnchecked<0>(kSpec, idx, std::nullopt);
    verify_state();
  }

  // Test 2.2: Null -> Non-null at critical indices
  // Setup: Ensure all elements are null.
  for (uint32_t i = 0; i < kMaxRows; ++i) {
    if (current_state[i].has_value()) {
      current_state[i] = std::nullopt;
      df.SetCellUnchecked<0>(kSpec, i, std::nullopt);
    }
  }
  verify_state();  // Verify setup

  for (uint32_t idx : critical_indices) {
    // Precondition: current_state[idx] is null.
    uint32_t new_val = (idx * 100) + 1;
    current_state[idx] = std::make_optional(new_val);
    df.SetCellUnchecked<0>(kSpec, idx, std::make_optional(new_val));
    verify_state();
  }

  // Test 2.3: Non-null -> Different Non-null at critical indices
  // Setup: Ensure all elements are non-null (they are from the previous step).
  for (uint32_t idx : critical_indices) {
    // Precondition: current_state[idx] is non-null.
    uint32_t new_val = current_state[idx].value() + 55;
    current_state[idx] = std::make_optional(new_val);
    df.SetCellUnchecked<0>(kSpec, idx, std::make_optional(new_val));
    verify_state();
  }

  // Test 2.4: Sequence of operations crossing a boundary (e.g., 60-70)
  uint32_t seq_start = 60;
  uint32_t seq_end = 70;

  // Sequence 2.4.1: Make the range [seq_start, seq_end] all non-null
  for (uint32_t i = seq_start; i <= seq_end; ++i) {
    uint32_t val = (i * 300) + 3;
    current_state[i] = std::make_optional(val);
    df.SetCellUnchecked<0>(kSpec, i, std::make_optional(val));
    verify_state();
  }

  // Sequence 2.4.2: Make the range [seq_start, seq_end] all null
  for (uint32_t i = seq_start; i <= seq_end; ++i) {
    current_state[i] = std::nullopt;
    df.SetCellUnchecked<0>(kSpec, i, std::nullopt);
    verify_state();
  }

  // Test 2.5: A mix of operations in a specific boundary region (e.g. 63, 64,
  // 65) Setup: 63=non-null, 64=null, 65=non-null
  current_state[63] = std::make_optional(6300u);
  df.SetCellUnchecked<0>(kSpec, 63, current_state[63]);
  current_state[64] = std::nullopt;
  df.SetCellUnchecked<0>(kSpec, 64, current_state[64]);
  current_state[65] = std::make_optional(6500u);
  df.SetCellUnchecked<0>(kSpec, 65, current_state[65]);
  verify_state();

  // Test: Flip them - 63=null
  current_state[63] = std::nullopt;
  df.SetCellUnchecked<0>(kSpec, 63, current_state[63]);
  verify_state();

  // Test: Flip them - 64=non-null
  current_state[64] = std::make_optional(6401u);
  df.SetCellUnchecked<0>(kSpec, 64, current_state[64]);
  verify_state();

  // Test: Flip them - 65=null
  current_state[65] = std::nullopt;
  df.SetCellUnchecked<0>(kSpec, 65, current_state[65]);
  verify_state();
}

TEST(DataframeTest, TypedCursor) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"id", "col2", "col3", "col4"},
      CreateTypedColumnSpec(Id(), NonNull(), IdSorted()),
      CreateTypedColumnSpec(Uint32(), NonNull(), Unsorted()),
      CreateTypedColumnSpec(Int64(), DenseNull(), Unsorted()),
      CreateTypedColumnSpec(String(), SparseNullWithPopcountAlways(),
                            Unsorted()));
  StringPool pool;
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &pool);
  df.InsertUnchecked(kSpec, std::monostate(), 10u, std::make_optional(0l),
                     std::make_optional(pool.InternString("foo")));
  df.InsertUnchecked(kSpec, std::monostate(), 20u, std::nullopt, std::nullopt);

  TypedCursor cursor(&df, {FilterSpec{0, 0, Eq{}, {}}}, {});
  {
    cursor.SetFilterValueUnchecked(0, int64_t(0l));
    cursor.ExecuteUnchecked();
    ASSERT_FALSE(cursor.Eof());
    ASSERT_EQ(cursor.GetCellUnchecked<0>(kSpec), 0u);
    ASSERT_EQ(cursor.GetCellUnchecked<1>(kSpec), 10u);
    ASSERT_EQ(cursor.GetCellUnchecked<2>(kSpec), 0l);
    ASSERT_EQ(cursor.GetCellUnchecked<3>(kSpec), pool.InternString("foo"));
    cursor.Next();
    ASSERT_TRUE(cursor.Eof());
  }
  {
    cursor.SetFilterValueUnchecked(0, int64_t(1l));
    cursor.ExecuteUnchecked();
    ASSERT_FALSE(cursor.Eof());
    ASSERT_EQ(cursor.GetCellUnchecked<0>(kSpec), 1u);
    ASSERT_EQ(cursor.GetCellUnchecked<1>(kSpec), 20u);
    ASSERT_EQ(cursor.GetCellUnchecked<2>(kSpec), std::nullopt);
    ASSERT_EQ(cursor.GetCellUnchecked<3>(kSpec), std::nullopt);
    cursor.Next();
    ASSERT_TRUE(cursor.Eof());
  }
}

TEST(DataframeTest, TypedCursorCreatedBeforeDataframeReplacement) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"value"}, CreateTypedColumnSpec(Int64(), NonNull(), Unsorted()));
  StringPool pool;
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &pool);
  TypedCursor cursor(&df, {FilterSpec{0, 0, Eq{}, {}}}, {});

  Dataframe replacement = Dataframe::CreateFromTypedSpec(kSpec, &pool);
  replacement.InsertUnchecked(kSpec, int64_t{7});
  replacement.Finalize();
  df = std::move(replacement);

  cursor.SetFilterValueUnchecked(0, int64_t{7});
  cursor.ExecuteUnchecked();
  ASSERT_FALSE(cursor.Eof());
  EXPECT_EQ(cursor.GetCellUnchecked<0>(kSpec), int64_t{7});
  cursor.Next();
  EXPECT_TRUE(cursor.Eof());
}

TEST(DataframeTest, TypedCursorReplansAfterDataframeReplacement) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"value"}, CreateTypedColumnSpec(Int64(), NonNull(), Unsorted()));
  StringPool pool;
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &pool);
  df.InsertUnchecked(kSpec, int64_t{10});
  df.InsertUnchecked(kSpec, int64_t{20});
  df.Finalize();
  TypedCursor filtered(&df, {FilterSpec{0, 0, Eq{}, {}}}, {});
  TypedCursor sorted(&df, {}, {{0, SortDirection::kAscending}});
  filtered.SetFilterValueUnchecked(0, int64_t{10});
  filtered.ExecuteUnchecked();
  ASSERT_FALSE(filtered.Eof());
  EXPECT_EQ(filtered.GetCellUnchecked<0>(kSpec), int64_t{10});
  sorted.ExecuteUnchecked();
  ASSERT_FALSE(sorted.Eof());
  EXPECT_EQ(sorted.GetCellUnchecked<0>(kSpec), int64_t{10});

  // Same row count and mutation history, but different values and sort order.
  Dataframe replacement = Dataframe::CreateFromTypedSpec(kSpec, &pool);
  replacement.InsertUnchecked(kSpec, int64_t{40});
  replacement.InsertUnchecked(kSpec, int64_t{30});
  replacement.Finalize();
  df = std::move(replacement);

  filtered.SetFilterValueUnchecked(0, int64_t{30});
  filtered.ExecuteUnchecked();
  ASSERT_FALSE(filtered.Eof());
  EXPECT_EQ(filtered.GetCellUnchecked<0>(kSpec), int64_t{30});
  filtered.Next();
  EXPECT_TRUE(filtered.Eof());
  sorted.ExecuteUnchecked();
  ASSERT_FALSE(sorted.Eof());
  EXPECT_EQ(sorted.GetCellUnchecked<0>(kSpec), int64_t{30});
  sorted.Next();
  ASSERT_FALSE(sorted.Eof());
  EXPECT_EQ(sorted.GetCellUnchecked<0>(kSpec), int64_t{40});
  sorted.Next();
  EXPECT_TRUE(sorted.Eof());
}

TEST(DataframeTest, TypedCursorReplacementWithRepeatedFilterColumn) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"value"}, CreateTypedColumnSpec(Int64(), NonNull(), Unsorted()));
  StringPool pool;
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &pool);
  df.InsertUnchecked(kSpec, int64_t{10});
  df.SetCellUnchecked<0>(kSpec, 0, int64_t{20});
  ASSERT_EQ(df.mutations(), uint64_t{2});
  TypedCursor cursor(
      &df, {FilterSpec{0, 0, Eq{}, {}}, FilterSpec{0, 1, Eq{}, {}}}, {});
  cursor.SetFilterValueUnchecked(0, int64_t{20});
  cursor.SetFilterValueUnchecked(1, int64_t{20});
  cursor.ExecuteUnchecked();
  ASSERT_FALSE(cursor.Eof());

  Dataframe replacement = Dataframe::CreateFromTypedSpec(kSpec, &pool);
  replacement.InsertUnchecked(kSpec, int64_t{30});
  ASSERT_EQ(replacement.mutations(), uint64_t{1});
  df = std::move(replacement);
  // Both watched sums are 3: the independent generation must force a replan.
  ASSERT_EQ(df.mutations(), uint64_t{3});
  cursor.SetFilterValueUnchecked(0, int64_t{30});
  cursor.SetFilterValueUnchecked(1, int64_t{30});
  cursor.ExecuteUnchecked();
  ASSERT_FALSE(cursor.Eof());
  EXPECT_EQ(cursor.GetCellUnchecked<0>(kSpec), int64_t{30});
}

TEST(DataframeTest, TypedCursorFilterListAndResetAfterDataframeReplacement) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"value"}, CreateTypedColumnSpec(Int64(), NonNull(), Unsorted()));
  StringPool pool;
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &pool);
  df.InsertUnchecked(kSpec, int64_t{10});
  df.Finalize();
  TypedCursor cursor(&df, {FilterSpec{0, 0, In{}, {}}}, {});
  TypedCursor::FilterValue value = int64_t{10};
  cursor.SetFilterValueListUnchecked(0, &value, 1);
  cursor.ExecuteUnchecked();
  ASSERT_FALSE(cursor.Eof());
  EXPECT_EQ(cursor.GetCellUnchecked<0>(kSpec), int64_t{10});

  Dataframe replacement = Dataframe::CreateFromTypedSpec(kSpec, &pool);
  replacement.InsertUnchecked(kSpec, int64_t{30});
  replacement.Finalize();
  df = std::move(replacement);
  value = int64_t{30};
  cursor.SetFilterValueListUnchecked(0, &value, 1);
  cursor.ExecuteUnchecked();
  ASSERT_FALSE(cursor.Eof());
  EXPECT_EQ(cursor.GetCellUnchecked<0>(kSpec), int64_t{30});

  Dataframe next = Dataframe::CreateFromTypedSpec(kSpec, &pool);
  next.InsertUnchecked(kSpec, int64_t{50});
  next.Finalize();
  df = std::move(next);
  cursor.Reset();
  value = int64_t{50};
  cursor.SetFilterValueListUnchecked(0, &value, 1);
  cursor.ExecuteUnchecked();
  ASSERT_FALSE(cursor.Eof());
  EXPECT_EQ(cursor.GetCellUnchecked<0>(kSpec), int64_t{50});
}

TEST(DataframeTest, TypedCursorSetMultipleTimes) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"id", "col2", "col3", "col4"},
      CreateTypedColumnSpec(Id(), NonNull(), IdSorted()),
      CreateTypedColumnSpec(Uint32(), NonNull(), Unsorted()),
      CreateTypedColumnSpec(Int64(), DenseNull(), Unsorted()),
      CreateTypedColumnSpec(String(), SparseNullWithPopcountAlways(),
                            Unsorted()));
  StringPool pool;
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &pool);
  df.InsertUnchecked(kSpec, std::monostate(), 10u, std::make_optional(0l),
                     std::make_optional(pool.InternString("foo")));
  df.InsertUnchecked(kSpec, std::monostate(), 20u, std::nullopt, std::nullopt);
  {
    TypedCursor cursor(&df, {}, {});
    cursor.ExecuteUnchecked();
    ASSERT_FALSE(cursor.Eof());
    ASSERT_EQ(cursor.GetCellUnchecked<1>(kSpec), 10u);
    cursor.SetCellUnchecked<1>(kSpec, 20u);
    ASSERT_EQ(cursor.GetCellUnchecked<1>(kSpec), 20u);
  }
  {
    TypedCursor cursor(&df, {FilterSpec{1, 0, Eq{}, {}}}, {});
    cursor.SetFilterValueUnchecked(0, int64_t(20));
    cursor.ExecuteUnchecked();
    ASSERT_FALSE(cursor.Eof());
    ASSERT_EQ(cursor.GetCellUnchecked<1>(kSpec), 20u);
    cursor.SetCellUnchecked<1>(kSpec, 20u);
    ASSERT_EQ(cursor.GetCellUnchecked<1>(kSpec), 20u);
  }
}

TEST(DataframeTest, TypedCursorInFilter) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"id", "col2"}, CreateTypedColumnSpec(Id(), NonNull(), IdSorted()),
      CreateTypedColumnSpec(Uint32(), NonNull(), Unsorted()));
  StringPool pool;
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &pool);
  df.InsertUnchecked(kSpec, std::monostate(), 10u);
  df.InsertUnchecked(kSpec, std::monostate(), 20u);
  df.InsertUnchecked(kSpec, std::monostate(), 30u);
  df.InsertUnchecked(kSpec, std::monostate(), 40u);
  df.InsertUnchecked(kSpec, std::monostate(), 50u);

  // Filter col2 IN (20, 40) using a pointer+size list.
  using FV = TypedCursor::FilterValue;
  TypedCursor cursor(&df, {FilterSpec{1, 0, In{}, {}}}, {});
  FV values[] = {int64_t(20), int64_t(40)};
  cursor.SetFilterValueListUnchecked(0, values, 2);
  cursor.ExecuteUnchecked();

  ASSERT_FALSE(cursor.Eof());
  ASSERT_EQ(cursor.GetCellUnchecked<1>(kSpec), 20u);
  cursor.Next();

  ASSERT_FALSE(cursor.Eof());
  ASSERT_EQ(cursor.GetCellUnchecked<1>(kSpec), 40u);
  cursor.Next();

  ASSERT_TRUE(cursor.Eof());
}

TEST(DataframeTest, TypedCursorInFilterEmpty) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"id", "col2"}, CreateTypedColumnSpec(Id(), NonNull(), IdSorted()),
      CreateTypedColumnSpec(Uint32(), NonNull(), Unsorted()));
  StringPool pool;
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &pool);
  df.InsertUnchecked(kSpec, std::monostate(), 10u);

  // Filter col2 IN () — empty list should return no rows.
  TypedCursor cursor(&df, {FilterSpec{1, 0, In{}, {}}}, {});
  cursor.SetFilterValueListUnchecked(0, nullptr, 0);
  cursor.ExecuteUnchecked();
  ASSERT_TRUE(cursor.Eof());
}

TEST(DataframeTest, TypedCursorInFilterReexecute) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"id", "col2"}, CreateTypedColumnSpec(Id(), NonNull(), IdSorted()),
      CreateTypedColumnSpec(Uint32(), NonNull(), Unsorted()));
  StringPool pool;
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &pool);
  df.InsertUnchecked(kSpec, std::monostate(), 10u);
  df.InsertUnchecked(kSpec, std::monostate(), 20u);
  df.InsertUnchecked(kSpec, std::monostate(), 30u);

  TypedCursor cursor(&df, {FilterSpec{1, 0, In{}, {}}}, {});

  // First execution: IN (10, 30)
  using FV = TypedCursor::FilterValue;
  FV values1[] = {int64_t(10), int64_t(30)};
  cursor.SetFilterValueListUnchecked(0, values1, 2);
  cursor.ExecuteUnchecked();
  ASSERT_FALSE(cursor.Eof());
  ASSERT_EQ(cursor.GetCellUnchecked<1>(kSpec), 10u);
  cursor.Next();
  ASSERT_FALSE(cursor.Eof());
  ASSERT_EQ(cursor.GetCellUnchecked<1>(kSpec), 30u);
  cursor.Next();
  ASSERT_TRUE(cursor.Eof());

  // Second execution with different values: IN (20)
  FV values2[] = {int64_t(20)};
  cursor.SetFilterValueListUnchecked(0, values2, 1);
  cursor.ExecuteUnchecked();
  ASSERT_FALSE(cursor.Eof());
  ASSERT_EQ(cursor.GetCellUnchecked<1>(kSpec), 20u);
  cursor.Next();
  ASSERT_TRUE(cursor.Eof());
}

TEST(DataframeTest, TypedCursorInFilterWithIndex) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"id", "track_id"}, CreateTypedColumnSpec(Id(), NonNull(), IdSorted()),
      CreateTypedColumnSpec(Uint32(), NonNull(), Unsorted()));
  StringPool pool;
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &pool);
  // Insert rows with track_ids: 1, 2, 1, 3, 2, 1
  df.InsertUnchecked(kSpec, std::monostate(), 1u);
  df.InsertUnchecked(kSpec, std::monostate(), 2u);
  df.InsertUnchecked(kSpec, std::monostate(), 1u);
  df.InsertUnchecked(kSpec, std::monostate(), 3u);
  df.InsertUnchecked(kSpec, std::monostate(), 2u);
  df.InsertUnchecked(kSpec, std::monostate(), 1u);
  df.Finalize();

  // Build an index on track_id (column 1). Sorted order: 1,1,1,2,2,3
  // Permutation: {0, 2, 5, 1, 4, 3}
  df = df.AddIndex(Index({1}, std::make_shared<std::vector<uint32_t>>(
                                  std::vector<uint32_t>{0, 2, 5, 1, 4, 3})));

  // IN (1, 3) should return rows 0, 2, 5 (track_id=1) and 3 (track_id=3).
  using FV = TypedCursor::FilterValue;
  TypedCursor cursor(&df, {FilterSpec{1, 0, In{}, {}}}, {});
  FV values[] = {int64_t(1), int64_t(3)};
  cursor.SetFilterValueListUnchecked(0, values, 2);
  cursor.ExecuteUnchecked();

  std::vector<uint32_t> result_ids;
  while (!cursor.Eof()) {
    result_ids.push_back(cursor.GetCellUnchecked<0>(kSpec));
    cursor.Next();
  }
  // Results should contain ids for rows 0, 2, 3, 5 (in some order).
  EXPECT_THAT(result_ids, testing::UnorderedElementsAre(0, 2, 3, 5));
}

// Regression test: IN values in reverse order should produce correct results.
TEST(DataframeTest, TypedCursorInFilterWithIndexReverseOrder) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"id", "track_id"}, CreateTypedColumnSpec(Id(), NonNull(), IdSorted()),
      CreateTypedColumnSpec(Uint32(), NonNull(), Unsorted()));
  StringPool pool;
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &pool);
  // Insert rows with track_ids: 1, 2, 1, 3, 2, 1
  df.InsertUnchecked(kSpec, std::monostate(), 1u);
  df.InsertUnchecked(kSpec, std::monostate(), 2u);
  df.InsertUnchecked(kSpec, std::monostate(), 1u);
  df.InsertUnchecked(kSpec, std::monostate(), 3u);
  df.InsertUnchecked(kSpec, std::monostate(), 2u);
  df.InsertUnchecked(kSpec, std::monostate(), 1u);
  df.Finalize();

  df = df.AddIndex(Index({1}, std::make_shared<std::vector<uint32_t>>(
                                  std::vector<uint32_t>{0, 2, 5, 1, 4, 3})));

  // IN (3, 1) — reverse order relative to index sort.
  using FV = TypedCursor::FilterValue;
  TypedCursor cursor(&df, {FilterSpec{1, 0, In{}, {}}}, {});
  FV values[] = {int64_t(3), int64_t(1)};
  cursor.SetFilterValueListUnchecked(0, values, 2);
  cursor.ExecuteUnchecked();

  std::vector<uint32_t> result_ids;
  while (!cursor.Eof()) {
    result_ids.push_back(cursor.GetCellUnchecked<0>(kSpec));
    cursor.Next();
  }
  EXPECT_THAT(result_ids, testing::UnorderedElementsAre(0, 2, 3, 5));
}

// Regression test: executing an IN filter on an indexed column must not corrupt
// the index permutation vector. A subsequent Eq query on the same index should
// still return correct results.
TEST(DataframeTest, TypedCursorInFilterWithIndexDoesNotCorruptIndex) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"id", "track_id"}, CreateTypedColumnSpec(Id(), NonNull(), IdSorted()),
      CreateTypedColumnSpec(Uint32(), NonNull(), Unsorted()));
  StringPool pool;
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &pool);
  // Insert rows with track_ids: 1, 2, 1, 3, 2, 1
  df.InsertUnchecked(kSpec, std::monostate(), 1u);
  df.InsertUnchecked(kSpec, std::monostate(), 2u);
  df.InsertUnchecked(kSpec, std::monostate(), 1u);
  df.InsertUnchecked(kSpec, std::monostate(), 3u);
  df.InsertUnchecked(kSpec, std::monostate(), 2u);
  df.InsertUnchecked(kSpec, std::monostate(), 1u);
  df.Finalize();

  df = df.AddIndex(Index({1}, std::make_shared<std::vector<uint32_t>>(
                                  std::vector<uint32_t>{0, 2, 5, 1, 4, 3})));

  // First: execute an IN filter that uses the index.
  {
    using FV = TypedCursor::FilterValue;
    TypedCursor cursor(&df, {FilterSpec{1, 0, In{}, {}}}, {});
    FV values[] = {int64_t(1), int64_t(3)};
    cursor.SetFilterValueListUnchecked(0, values, 2);
    cursor.ExecuteUnchecked();

    std::vector<uint32_t> result_ids;
    while (!cursor.Eof()) {
      result_ids.push_back(cursor.GetCellUnchecked<0>(kSpec));
      cursor.Next();
    }
    EXPECT_THAT(result_ids, testing::UnorderedElementsAre(0, 2, 3, 5));
  }

  // Second: execute an Eq filter on the same indexed column. If the IN filter
  // corrupted the index permutation vector, this will return wrong results.
  {
    TypedCursor cursor(&df, {FilterSpec{1, 0, Eq{}, {}}}, {});
    cursor.SetFilterValueUnchecked(0, int64_t(2));
    cursor.ExecuteUnchecked();

    std::vector<uint32_t> result_ids;
    while (!cursor.Eof()) {
      result_ids.push_back(cursor.GetCellUnchecked<0>(kSpec));
      cursor.Next();
    }
    EXPECT_THAT(result_ids, testing::UnorderedElementsAre(1, 4));
  }
}

TEST(DataframeTest, TypedCursorNullFiltersAndMultiBatchAndSort) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"id", "track", "val"},
      CreateTypedColumnSpec(Id(), NonNull(), IdSorted()),
      CreateTypedColumnSpec(Uint32(), NonNull(), Unsorted()),
      CreateTypedColumnSpec(Int64(), SparseNull(), Unsorted()));
  StringPool pool;
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &pool);
  constexpr uint32_t kRows = 2500;
  for (uint32_t i = 0; i < kRows; ++i) {
    std::optional<int64_t> val;
    if (i % 3 != 0) {
      val = static_cast<int64_t>(kRows - i);
    }
    df.InsertUnchecked(kSpec, std::monostate(), i % 7, val);
  }
  df.Finalize();
  uint32_t idx_col = 1;
  auto idx_or = df.BuildIndex(&idx_col, &idx_col + 1);
  ASSERT_OK(idx_or.status());
  df = df.AddIndex(std::move(*idx_or));

  // Unfiltered multi-batch scan exercises the contiguous fast path across
  // multiple batches.
  {
    TypedCursor cursor(&df, {}, {});
    cursor.ExecuteUnchecked();
    uint32_t expected = 0;
    for (; !cursor.Eof(); cursor.Next()) {
      ASSERT_EQ(cursor.RowIndex(), expected++);
    }
    EXPECT_EQ(expected, kRows);
  }

  // IsNull / IsNotNull across multiple batches.
  {
    TypedCursor is_null(&df, {FilterSpec{2, 0, IsNull{}, {}}}, {});
    is_null.ExecuteUnchecked();
    uint32_t count = 0;
    for (; !is_null.Eof(); is_null.Next()) {
      EXPECT_EQ(is_null.RowIndex() % 3, 0u);
      ++count;
    }
    EXPECT_EQ(count, (kRows + 2) / 3);

    TypedCursor is_not_null(&df, {FilterSpec{2, 0, IsNotNull{}, {}}}, {});
    is_not_null.ExecuteUnchecked();
    uint32_t non_null_count = 0;
    for (; !is_not_null.Eof(); is_not_null.Next()) {
      EXPECT_NE(is_not_null.RowIndex() % 3, 0u);
      ++non_null_count;
    }
    EXPECT_EQ(non_null_count, kRows - count);
  }

  // Index lookup combined with descending sort on the sorted id column.
  {
    TypedCursor cursor(&df, {FilterSpec{1, 0, Eq{}, {}}},
                       {SortSpec{0, SortDirection::kDescending}});
    cursor.SetFilterValueUnchecked(0, uint32_t{3});
    cursor.ExecuteUnchecked();
    std::vector<uint32_t> rows;
    for (; !cursor.Eof(); cursor.Next()) {
      rows.push_back(cursor.RowIndex());
    }
    ASSERT_FALSE(rows.empty());
    EXPECT_TRUE(std::is_sorted(rows.begin(), rows.end(), std::greater<>()));
    for (uint32_t r : rows) {
      EXPECT_EQ(r % 7, 3u);
    }
  }
}

TEST(DataframeTest,
     QueryPlanEqualityFilterOnNoDuplicatesColumnEstimatesOneRow) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"unique_int_col", "other_col"},
      CreateTypedColumnSpec(Int64(), NonNull(), Unsorted(),
                            NoDuplicates{}),  // Target column with NoDuplicates
      CreateTypedColumnSpec(Int64(), NonNull(), Unsorted(),
                            HasDuplicates{})  // Other column
  );

  StringPool pool;
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &pool);

  // Insert unique, non-null data into the first column
  df.InsertUnchecked(kSpec, int64_t{10}, int64_t{100});
  df.InsertUnchecked(kSpec, int64_t{20}, int64_t{200});
  df.InsertUnchecked(kSpec, int64_t{30}, int64_t{300});
  df.Finalize();

  // Plan a query with an equality filter on the "unique_int_col".
  std::vector<FilterSpec> filters = {{0, 0, Eq{}, int64_t{20}}};
  LimitSpec limit_spec;

  ASSERT_OK_AND_ASSIGN(LogicalPlan plan,
                       df.PlanQueryLogical(filters, {}, {}, limit_spec, 1u));

  // Assert that the estimated_row_count and max_row_count are 1.
  EXPECT_EQ(plan.rows.estimated, 1u);
  EXPECT_EQ(plan.rows.max, 1u);
}

TEST(DataframeTest, SortedFilterWithDuplicatesAndRowCountOfOne) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"sorted_col"},
      CreateTypedColumnSpec(Int64(), NonNull(), Sorted{}, HasDuplicates{}));

  StringPool pool;
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &pool);

  df.InsertUnchecked(kSpec, int64_t{10});
  df.InsertUnchecked(kSpec, int64_t{20});
  df.InsertUnchecked(kSpec, int64_t{20});
  df.Finalize();

  std::vector<FilterSpec> filters = {{0, 0, Eq{}, int64_t{20}}};
  ASSERT_OK_AND_ASSIGN(LogicalPlan plan,
                       df.PlanQueryLogical(filters, {}, {}, {}, 1u));
  EXPECT_EQ(plan.rows.estimated, 1u);
}

TEST(DataframeTest, SortedFilterWithDuplicatesAndRowCountOfZero) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"sorted_col"},
      CreateTypedColumnSpec(Int64(), NonNull(), Sorted{}, HasDuplicates{}));

  StringPool pool;
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &pool);
  df.Finalize();

  std::vector<FilterSpec> filters = {{0, 0, Eq{}, int64_t{20}}};
  ASSERT_OK_AND_ASSIGN(LogicalPlan plan,
                       df.PlanQueryLogical(filters, {}, {}, {}, 1u));
  EXPECT_EQ(plan.rows.estimated, 0u);
}

TEST(DataframeTest, SortOnNullableStringDoesNotShrinkRowEstimate) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"str_a", "str_b"},
      CreateTypedColumnSpec(String(), SparseNull(), Unsorted()),
      CreateTypedColumnSpec(String(), SparseNull(), Unsorted()));

  StringPool pool;
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &pool);
  for (uint32_t i = 0; i < 64; ++i) {
    df.InsertUnchecked(kSpec, std::make_optional(pool.InternString("a")),
                       std::make_optional(pool.InternString("b")));
  }
  df.Finalize();

  // Sorting collects string ranks through a scratch copy which has its nulls
  // pruned. That prunes no result rows, so the estimate must not move.
  std::vector<FilterSpec> filters;
  std::vector<SortSpec> sorts = {{0, SortDirection::kAscending},
                                 {1, SortDirection::kAscending}};
  ASSERT_OK_AND_ASSIGN(LogicalPlan plan,
                       df.PlanQueryLogical(filters, {}, sorts, {}, 0b11));
  EXPECT_EQ(plan.rows.estimated, 64u);
  EXPECT_EQ(plan.rows.max, 64u);
}

TEST(DataframeTest, ScanInFilterUsesInSelectivity) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"unsorted_col"},
      CreateTypedColumnSpec(Uint32(), NonNull(), Unsorted(), HasDuplicates{}));

  StringPool pool;
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &pool);
  for (uint32_t i = 0; i < 1024; ++i) {
    df.InsertUnchecked(kSpec, i % 128);
  }
  df.Finalize();

  // An IN is a union of equalities over the list values, so with 128 distinct
  // values it keeps 25 assumed values x 8 rows each. Without that model it
  // would get the plain halving a generic inequality gets, i.e. 512.
  std::vector<FilterSpec> filters = {{0, 0, In{}, std::nullopt}};
  ASSERT_OK_AND_ASSIGN(LogicalPlan plan,
                       df.PlanQueryLogical(filters, {}, {}, {}, 1u));
  EXPECT_EQ(plan.rows.estimated, 200u);
}

// Diff-based tests for the logical plan: what the planner decided, before any
// backend lowers it.
class LogicalPlanTest : public ::testing::Test {
 protected:
  void RunLogicalPlanTest(Dataframe& df,
                          std::vector<FilterSpec>& filters,
                          const std::vector<DistinctSpec>& distinct_specs,
                          const std::vector<SortSpec>& sort_specs,
                          LimitSpec limit_spec,
                          const std::string& expected,
                          uint64_t cols_used = 0xFFFFFFFF) {
    ASSERT_OK_AND_ASSIGN(
        LogicalPlan plan,
        df.PlanQueryLogical(filters, distinct_specs, sort_specs, limit_spec,
                            cols_used));
    EXPECT_THAT(plan.ToString(), EqualsIgnoringWhitespace(expected));
  }

  StringPool string_pool_;
};

TEST_F(LogicalPlanTest, SortedColumnUsesBinarySearch) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"sorted"},
      CreateTypedColumnSpec(Int64(), NonNull(), Sorted{}, HasDuplicates{}));
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &string_pool_);
  for (uint32_t i = 0; i < 1024; ++i) {
    df.InsertUnchecked(kSpec, int64_t{i});
  }
  df.Finalize();

  std::vector<FilterSpec> filters = {{0, 0, Ge{}, std::nullopt}};
  RunLogicalPlanTest(df, filters, {}, {}, {}, R"(
    Scan[rows=1024/1024]
    Filter[col=0 op=Ge value=0 storage=Int64 null=NonNull strategy=BinarySearch rows=1024/1024->512/1024]
    Output[cols_used=0xffffffff]
  )");
}

TEST_F(LogicalPlanTest, NullableColumnScanRecordsNullPrune) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"maybe_str"}, CreateTypedColumnSpec(String(), SparseNull(), Unsorted()));
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &string_pool_);
  for (uint32_t i = 0; i < 64; ++i) {
    df.InsertUnchecked(kSpec,
                       std::make_optional(string_pool_.InternString("a")));
  }
  df.Finalize();

  // A Glob cannot be answered by anything but a scan, and the column is
  // nullable, so the nulls come out before the pattern is applied.
  std::vector<FilterSpec> filters = {{0, 0, Glob{}, std::nullopt}};
  RunLogicalPlanTest(df, filters, {}, {}, {}, R"(
    Scan[rows=64/64]
    Filter[col=0 op=Glob value=0 storage=String null=SparseNull strategy=IndexListScan rows=64/64->16/64 after_null_prune=32]
    Output[cols_used=0xffffffff]
  )");
}

TEST_F(LogicalPlanTest, SortOnAlreadySortedColumnIsNotPlanned) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"sorted"},
      CreateTypedColumnSpec(Int64(), NonNull(), Sorted{}, HasDuplicates{}));
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &string_pool_);
  df.InsertUnchecked(kSpec, int64_t{1});
  df.InsertUnchecked(kSpec, int64_t{2});
  df.Finalize();

  std::vector<FilterSpec> filters;
  std::vector<SortSpec> sorts = {{0, SortDirection::kAscending}};
  RunLogicalPlanTest(df, filters, {}, sorts, {}, R"(
    Scan[rows=2/2]
    Output[cols_used=0xffffffff]
  )");

  // The opposite direction only needs the rows turned round.
  std::vector<SortSpec> desc = {{0, SortDirection::kDescending}};
  RunLogicalPlanTest(df, filters, {}, desc, {}, R"(
    Scan[rows=2/2]
    Reverse[]
    Output[cols_used=0xffffffff]
  )");
}

TEST_F(LogicalPlanTest, OrderByWithLimitOfOneBecomesMinMax) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"vals"},
      CreateTypedColumnSpec(Int64(), NonNull(), Unsorted(), HasDuplicates{}));
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &string_pool_);
  for (uint32_t i = 0; i < 100; ++i) {
    df.InsertUnchecked(kSpec, int64_t{i});
  }
  df.Finalize();

  std::vector<FilterSpec> filters;
  std::vector<SortSpec> sorts = {{0, SortDirection::kDescending}};
  RunLogicalPlanTest(df, filters, {}, sorts, LimitSpec{1, std::nullopt}, R"(
    Scan[rows=100/100]
    MinMax[col=0 dir=desc rows=100/100->1/1]
    Output[cols_used=0xffffffff]
  )");
}

TEST_F(LogicalPlanTest, ImpossibleFilterPlansNoWork) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"nums"},
      CreateTypedColumnSpec(Int64(), NonNull(), Unsorted(), HasDuplicates{}));
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &string_pool_);
  df.InsertUnchecked(kSpec, int64_t{1});
  df.Finalize();

  // Glob cannot apply to an integer column, so nothing can match. No filter
  // value slot is claimed: the caller keeps responsibility for the constraint.
  std::vector<FilterSpec> filters = {{0, 0, Glob{}, std::nullopt}};
  RunLogicalPlanTest(df, filters, {}, {}, {}, R"(
    Scan[rows=1/1]
    Empty[]
    Output[cols_used=0xffffffff]
  )");
  EXPECT_EQ(filters[0].value_index, std::nullopt);
}

TEST_F(LogicalPlanTest, IndexServesEqualityFilters) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"a", "b"},
      CreateTypedColumnSpec(Uint32(), NonNull(), Unsorted(), HasDuplicates{}),
      CreateTypedColumnSpec(Uint32(), NonNull(), Unsorted(), HasDuplicates{}));
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &string_pool_);
  for (uint32_t i = 0; i < 128; ++i) {
    df.InsertUnchecked(kSpec, i % 8, i % 4);
  }
  df.Finalize();
  std::vector<uint32_t> perm(128);
  std::iota(perm.begin(), perm.end(), 0);
  df = df.AddIndex(
      Index({0}, std::make_shared<std::vector<uint32_t>>(std::move(perm))));

  std::vector<FilterSpec> filters = {{0, 0, Eq{}, std::nullopt}};
  RunLogicalPlanTest(df, filters, {}, {}, {}, R"(
    Scan[rows=128/128]
    IndexFilter[index=0 (col=0 op=Eq value=0 storage=Uint32 null=NonNull rows=128/128->16/128)]
    Output[cols_used=0x3]
  )",
                     /*cols_used=*/0b11);
}

}  // namespace perfetto::trace_processor::core::dataframe
