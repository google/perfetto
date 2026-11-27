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

#include "src/profiling/unwind/proc_pid_maps.h"

#include <sys/mman.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace profiling {
namespace {

bool ParseSingleLine(std::string line) {
  uint64_t start = 0;
  uint64_t end = 0;
  uint16_t flags = 0;
  uint64_t pgoff = 0;
  ino_t inode = 0;
  char* name = nullptr;
  bool shared = false;
  char* next_line = nullptr;
  return ParseMapsFileLine(line.data(), start, end, flags, pgoff, inode, &name,
                           shared, &next_line);
}

TEST(ProcPidMapsTest, ParseProcMaps) {
  std::string maps_content =
      "00400000-00409000 r-xp 00001000 fc:00 426998  /usr/bin/app\n"
      "7fff0000-7fff2000 rw-p 00000000 00:00 0       [stack]\n"
      "7fff2000-7fff3000 rw-s 00002000 00:01 1234    /dev/shm/test\n"
      "7fff3000-7fff4000 ---p 00000000 00:00 0\n";

  std::vector<MapInfo> parsed;
  ASSERT_TRUE(ReadMapFileContent(
      maps_content.data(),
      [&](const MapInfo& entry) { parsed.push_back(entry); }));

  ASSERT_EQ(parsed.size(), 4u);
  EXPECT_EQ(parsed[0].start, 0x00400000u);
  EXPECT_EQ(parsed[0].end, 0x00409000u);
  EXPECT_EQ(parsed[0].flags, static_cast<uint16_t>(PROT_READ | PROT_EXEC));
  EXPECT_EQ(parsed[0].pgoff, 0x1000u);
  EXPECT_EQ(parsed[0].inode, 426998u);
  EXPECT_EQ(parsed[0].name, "/usr/bin/app");
  EXPECT_FALSE(parsed[0].shared);

  EXPECT_EQ(parsed[1].start, 0x7fff0000u);
  EXPECT_EQ(parsed[1].end, 0x7fff2000u);
  EXPECT_EQ(parsed[1].flags, static_cast<uint16_t>(PROT_READ | PROT_WRITE));
  EXPECT_EQ(parsed[1].name, "[stack]");
  EXPECT_FALSE(parsed[1].shared);

  EXPECT_EQ(parsed[2].flags, static_cast<uint16_t>(PROT_READ | PROT_WRITE));
  EXPECT_EQ(parsed[2].pgoff, 0x2000u);
  EXPECT_EQ(parsed[2].name, "/dev/shm/test");
  EXPECT_TRUE(parsed[2].shared);

  // Anonymous mapping without a name column.
  EXPECT_EQ(parsed[3].flags, 0u);
  EXPECT_TRUE(parsed[3].name.empty());
}

TEST(ProcPidMapsTest, ParseProcMapsLineRejectsMalformed) {
  EXPECT_FALSE(ParseSingleLine(""));
  EXPECT_FALSE(ParseSingleLine("00400000 r-xp 00001000 fc:00 1 x"));
  EXPECT_FALSE(ParseSingleLine("00400000-00409000 rwxq 0 fc:00 1"));
  EXPECT_FALSE(ParseSingleLine("00400000-00409000 r-xp 0 fc00 1"));
  EXPECT_FALSE(ParseSingleLine("00400000-00409000 r-xp 0 fc:00"));

  // A bad line aborts the whole parse.
  std::string content =
      "00400000-00409000 r-xp 00001000 fc:00 426998  /usr/bin/app\n"
      "garbage\n";
  size_t calls = 0;
  EXPECT_FALSE(
      ReadMapFileContent(content.data(), [&](const MapInfo&) { calls++; }));
  EXPECT_EQ(calls, 1u);
}

TEST(ProcPidMapsTest, ReadSelfProcessMaps) {
  std::vector<MapInfo> maps;
  ASSERT_TRUE(ReadProcessMaps(getpid(), &maps));
  EXPECT_GT(maps.size(), 0u);
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto
