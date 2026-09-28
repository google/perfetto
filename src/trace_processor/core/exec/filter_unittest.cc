/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/trace_processor/core/exec/filter.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using testing::ElementsAre;

// A batch of five rows: `n` holds 10, 20, null, 40, 50 and `s` holds "b",
// "a", "c", "a", "d".
class FilterTest : public ::testing::Test {
 protected:
  FilterTest() {
    for (uint32_t i = 0; i < 5; ++i) {
      if (i != 2) {
        validity_.set(i);
      }
    }
    for (const char* s : {"b", "a", "c", "a", "d"}) {
      strings_.push_back(pool_.InternString(s));
    }
    batch_.AddColumn(
        ColumnView::Reference(StorageType{Int64{}}, numbers_, &validity_));
    batch_.AddColumn(
        ColumnView::Reference(StorageType{String{}}, strings_.data(), nullptr));
    batch_.SetCardinality(5);
  }

  // The values of `n` in the rows every condition keeps.
  std::vector<int64_t> Kept(std::vector<Filter::Condition> conditions) {
    Filter filter(std::move(conditions), &pool_);
    std::unique_ptr<OperatorState> state = filter.MakeState();
    RowBatch out;
    EXPECT_EQ(filter.Execute(batch_, out, *state), OpResult::kNeedMoreInput);
    return test::ReadColumn<int64_t>(out, 0);
  }

  static Filter::Condition On(uint32_t column,
                              Op op,
                              std::vector<Filter::Value> values) {
    return {column, op, std::move(values)};
  }

  StringPool pool_;
  int64_t numbers_[5] = {10, 20, 0, 40, 50};
  BitVector validity_ = BitVector::CreateWithSize(5);
  std::vector<StringPool::Id> strings_;
  RowBatch batch_;
};

// As in SQL, no comparison is true of a null: only IS NULL keeps one.
TEST_F(FilterTest, KeepsTheRowsEveryConditionHoldsFor) {
  EXPECT_THAT(Kept({On(0, Eq{}, {int64_t{20}})}), ElementsAre(20));
  EXPECT_THAT(Kept({On(0, Ne{}, {int64_t{20}})}), ElementsAre(10, 40, 50));
  EXPECT_THAT(Kept({On(0, Lt{}, {int64_t{40}})}), ElementsAre(10, 20));
  EXPECT_THAT(Kept({On(0, Le{}, {int64_t{40}})}), ElementsAre(10, 20, 40));
  EXPECT_THAT(Kept({On(0, Gt{}, {int64_t{20}})}), ElementsAre(40, 50));
  EXPECT_THAT(Kept({On(0, Ge{}, {int64_t{20}})}), ElementsAre(20, 40, 50));
  EXPECT_THAT(Kept({On(0, In{}, {int64_t{50}, int64_t{10}, int64_t{30}})}),
              ElementsAre(10, 50));
  EXPECT_THAT(Kept({On(0, IsNotNull{}, {})}), ElementsAre(10, 20, 40, 50));
  EXPECT_EQ(Kept({On(0, IsNull{}, {})}).size(), 1u);

  // Strings compare by their bytes.
  EXPECT_THAT(Kept({On(1, Eq{}, {std::string("a")})}), ElementsAre(20, 40));
  EXPECT_THAT(Kept({On(1, Gt{}, {std::string("b")})}), ElementsAre(0, 50));
  EXPECT_THAT(Kept({On(1, In{}, {std::string("d"), std::string("b")})}),
              ElementsAre(10, 50));
  // A string no row holds matches nothing, rather than failing.
  EXPECT_THAT(Kept({On(1, Eq{}, {std::string("z")})}), ElementsAre());

  // Every condition must hold.
  EXPECT_THAT(
      Kept({On(1, Eq{}, {std::string("a")}), On(0, Gt{}, {int64_t{20}})}),
      ElementsAre(40));
}

// Values of another type convert as a dataframe converts them, since both
// use the same casts: exactly, as SQL compares values.
TEST_F(FilterTest, ConvertsValuesAsADataframeDoes) {
  EXPECT_THAT(Kept({On(0, Lt{}, {25.5})}), ElementsAre(10, 20));
  EXPECT_THAT(Kept({On(0, Ge{}, {20.0})}), ElementsAre(20, 40, 50));
  EXPECT_THAT(Kept({On(0, Eq{}, {20.5})}), ElementsAre());
  EXPECT_THAT(Kept({On(0, Ne{}, {20.5})}), ElementsAre(10, 20, 40, 50));
  // Every number sorts before every string.
  EXPECT_THAT(Kept({On(0, Lt{}, {std::string("a")})}),
              ElementsAre(10, 20, 40, 50));
  EXPECT_THAT(Kept({On(0, Eq{}, {std::string("20")})}), ElementsAre());
  EXPECT_THAT(Kept({On(1, Gt{}, {int64_t{1}})}),
              ElementsAre(10, 20, 0, 40, 50));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
