/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/descendant.h"

#include <memory>

#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

TEST(Descendant, SliceTableNullConstraint) {
  // Insert a row to make sure that we are not returning an empty table just
  // because the source is empty.
  TraceStorage storage;
  storage.mutable_slice_table()->Insert({});

  Descendant generator{Descendant::Type::kSlice, &storage};
  auto cursor = generator.MakeCursor();

  // Check that if we pass start_id = NULL as a constraint, we correctly return
  // an empty table.
  bool res = cursor->Run({SqlValue()});
  ASSERT_TRUE(res);
  ASSERT_EQ(cursor->dataframe()->row_count(), 0u);
}

TEST(Descendant, SliceByStackTableNullConstraint) {
  // Insert a row to make sure that we are not returning an empty table just
  // because the source is empty.
  TraceStorage storage;
  storage.mutable_slice_table()->Insert({});

  Descendant generator{Descendant::Type::kSliceByStack, &storage};
  auto cursor = generator.MakeCursor();

  // Check that if we pass start_id = NULL as a constraint, we correctly return
  // an empty table.
  bool res = cursor->Run({SqlValue()});
  ASSERT_TRUE(res);
  ASSERT_EQ(cursor->dataframe()->row_count(), 0u);
}

}  // namespace
}  // namespace perfetto::trace_processor
