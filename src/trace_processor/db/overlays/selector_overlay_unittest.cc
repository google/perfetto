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

#include "src/trace_processor/db/overlays/selector_overlay.h"
#include "src/trace_processor/db/overlays/types.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace overlays {
namespace {

TEST(SelectorOverlay, MapToStorageRangeFirst) {
  BitVector selector{0, 0, 0, 1, 1, 0, 1, 1, 0, 0, 1};
  SelectorOverlay overlay(&selector);
  StorageRange r = overlay.MapToStorageRange(TableRange(1, 4));

  ASSERT_EQ(r.range.start, 4u);
  ASSERT_EQ(r.range.end, 8u);
}

TEST(SelectorOverlay, MapToStorageRangeSecond) {
  BitVector selector{0, 0, 0, 1, 1, 0, 1, 1, 0, 1, 0};
  SelectorOverlay overlay(&selector);
  StorageRange r = overlay.MapToStorageRange(TableRange(1, 3));

  ASSERT_EQ(r.range.start, 4u);
  ASSERT_EQ(r.range.end, 7u);
}

TEST(SelectorOverlay, MapToTableRangeFirst) {
  BitVector selector{0, 1, 0, 1, 1, 0, 1, 1, 0, 0, 1};
  SelectorOverlay overlay(&selector);
  auto r =
      overlay.MapToTableRangeOrBitVector(StorageRange(2, 5), OverlayOp::kOther);

  Range range = std::move(r).TakeIfRange();
  ASSERT_EQ(range.start, 1u);
  ASSERT_EQ(range.end, 3u);
}

TEST(SelectorOverlay, MapToTableRangeSecond) {
  BitVector selector{0, 1, 0, 1, 1, 0, 1, 1, 0, 1, 0};
  SelectorOverlay overlay(&selector);
  auto r = overlay.MapToTableRangeOrBitVector(StorageRange(0, 10),
                                              OverlayOp::kOther);

  Range range = std::move(r).TakeIfRange();
  ASSERT_EQ(range.start, 0u);
  ASSERT_EQ(range.end, 6u);
}

TEST(SelectorOverlay, MapToTableBitVector) {
  BitVector selector{0, 1, 1, 0, 0, 1, 1, 0};
  SelectorOverlay overlay(&selector);

  BitVector storage_bv{1, 0, 1, 0, 1, 0, 1, 0};
  TableBitVector table_bv =
      overlay.MapToTableBitVector({std::move(storage_bv)}, OverlayOp::kOther);

  ASSERT_EQ(table_bv.bv.size(), 4u);
  ASSERT_EQ(table_bv.bv.CountSetBits(), 2u);
  ASSERT_TRUE(table_bv.bv.IsSet(1));
  ASSERT_TRUE(table_bv.bv.IsSet(3));
}

TEST(SelectorOverlay, IsStorageLookupRequired) {
  BitVector selector{0, 1, 1, 0, 0, 1, 1, 0};
  SelectorOverlay overlay(&selector);

  std::vector<uint32_t> table_idx{0, 1, 2};
  BitVector lookup_bv =
      overlay.IsStorageLookupRequired(OverlayOp::kIsNull, {table_idx});

  ASSERT_EQ(lookup_bv.size(), 3u);
}

TEST(SelectorOverlay, MapToStorageIndexVector) {
  BitVector selector{0, 1, 1, 0, 0, 1, 1, 0};
  SelectorOverlay overlay(&selector);

  std::vector<uint32_t> table_idx{1, 3, 2};
  StorageIndexVector storage_iv = overlay.MapToStorageIndexVector({table_idx});

  std::vector<uint32_t> res{2, 6, 5};
  ASSERT_EQ(storage_iv.indices, res);
}

}  // namespace
}  // namespace overlays
}  // namespace trace_processor
}  // namespace perfetto
