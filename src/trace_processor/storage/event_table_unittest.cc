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

#include <cstdint>
#include <memory>

#include "src/trace_processor/core/dataframe/event_table_backend.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/slice_tables_py.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

// Inserts enough slices to seal the first rows of the slice table.
void InsertSlices(TraceStorage& storage, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    storage.mutable_slice_table()->Insert(
        {/*ts=*/i, /*dur=*/-1, TrackId(0), kNullStringId, kNullStringId});
  }
}

int64_t Dur(const TraceStorage& storage, uint32_t row) {
  auto slice = storage.slice_table().CreateIdCursor();
  slice.SeekToId(SliceId{row});
  return slice.dur();
}

TEST(EventTableTest, WritesToSealedRowsWaitForFlush) {
  TraceStorage storage;
  storage.SetEventTableBackend(
      std::make_unique<dataframe::BufferingEventTableBackend>());
  constexpr uint32_t kRows = 2 * dataframe::kEventTableTailRows;
  InsertSlices(storage, kRows);

  // Row 0 is sealed, the last row is still in the tail.
  (*storage.mutable_slice_table())[0u].set_dur(10);
  (*storage.mutable_slice_table())[kRows - 1].set_dur(20);
  EXPECT_EQ(Dur(storage, 0), -1);
  EXPECT_EQ(Dur(storage, kRows - 1), 20);

  storage.FlushEventTables();
  EXPECT_EQ(Dur(storage, 0), 10);
}

TEST(EventTableTest, InMemoryBackendWritesInPlace) {
  TraceStorage storage;
  constexpr uint32_t kRows = 2 * dataframe::kEventTableTailRows;
  InsertSlices(storage, kRows);

  (*storage.mutable_slice_table())[0u].set_dur(10);
  EXPECT_EQ(Dur(storage, 0), 10);
}

}  // namespace
}  // namespace perfetto::trace_processor
