/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/profiling/perf/event_config.h"

#include <linux/perf_event.h>
#include <stdint.h>
#include <time.h>
#include <optional>

#include "perfetto/base/logging.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/common/perf_events.gen.h"
#include "protos/perfetto/config/data_source_config.gen.h"
#include "protos/perfetto/config/profiling/perf_event_config.gen.h"

using ::testing::UnorderedElementsAreArray;

namespace perfetto {
namespace profiling {
namespace {

bool IsPowerOfTwo(size_t v) {
  return (v != 0 && ((v & (v - 1)) == 0));
}

std::optional<EventConfig> CreateEventConfig(
    const protos::gen::PerfEventConfig& perf_cfg,
    const EventConfig::tracepoint_id_fn_t& tracepoint_id_lookup =
        [](const std::string&, const std::string&) { return 0; }) {
  protos::gen::DataSourceConfig ds_cfg;
  ds_cfg.set_perf_event_config_raw(perf_cfg.SerializeAsString());
  return EventConfig::Create(perf_cfg, ds_cfg,
                             /*process_sharding=*/std::nullopt,
                             tracepoint_id_lookup);
}

TEST(EventConfigTest, AttrStructConstructed) {
  protos::gen::PerfEventConfig cfg;
  std::optional<EventConfig> event_config = CreateEventConfig(cfg);

  ASSERT_TRUE(event_config.has_value());
  ASSERT_TRUE(event_config->perf_attr() != nullptr);
}

TEST(EventConfigTest, RingBufferPagesValidated) {
  {  // if unset, a default is used
    protos::gen::PerfEventConfig cfg;
    std::optional<EventConfig> event_config = CreateEventConfig(cfg);

    ASSERT_TRUE(event_config.has_value());
    ASSERT_GT(event_config->ring_buffer_pages(), 0u);
    ASSERT_TRUE(IsPowerOfTwo(event_config->ring_buffer_pages()));
  }
  {  // power of two pages accepted
    uint32_t num_pages = 128;
    protos::gen::PerfEventConfig cfg;
    cfg.set_ring_buffer_pages(num_pages);
    std::optional<EventConfig> event_config = CreateEventConfig(cfg);

    ASSERT_TRUE(event_config.has_value());
    ASSERT_EQ(event_config->ring_buffer_pages(), num_pages);
  }
  {  // entire config rejected if not a power of two of pages
    protos::gen::PerfEventConfig cfg;
    cfg.set_ring_buffer_pages(7);
    std::optional<EventConfig> event_config = CreateEventConfig(cfg);

    ASSERT_FALSE(event_config.has_value());
  }
}

TEST(EventConfigTest, ReadTickPeriodDefaultedIfUnset) {
  {  // if unset, a default is used
    protos::gen::PerfEventConfig cfg;
    std::optional<EventConfig> event_config = CreateEventConfig(cfg);

    ASSERT_TRUE(event_config.has_value());
    ASSERT_GT(event_config->read_tick_period_ms(), 0u);
  }
  {  // otherwise, given value used
    uint32_t period_ms = 250;
    protos::gen::PerfEventConfig cfg;
    cfg.set_ring_buffer_read_period_ms(period_ms);
    std::optional<EventConfig> event_config = CreateEventConfig(cfg);

    ASSERT_TRUE(event_config.has_value());
    ASSERT_EQ(event_config->read_tick_period_ms(), period_ms);
  }
}

TEST(EventConfigTest, RemotePeriodTimeoutDefaultedIfUnset) {
  {  // if unset, a default is used
    protos::gen::PerfEventConfig cfg;
    std::optional<EventConfig> event_config = CreateEventConfig(cfg);

    ASSERT_TRUE(event_config.has_value());
    ASSERT_GT(event_config->remote_descriptor_timeout_ms(), 0u);
  }
  {  // otherwise, given value used
    uint32_t timeout_ms = 300;
    protos::gen::PerfEventConfig cfg;
    cfg.set_remote_descriptor_timeout_ms(timeout_ms);
    std::optional<EventConfig> event_config = CreateEventConfig(cfg);

    ASSERT_TRUE(event_config.has_value());
    ASSERT_EQ(event_config->remote_descriptor_timeout_ms(), timeout_ms);
  }
}

TEST(EventConfigTest, SelectSamplingInterval) {
  {  // period:
    protos::gen::PerfEventConfig cfg;
    cfg.mutable_timebase()->set_period(100);
    std::optional<EventConfig> event_config = CreateEventConfig(cfg);

    ASSERT_TRUE(event_config.has_value());
    EXPECT_FALSE(event_config->perf_attr()->freq);
    EXPECT_EQ(event_config->perf_attr()->sample_period, 100u);
  }
  {  // frequency:
    protos::gen::PerfEventConfig cfg;
    cfg.mutable_timebase()->set_frequency(4000);
    std::optional<EventConfig> event_config = CreateEventConfig(cfg);

    ASSERT_TRUE(event_config.has_value());
    EXPECT_TRUE(event_config->perf_attr()->freq);
    EXPECT_EQ(event_config->perf_attr()->sample_freq, 4000u);
  }
  {  // legacy frequency field:
    protos::gen::PerfEventConfig cfg;
    cfg.set_sampling_frequency(5000);
    std::optional<EventConfig> event_config = CreateEventConfig(cfg);

    ASSERT_TRUE(event_config.has_value());
    EXPECT_TRUE(event_config->perf_attr()->freq);
    EXPECT_EQ(event_config->perf_attr()->sample_freq, 5000u);
  }
  {  // default is 10 Hz (implementation-defined)
    protos::gen::PerfEventConfig cfg;
    std::optional<EventConfig> event_config = CreateEventConfig(cfg);

    ASSERT_TRUE(event_config.has_value());
    EXPECT_TRUE(event_config->perf_attr()->freq);
    EXPECT_EQ(event_config->perf_attr()->sample_freq, 10u);
  }
}

TEST(EventConfigTest, SelectTimebaseEvent) {
  auto id_lookup = [](const std::string& group, const std::string& name) {
    return (group == "sched" && name == "sched_switch") ? 42 : 0;
  };

  {
    protos::gen::PerfEventConfig cfg;
    protos::gen::PerfEvents::Tracepoint* mutable_tracepoint =
        cfg.mutable_timebase()->mutable_tracepoint();
    mutable_tracepoint->set_name("sched:sched_switch");

    std::optional<EventConfig> event_config = CreateEventConfig(cfg, id_lookup);

    ASSERT_TRUE(event_config.has_value());
    EXPECT_EQ(event_config->perf_attr()->type, PERF_TYPE_TRACEPOINT);
    EXPECT_EQ(event_config->perf_attr()->config, 42u);
  }
  {  // default is the CPU timer:
    protos::gen::PerfEventConfig cfg;
    cfg.mutable_timebase()->set_frequency(1000);
    std::optional<EventConfig> event_config = CreateEventConfig(cfg);

    ASSERT_TRUE(event_config.has_value());
    EXPECT_EQ(event_config->perf_attr()->type, PERF_TYPE_SOFTWARE);
    EXPECT_EQ(event_config->perf_attr()->config, PERF_COUNT_SW_CPU_CLOCK);
  }
}

TEST(EventConfigTest, ParseTargetfilter) {
  {
    protos::gen::PerfEventConfig cfg;
    auto* mutable_scope = cfg.mutable_callstack_sampling()->mutable_scope();
    mutable_scope->add_target_pid(42);
    mutable_scope->add_target_cmdline("traced_probes");
    mutable_scope->add_target_cmdline("traced");
    mutable_scope->set_additional_cmdline_count(3);
    mutable_scope->add_exclude_cmdline("heapprofd");

    std::optional<EventConfig> event_config = CreateEventConfig(cfg);

    ASSERT_TRUE(event_config.has_value());
    const auto& filter = event_config->filter();
    EXPECT_THAT(filter.pids, UnorderedElementsAreArray({42}));
    EXPECT_THAT(filter.cmdlines,
                UnorderedElementsAreArray({"traced_probes", "traced"}));
    EXPECT_EQ(filter.additional_cmdline_count, 3u);
    EXPECT_TRUE(filter.exclude_pids.empty());
    EXPECT_THAT(filter.exclude_cmdlines,
                UnorderedElementsAreArray({"heapprofd"}));
  }
  {  // legacy:
    protos::gen::PerfEventConfig cfg;
    cfg.set_all_cpus(true);
    cfg.add_target_pid(42);
    cfg.add_target_cmdline("traced_probes");
    cfg.add_target_cmdline("traced");
    cfg.set_additional_cmdline_count(3);
    cfg.add_exclude_cmdline("heapprofd");

    std::optional<EventConfig> event_config = CreateEventConfig(cfg);

    ASSERT_TRUE(event_config.has_value());
    const auto& filter = event_config->filter();
    EXPECT_THAT(filter.pids, UnorderedElementsAreArray({42}));
    EXPECT_THAT(filter.cmdlines,
                UnorderedElementsAreArray({"traced_probes", "traced"}));
    EXPECT_EQ(filter.additional_cmdline_count, 3u);
    EXPECT_TRUE(filter.exclude_pids.empty());
    EXPECT_THAT(filter.exclude_cmdlines,
                UnorderedElementsAreArray({"heapprofd"}));
  }
}

TEST(EventConfigTest, CounterOnlyModeDetection) {
  {  // hardware counter:
    protos::gen::PerfEventConfig cfg;
    auto* mutable_timebase = cfg.mutable_timebase();
    mutable_timebase->set_period(500);
    mutable_timebase->set_counter(protos::gen::PerfEvents::HW_CPU_CYCLES);

    std::optional<EventConfig> event_config = CreateEventConfig(cfg);

    ASSERT_TRUE(event_config.has_value());
    EXPECT_EQ(event_config->perf_attr()->type, PERF_TYPE_HARDWARE);
    EXPECT_EQ(event_config->perf_attr()->config, PERF_COUNT_HW_CPU_CYCLES);
    EXPECT_EQ(event_config->perf_attr()->sample_type &
                  (PERF_SAMPLE_STACK_USER | PERF_SAMPLE_REGS_USER),
              0u);
  }
  {  // software counter:
    protos::gen::PerfEventConfig cfg;
    auto* mutable_timebase = cfg.mutable_timebase();
    mutable_timebase->set_period(500);
    mutable_timebase->set_counter(protos::gen::PerfEvents::SW_PAGE_FAULTS);

    std::optional<EventConfig> event_config = CreateEventConfig(cfg);

    ASSERT_TRUE(event_config.has_value());
    EXPECT_EQ(event_config->perf_attr()->type, PERF_TYPE_SOFTWARE);
    EXPECT_EQ(event_config->perf_attr()->config, PERF_COUNT_SW_PAGE_FAULTS);
    EXPECT_EQ(event_config->perf_attr()->sample_type &
                  (PERF_SAMPLE_STACK_USER | PERF_SAMPLE_REGS_USER),
              0u);
  }
}

// Tests the polled (periodic read syscall) configuration.
TEST(EventConfigTest, CounterPolling) {
  {  // single counter:
    protos::gen::PerfEventConfig cfg;
    auto* mutable_timebase = cfg.mutable_timebase();
    mutable_timebase->set_poll_period_ms(200);
    mutable_timebase->set_counter(protos::gen::PerfEvents::HW_CPU_CYCLES);

    std::optional<EventConfig> event_config = CreateEventConfig(cfg);

    ASSERT_TRUE(event_config.has_value());
    EXPECT_EQ(event_config->perf_attr()->type, PERF_TYPE_HARDWARE);
    EXPECT_EQ(event_config->perf_attr()->config, PERF_COUNT_HW_CPU_CYCLES);
    EXPECT_EQ(event_config->perf_attr()->read_format, PERF_FORMAT_GROUP);
    EXPECT_EQ(event_config->perf_attr()->sample_type, 0u);

    EXPECT_EQ(event_config->recording_mode(), RecordingMode::kPolling);
  }
  {  // multiple counters:
    protos::gen::PerfEventConfig cfg;
    auto* mutable_timebase = cfg.mutable_timebase();
    mutable_timebase->set_poll_period_ms(200);
    mutable_timebase->set_counter(protos::gen::PerfEvents::SW_PAGE_FAULTS);

    auto* counter_follower = cfg.add_followers();
    counter_follower->set_counter(
        protos::gen::PerfEvents::HW_BRANCH_INSTRUCTIONS);

    std::optional<EventConfig> event_config = CreateEventConfig(cfg);

    ASSERT_TRUE(event_config.has_value());
    EXPECT_EQ(event_config->perf_attr()->type, PERF_TYPE_SOFTWARE);
    EXPECT_EQ(event_config->perf_attr()->config, PERF_COUNT_SW_PAGE_FAULTS);
    EXPECT_EQ(event_config->perf_attr()->read_format, PERF_FORMAT_GROUP);
    EXPECT_EQ(event_config->perf_attr()->sample_type, 0u);

    ASSERT_EQ(event_config->follower_events().size(), 1u);
    ASSERT_EQ(event_config->perf_attr_followers().size(), 1u);

    const auto& follower = event_config->perf_attr_followers().at(0);
    EXPECT_EQ(follower.type, PERF_TYPE_HARDWARE);
    EXPECT_EQ(follower.config, PERF_COUNT_HW_BRANCH_INSTRUCTIONS);

    EXPECT_EQ(event_config->recording_mode(), RecordingMode::kPolling);
  }
}

TEST(EventConfigTest, CallstackSamplingModeDetection) {
  {  // set-but-empty |callstack_sampling| field enables userspace callstacks
    protos::gen::PerfEventConfig cfg;
    cfg.mutable_callstack_sampling();  // set field

    std::optional<EventConfig> event_config = CreateEventConfig(cfg);

    ASSERT_TRUE(event_config.has_value());
    EXPECT_TRUE(event_config->sample_callstacks());
    EXPECT_TRUE(event_config->user_frames());
    EXPECT_FALSE(event_config->kernel_frames());
    EXPECT_EQ(
        event_config->perf_attr()->sample_type &
            (PERF_SAMPLE_STACK_USER | PERF_SAMPLE_REGS_USER),
        static_cast<uint64_t>(PERF_SAMPLE_STACK_USER | PERF_SAMPLE_REGS_USER));

    EXPECT_NE(event_config->perf_attr()->sample_regs_user, 0u);
    EXPECT_NE(event_config->perf_attr()->sample_stack_user, 0u);
  }
  {  // kernel-only callstacks
    protos::gen::PerfEventConfig cfg;
    cfg.mutable_callstack_sampling()->set_kernel_frames(true);
    cfg.mutable_callstack_sampling()->set_user_frames(
        protos::gen::PerfEventConfig::UNWIND_SKIP);

    std::optional<EventConfig> event_config = CreateEventConfig(cfg);

    ASSERT_TRUE(event_config.has_value());
    EXPECT_TRUE(event_config->sample_callstacks());
    EXPECT_FALSE(event_config->user_frames());
    EXPECT_TRUE(event_config->kernel_frames());
    EXPECT_EQ(event_config->perf_attr()->sample_type &
                  (PERF_SAMPLE_STACK_USER | PERF_SAMPLE_REGS_USER),
              0u);
    EXPECT_EQ(event_config->perf_attr()->sample_type & (PERF_SAMPLE_CALLCHAIN),
              static_cast<uint64_t>(PERF_SAMPLE_CALLCHAIN));

    EXPECT_EQ(event_config->perf_attr()->sample_regs_user, 0u);
    EXPECT_EQ(event_config->perf_attr()->sample_stack_user, 0u);

    EXPECT_NE(event_config->perf_attr()->exclude_callchain_user, 0u);
  }
}

TEST(EventConfigTest, EnableKernelFrames) {
  {
    protos::gen::PerfEventConfig cfg;
    cfg.mutable_callstack_sampling()->set_kernel_frames(true);
    std::optional<EventConfig> event_config = CreateEventConfig(cfg);

    ASSERT_TRUE(event_config.has_value());
    EXPECT_TRUE(event_config->kernel_frames());
  }
  {  // legacy config:
    protos::gen::PerfEventConfig cfg;
    cfg.set_all_cpus(true);  // used to detect compat mode
    cfg.set_kernel_frames(true);
    std::optional<EventConfig> event_config = CreateEventConfig(cfg);

    ASSERT_TRUE(event_config.has_value());
    EXPECT_TRUE(event_config->kernel_frames());
  }
  {  // default is false
    protos::gen::PerfEventConfig cfg;
    std::optional<EventConfig> event_config = CreateEventConfig(cfg);

    ASSERT_TRUE(event_config.has_value());
    EXPECT_FALSE(event_config->kernel_frames());
  }
}

TEST(EventConfigTest, TimestampClockId) {
  {  // if unset, a default is used
    protos::gen::PerfEventConfig cfg;
    std::optional<EventConfig> event_config = CreateEventConfig(cfg);

    ASSERT_TRUE(event_config.has_value());
    EXPECT_TRUE(event_config->perf_attr()->use_clockid);
    EXPECT_EQ(event_config->perf_attr()->clockid, CLOCK_MONOTONIC_RAW);
  }
  {  // explicit boottime
    protos::gen::PerfEventConfig cfg;
    cfg.mutable_timebase()->set_timestamp_clock(
        protos::gen::PerfEvents::PERF_CLOCK_BOOTTIME);
    std::optional<EventConfig> event_config = CreateEventConfig(cfg);

    ASSERT_TRUE(event_config.has_value());
    EXPECT_TRUE(event_config->perf_attr()->use_clockid);
    EXPECT_EQ(event_config->perf_attr()->clockid, CLOCK_BOOTTIME);
  }
  {  // explicit monotonic
    protos::gen::PerfEventConfig cfg;
    cfg.mutable_timebase()->set_timestamp_clock(
        protos::gen::PerfEvents::PERF_CLOCK_MONOTONIC);
    std::optional<EventConfig> event_config = CreateEventConfig(cfg);

    ASSERT_TRUE(event_config.has_value());
    EXPECT_TRUE(event_config->perf_attr()->use_clockid);
    EXPECT_EQ(event_config->perf_attr()->clockid, CLOCK_MONOTONIC);
  }
}

TEST(EventConfigTest, GroupMultipleType) {
  protos::gen::PerfEventConfig cfg;
  {
    // timebase:
    auto* mutable_timebase = cfg.mutable_timebase();
    mutable_timebase->set_period(500);
    mutable_timebase->set_counter(protos::gen::PerfEvents::HW_CPU_CYCLES);
    mutable_timebase->set_name("timebase");

    // raw follower:
    auto* raw_follower = cfg.add_followers();
    raw_follower->set_name("raw");
    auto* raw_event = raw_follower->mutable_raw_event();
    raw_event->set_type(8);
    raw_event->set_config(8);

    // HW counter follower:
    auto* counter_follower = cfg.add_followers();
    counter_follower->set_name("counter");
    counter_follower->set_counter(
        protos::gen::PerfEvents::HW_BRANCH_INSTRUCTIONS);

    // tracepoint follower:
    auto* tracepoint_follower = cfg.add_followers();
    tracepoint_follower->set_name("tracepoint");
    auto* tracepoint_event = tracepoint_follower->mutable_tracepoint();
    tracepoint_event->set_name("sched:sched_switch");
  }

  auto id_lookup = [](const std::string& group, const std::string& name) {
    return (group == "sched" && name == "sched_switch") ? 42 : 0;
  };
  std::optional<EventConfig> event_config = CreateEventConfig(cfg, id_lookup);

  ASSERT_TRUE(event_config.has_value());
  EXPECT_EQ(event_config->perf_attr()->type, PERF_TYPE_HARDWARE);
  EXPECT_EQ(event_config->perf_attr()->config, PERF_COUNT_HW_CPU_CYCLES);
  EXPECT_EQ(event_config->perf_attr()->sample_type &
                (PERF_SAMPLE_STACK_USER | PERF_SAMPLE_REGS_USER),
            0u);
  EXPECT_EQ(event_config->perf_attr()->read_format, PERF_FORMAT_GROUP);

  ASSERT_EQ(event_config->perf_attr_followers().size(), 3u);

  const auto& raw_event = event_config->perf_attr_followers().at(0);
  EXPECT_EQ(raw_event.type, 8u);
  EXPECT_EQ(raw_event.config, 8u);
  EXPECT_TRUE(raw_event.sample_type & PERF_SAMPLE_READ);

  const auto& hw_counter = event_config->perf_attr_followers().at(1);
  EXPECT_EQ(hw_counter.type, PERF_TYPE_HARDWARE);
  EXPECT_EQ(hw_counter.config, PERF_COUNT_HW_BRANCH_INSTRUCTIONS);
  EXPECT_TRUE(hw_counter.sample_type & PERF_SAMPLE_READ);

  const auto& tracepoint = event_config->perf_attr_followers().at(2);
  EXPECT_EQ(tracepoint.type, PERF_TYPE_TRACEPOINT);
  EXPECT_EQ(tracepoint.config, 42u);
  EXPECT_TRUE(tracepoint.sample_type & PERF_SAMPLE_READ);
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto
