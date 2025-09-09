/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/thread_utils.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/tracing/core/trace_packet.h"
#include "perfetto/ext/tracing/core/tracing_service.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "src/base/test/test_task_runner.h"
#include "src/traced/probes/ftrace/ftrace_controller.h"
#include "test/gtest_and_gmock.h"
#include "test/test_helper.h"

#include "protos/perfetto/config/priority_boost/priority_boost_config.gen.h"
#include "protos/perfetto/config/trace_config.gen.h"

#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
#include "test/android_test_utils.h"
#endif

namespace perfetto {
namespace base {
// For ASSERT_EQ()
inline bool operator==(const SchedOsHooks::SchedOsConfig& lhs,
                       const SchedOsHooks::SchedOsConfig& rhs) {
  return std::tie(lhs.policy, lhs.rt_prio, lhs.nice) ==
         std::tie(rhs.policy, rhs.rt_prio, rhs.nice);
}

// For ASSERT_EQ()
inline std::ostream& operator<<(std::ostream& os,
                                const SchedOsHooks::SchedOsConfig& s) {
  return os << "SchedOsConfig{policy: " << s.policy << ", prio: " << s.rt_prio
            << ", nice: " << s.nice << "}";
}

inline std::string ToString(const SchedOsHooks::SchedOsConfig& cfg) {
  std::stringstream ss;
  ss << cfg;
  return ss.str();
}
}  // namespace base

namespace {
using ::testing::Eq;
using ::testing::NiceMock;
using ::testing::Return;

// We have two quite different code flows for this test, depending on the way it
// is being built and run.
//
// 1. When running as part of Android Tree (#if
// PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)), we test that the external,
// started by Android, traced and traced_probes daemons can change their
// priorities In this case we read /proc/%d/stat to query their state.
//
// 2. When running on Linux (#if PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)),
// we use MockSchedOsManager to check that the traced/traced_probes code changes
// their priority as expected.
// In this case, the traced/traced_probes are just the separate threads of the
// test binary. 'base::ScopedSchedBoost' doesn't expect that the priority is
// updated for the single thread and not the whole app, so we test only one
// thread at a time.
class PerfettoPriorityBoostIntegrationTest : public ::testing::Test {
 public:
  void SetUp() override {
#if PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
    base::ScopedSchedBoost::ResetForTesting(&sched_manager_);
#endif
  }

  base::SchedOsHooks::SchedOsConfig GetSchedInfo(int tid) {
#if PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
    PERFETTO_DCHECK(tid == sched_manager_.expected_boosted_thread);
    return sched_manager_.current_config;
#else
    return GetRealSchedInfo(tid);
#endif
  }

  static base::SchedOsHooks::SchedOsConfig GetRealSchedInfo(int tid) {
    base::StackString<128> stat_path("/proc/%d/stat", tid);
    std::string line;
    bool ok = base::ReadFile(stat_path.c_str(), &line);
    PERFETTO_DCHECK(ok);
    std::vector parts = base::SplitString(line, " ");
    int nice = base::StringToInt32(parts[18]).value();
    int rt_prio = base::StringToInt32(parts[39]).value();
    int policy = base::StringToInt32(parts[40]).value();
    return base::SchedOsHooks::SchedOsConfig{policy, rt_prio, nice};
  }

#if PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
  class MockSchedOsHooks : public base::SchedOsHooks {
   public:
    MockSchedOsHooks() : current_config(kInitConfig) {
      ON_CALL(*this, GetCurrentSchedConfig()).WillByDefault([&] {
        return current_config;
      });
      ON_CALL(*this, SetSchedConfig)
          .WillByDefault([&](const SchedOsConfig& arg) {
            PERFETTO_DCHECK(base::GetThreadId() == expected_boosted_thread);
            current_config = arg;
            return base::OkStatus();
          });
    }
    MOCK_METHOD(base::Status,
                SetSchedConfig,
                (const SchedOsConfig&),
                (override));
    MOCK_METHOD(base::StatusOr<base::SchedOsHooks::SchedOsConfig>,
                GetCurrentSchedConfig,
                (),
                (const, override));

    ~MockSchedOsHooks() override = default;

    base::PlatformThreadId expected_boosted_thread = -1;

    SchedOsConfig current_config;
    static constexpr SchedOsConfig kInitConfig{SCHED_OTHER, 0, 0};
  };

