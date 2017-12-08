/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "src/tracing/core/chunked_protobuf_input_stream.h"

#include "gtest/gtest.h"
#include "perfetto/base/utils.h"

namespace perfetto {
namespace {

// The tests below work on chunks, that are a (start pointer, size) tuple but
// never dereference the memory in the pointer. Hence, we just use an array of
// integers that is used both to derive N distinct pointers and to keep track
// of N distinct sizes. In other words, the tests below will see Chunks of the
// form {start: &kBuf[0], end: &kBuf[0] + kBuf[0]}, and so on. As long as we
// don't dereference those pointers, this int array should be enough.
const int kBufs[]{100, 200, 1024, 0, 10, 0, 1, 1, 7};

TEST(ChunkedProtobufInputStreamTest, SingleChunk) {
  ChunkSequence seq;
  seq.emplace_back(&kBufs[0], kBufs[0]);
  ChunkedProtobufInputStream istr(&seq);

  const void* ptr = nullptr;
  int size = 0;
  ASSERT_TRUE(istr.Next(&ptr, &size));
  ASSERT_EQ(&kBufs[0], ptr);
  ASSERT_EQ(kBufs[0], size);
  ASSERT_EQ(kBufs[0], istr.ByteCount());
  ASSERT_FALSE(istr.Next(&ptr, &size));

  // Backup and read again.
  istr.BackUp(10);
  ASSERT_EQ(kBufs[0] - 10, istr.ByteCount());
  ASSERT_TRUE(istr.Next(&ptr, &size));
  ASSERT_EQ(reinterpret_cast<const void*>(
                reinterpret_cast<uintptr_t>(&kBufs[0]) + kBufs[0] - 10),
            ptr);
  ASSERT_EQ(10, size);
  ASSERT_EQ(kBufs[0], istr.ByteCount());
  ASSERT_FALSE(istr.Next(&ptr, &size));

  // Backup, skip and read again.
  istr.BackUp(50);
  ASSERT_EQ(kBufs[0] - 50, istr.ByteCount());
  ASSERT_TRUE(istr.Skip(10));
  ASSERT_TRUE(istr.Next(&ptr, &size));
  ASSERT_EQ(reinterpret_cast<const void*>(
                reinterpret_cast<uintptr_t>(&kBufs[0]) + kBufs[0] - 50 + 10),
            ptr);
  ASSERT_EQ(50 - 10, size);
  ASSERT_EQ(kBufs[0], istr.ByteCount());
  ASSERT_FALSE(istr.Next(&ptr, &size));
}

TEST(ChunkedProtobufInputStreamTest, SimpleSequence) {
  ChunkSequence seq;
  for (size_t i = 0; i < base::ArraySize(kBufs); i++)
    seq.emplace_back(&kBufs[i], kBufs[i]);
  ChunkedProtobufInputStream istr(&seq);
  int num_bytes = 0;
  const void* ptr = nullptr;
  int size = 0;
  for (size_t i = 0; i < base::ArraySize(kBufs); i++) {
    ASSERT_EQ(num_bytes, istr.ByteCount());
    ASSERT_TRUE(istr.Next(&ptr, &size));
    ASSERT_EQ(&kBufs[i], ptr);
    ASSERT_EQ(kBufs[i], size);
    num_bytes += kBufs[i];
    ASSERT_EQ(num_bytes, istr.ByteCount());
  }
  ASSERT_FALSE(istr.Next(&ptr, &size));
}

TEST(ChunkedProtobufInputStreamTest, SequenceWithSkipsAndBackups) {
  ChunkSequence seq;
  for (size_t i = 0; i < base::ArraySize(kBufs); i++)
    seq.emplace_back(&kBufs[i], kBufs[i]);
  ChunkedProtobufInputStream istr(&seq);
  ASSERT_TRUE(istr.Skip(99));
  ASSERT_EQ(99, istr.ByteCount());

  ASSERT_TRUE(istr.Skip(1 + 200 + 1023));
  ASSERT_EQ(99 + 1 + 200 + 1023, istr.ByteCount());

  ASSERT_TRUE(istr.Skip(1 + 0 + 10 + 0 + 1 + 1 + 3));
  ASSERT_EQ(99 + 1 + 200 + 1023 + 1 + 0 + 10 + 0 + 1 + 1 + 3, istr.ByteCount());

  const void* ptr = nullptr;
  int size = 0;
  ASSERT_TRUE(istr.Next(&ptr, &size));
  ASSERT_EQ(kBufs[8] - 3, size);
  ASSERT_EQ(
      reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(&kBufs[8]) + 3),
      ptr);

  istr.BackUp(7 + 1 + 1 + 0 + 10);
  ASSERT_TRUE(istr.Next(&ptr, &size));
  ASSERT_EQ(&kBufs[4], ptr);
  ASSERT_EQ(kBufs[4], size);
}

}  // namespace
}  // namespace perfetto
