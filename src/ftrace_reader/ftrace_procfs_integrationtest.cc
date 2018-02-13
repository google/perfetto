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

#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include "ftrace_procfs.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "perfetto/ftrace_reader/ftrace_controller.h"

using testing::HasSubstr;
using testing::Not;
using testing::Contains;

namespace perfetto {
namespace {

const char kTracingPath[] = "/sys/kernel/debug/tracing/";

void ResetFtrace(FtraceProcfs* ftrace) {
  ftrace->DisableAllEvents();
  ftrace->ClearTrace();
  ftrace->EnableTracing();
}

std::string ReadFile(const std::string& name) {
  std::string path = std::string(kTracingPath) + name;
  std::ifstream fin(path, std::ios::in);
  if (!fin) {
    return "";
  }
  std::ostringstream stream;
  stream << fin.rdbuf();
  fin.close();
  return stream.str();
}

std::string GetTraceOutput() {
  std::string output = ReadFile("trace");
  if (output.empty()) {
    ADD_FAILURE() << "Could not read trace output";
  }
  return output;
}

}  // namespace

// TODO(lalitm): reenable these thests (see b/72306171).
TEST(FtraceProcfsIntegrationTest, DISABLED_CreateWithGoodPath) {
  EXPECT_TRUE(FtraceProcfs::Create(kTracingPath));
}

TEST(FtraceProcfsIntegrationTest, DISABLED_CreateWithBadPath) {
  EXPECT_FALSE(FtraceProcfs::Create(kTracingPath + std::string("bad_path")));
}

TEST(FtraceProcfsIntegrationTest, DISABLED_ClearTrace) {
  FtraceProcfs ftrace(kTracingPath);
  ResetFtrace(&ftrace);
  ftrace.WriteTraceMarker("Hello, World!");
  ftrace.ClearTrace();
  EXPECT_THAT(GetTraceOutput(), Not(HasSubstr("Hello, World!")));
}

TEST(FtraceProcfsIntegrationTest, DISABLED_TraceMarker) {
  FtraceProcfs ftrace(kTracingPath);
  ResetFtrace(&ftrace);
  ftrace.WriteTraceMarker("Hello, World!");
  EXPECT_THAT(GetTraceOutput(), HasSubstr("Hello, World!"));
}

TEST(FtraceProcfsIntegrationTest, DISABLED_EnableDisableEvent) {
  FtraceProcfs ftrace(kTracingPath);
  ResetFtrace(&ftrace);
  ftrace.EnableEvent("sched", "sched_switch");
  sleep(1);
  EXPECT_THAT(GetTraceOutput(), HasSubstr("sched_switch"));

  ftrace.DisableEvent("sched", "sched_switch");
  ftrace.ClearTrace();
  sleep(1);
  EXPECT_THAT(GetTraceOutput(), Not(HasSubstr("sched_switch")));
}

TEST(FtraceProcfsIntegrationTest, DISABLED_EnableDisableTracing) {
  FtraceProcfs ftrace(kTracingPath);
  ResetFtrace(&ftrace);
  EXPECT_TRUE(ftrace.IsTracingEnabled());
  ftrace.WriteTraceMarker("Before");
  ftrace.DisableTracing();
  EXPECT_FALSE(ftrace.IsTracingEnabled());
  ftrace.WriteTraceMarker("During");
  ftrace.EnableTracing();
  EXPECT_TRUE(ftrace.IsTracingEnabled());
  ftrace.WriteTraceMarker("After");
  EXPECT_THAT(GetTraceOutput(), HasSubstr("Before"));
  EXPECT_THAT(GetTraceOutput(), Not(HasSubstr("During")));
  EXPECT_THAT(GetTraceOutput(), HasSubstr("After"));
}

TEST(FtraceProcfsIntegrationTest, DISABLED_ReadFormatFile) {
  FtraceProcfs ftrace(kTracingPath);
  std::string format = ftrace.ReadEventFormat("ftrace", "print");
  EXPECT_THAT(format, HasSubstr("name: print"));
  EXPECT_THAT(format, HasSubstr("field:char buf"));
}

TEST(FtraceProcfsIntegrationTest, DISABLED_ReadAvailableEvents) {
  FtraceProcfs ftrace(kTracingPath);
  std::string format = ftrace.ReadAvailableEvents();
  EXPECT_THAT(format, HasSubstr("sched:sched_switch"));
}

TEST(FtraceProcfsIntegrationTest, DISABLED_CanOpenTracePipeRaw) {
  FtraceProcfs ftrace(kTracingPath);
  EXPECT_TRUE(ftrace.OpenPipeForCpu(0));
}

TEST(FtraceProcfsIntegrationTest, DISABLED_Clock) {
  FtraceProcfs ftrace(kTracingPath);
  std::set<std::string> clocks = ftrace.AvailableClocks();
  EXPECT_THAT(clocks, Contains("local"));
  EXPECT_THAT(clocks, Contains("global"));

  EXPECT_TRUE(ftrace.SetClock("global"));
  EXPECT_EQ(ftrace.GetClock(), "global");
  EXPECT_TRUE(ftrace.SetClock("local"));
  EXPECT_EQ(ftrace.GetClock(), "local");
}

TEST(FtraceProcfsIntegrationTest, DISABLED_CanSetBufferSize) {
  FtraceProcfs ftrace(kTracingPath);
  EXPECT_TRUE(ftrace.SetCpuBufferSizeInPages(4ul));
  EXPECT_EQ(ReadFile("buffer_size_kb"), "16\n");  // (4096 * 4) / 1024
  EXPECT_TRUE(ftrace.SetCpuBufferSizeInPages(5ul));
  EXPECT_EQ(ReadFile("buffer_size_kb"), "20\n");  // (4096 * 5) / 1024
}

TEST(FtraceProcfsIntegrationTest, FtraceControllerHardReset) {
  FtraceProcfs ftrace(kTracingPath);
  ResetFtrace(&ftrace);

  ftrace.SetCpuBufferSizeInPages(4ul);
  ftrace.EnableTracing();
  ftrace.EnableEvent("sched", "sched_switch");
  ftrace.WriteTraceMarker("Hello, World!");

  EXPECT_EQ(ReadFile("buffer_size_kb"), "16\n");
  EXPECT_EQ(ReadFile("tracing_on"), "1\n");
  EXPECT_EQ(ReadFile("events/enable"), "X\n");
  EXPECT_THAT(GetTraceOutput(), HasSubstr("Hello"));

  HardResetFtraceState();

  EXPECT_EQ(ReadFile("buffer_size_kb"), "4\n");
  EXPECT_EQ(ReadFile("tracing_on"), "0\n");
  EXPECT_EQ(ReadFile("events/enable"), "0\n");
  EXPECT_THAT(GetTraceOutput(), Not(HasSubstr("Hello")));
}

}  // namespace perfetto
