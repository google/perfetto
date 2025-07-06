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

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX)

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/temp_file.h"
#include "perfetto/ext/base/thread_utils.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/traced/traced.h"
#include "perfetto/ext/tracing/core/commit_data_request.h"
#include "perfetto/ext/tracing/core/trace_packet.h"
#include "perfetto/ext/tracing/core/tracing_service.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/tracing_service_state.h"
#include "src/base/test/test_task_runner.h"
#include "src/base/test/utils.h"
#include "src/traced/probes/ftrace/ftrace_controller.h"
#include "src/traced/probes/ftrace/ftrace_procfs.h"
#include "test/gtest_and_gmock.h"
#include "test/test_helper.h"

#include "protos/perfetto/config/priority_boost/priority_boost_config.gen.h"
#include "protos/perfetto/config/test_config.gen.h"
#include "protos/perfetto/config/trace_config.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_stats.gen.h"
#include "protos/perfetto/trace/perfetto/tracing_service_event.gen.h"
#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/trace.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
#include "test/android_test_utils.h"
#endif

namespace perfetto {

namespace {

using ::testing::ContainsRegex;
using ::testing::Each;
using ::testing::ElementsAreArray;
using ::testing::HasSubstr;
using ::testing::Property;
using ::testing::SizeIs;
using ::testing::UnorderedElementsAreArray;

class PerfettoPriorityBoostIntegrationTest : public ::testing::Test {
 public:
  struct SystemSchedInfo {
    std::string comm;
    int prio;
    int nice;
    int rt_prio;
    int policy;

    std::string ToString() const {
      std::ostringstream oss;
      oss << "SystemSchedInfo{";
      oss << "comm: " << comm << ", prio: " << prio << ", nice: " << nice
          << ", rt_prio: " << rt_prio << ", policy: " << policy;
      oss << "}";
      return oss.str();
    }
  };

  static std::optional<SystemSchedInfo> GetSchedInfo(int tid) {
    base::StackString<128> stat_path("/proc/%d/stat", tid);
    std::string line;
    if (!base::ReadFile(stat_path.c_str(), &line)) {
      PERFETTO_ELOG("Can't read file: %s", stat_path.c_str());
      return std::nullopt;
    }
    std::vector parts = base::SplitString(line, " ");
    int prio = base::StringToInt32(parts[17]).value();
    int nice = base::StringToInt32(parts[18]).value();
    int rt_prio = base::StringToInt32(parts[39]).value();
    int policy = base::StringToInt32(parts[40]).value();
    SystemSchedInfo info{parts[1], prio, nice, rt_prio, policy};
    return info;
  }
};
}  // namespace

TEST_F(PerfettoPriorityBoostIntegrationTest, TestTraced) {
  base::TestTaskRunner task_runner;

  TestHelper helper(&task_runner);
  helper.StartServiceIfRequired();

  int traced_tid = -1;
  int traced_probes_tid = -1;

#if PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
  PERFETTO_DLOG("PERFETTO_START_DAEMONS");
  ProbesProducerThread probes(GetTestProducerSockName());
  probes.Connect();
#elif
  GTEST_SKIP() << "This test requires PERFETTO_START_DAEMONS or ANDROID_BUILD";
  PERFETTO_DLOG("NOT PERFETTO_START_DAEMONS");
#endif

  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();
  helper.WaitForDataSourceConnected("linux.ftrace");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(64);
  auto* priority_boost_config = trace_config.mutable_priority_boost();
  priority_boost_config->set_policy(
      protos::gen::PriorityBoostConfig::POLICY_SCHED_FIFO);
  priority_boost_config->set_priority(42);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("linux.ftrace");
  ds_config->set_target_buffer(0);

  auto* ds_priority_boost_config = ds_config->mutable_priority_boost();
  ds_priority_boost_config->set_policy(
      protos::gen::PriorityBoostConfig::POLICY_SCHED_OTHER);
  ds_priority_boost_config->set_priority(7);

  protos::gen::FtraceConfig ftrace_config;
  ftrace_config.add_ftrace_events("sched_switch");
  ds_config->set_ftrace_config_raw(ftrace_config.SerializeAsString());

  helper.StartTracing(trace_config);
  helper.WaitForDataSourceConnected("linux.ftrace");

#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
  PERFETTO_DLOG("PERFETTO_ANDROID_BUILD");
  traced_tid = PidForProcessName("/system/bin/traced");
  traced_probes_tid = PidForProcessName("/system/bin/traced_probes");
#else
  traced_tid = helper.service_thread()->GetThreadIdForTesting();
  traced_probes_tid = probes.runner()->GetThreadIdForTesting();
#endif

  PERFETTO_DLOG("traced_tid: %d, traced_probes_tid: %d", traced_tid,
                traced_probes_tid);

  auto traced_sched_info = GetSchedInfo(traced_tid);
  auto traced_probes_sched_info = GetSchedInfo(traced_probes_tid);

  PERFETTO_DLOG("traced_sched_info: %s",
                traced_sched_info.value().ToString().c_str());
  PERFETTO_DLOG("traced_probes_sched_info: %s",
                traced_probes_sched_info.value().ToString().c_str());

  // helper.FlushAndWait(kDefaultFlushTimeoutMs);
  helper.DisableTracing();
  helper.WaitForTracingDisabled();

  helper.ReadData();
  helper.WaitForReadData();

  PERFETTO_DLOG("Tracing disabled, data read");
  PERFETTO_DLOG("traced_sched_info: %s",
                traced_sched_info.value().ToString().c_str());
  PERFETTO_DLOG("traced_probes_sched_info: %s",
                traced_probes_sched_info.value().ToString().c_str());
}

}  // namespace perfetto
#endif  // OS_ANDROID || OS_LINUX
