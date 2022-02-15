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
#include "src/base/test/test_task_runner.h"
#include "src/base/test/utils.h"
#include "test/gtest_and_gmock.h"
#include "test/test_helper.h"

#include "protos/perfetto/config/power/android_power_config.pbzero.h"
#include "protos/perfetto/config/test_config.gen.h"
#include "protos/perfetto/config/trace_config.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_stats.gen.h"
#include "protos/perfetto/trace/perfetto/tracing_service_event.gen.h"
#include "protos/perfetto/trace/power/battery_counters.gen.h"
#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/trace.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/perfetto/trace/trigger.gen.h"

#include "protos/perfetto/common/sys_stats_counters.gen.h"
#include "protos/perfetto/config/sys_stats/sys_stats_config.gen.h"
#include "protos/perfetto/trace/sys_stats/sys_stats.gen.h"

namespace perfetto {

namespace {

using ::testing::ContainsRegex;
using ::testing::Each;
using ::testing::ElementsAreArray;
using ::testing::HasSubstr;
using ::testing::Property;
using ::testing::SizeIs;

// For the SaveForBugreport* tests.
void SetTraceConfigForBugreportTest(TraceConfig* trace_config) {
  trace_config->add_buffers()->set_size_kb(4096);
  trace_config->set_duration_ms(60000);  // Will never hit this.
  trace_config->set_bugreport_score(10);
  auto* ds_config = trace_config->add_data_sources()->mutable_config();
  ds_config->set_name("android.perfetto.FakeProducer");
  ds_config->mutable_for_testing()->set_message_count(3);
  ds_config->mutable_for_testing()->set_message_size(10);
  ds_config->mutable_for_testing()->set_send_batch_on_register(true);
}

// For the SaveForBugreport* tests.
static void VerifyBugreportTraceContents() {
  // Read the trace written in the fixed location (/data/misc/perfetto-traces/
  // on Android, /tmp/ on Linux/Mac) and make sure it has the right contents.
  std::string trace_str;
  base::ReadFile(GetBugreportPath(), &trace_str);
  ASSERT_FALSE(trace_str.empty());
  protos::gen::Trace trace;
  ASSERT_TRUE(trace.ParseFromString(trace_str));
  int test_packets = 0;
  for (const auto& p : trace.packet())
    test_packets += p.has_for_testing() ? 1 : 0;
  ASSERT_EQ(test_packets, 3);
}

}  // namespace

TEST(PerfettoAndroidIntegrationTest, TestKmemActivity) {
  using C = protos::gen::VmstatCounters;

  base::TestTaskRunner task_runner;

  TestHelper helper(&task_runner);

  helper.StartServiceIfRequired();

#if PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
  ProbesProducerThread probes(GetTestProducerSockName());
  probes.Connect();
#endif

  auto* producer = helper.ConnectFakeProducer();
  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();
  helper.WaitForDataSourceConnected("linux.ftrace");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(1024);
  trace_config.set_unique_session_name("kmem_activity_test");

  auto* ftrace_ds_config = trace_config.add_data_sources()->mutable_config();
  ftrace_ds_config->set_name("linux.ftrace");
  protos::gen::FtraceConfig ftrace_config = CreateFtraceConfig({
      "vmscan/mm_vmscan_kswapd_wake",
      "vmscan/mm_vmscan_kswapd_sleep",
      "vmscan/mm_vmscan_direct_reclaim_begin",
      "vmscan/mm_vmscan_direct_reclaim_end",
      "compaction/mm_compaction_begin",
      "compaction/mm_compaction_end",
  });
  ftrace_ds_config->set_ftrace_config_raw(ftrace_config.SerializeAsString());

  auto* sys_stats_ds_config = trace_config.add_data_sources()->mutable_config();
  sys_stats_ds_config->set_name("linux.sys_stats");
  protos::gen::SysStatsConfig sys_stats_config;
  sys_stats_config.set_vmstat_period_ms(50);
  std::vector<C> vmstat_counters = {
      C::VMSTAT_NR_FREE_PAGES,
      C::VMSTAT_NR_SLAB_RECLAIMABLE,
      C::VMSTAT_NR_SLAB_UNRECLAIMABLE,
      C::VMSTAT_NR_ACTIVE_FILE,
      C::VMSTAT_NR_INACTIVE_FILE,
      C::VMSTAT_NR_ACTIVE_ANON,
      C::VMSTAT_NR_INACTIVE_ANON,
      C::VMSTAT_WORKINGSET_REFAULT,
      C::VMSTAT_WORKINGSET_ACTIVATE,
      C::VMSTAT_NR_FILE_PAGES,
      C::VMSTAT_PGPGIN,
      C::VMSTAT_PGPGOUT,
      C::VMSTAT_PSWPIN,
      C::VMSTAT_PSWPOUT,
      C::VMSTAT_PGSTEAL_KSWAPD_DMA,
      C::VMSTAT_PGSTEAL_KSWAPD_NORMAL,
      C::VMSTAT_PGSTEAL_KSWAPD_MOVABLE,
      C::VMSTAT_PGSTEAL_DIRECT_DMA,
      C::VMSTAT_PGSTEAL_DIRECT_NORMAL,
      C::VMSTAT_PGSTEAL_DIRECT_MOVABLE,
      C::VMSTAT_PGSCAN_KSWAPD_DMA,
      C::VMSTAT_PGSCAN_KSWAPD_NORMAL,
      C::VMSTAT_PGSCAN_KSWAPD_MOVABLE,
      C::VMSTAT_PGSCAN_DIRECT_DMA,
      C::VMSTAT_PGSCAN_DIRECT_NORMAL,
      C::VMSTAT_PGSCAN_DIRECT_MOVABLE,
      C::VMSTAT_COMPACT_MIGRATE_SCANNED,
      C::VMSTAT_COMPACT_FREE_SCANNED,
  };
  for (const auto& counter : vmstat_counters) {
    sys_stats_config.add_vmstat_counters(counter);
  }
  sys_stats_ds_config->set_sys_stats_config_raw(
      sys_stats_config.SerializeAsString());

  auto* trigger_cfg = trace_config.mutable_trigger_config();
  trigger_cfg->set_trigger_mode(
      protos::gen::TraceConfig::TriggerConfig::START_TRACING);
  trigger_cfg->set_trigger_timeout_ms(15000);
  auto* trigger = trigger_cfg->add_triggers();
  trigger->set_name("kmem_activity");
  // |stop_delay_ms| must be long enough that we can write the packets in
  // before the trace finishes.
  trigger->set_stop_delay_ms(1000);

  helper.StartTracing(trace_config);

  // Linearize with StartTracing. This ensures that the service has seen the
  // StartTracing IPC and has armed the triggers.
  helper.FlushAndWait(kDefaultTestTimeoutMs);

  // Generating synthetic memory pressure to trigger kmem activity is
  // inherently flaky on different devices. The same goes for writing
  // /proc/sys/vm/compact_memory to trigger compaction, since compaction is
  // only started if needed (even if explicitly triggered from proc).
  // Trigger kmem activity using perfetto trigger.
  producer->ActivateTrigger("kmem_activity");

  helper.WaitForTracingDisabled();

  helper.ReadData();
  helper.WaitForReadData();

  const auto& packets = helper.trace();
  ASSERT_GT(packets.size(), 0u);

  bool sys_stats_captured = false;
  for (const auto& packet : packets) {
    for (int ev = 0; ev < packet.ftrace_events().event_size(); ev++) {
      auto ftrace_event =
          packet.ftrace_events().event()[static_cast<size_t>(ev)];
      ASSERT_TRUE(ftrace_event.has_mm_vmscan_kswapd_wake() ||
                  ftrace_event.has_mm_vmscan_kswapd_sleep() ||
                  ftrace_event.has_mm_vmscan_direct_reclaim_begin() ||
                  ftrace_event.has_mm_vmscan_direct_reclaim_end() ||
                  ftrace_event.has_mm_compaction_begin() ||
                  ftrace_event.has_mm_compaction_end());
    }

    if (packet.has_sys_stats()) {
      sys_stats_captured = true;
      const auto& sys_stats = packet.sys_stats();
      const auto& vmstat = sys_stats.vmstat();
      ASSERT_GT(vmstat.size(), 0u);
      for (const auto& vmstat_value : vmstat) {
        ASSERT_NE(std::find(vmstat_counters.begin(), vmstat_counters.end(),
                            vmstat_value.key()),
                  vmstat_counters.end());
      }
    }
  }

  // Don't explicitly check that ftrace events were captured, since this test
  // doesn't rely on memory pressure.
  ASSERT_TRUE(sys_stats_captured);
}

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

TEST(PerfettoAndroidIntegrationTest, SaveForBugreport) {
  base::TestTaskRunner task_runner;

  TestHelper helper(&task_runner);
  helper.StartServiceIfRequired();
  helper.ConnectFakeProducer();
  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  TraceConfig trace_config;
  SetTraceConfigForBugreportTest(&trace_config);

  helper.StartTracing(trace_config);
  helper.WaitForProducerEnabled();

  EXPECT_TRUE(helper.SaveTraceForBugreportAndWait());
  helper.WaitForTracingDisabled();

  VerifyBugreportTraceContents();

  // Now read the trace returned to the consumer via ReadBuffers. This should
  // be always empty because --save-for-bugreport takes it over and makes the
  // buffers unreadable by the consumer (by virtue of force-setting
  // write_into_file, which is incompatible with ReadBuffers()). The only
  // content should be the |seized_for_bugreport| flag.
  helper.ReadData();
  helper.WaitForReadData();
  const auto& packets = helper.full_trace();
  ASSERT_EQ(packets.size(), 1u);
  for (const auto& p : packets) {
    ASSERT_TRUE(p.has_service_event());
    ASSERT_TRUE(p.service_event().seized_for_bugreport());
  }
}

// Tests that the SaveForBugreport logic works also for traces with
// write_into_file = true (with a passed file descriptor).
TEST(PerfettoAndroidIntegrationTest, SaveForBugreport_WriteIntoFile) {
  base::TestTaskRunner task_runner;

  TestHelper helper(&task_runner);
  helper.StartServiceIfRequired();
  helper.ConnectFakeProducer();
  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  TraceConfig trace_config;
  SetTraceConfigForBugreportTest(&trace_config);
  trace_config.set_file_write_period_ms(60000);  // Will never hit this.
  trace_config.set_write_into_file(true);

  auto pipe_pair = base::Pipe::Create();
  helper.StartTracing(trace_config, std::move(pipe_pair.wr));
  helper.WaitForProducerEnabled();

  EXPECT_TRUE(helper.SaveTraceForBugreportAndWait());
  helper.WaitForTracingDisabled();

  VerifyBugreportTraceContents();

  // Now read the original file descriptor passed in.
  std::string trace_bytes;
  ASSERT_TRUE(base::ReadPlatformHandle(*pipe_pair.rd, &trace_bytes));
  protos::gen::Trace trace;
  ASSERT_TRUE(trace.ParseFromString(trace_bytes));
  ASSERT_EQ(trace.packet().size(), 1u);
  for (const auto& p : trace.packet()) {
    ASSERT_TRUE(p.has_service_event());
    ASSERT_TRUE(p.service_event().seized_for_bugreport());
  }
}

// Tests that SaveTraceForBugreport() works also if the trace has triggers
// defined and those triggers have not been hit. This is a regression test for
// b/188008375 .
#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
// Disabled due to b/191940560
#define MAYBE_SaveForBugreport_Triggers DISABLED_SaveForBugreport_Triggers
#else
#define MAYBE_SaveForBugreport_Triggers SaveForBugreport_Triggers
#endif
TEST(PerfettoAndroidIntegrationTest, MAYBE_SaveForBugreport_Triggers) {
  base::TestTaskRunner task_runner;

  TestHelper helper(&task_runner);
  helper.StartServiceIfRequired();
  helper.ConnectFakeProducer();
  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  TraceConfig trace_config;
  SetTraceConfigForBugreportTest(&trace_config);
  trace_config.set_duration_ms(0);  // set_trigger_timeout_ms is used instead.
  auto* trigger_config = trace_config.mutable_trigger_config();
  trigger_config->set_trigger_timeout_ms(8.64e+7);
  trigger_config->set_trigger_mode(TraceConfig::TriggerConfig::STOP_TRACING);
  auto* trigger = trigger_config->add_triggers();
  trigger->set_name("trigger_name");
  trigger->set_stop_delay_ms(1);

  helper.StartTracing(trace_config);
  helper.WaitForProducerEnabled();

  EXPECT_TRUE(helper.SaveTraceForBugreportAndWait());
  helper.WaitForTracingDisabled();

  VerifyBugreportTraceContents();

  // Now read the original trace.
  helper.ReadData();
  helper.WaitForReadData();
  const auto& packets = helper.full_trace();
  ASSERT_EQ(packets.size(), 1u);
  for (const auto& p : packets) {
    ASSERT_TRUE(p.has_service_event());
    ASSERT_TRUE(p.service_event().seized_for_bugreport());
  }
}

}  // namespace perfetto

#endif  // PERFETTO_OS_ANDROID
