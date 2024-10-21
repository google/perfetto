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
#include <random>
#include "test/gtest_and_gmock.h"

#include "perfetto/ext/base/android_utils.h"
#include "perfetto/ext/base/uuid.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "src/base/test/test_task_runner.h"
#include "test/android_test_utils.h"
#include "test/test_helper.h"

#include "protos/perfetto/config/test_config.gen.h"
#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/trace.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"

namespace perfetto {
namespace {

TEST(PerfettoReporterTest, TestEndToEndReport) {
  base::TestTaskRunner task_runner;
  TestHelper helper(&task_runner);
  helper.ConnectFakeProducer();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(1024);
  trace_config.set_duration_ms(200);
  trace_config.set_unique_session_name("TestEndToEndReport");

  // Make the trace as small as possible (see b/282508742).
  auto* builtin = trace_config.mutable_builtin_data_sources();
  builtin->set_disable_clock_snapshotting(true);
  builtin->set_disable_system_info(true);
  builtin->set_disable_service_events(true);
  builtin->set_disable_chunk_usage_histograms(true);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.perfetto.FakeProducer");
  ds_config->set_target_buffer(0);

  base::Uuid uuid = base::Uuidv4();
  trace_config.set_trace_uuid_lsb(uuid.lsb());
  trace_config.set_trace_uuid_msb(uuid.msb());

  static constexpr uint32_t kRandomSeed = 42;
  static constexpr uint32_t kEventCount = 1;
  static constexpr uint32_t kMessageSizeBytes = 2;
  ds_config->mutable_for_testing()->set_seed(kRandomSeed);
  ds_config->mutable_for_testing()->set_message_count(kEventCount);
  ds_config->mutable_for_testing()->set_message_size(kMessageSizeBytes);
  ds_config->mutable_for_testing()->set_send_batch_on_register(true);

  auto* report_config = trace_config.mutable_android_report_config();
  report_config->set_reporter_service_package("android.perfetto.cts.reporter");
  report_config->set_reporter_service_class(
      "android.perfetto.cts.reporter.PerfettoReportService");
  report_config->set_use_pipe_in_framework_for_testing(true);

  // We have to construct all the processes we want to fork before we start the
  // service with |StartServiceIfRequired()|. this is because it is unsafe
  // (could deadlock) to fork after we've spawned some threads which might
  // printf (and thus hold locks).
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

  static constexpr char kPath[] =
      "/sdcard/Android/data/android.perfetto.cts.reporter/files/";
  std::string path = kPath + uuid.ToPrettyString();
  static constexpr uint32_t kIterationSleepMs = 500;
  static constexpr uint32_t kIterationCount =
      kDefaultTestTimeoutMs / kIterationSleepMs;
  for (size_t i = 0; i < kIterationCount; ++i) {
    if (!base::FileExists(path)) {
      base::SleepMicroseconds(kIterationSleepMs * 1000);
      continue;
    }

    std::string trace_str;
    ASSERT_TRUE(base::ReadFile(path, &trace_str));

    protos::gen::Trace trace;
    ASSERT_TRUE(trace.ParseFromString(trace_str));
    int for_testing = 0;
    for (const auto& packet : trace.packet()) {
      for_testing += packet.has_for_testing();
    }
    ASSERT_EQ(for_testing, kEventCount);
    return;
  }
  FAIL() << "Timed out waiting for trace file";
}

}  // namespace
}  // namespace perfetto
