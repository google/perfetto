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

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "perfetto/base/file_utils.h"
#include "src/traced/probes/ftrace/ftrace_controller.h"
#include "src/traced/probes/ftrace/ftrace_procfs.h"

using testing::HasSubstr;
using testing::Not;
using testing::Contains;

namespace perfetto {
namespace {

constexpr char kTracingPath[] = "/sys/kernel/debug/tracing/";

void ResetFtrace(FtraceProcfs* ftrace) {
  ftrace->DisableAllEvents();
  ftrace->ClearTrace();
  ftrace->EnableTracing();
}

std::string ReadFile(const std::string& name) {
  std::string result;
  PERFETTO_CHECK(base::ReadFile(kTracingPath + name, &result));
  return result;
}

std::string GetTraceOutput() {
  std::string output = ReadFile("trace");
  if (output.empty()) {
    ADD_FAILURE() << "Could not read trace output";
  }
  return output;
}

}  // namespace

// TODO(lalitm): reenable these tests (see b/72306171).
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#define MAYBE_CreateWithGoodPath CreateWithGoodPath
#else
#define MAYBE_CreateWithGoodPath DISABLED_CreateWithGoodPath
#endif
TEST(FtraceProcfsIntegrationTest, MAYBE_CreateWithGoodPath) {
  EXPECT_TRUE(FtraceProcfs::Create(kTracingPath));
}

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#define MAYBE_CreateWithBadPath CreateWithBadPath
#else
#define MAYBE_CreateWithBadPath DISABLED_CreateWithBadath
#endif
TEST(FtraceProcfsIntegrationTest, MAYBE_CreateWithBadPath) {
  EXPECT_FALSE(FtraceProcfs::Create(kTracingPath + std::string("bad_path")));
}

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#define MAYBE_ClearTrace ClearTrace
#else
#define MAYBE_ClearTrace DISABLED_ClearTrace
#endif
TEST(FtraceProcfsIntegrationTest, MAYBE_ClearTrace) {
  FtraceProcfs ftrace(kTracingPath);
  ResetFtrace(&ftrace);
  ftrace.WriteTraceMarker("Hello, World!");
  ftrace.ClearTrace();
  EXPECT_THAT(GetTraceOutput(), Not(HasSubstr("Hello, World!")));
}

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#define MAYBE_TraceMarker TraceMarker
#else
#define MAYBE_TraceMarker DISABLED_TraceMarker
#endif
TEST(FtraceProcfsIntegrationTest, MAYBE_TraceMarker) {
  FtraceProcfs ftrace(kTracingPath);
  ResetFtrace(&ftrace);
  ftrace.WriteTraceMarker("Hello, World!");
  EXPECT_THAT(GetTraceOutput(), HasSubstr("Hello, World!"));
}

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#define MAYBE_EnableDisableEvent EnableDisableEvent
#else
#define MAYBE_EnableDisableEvent DISABLED_EnableDisableEvent
#endif
TEST(FtraceProcfsIntegrationTest, MAYBE_EnableDisableEvent) {
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

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#define MAYBE_EnableDisableTracing EnableDisableTracing
#else
#define MAYBE_EnableDisableTracing DISABLED_EnableDisableTracing
#endif
TEST(FtraceProcfsIntegrationTest, MAYBE_EnableDisableTracing) {
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

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#define MAYBE_ReadFormatFile ReadFormatFile
#else
#define MAYBE_ReadFormatFile DISABLED_ReadFormatFile
#endif
TEST(FtraceProcfsIntegrationTest, MAYBE_ReadFormatFile) {
  FtraceProcfs ftrace(kTracingPath);
  std::string format = ftrace.ReadEventFormat("ftrace", "print");
  EXPECT_THAT(format, HasSubstr("name: print"));
  EXPECT_THAT(format, HasSubstr("field:char buf"));
}

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#define MAYBE_CanOpenTracePipeRaw CanOpenTracePipeRaw
#else
#define MAYBE_CanOpenTracePipeRaw DISABLED_CanOpenTracePipeRaw
#endif
TEST(FtraceProcfsIntegrationTest, MAYBE_CanOpenTracePipeRaw) {
  FtraceProcfs ftrace(kTracingPath);
  EXPECT_TRUE(ftrace.OpenPipeForCpu(0));
}

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#define MAYBE_Clock Clock
#else
#define MAYBE_Clock DISABLED_Clock
#endif
TEST(FtraceProcfsIntegrationTest, MAYBE_Clock) {
  FtraceProcfs ftrace(kTracingPath);
  std::set<std::string> clocks = ftrace.AvailableClocks();
  EXPECT_THAT(clocks, Contains("local"));
  EXPECT_THAT(clocks, Contains("global"));

  EXPECT_TRUE(ftrace.SetClock("global"));
  EXPECT_EQ(ftrace.GetClock(), "global");
  EXPECT_TRUE(ftrace.SetClock("local"));
  EXPECT_EQ(ftrace.GetClock(), "local");
}

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#define MAYBE_CanSetBufferSize CanSetBufferSize
#else
#define MAYBE_CanSetBufferSize DISABLED_CanSetBufferSize
#endif
TEST(FtraceProcfsIntegrationTest, MAYBE_CanSetBufferSize) {
  FtraceProcfs ftrace(kTracingPath);
  EXPECT_TRUE(ftrace.SetCpuBufferSizeInPages(4ul));
  EXPECT_EQ(ReadFile("buffer_size_kb"), "16\n");  // (4096 * 4) / 1024
  EXPECT_TRUE(ftrace.SetCpuBufferSizeInPages(5ul));
  EXPECT_EQ(ReadFile("buffer_size_kb"), "20\n");  // (4096 * 5) / 1024
}

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#define MAYBE_FtraceControllerHardReset FtraceControllerHardReset
#else
#define MAYBE_FtraceControllerHardReset DISABLED_FtraceControllerHardReset
#endif
TEST(FtraceProcfsIntegrationTest, MAYBE_FtraceControllerHardReset) {
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
