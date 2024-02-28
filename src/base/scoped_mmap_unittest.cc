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

#include "perfetto/ext/base/scoped_mmap.h"

#include "src/base/test/tmp_dir_tree.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::base {
namespace {

class ScopedMmapTest : public ::testing::Test {
  void SetUp() override {
#if !PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) &&   \
    !PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) && \
    !PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE) &&   \
    !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
    GTEST_SKIP() << "mmap not supported";
#endif
  }
};

TEST_F(ScopedMmapTest, WholeNonExistingFile) {
  base::TmpDirTree tmp;

  ScopedMmap mapped = ReadMmapWholeFile(tmp.AbsolutePath("f1.txt").c_str());

  EXPECT_FALSE(mapped.IsValid());
}

TEST_F(ScopedMmapTest, PartNonExistingFile) {
  base::TmpDirTree tmp;

  ScopedMmap mapped = ReadMmapFilePart(tmp.AbsolutePath("f1.txt").c_str(), 4);

  EXPECT_FALSE(mapped.IsValid());
}

TEST_F(ScopedMmapTest, WholeOneByteFile) {
  base::TmpDirTree tmp;
  tmp.AddFile("f1.txt", "c");

  ScopedMmap mapped = ReadMmapWholeFile(tmp.AbsolutePath("f1.txt").c_str());

  ASSERT_TRUE(mapped.IsValid());
  ASSERT_NE(mapped.data(), nullptr);
  ASSERT_EQ(mapped.length(), 1u);
  EXPECT_EQ(*static_cast<char*>(mapped.data()), 'c');
}

TEST_F(ScopedMmapTest, PartThreeBytes) {
  base::TmpDirTree tmp;
  tmp.AddFile("f1.txt", "ccccc");

  ScopedMmap mapped = ReadMmapFilePart(tmp.AbsolutePath("f1.txt").c_str(), 3);

  ASSERT_TRUE(mapped.IsValid());
  ASSERT_NE(mapped.data(), nullptr);
  ASSERT_EQ(mapped.length(), 3u);
}

TEST_F(ScopedMmapTest, Reset) {
  base::TmpDirTree tmp;
  tmp.AddFile("f1.txt", "ccccc");
  ScopedMmap mapped = ReadMmapWholeFile(tmp.AbsolutePath("f1.txt").c_str());
  ASSERT_TRUE(mapped.IsValid());

  EXPECT_TRUE(mapped.reset());

  EXPECT_FALSE(mapped.IsValid());
}

}  // namespace
}  // namespace perfetto::base
