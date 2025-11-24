/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include <sys/system_properties.h>
#include "test/gtest_and_gmock.h"

#include "perfetto/ext/base/uuid.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "src/base/test/test_task_runner.h"
#include "test/test_helper.h"

#include "protos/perfetto/config/test_config.gen.h"
#include "protos/perfetto/trace/trace.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"

namespace perfetto {
namespace {

class PerfettoReporterTest : public ::testing::Test {
 protected:
  // Both "persistent" and "reported" files are cleaned up using
  // "RunCommandTargetPreparer" in AndroidTest.xml.
  static constexpr char kPersistentTracesDir[] =
      "/data/misc/perfetto-traces/persistent";
  static constexpr char kReportedTracesDir[] =
      "/sdcard/Android/data/android.perfetto.cts.reporter/files";

  static constexpr uint32_t kTraceEventCount = 1;
  static constexpr uint32_t kTraceDurationOneHourInMs = 3600000;  // 1 hour

  static TraceConfig CreateMinimalTraceReporterConfig(
      const std::string& session_name,
      uint32_t trace_duration_ms,
      base::Uuid uuid,
      uint32_t kEventCount) {
    TraceConfig trace_config;
    trace_config.add_buffers()->set_size_kb(1024);
    trace_config.set_duration_ms(trace_duration_ms);
    trace_config.set_unique_session_name(session_name);

    // Make the trace as small as possible (see b/282508742).
    auto* builtin = trace_config.mutable_builtin_data_sources();
    builtin->set_disable_clock_snapshotting(true);
    builtin->set_disable_system_info(true);
    builtin->set_disable_service_events(true);
    builtin->set_disable_chunk_usage_histograms(true);

    auto* ds_config = trace_config.add_data_sources()->mutable_config();
    ds_config->set_name("android.perfetto.FakeProducer");
    ds_config->set_target_buffer(0);

    trace_config.set_trace_uuid_lsb(uuid.lsb());
    trace_config.set_trace_uuid_msb(uuid.msb());

    static constexpr uint32_t kRandomSeed = 42;
    static constexpr uint32_t kMessageSizeBytes = 2;
    ds_config->mutable_for_testing()->set_seed(kRandomSeed);
    ds_config->mutable_for_testing()->set_message_count(kEventCount);
    ds_config->mutable_for_testing()->set_message_size(kMessageSizeBytes);
    ds_config->mutable_for_testing()->set_send_batch_on_register(true);

    auto* report_config = trace_config.mutable_android_report_config();
    report_config->set_reporter_service_package(
        "android.perfetto.cts.reporter");
    report_config->set_reporter_service_class(
        "android.perfetto.cts.reporter.PerfettoReportService");
    report_config->set_use_pipe_in_framework_for_testing(true);

    return trace_config;
  }

  static void AssertTraceWasReported(base::Uuid uuid, uint32_t kEventCount) {
    std::string path =
        std::string(kReportedTracesDir) + "/" + uuid.ToPrettyString();
    ASSERT_TRUE(WaitForFile(path))
        << "Timed out waiting for a reported trace file: " << path;

    std::string trace_str;
    ASSERT_TRUE(base::ReadFile(path, &trace_str));

    protos::gen::Trace trace;
    ASSERT_TRUE(trace.ParseFromString(trace_str));
    int for_testing = 0;
    for (const auto& packet : trace.packet()) {
      for_testing += packet.has_for_testing();
    }
    ASSERT_EQ(for_testing, kEventCount);
  }

