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

#include "src/traced/probes/ftrace/ftrace_procfs.h"

#include "test/gtest_and_gmock.h"

using testing::AnyNumber;
using testing::IsEmpty;
using testing::Return;
using testing::UnorderedElementsAre;

namespace perfetto {
namespace {

class MockFtraceProcfs : public FtraceProcfs {
 public:
  MockFtraceProcfs() : FtraceProcfs("/root/") {}

  MOCK_METHOD(bool,
              WriteToFile,
              (const std::string& path, const std::string& str),
              (override));
  MOCK_METHOD(char, ReadOneCharFromFile, (const std::string& path), (override));
  MOCK_METHOD(bool, ClearFile, (const std::string& path), (override));
  MOCK_METHOD(std::string,
              ReadFileIntoString,
              (const std::string& path),
              (const, override));
  MOCK_METHOD(size_t, NumberOfCpus, (), (const, override));
};

TEST(FtraceProcfsTest, ParseAvailableClocks) {
  MockFtraceProcfs ftrace;

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

}  // namespace
}  // namespace perfetto
