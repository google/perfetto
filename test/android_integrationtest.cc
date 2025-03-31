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

#include "perfetto/base/build_config.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/pipe.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/tracing/core/commit_data_request.h"
#include "perfetto/ext/tracing/core/trace_packet.h"
#include "perfetto/ext/tracing/core/tracing_service.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "src/base/test/test_task_runner.h"
#include "src/base/test/utils.h"
#include "test/gtest_and_gmock.h"
#include "test/test_helper.h"

#include "protos/perfetto/common/sys_stats_counters.gen.h"
#include "protos/perfetto/config/power/android_power_config.pbzero.h"
#include "protos/perfetto/config/sys_stats/sys_stats_config.gen.h"
#include "protos/perfetto/config/test_config.gen.h"
#include "protos/perfetto/config/trace_config.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_stats.gen.h"
#include "protos/perfetto/trace/perfetto/tracing_service_event.gen.h"
#include "protos/perfetto/trace/power/battery_counters.gen.h"
#include "protos/perfetto/trace/sys_stats/sys_stats.gen.h"
#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/trace.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/perfetto/trace/trigger.gen.h"

namespace perfetto {

namespace {

using ::testing::ContainsRegex;
using ::testing::Each;
using ::testing::ElementsAreArray;
using ::testing::HasSubstr;
using ::testing::Property;
using ::testing::SizeIs;

}  // namespace

TEST(PerfettoAndroidIntegrationTest, TestBatteryTracing) {
  base::TestTaskRunner task_runner;

  TestHelper helper(&task_runner);
  helper.StartServiceIfRequired();

#if PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
  ProbesProducerThread probes(GetTestProducerSockName());
  probes.Connect();
#endif

  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  trace_config.set_duration_ms(3000);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.power");
  ds_config->set_target_buffer(0);

  using protos::pbzero::AndroidPowerConfig;
  protozero::HeapBuffered<AndroidPowerConfig> power_config;
  power_config->set_battery_poll_ms(250);
  power_config->add_battery_counters(
      AndroidPowerConfig::BATTERY_COUNTER_CHARGE);
  power_config->add_battery_counters(
      AndroidPowerConfig::BATTERY_COUNTER_CAPACITY_PERCENT);
  ds_config->set_android_power_config_raw(power_config.SerializeAsString());

  helper.StartTracing(trace_config);
  helper.WaitForTracingDisabled();

  helper.ReadData();
  helper.WaitForReadData();

  const auto& packets = helper.trace();
  ASSERT_GT(packets.size(), 0u);

  bool has_battery_packet = false;
  for (const auto& packet : packets) {
    if (!packet.has_battery())
      continue;
    has_battery_packet = true;
    // Unfortunately we cannot make any assertions on the charge counter.
    // On some devices it can reach negative values (b/64685329).
    EXPECT_GE(packet.battery().capacity_percent(), 0.f);
    EXPECT_LE(packet.battery().capacity_percent(), 100.f);
  }

  ASSERT_TRUE(has_battery_packet);
}

}  // namespace perfetto

#endif  // PERFETTO_OS_ANDROID
