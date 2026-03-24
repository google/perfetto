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

#include <cstdint>
#include <memory>
#include <optional>

#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/tables_py.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/slice_tables_py.h"
#include "src/trace_processor/tables/track_tables_py.h"
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

// Regression test: a child instant slice at the exact boundary between its
// parent and a subsequent sibling ("uncle") slice should not appear as a
// descendant of the uncle.
TEST(Descendant, BoundaryInstantSliceNotDescendantOfUncle) {
  TraceStorage storage;
  auto* slices = storage.mutable_slice_table();

  // Parent slice: ts=100, dur=101 (ts_upper_bound=201), depth=0.
  // The child at ts=200 is strictly inside the parent's half-open interval
  // [100, 201).
  tables::SliceTable::Row parent_row;
  parent_row.ts = 100;
  parent_row.dur = 101;
  parent_row.depth = 0;
  parent_row.track_id = tables::TrackTable::Id{1};
  parent_row.parent_id = std::nullopt;
  auto parent_id = slices->Insert(parent_row).id;

  // Child instant slice at the boundary: ts=200, dur=0, depth=1,
  // parent_id=parent.
  tables::SliceTable::Row child_row;
  child_row.ts = 200;
  child_row.dur = 0;
  child_row.depth = 1;
  child_row.track_id = tables::TrackTable::Id{1};
  child_row.parent_id = parent_id;
  auto child_id = slices->Insert(child_row).id;

  // Uncle slice starting at the same boundary: ts=200, dur=100, depth=0.
  tables::SliceTable::Row uncle_row;
  uncle_row.ts = 200;
  uncle_row.dur = 100;
  uncle_row.depth = 0;
  uncle_row.track_id = tables::TrackTable::Id{1};
  uncle_row.parent_id = std::nullopt;
  auto uncle_id = slices->Insert(uncle_row).id;

  Descendant generator{Descendant::Type::kSlice, &storage};

  // descendant_slice(parent) should include the child.
  {
    auto cursor = generator.MakeCursor();
    bool res = cursor->Run({SqlValue::Long(parent_id.value)});
    ASSERT_TRUE(res);
    const auto* df = cursor->dataframe();
    bool found_child = false;
    for (uint32_t i = 0; i < df->row_count(); ++i) {
      auto id = df->GetCellUnchecked<tables::SliceSubsetTable::ColumnIndex::id>(
          tables::SliceSubsetTable::kSpec, i);
      if (SliceId(id) == child_id) {
        found_child = true;
      }
    }
    EXPECT_TRUE(found_child) << "Child slice should be a descendant of parent";
  }

  // descendant_slice(uncle) should NOT include the child.
  {
    auto cursor = generator.MakeCursor();
    bool res = cursor->Run({SqlValue::Long(uncle_id.value)});
    ASSERT_TRUE(res);
    const auto* df = cursor->dataframe();
    bool found_child = false;
    for (uint32_t i = 0; i < df->row_count(); ++i) {
      auto id = df->GetCellUnchecked<tables::SliceSubsetTable::ColumnIndex::id>(
          tables::SliceSubsetTable::kSpec, i);
      if (SliceId(id) == child_id) {
        found_child = true;
      }
    }
    EXPECT_FALSE(found_child)
        << "Child slice should NOT be a descendant of uncle";
  }
}

}  // namespace
}  // namespace perfetto::trace_processor