  NiceMock<MockSchedOsHooks> sched_manager_;
#endif
};

constexpr char kTestDataSourceName[] = "linux.system_info";

TraceConfig CreateTraceConfigWithDataSourcePriorityBoost(
    protos::gen::PriorityBoostConfig_BoostPolicy policy,
    uint32_t priority) {
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(64);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name(kTestDataSourceName);
  ds_config->set_target_buffer(0);

  auto* ds_priority_boost_config = ds_config->mutable_priority_boost();
  ds_priority_boost_config->set_policy(policy);
  ds_priority_boost_config->set_priority(priority);
  return trace_config;
}

void TestHelperStartTraceAndWaitForTraced(TestHelper& helper,
                                          const TraceConfig& trace_config) {
  static bool first_time = true;
  if (first_time) {
    first_time = false;
    helper.StartServiceIfRequired();
  }
  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  // Wait for the traced_probes service to connect. We want to start tracing
  // only after it connects, otherwise we'll start a tracing session with 0
  // producers connected (which is valid but not what we want here).
  helper.WaitForDataSourceConnected(kTestDataSourceName);

  helper.StartTracing(trace_config);
  helper.WaitForAllDataSourceStarted();
}

}  // namespace

TEST_F(PerfettoPriorityBoostIntegrationTest, TestTracedProbes) {
  base::TestTaskRunner task_runner;

  TestHelper helper_fifo_42(&task_runner);
  TestHelper helper_other_7(&task_runner);

  base::PlatformThreadId traced_probes_tid = -1;

#if PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
  ProbesProducerThread probes(GetTestProducerSockName());
  probes.Connect();
  traced_probes_tid = probes.runner()->GetThreadIdForTesting();
  sched_manager_.expected_boosted_thread = traced_probes_tid;
#elif PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
  traced_probes_tid = PidForProcessName("/system/bin/traced_probes");
#else
#error "Need to start daemons for Linux test or be built on Android."
#endif

  ASSERT_NE(traced_probes_tid, -1);

  base::SchedOsHooks::SchedOsConfig init_traced_probes_sched_info =
      GetSchedInfo(traced_probes_tid);

  TestHelperStartTraceAndWaitForTraced(
      helper_fifo_42,
      CreateTraceConfigWithDataSourcePriorityBoost(
          protos::gen::PriorityBoostConfig::POLICY_SCHED_FIFO, 42));

  TestHelperStartTraceAndWaitForTraced(
      helper_other_7,
      CreateTraceConfigWithDataSourcePriorityBoost(
          protos::gen::PriorityBoostConfig::POLICY_SCHED_OTHER, 7));

  {
    auto traced_probes_sched_info_boosted = GetSchedInfo(traced_probes_tid);
    ASSERT_THAT(traced_probes_sched_info_boosted,
                Eq(base::SchedOsHooks::SchedOsConfig{SCHED_FIFO, 42, 0}));
  }
  helper_fifo_42.DisableTracing();
  helper_fifo_42.WaitForTracingDisabled();

  {
    auto traced_probes_sched_info_stopped = GetSchedInfo(traced_probes_tid);
    ASSERT_THAT(traced_probes_sched_info_stopped,
                Eq(base::SchedOsHooks::SchedOsConfig{SCHED_OTHER, 0, -7}));
  }

  helper_other_7.DisableTracing();
  helper_other_7.WaitForTracingDisabled();

  {
    auto traced_probes_sched_info_stopped_2 = GetSchedInfo(traced_probes_tid);
    ASSERT_EQ(traced_probes_sched_info_stopped_2,
              init_traced_probes_sched_info);
  }
}

TEST_F(PerfettoPriorityBoostIntegrationTest, TestTraced) {
  base::TestTaskRunner task_runner;

  TestHelper helper(&task_runner);
  helper.StartServiceIfRequired();
  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  base::PlatformThreadId traced_tid = -1;

#if PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
  traced_tid = helper.service_thread()->GetThreadIdForTesting();
  sched_manager_.expected_boosted_thread = traced_tid;
#elif PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
  traced_tid = PidForProcessName("/system/bin/traced");
#else
#error "Need to start daemons for Linux test or be built on Android."
#endif

  ASSERT_NE(traced_tid, -1);

  base::SchedOsHooks::SchedOsConfig init_traced_sched_info =
      GetSchedInfo(traced_tid);

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(64);
  auto* priority_boost_config = trace_config.mutable_priority_boost();
  priority_boost_config->set_policy(
      protos::gen::PriorityBoostConfig::POLICY_SCHED_OTHER);
  priority_boost_config->set_priority(13);

  helper.StartTracing(trace_config);
  helper.WaitForAllDataSourceStarted();
  {
    auto traced_sched_info_boosted = GetSchedInfo(traced_tid);
    ASSERT_THAT(traced_sched_info_boosted,
                Eq(base::SchedOsHooks::SchedOsConfig{SCHED_OTHER, 0, -13}));
  }

  helper.FreeBuffers();
  helper.WaitForTracingDisabled();
  // The tracing session is destroyed at this point, and the priority is
  // restored to the initial value
  auto traced_sched_info_stopped = GetSchedInfo(traced_tid);
  ASSERT_EQ(traced_sched_info_stopped, init_traced_sched_info);
}

}  // namespace perfetto
#endif  // OS_ANDROID || OS_LINUX
