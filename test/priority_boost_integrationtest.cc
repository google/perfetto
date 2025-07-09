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
inline bool operator==(const SchedOsManager::SchedOsConfig& lhs,
                       const SchedOsManager::SchedOsConfig& rhs) {
  return std::tie(lhs.policy, lhs.rt_prio, lhs.nice) ==
         std::tie(rhs.policy, rhs.rt_prio, rhs.nice);
}

// For ASSERT_EQ()
inline std::ostream& operator<<(std::ostream& os,
                                const SchedOsManager::SchedOsConfig& s) {
  return os << "SchedOsConfig{policy: " << s.policy << ", prio: " << s.rt_prio
            << ", nice: " << s.nice << "}";
}

inline std::string ToString(const SchedOsManager::SchedOsConfig& cfg) {
  std::stringstream ss;
  ss << cfg;
  return ss.str();
}
}  // namespace base

namespace {
using ::testing::Eq;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::Return;

class PerfettoPriorityBoostIntegrationTest : public ::testing::Test {
 public:
  void SetUp() override {
    base::ScopedSchedBoost::ResetForTesting(&sched_manager_);
  }

  base::SchedOsManager::SchedOsConfig GetSchedInfo(int tid) {
#if PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
    PERFETTO_CHECK(tid == sched_manager_.expected_boosted_thread);
    return sched_manager_.current_config;
#else
    return GetRealSchedInfo(tid);
#endif
  }

#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
  static base::SchedOsManager::SchedOsConfig GetRealSchedInfo(int tid) {
    base::StackString<128> stat_path("/proc/%d/stat", tid);
    std::string line;
    PERFETTO_CHECK(base::ReadFile(stat_path.c_str(), &line));
    std::vector parts = base::SplitString(line, " ");
    int nice = base::StringToInt32(parts[18]).value();
    int rt_prio = base::StringToInt32(parts[39]).value();
    int policy = base::StringToInt32(parts[40]).value();
    return base::SchedOsManager::SchedOsConfig{policy, rt_prio, nice};
  }
#endif

  class MockSchedOsManager : public base::SchedOsManager {
   public:
    MockSchedOsManager() : current_config(kInitConfig) {
      ON_CALL(*this, GetCurrentSchedConfig()).WillByDefault(Invoke([&] {
        return current_config;
      }));
      ON_CALL(*this, SetSchedConfig)
          .WillByDefault(Invoke([&](const SchedOsConfig& arg) {
            PERFETTO_CHECK(base::GetThreadId() == expected_boosted_thread);
            current_config = arg;
            return base::OkStatus();
          }));
    }
    MOCK_METHOD(base::Status,
                SetSchedConfig,
                (const SchedOsConfig&),
                (override));
    MOCK_METHOD(base::StatusOr<base::SchedOsManager::SchedOsConfig>,
                GetCurrentSchedConfig,
                (),
                (const, override));

    ~MockSchedOsManager() override = default;

    base::PlatformThreadId expected_boosted_thread = -1;

    SchedOsConfig current_config;
    static constexpr SchedOsConfig kInitConfig{SCHED_OTHER, 0, 0};
  };

  NiceMock<MockSchedOsManager> sched_manager_;
};
}  // namespace

TEST_F(PerfettoPriorityBoostIntegrationTest, TestTracedProbes) {
  base::TestTaskRunner task_runner;

  TestHelper helper(&task_runner);
  helper.StartServiceIfRequired();

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

  base::SchedOsManager::SchedOsConfig init_traced_probes_sched_info =
      GetSchedInfo(traced_probes_tid);

  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  // Wait for the traced_probes service to connect. We want to start tracing
  // only after it connects, otherwise we'll start a tracing session with 0
  // producers connected (which is valid but not what we want here).
  helper.WaitForDataSourceConnected("linux.system_info");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(64);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("linux.system_info");
  ds_config->set_target_buffer(0);

  auto* ds_priority_boost_config = ds_config->mutable_priority_boost();
  ds_priority_boost_config->set_policy(
      protos::gen::PriorityBoostConfig::POLICY_SCHED_FIFO);
  ds_priority_boost_config->set_priority(42);

  helper.StartTracing(trace_config);
  helper.WaitForAllDataSourceStarted();

  PERFETTO_CHECK(traced_probes_tid != -1);
  {
    auto traced_probes_sched_info_boosted = GetSchedInfo(traced_probes_tid);
    PERFETTO_LOG("traced_probes_sched_info_boosted: %s",
                 ToString(traced_probes_sched_info_boosted).c_str());
    ASSERT_THAT(traced_probes_sched_info_boosted,
                Eq(base::SchedOsManager::SchedOsConfig{SCHED_FIFO, 42, 0}));
  }
  helper.DisableTracing();
  helper.WaitForTracingDisabled();

  {
    auto traced_probes_sched_info_stopped = GetSchedInfo(traced_probes_tid);
    PERFETTO_LOG("traced_probes_sched_info_stopped: %s",
                 ToString(traced_probes_sched_info_stopped).c_str());

    ASSERT_EQ(traced_probes_sched_info_stopped, init_traced_probes_sched_info);
  }
}

}  // namespace perfetto
#endif  // OS_ANDROID || OS_LINUX
