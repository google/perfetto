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

#include "src/traced/probes/ftrace/tracefs.h"
#include "perfetto/ext/base/utils.h"

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace {

using testing::_;
using testing::AnyNumber;
using testing::DoAll;
using testing::ElementsAre;
using testing::IsEmpty;
using testing::MatchesRegex;
using testing::Mock;
using testing::Optional;
using testing::Return;
using testing::SetArgPointee;
using testing::UnorderedElementsAre;
class MockTracefs : public Tracefs {
 public:
  MockTracefs() : Tracefs("/root/") {}

  MOCK_METHOD(bool,
              WriteToFile,
              (const std::string& path, const std::string& str),
              (override));
  MOCK_METHOD(char, ReadOneCharFromFile, (const std::string& path), (override));
  MOCK_METHOD(bool, ClearFile, (const std::string& path), (override));
  MOCK_METHOD(bool,
              ReadFile,
              (const std::string& path, std::string* contents),
              (const, override));
  MOCK_METHOD(std::string,
              ReadFileIntoString,
              (const std::string& path),
              (const, override));
  MOCK_METHOD(size_t, NumberOfCpus, (), (const, override));
  MOCK_METHOD(size_t, NumberOfOnlineCpus, (), (const, override));
  MOCK_METHOD(std::optional<std::vector<uint32_t>>,
              GetOfflineCpus,
              (),
              (const, override));
};

TEST(TracefsTest, ParseAvailableClocks) {
  MockTracefs ftrace;

  EXPECT_CALL(ftrace, ReadFileIntoString("/root/trace_clock"))
      .WillOnce(Return("[local] global boot"));
  EXPECT_THAT(ftrace.AvailableClocks(),
              UnorderedElementsAre("local", "global", "boot"));

  EXPECT_CALL(ftrace, ReadFileIntoString("/root/trace_clock"))
      .WillOnce(Return("[local] global boot"));
  EXPECT_THAT(ftrace.GetClock(), "local");

  EXPECT_CALL(ftrace, ReadFileIntoString("/root/trace_clock"))
      .WillOnce(Return("local [global] boot"));
  EXPECT_THAT(ftrace.GetClock(), "global");

  EXPECT_CALL(ftrace, ReadFileIntoString("/root/trace_clock"))
      .WillOnce(Return("local global [boot]"));
  EXPECT_THAT(ftrace.GetClock(), "boot");

  EXPECT_CALL(ftrace, ReadFileIntoString("/root/trace_clock"))
      .WillOnce(Return(""));
  EXPECT_THAT(ftrace.AvailableClocks(), IsEmpty());

  // trace_clock text may end in a new line:
  EXPECT_CALL(ftrace, ReadFileIntoString("/root/trace_clock"))
      .WillOnce(Return("[local] global boot\n"));
  EXPECT_THAT(ftrace.AvailableClocks(),
              UnorderedElementsAre("local", "global", "boot"));

  EXPECT_CALL(ftrace, ReadFileIntoString("/root/trace_clock"))
      .WillOnce(Return("local global [boot]\n"));
  EXPECT_THAT(ftrace.AvailableClocks(),
              UnorderedElementsAre("local", "global", "boot"));

  EXPECT_CALL(ftrace, ReadFileIntoString("/root/trace_clock"))
      .WillOnce(Return("local global [boot]\n"));
  EXPECT_THAT(ftrace.GetClock(), "boot");

  EXPECT_CALL(ftrace, ReadFileIntoString("/root/trace_clock"))
      .WillOnce(Return("\n"));
  EXPECT_THAT(ftrace.AvailableClocks(), IsEmpty());

  // We should handle many newlines (just in case):
  EXPECT_CALL(ftrace, ReadFileIntoString("/root/trace_clock"))
      .WillOnce(Return("local global [boot]\n\n\n"));
  EXPECT_THAT(ftrace.GetClock(), "boot");

  EXPECT_CALL(ftrace, ReadFileIntoString("/root/trace_clock"))
      .WillOnce(Return("local global [boot]\n\n"));
  EXPECT_THAT(ftrace.GetClock(), "boot");

  EXPECT_CALL(ftrace, ReadFileIntoString("/root/trace_clock"))
      .WillOnce(Return("\n\n\n\n"));
  EXPECT_THAT(ftrace.AvailableClocks(), IsEmpty());
}

TEST(TracefsTest, ReadBufferSizeInPages) {
  MockTracefs ftrace;
  uint32_t page_in_kb = base::GetSysPageSize() / 1024ul;

  // Boundary checks
  EXPECT_CALL(ftrace, ReadFileIntoString("/root/buffer_size_kb"))
      .WillOnce(Return(std::to_string(page_in_kb) + "\n"));
  EXPECT_THAT(ftrace.GetCpuBufferSizeInPages(), 1);

  EXPECT_CALL(ftrace, ReadFileIntoString("/root/buffer_size_kb"))
      .WillOnce(Return(std::to_string(page_in_kb - 1) + "\n"));
  EXPECT_THAT(ftrace.GetCpuBufferSizeInPages(), 1);

  EXPECT_CALL(ftrace, ReadFileIntoString("/root/buffer_size_kb"))
      .WillOnce(Return(std::to_string(page_in_kb + 1) + "\n"));
  EXPECT_THAT(ftrace.GetCpuBufferSizeInPages(), 2);

  EXPECT_CALL(ftrace, ReadFileIntoString("/root/buffer_size_kb"))
      .WillOnce(Return(std::to_string(2 * page_in_kb) + "\n"));
  EXPECT_THAT(ftrace.GetCpuBufferSizeInPages(), 2);

  EXPECT_CALL(ftrace, ReadFileIntoString("/root/buffer_size_kb"))
      .WillOnce(Return(std::to_string(2 * page_in_kb + 1) + "\n"));
  EXPECT_THAT(ftrace.GetCpuBufferSizeInPages(), 3);

  // Read before setup buffer size.
  EXPECT_CALL(ftrace, ReadFileIntoString("/root/buffer_size_kb"))
      .WillOnce(
          Return(std::to_string(2 * page_in_kb - 1) + " (expanded: 1408)\n"));
  EXPECT_THAT(ftrace.GetCpuBufferSizeInPages(), 2);

  // Failed to read file (e.g. permission error)
  EXPECT_CALL(ftrace, ReadFileIntoString("/root/buffer_size_kb"))
      .WillOnce(Return(""));
  EXPECT_THAT(ftrace.GetCpuBufferSizeInPages(), 1);

  // Wrong string
  EXPECT_CALL(ftrace, ReadFileIntoString("/root/buffer_size_kb"))
      .WillOnce(Return("\n\n\n\n"));
  EXPECT_THAT(ftrace.GetCpuBufferSizeInPages(), 1);
}

TEST(TracefsTest, ClearTrace) {
  MockTracefs ftrace;

  // Test: Fast path when all CPUs are online.
  EXPECT_CALL(ftrace, NumberOfCpus()).WillRepeatedly(Return(4));
  EXPECT_CALL(ftrace, NumberOfOnlineCpus()).WillRepeatedly(Return(4));
  EXPECT_CALL(ftrace, ClearFile("/root/trace")).WillOnce(Return(true));
  EXPECT_CALL(ftrace, GetOfflineCpus()).Times(0);
  EXPECT_CALL(ftrace, ClearFile(MatchesRegex("/root/per_cpu/cpu[0-9]/trace")))
      .Times(0);
  ftrace.ClearTrace();
  ASSERT_TRUE(Mock::VerifyAndClearExpectations(&ftrace));

  // Test: Only the buffers for the specified offline CPUs are cleared.
  EXPECT_CALL(ftrace, NumberOfCpus()).WillRepeatedly(Return(4));
  EXPECT_CALL(ftrace, NumberOfOnlineCpus()).WillRepeatedly(Return(2));
  EXPECT_CALL(ftrace, ClearFile("/root/trace")).WillOnce(Return(true));
  EXPECT_CALL(ftrace, GetOfflineCpus())
      .WillOnce(Return(std::vector<uint32_t>{1, 3}));
  EXPECT_CALL(ftrace, ClearFile("/root/per_cpu/cpu0/trace")).Times(0);
  EXPECT_CALL(ftrace, ClearFile("/root/per_cpu/cpu1/trace"))
      .WillOnce(Return(true));
  EXPECT_CALL(ftrace, ClearFile("/root/per_cpu/cpu2/trace")).Times(0);
  EXPECT_CALL(ftrace, ClearFile("/root/per_cpu/cpu3/trace"))
      .WillOnce(Return(true));
  ftrace.ClearTrace();

  // Test: Fallback behavior when the offline CPU list can't be read.
  EXPECT_CALL(ftrace, NumberOfCpus()).WillRepeatedly(Return(4));
  EXPECT_CALL(ftrace, NumberOfOnlineCpus()).WillRepeatedly(Return(2));
  EXPECT_CALL(ftrace, ClearFile("/root/trace")).WillOnce(Return(true));
  EXPECT_CALL(ftrace, GetOfflineCpus()).WillOnce(Return(std::nullopt));
  for (size_t cpu = 0; cpu < 4; cpu++) {
    EXPECT_CALL(ftrace,
                ClearFile("/root/per_cpu/cpu" + std::to_string(cpu) + "/trace"))
        .WillOnce(Return(true));
  }
  ftrace.ClearTrace();
  ASSERT_TRUE(Mock::VerifyAndClearExpectations(&ftrace));
}

TEST(TracefsTest, GetOfflineCpus) {
  class MockReadFileTracefs : public Tracefs {
   public:
    MockReadFileTracefs() : Tracefs("/root/") {}
    using Tracefs::GetOfflineCpus;

    MOCK_METHOD(bool,
                ReadFile,
                (const std::string& path, std::string* contents),
                (const, override));
  } ftrace;

  // ReadFile fails.
  EXPECT_CALL(ftrace, ReadFile("/sys/devices/system/cpu/offline", _))
      .WillOnce(Return(false));
  EXPECT_EQ(ftrace.GetOfflineCpus(), std::nullopt);

  // Invalid value.
  EXPECT_CALL(ftrace, ReadFile("/sys/devices/system/cpu/offline", _))
      .WillOnce(DoAll(SetArgPointee<1>("1,a,3"), Return(true)));
  EXPECT_EQ(ftrace.GetOfflineCpus(), std::nullopt);

  // Empty offline CPU list.
  EXPECT_CALL(ftrace, ReadFile("/sys/devices/system/cpu/offline", _))
      .WillOnce(DoAll(SetArgPointee<1>(""), Return(true)));
  EXPECT_THAT(ftrace.GetOfflineCpus(), Optional(IsEmpty()));

  // Comma-separated list of single offline CPUs.
  EXPECT_CALL(ftrace, ReadFile("/sys/devices/system/cpu/offline", _))
      .WillOnce(DoAll(SetArgPointee<1>("1,3\n"), Return(true)));
  EXPECT_THAT(ftrace.GetOfflineCpus(), Optional(ElementsAre(1, 3)));

  // Range of offline CPUs (e.g., "0-2").
  EXPECT_CALL(ftrace, ReadFile("/sys/devices/system/cpu/offline", _))
      .WillOnce(DoAll(SetArgPointee<1>("0-2,4-5\n"), Return(true)));
  EXPECT_THAT(ftrace.GetOfflineCpus(), Optional(ElementsAre(0, 1, 2, 4, 5)));

  // Combination of single CPUs and ranges.
  EXPECT_CALL(ftrace, ReadFile("/sys/devices/system/cpu/offline", _))
      .WillOnce(DoAll(SetArgPointee<1>("0,2-3,5\n"), Return(true)));
  EXPECT_THAT(ftrace.GetOfflineCpus(), Optional(ElementsAre(0, 2, 3, 5)));
}

}  // namespace
}  // namespace perfetto
