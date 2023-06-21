/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "src/trace_processor/db/overlays/arrangement_overlay.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace overlays {
namespace {

TEST(ArrangementOverlay, MapToStorageRangeFirst) {
  std::vector<uint32_t> arrangement{1, 1, 2, 2, 3, 3, 4, 4, 1, 1};
  ArrangementOverlay overlay(&arrangement);
  StorageRange r = overlay.MapToStorageRange(TableRange(2, 4));

  ASSERT_EQ(r.range.start, 2u);
  ASSERT_EQ(r.range.end, 3u);
}

TEST(ArrangementOverlay, MapToStorageRangeSecond) {
  std::vector<uint32_t> arrangement{1, 1, 2, 2, 3, 3, 4, 4, 1, 1};
  ArrangementOverlay overlay(&arrangement);
  StorageRange r = overlay.MapToStorageRange(TableRange(5, 10));

  ASSERT_EQ(r.range.start, 1u);
  ASSERT_EQ(r.range.end, 5u);
}

TEST(ArrangementOverlay, MapToTableBitVector) {
  std::vector<uint32_t> arrangement{1, 1, 2, 2, 3, 3, 4, 4, 1, 1};
  ArrangementOverlay overlay(&arrangement);

  BitVector storage_bv{0, 1, 0, 1, 0};

  // Table bv:
  // 1, 1, 0, 0, 1, 1, 0, 0, 1, 1
  TableBitVector table_bv =
      overlay.MapToTableBitVector({std::move(storage_bv)}, OverlayOp::kOther);

  ASSERT_EQ(table_bv.bv.size(), 10u);
  ASSERT_EQ(table_bv.bv.CountSetBits(), 6u);

  ASSERT_TRUE(table_bv.bv.IsSet(0));
  ASSERT_TRUE(table_bv.bv.IsSet(1));
  ASSERT_TRUE(table_bv.bv.IsSet(4));
  ASSERT_TRUE(table_bv.bv.IsSet(5));
  ASSERT_TRUE(table_bv.bv.IsSet(8));
  ASSERT_TRUE(table_bv.bv.IsSet(9));
}

TEST(ArrangementOverlay, IsStorageLookupRequired) {
  std::vector<uint32_t> arrangement{0, 1, 1, 0, 0, 1, 1, 0};
  ArrangementOverlay overlay(&arrangement);

  std::vector<uint32_t> table_idx{0, 1, 2};
  BitVector lookup_bv =
      overlay.IsStorageLookupRequired(OverlayOp::kIsNull, {table_idx});

  ASSERT_EQ(lookup_bv.size(), 3u);
}

TEST(ArrangementOverlay, MapToStorageIndexVector) {
  std::vector<uint32_t> arrangement{1, 1, 2, 2, 3, 3, 4, 4, 1, 1};
  ArrangementOverlay overlay(&arrangement);

  std::vector<uint32_t> table_idx{1, 3, 7};
  StorageIndexVector storage_iv = overlay.MapToStorageIndexVector({table_idx});

  std::vector<uint32_t> res{1, 2, 4};
  ASSERT_EQ(storage_iv.indices, res);
}

}  // namespace
}  // namespace overlays
}  // namespace trace_processor
}  // namespace perfetto
