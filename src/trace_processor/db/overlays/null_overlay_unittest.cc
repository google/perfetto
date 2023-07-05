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

#include "src/trace_processor/db/overlays/null_overlay.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace overlays {
namespace {

TEST(NullOverlay, MapToStorageRangeOutsideBoundary) {
  BitVector bv{0, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0};
  NullOverlay overlay(&bv);
  StorageRange r = overlay.MapToStorageRange(TableRange(1, 6));

  ASSERT_EQ(r.range.start, 0u);
  ASSERT_EQ(r.range.end, 2u);
}

TEST(NullOverlay, MapToStorageRangeOnBoundary) {
  BitVector bv{0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 0};
  NullOverlay overlay(&bv);
  StorageRange r = overlay.MapToStorageRange(TableRange(3, 8));

  ASSERT_EQ(r.range.start, 1u);
  ASSERT_EQ(r.range.end, 4u);
}

TEST(NullOverlay, MapToTableRangeOutsideBoundary) {
  BitVector bv{0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 0};
  NullOverlay overlay(&bv);
  auto r =
      overlay.MapToTableRangeOrBitVector(StorageRange(1, 3), OverlayOp::kOther);

  // All set bits between |bv| index 3 and 6.
  ASSERT_EQ(std::move(r).TakeIfBitVector().CountSetBits(), 2u);
}

TEST(NullOverlay, MapToTableRangeOnBoundary) {
  BitVector bv{0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 0};
  NullOverlay overlay(&bv);
  auto r =
      overlay.MapToTableRangeOrBitVector(StorageRange(0, 5), OverlayOp::kOther);

  ASSERT_EQ(std::move(r).TakeIfBitVector().CountSetBits(), 5u);
}

TEST(NullOverlay, MapToTableBitVector) {
  BitVector bv{0, 1, 1, 0, 0, 1, 1, 0};
  NullOverlay overlay(&bv);

  BitVector storage_bv{0, 1, 0, 1};
  TableBitVector table_bv =
      overlay.MapToTableBitVector({std::move(storage_bv)}, OverlayOp::kOther);

  ASSERT_EQ(table_bv.bv.CountSetBits(), 2u);
  ASSERT_TRUE(table_bv.bv.IsSet(2));
  ASSERT_TRUE(table_bv.bv.IsSet(6));
}

TEST(NullOverlay, MapToTableBitVectorIsNull) {
  BitVector bv{0, 1, 1, 0, 0, 1, 1, 0};
  NullOverlay overlay(&bv);

  BitVector storage_bv{0, 1, 0, 1};
  TableBitVector table_bv =
      overlay.MapToTableBitVector({std::move(storage_bv)}, OverlayOp::kIsNull);

  // Result is all of the zeroes from |bv| and set bits from |storage_bv|
  // 1, 0, 1, 1, 1, 0, 1, 1

  ASSERT_EQ(table_bv.bv.CountSetBits(), 6u);
  ASSERT_FALSE(table_bv.bv.IsSet(1));
  ASSERT_FALSE(table_bv.bv.IsSet(5));
}

TEST(NullOverlay, IsStorageLookupRequiredNullOp) {
  BitVector bv{0, 1, 1, 0, 0, 1, 1, 0};
  NullOverlay overlay(&bv);

  std::vector<uint32_t> table_idx{0, 2, 4, 6};
  BitVector lookup_bv =
      overlay.IsStorageLookupRequired(OverlayOp::kIsNull, {table_idx});

  ASSERT_EQ(lookup_bv.CountSetBits(), 0u);
}

TEST(NullOverlay, IsStorageLookupRequiredOtherOp) {
  BitVector bv{0, 1, 1, 0, 0, 1, 1, 0};
  NullOverlay overlay(&bv);

  std::vector<uint32_t> table_idx{0, 2, 4, 6};
  BitVector lookup_bv =
      overlay.IsStorageLookupRequired(OverlayOp::kOther, {table_idx});

  ASSERT_EQ(lookup_bv.size(), 4u);
  ASSERT_EQ(lookup_bv.CountSetBits(), 2u);
  ASSERT_TRUE(lookup_bv.IsSet(1));
  ASSERT_TRUE(lookup_bv.IsSet(3));
}

TEST(NullOverlay, MapToStorageIndexVector) {
  BitVector bv{0, 1, 1, 0, 0, 1, 1, 0};
  NullOverlay overlay(&bv);

  std::vector<uint32_t> table_idx{1, 5, 2};
  StorageIndexVector storage_iv = overlay.MapToStorageIndexVector({table_idx});

  std::vector<uint32_t> res{0, 2, 1};
  ASSERT_EQ(storage_iv.indices, res);
}

TEST(NullOverlay, IndexSearchOtherOp) {
  BitVector bv{0, 1, 1, 0, 0, 1, 1, 0};
  NullOverlay overlay(&bv);

  std::vector<uint32_t> table_idx{0, 3, 4};
  BitVector idx_search_bv = overlay.IndexSearch(OverlayOp::kOther, {table_idx});

  ASSERT_EQ(idx_search_bv.CountSetBits(), 0u);
}

TEST(NullOverlay, IndexSearchIsNullOp) {
  BitVector bv{0, 1, 1, 0, 0, 1, 1, 0};
  NullOverlay overlay(&bv);

  std::vector<uint32_t> table_idx{0, 3, 4};
  BitVector idx_search_bv =
      overlay.IndexSearch(OverlayOp::kIsNull, {table_idx});

  ASSERT_EQ(idx_search_bv.size(), 3u);
  ASSERT_EQ(idx_search_bv.CountSetBits(), 3u);
}

TEST(NullOverlay, IndexSearchIsNotNullOp) {
  BitVector bv{0, 1, 1, 0, 0, 1, 1, 0};
  NullOverlay overlay(&bv);

  std::vector<uint32_t> table_idx{0, 3, 4};
  BitVector idx_search_bv =
      overlay.IndexSearch(OverlayOp::kIsNotNull, {table_idx});

  ASSERT_EQ(idx_search_bv.size(), 3u);
  ASSERT_EQ(idx_search_bv.CountSetBits(), 0u);
}

}  // namespace
}  // namespace overlays
}  // namespace trace_processor
}  // namespace perfetto