  static bool WaitForFile(std::string path) {
    static constexpr uint32_t kIterationSleepMs = 500;
    static constexpr uint32_t kIterationCount =
        kDefaultTestTimeoutMs / kIterationSleepMs;
    for (size_t i = 0; i < kIterationCount; ++i) {
      if (base::FileExists(path))
        return true;
      base::SleepMicroseconds(kIterationSleepMs * 1000);
    }
    return false;
  }
};

TEST_F(PerfettoReporterTest, TestEndToEndReport) {
  base::TestTaskRunner task_runner;
  TestHelper helper(&task_runner);
  helper.ConnectFakeProducer();

  base::Uuid uuid = base::Uuidv4();
  TraceConfig trace_config = CreateMinimalTraceReporterConfig(
      "TestEndToEndReport", 200, uuid, kTraceEventCount);

  auto perfetto_proc = Exec("perfetto",
                            {
                                "--upload",
                                "--no-guardrails",
                                "-c",
                                "-",
                            },
                            trace_config.SerializeAsString());

  std::string stderr_str;
  EXPECT_EQ(0, perfetto_proc.Run(&stderr_str)) << stderr_str;

  AssertTraceWasReported(uuid, kTraceEventCount);
}

TEST_F(PerfettoReporterTest, TestEndToEndReportPersistent) {
  base::TestTaskRunner task_runner;
  TestHelper helper(&task_runner);
  helper.ConnectFakeProducer();

  const std::string kSessionName = "TestEndToEndReportPersistent";
  const std::string trace_file =
      std::string(kPersistentTracesDir) + "/" + kSessionName + ".pftrace";

  base::Uuid uuid = base::Uuidv4();
  TraceConfig trace_config = CreateMinimalTraceReporterConfig(
      kSessionName, kTraceDurationOneHourInMs, uuid, kTraceEventCount);
  trace_config.set_persist_trace_after_reboot(true);
  trace_config.set_write_into_file(true);

  auto perfetto_proc = Exec("perfetto",
                            {
                                "--upload",
                                "--no-guardrails",
                                "-c",
                                "-",
                            },
                            trace_config.SerializeAsString());

  std::thread background_trace([&perfetto_proc]() {
    std::string stderr_str;
    ASSERT_EQ(0, perfetto_proc.Run(&stderr_str)) << stderr_str;
  });

  ASSERT_TRUE(WaitForFile(trace_file))
      << "Timed out waiting for a running trace file: " << trace_file;

  perfetto_proc.SendSigterm();
  background_trace.join();

  AssertTraceWasReported(uuid, kTraceEventCount);

  ASSERT_FALSE(base::FileExists(trace_file));
}

TEST_F(PerfettoReporterTest, TestEndToEndReportPersistentAlreadyStarted) {
  base::TestTaskRunner task_runner;
  TestHelper helper(&task_runner);
  helper.ConnectFakeProducer();

  const std::string kSessionName = "TestEndToEndReportPersistentAlreadyStarted";
  const std::string trace_file =
      std::string(kPersistentTracesDir) + "/" + kSessionName + ".pftrace";

  base::Uuid uuid = base::Uuidv4();
  TraceConfig trace_config = CreateMinimalTraceReporterConfig(
      kSessionName, kTraceDurationOneHourInMs, uuid, kTraceEventCount);
  trace_config.set_persist_trace_after_reboot(true);
  trace_config.set_write_into_file(true);

  std::initializer_list<std::string> args = {
      "--upload",
      "--no-guardrails",
      "-c",
      "-",
  };
  auto perfetto_proc = Exec("perfetto", args, trace_config.SerializeAsString());
  // Command to start a second session identical to the previous one.
  auto perfetto_proc_2 =
      Exec("perfetto", args, trace_config.SerializeAsString());

  // Start a first perfetto session.
  std::thread background_trace([&perfetto_proc]() {
    std::string stderr_str;
    ASSERT_EQ(0, perfetto_proc.Run(&stderr_str)) << stderr_str;
  });

  ASSERT_TRUE(WaitForFile(trace_file))
      << "Timed out waiting for a running trace file: " << trace_file;

  // Now start a second perfetto session with the same name. Error should be
  // reported, exit code should be zero.
  std::string stderr_str;
  ASSERT_EQ(0, perfetto_proc_2.Run(&stderr_str)) << stderr_str;
  const std::string error_message = "A trace with this unique session name (" +
                                    kSessionName + ") already exists";
  EXPECT_THAT(stderr_str, ::testing::HasSubstr(error_message));

  // We can normally stop first session.
  perfetto_proc.SendSigterm();
  background_trace.join();

  AssertTraceWasReported(uuid, kTraceEventCount);

  ASSERT_FALSE(base::FileExists(trace_file));
}

TEST_F(PerfettoReporterTest, TestEndToEndReportPersistentTraceExists) {
  base::TestTaskRunner task_runner;
  TestHelper helper(&task_runner);
  helper.ConnectFakeProducer();

  const std::string kSessionName = "TestEndToEndReportPersistentTraceExists";
  const std::string trace_file =
      std::string(kPersistentTracesDir) + "/" + kSessionName + ".pftrace";
  // Create a trace file, it could be, for example, a trace from the previous
  // run that was by mistake not removed on reboot.
  auto fd = base::OpenFile(trace_file, O_RDWR | O_CREAT | O_TRUNC, 0600);
  ASSERT_TRUE(fd) << "Failed to create an 'existing' trace file.";

  base::Uuid uuid = base::Uuidv4();
  TraceConfig trace_config = CreateMinimalTraceReporterConfig(
      kSessionName, kTraceDurationOneHourInMs, uuid, kTraceEventCount);
  trace_config.set_persist_trace_after_reboot(true);
  trace_config.set_write_into_file(true);

  auto perfetto_proc = Exec("perfetto",
                            {
                                "--upload",
                                "--no-guardrails",
                                "-c",
                                "-",
                            },
                            trace_config.SerializeAsString());

  std::string stderr_str;
  ASSERT_EQ(0, perfetto_proc.Run(&stderr_str)) << stderr_str;
  const std::string error_message =
      "Failed to create the trace file " + trace_file;
  EXPECT_THAT(stderr_str, ::testing::HasSubstr(error_message));

  // File was not removed by perfetto_cmd.
  ASSERT_TRUE(base::FileExists(trace_file));
}

}  // namespace
}  // namespace perfetto
