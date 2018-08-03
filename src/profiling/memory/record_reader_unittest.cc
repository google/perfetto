/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/profiling/memory/record_reader.h"

#include "perfetto/base/scoped_file.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace perfetto {
namespace {

int ScopedPipe(base::ScopedFile scoped_fds[2]) {
  int fds[2];
  if (pipe(fds) == -1)
    return -1;
  scoped_fds[0].reset(fds[0]);
  scoped_fds[1].reset(fds[1]);
  return 0;
}

TEST(RecordReaderTest, ZeroLengthRecord) {
  bool called = false;
  auto callback_fn = [&called](size_t size, std::unique_ptr<uint8_t[]>) {
    called = true;
    ASSERT_EQ(size, 0u);
  };
  base::ScopedFile fd[2];
  ASSERT_NE(ScopedPipe(fd), -1);
  RecordReader r(std::move(callback_fn));
  uint64_t size = 0;
  ASSERT_NE(write(*fd[1], &size, sizeof(size)), -1);

  size_t itr = 0;
  while (!called && ++itr < 1000) {
    ssize_t rd = r.Read(*fd[0]);
    ASSERT_NE(rd, -1);
    ASSERT_NE(rd, 0);
  }
  ASSERT_TRUE(called);
}

TEST(RecordReaderTest, OneRecord) {
  bool called = false;
  auto callback_fn = [&called](size_t size, std::unique_ptr<uint8_t[]>) {
    called = true;
    ASSERT_EQ(size, 1u);
  };
  base::ScopedFile fd[2];
  ASSERT_NE(ScopedPipe(fd), -1);
  RecordReader r(std::move(callback_fn));
  uint64_t size = 1;
  ASSERT_NE(write(*fd[1], &size, sizeof(size)), -1);
  ASSERT_NE(write(*fd[1], "1", 1), -1);
  size_t itr = 0;
  while (!called && ++itr < 1000) {
    ssize_t rd = r.Read(*fd[0]);
    ASSERT_NE(rd, -1);
    ASSERT_NE(rd, 0);
  }
  ASSERT_TRUE(called);
}

TEST(RecordReaderTest, TwoRecords) {
  size_t called = 0;
  auto callback_fn = [&called](size_t size, std::unique_ptr<uint8_t[]>) {
    ASSERT_EQ(size, ++called);
  };
  base::ScopedFile fd[2];
  ASSERT_NE(ScopedPipe(fd), -1);
  RecordReader r(std::move(callback_fn));
  uint64_t size = 1;
  ASSERT_NE(write(*fd[1], &size, sizeof(size)), -1);
  ASSERT_NE(write(*fd[1], "1", 1), -1);
  size = 2;
  ASSERT_NE(write(*fd[1], &size, sizeof(size)), -1);
  ASSERT_NE(write(*fd[1], "12", 2), -1);
  size_t itr = 0;
  while (!called && ++itr < 1000) {
    ssize_t rd = r.Read(*fd[0]);
    ASSERT_NE(rd, -1);
    ASSERT_NE(rd, 0);
  }
  ASSERT_TRUE(called);
}

}  // namespace
}  // namespace perfetto
