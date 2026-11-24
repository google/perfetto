/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "perfetto/trace_processor/trace_blob.h"

#include <cstdint>
#include <utility>
#include <vector>

#include "perfetto/trace_processor/trace_blob_view.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

TEST(TraceBlob, MoveAssignment) {
  TraceBlob b1 = TraceBlob::Allocate(16);
  TraceBlob b2 = TraceBlob::Allocate(16);

  b1 = std::move(b2);
}

TEST(TraceBlob, MoveLeavesSourceEmpty) {
  TraceBlob b1 = TraceBlob::Allocate(16);
  const uint8_t* data = b1.data();

  TraceBlob b2(std::move(b1));
  EXPECT_EQ(b2.data(), data);
  EXPECT_EQ(b2.size(), 16u);
  EXPECT_EQ(b1.data(), nullptr);
  EXPECT_EQ(b1.size(), 0u);
}

struct FlaggedOwner {
  explicit FlaggedOwner(bool* destroyed) : destroyed_(destroyed) {}
  ~FlaggedOwner() {
    if (destroyed_)
      *destroyed_ = true;
  }
  FlaggedOwner(FlaggedOwner&& o) noexcept
      : destroyed_(std::exchange(o.destroyed_, nullptr)) {}
  FlaggedOwner& operator=(FlaggedOwner&&) = delete;
  FlaggedOwner(const FlaggedOwner&) = delete;
  FlaggedOwner& operator=(const FlaggedOwner&) = delete;

  std::vector<uint8_t> bytes = std::vector<uint8_t>(32, 0xAB);
  bool* destroyed_;
};

TEST(TraceBlob, AdoptDestroysOwnerAfterLastView) {
  bool destroyed = false;
  FlaggedOwner owner(&destroyed);
  uint8_t* data = owner.bytes.data();

  TraceBlobView view(TraceBlob::Adopt(data, 32, std::move(owner)));
  EXPECT_FALSE(destroyed);

  TraceBlobView slice = view.slice_off(8, 8);
  view = TraceBlobView();
  EXPECT_FALSE(destroyed);
  EXPECT_EQ(slice.data()[0], 0xAB);

  slice = TraceBlobView();
  EXPECT_TRUE(destroyed);
}

TEST(TraceBlob, AdoptWithDeleterAndContext) {
  static int calls = 0;
  calls = 0;
  uint8_t bytes[4] = {};
  {
    TraceBlob blob =
        TraceBlob::Adopt(bytes, sizeof(bytes), &calls,
                         [](void* ctx) { ++*static_cast<int*>(ctx); });
    EXPECT_EQ(blob.data(), bytes);
    EXPECT_EQ(calls, 0);
  }
  EXPECT_EQ(calls, 1);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
