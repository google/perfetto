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

#include "src/trace_processor/core/dataframe/adhoc_dataframe_builder.h"

#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/ext/base/status_or.h"
#include "src/base/test/status_matchers.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::dataframe {

inline void PrintTo(const ColumnSpec& spec, std::ostream* os) {
  *os << "\n  ColumnSpec{\n"
      << "    type: " << spec.type.ToString() << ",\n"
      << "    nullability: " << spec.nullability.ToString() << ",\n"
      << "    sort_state: " << spec.sort_state.ToString() << ",\n"
      << "    duplicate_state: " << spec.duplicate_state.ToString() << "\n"
      << "  }";
}

inline bool operator==(const ColumnSpec& lhs, const ColumnSpec& rhs) {
  return lhs.type == rhs.type && lhs.nullability == rhs.nullability &&
         lhs.sort_state == rhs.sort_state &&
         lhs.duplicate_state == rhs.duplicate_state;
}

namespace {

using testing::ElementsAre;

class AdhocDataframeBuilderTest : public ::testing::Test {
 protected:
  StringPool pool_;
};

TEST_F(AdhocDataframeBuilderTest, StringColumnWithNullId) {
  AdhocDataframeBuilder builder({"str_col"}, &pool_);

  builder.PushNonNull(0, pool_.InternString("hello"));
  builder.PushNonNull(0, StringPool::Id::Null());
  builder.PushNonNull(0, pool_.InternString("world"));

  base::StatusOr<Dataframe> df_status = std::move(builder).Build();
  ASSERT_OK(df_status.status());
  Dataframe df = std::move(df_status.value());

  auto spec = df.CreateSpec();
  ASSERT_THAT(spec.column_names, ElementsAre("str_col", "_auto_id"));
  ASSERT_THAT(
      spec.column_specs,
      ElementsAre(
          ColumnSpec{String{}, SparseNull{}, Unsorted{}, HasDuplicates{}},
          ColumnSpec{Id{}, NonNull{}, IdSorted{}, NoDuplicates{}}));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::dataframe
