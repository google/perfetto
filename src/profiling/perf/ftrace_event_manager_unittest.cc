/*
 * Copyright (C) 2025 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "src/profiling/perf/ftrace_event_manager.h"
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "perfetto/ext/tracing/core/tracing_service.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_stats.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_stats.pbzero.h"
#include "src/profiling/perf/common_types.h"
#include "src/profiling/perf/event_config.h"
#include "src/traced/probes/ftrace/proto_translation_table.h"
#include "src/traced/probes/ftrace/test/cpu_reader_support.h"
#include "src/tracing/core/trace_writer_for_testing.h"
#include "src/tracing/test/mock_producer_endpoint.h"

namespace perfetto::profiling {

namespace {

using perfetto::BufferExhaustedPolicy;
using perfetto::BufferID;
using perfetto::TraceWriter;
using perfetto::TracingService;
using protos::pbzero::FtraceParseStatus;

static constexpr uint16_t kFtracePrintId = 5;
static constexpr uint16_t kSchedWakingId = 44;
static constexpr uint16_t kSchedSwitchId = 47;
static constexpr uint16_t kOtherEventId = 10;
static constexpr size_t kSchedSwitchSize = 64;
static constexpr size_t kSchedWakingSize = 40;

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

class ProducerEndpointForTesting : public MockProducerEndpoint {
 public:
  ProducerEndpointForTesting() = default;
  ~ProducerEndpointForTesting() override = default;

  std::unique_ptr<TraceWriter> CreateTraceWriter(
      BufferID /*target_buffer*/,
      BufferExhaustedPolicy /*policy*/) override {
    auto w = std::make_unique<TraceWriterForTesting>();
    last_writer_ = w.get();
    return w;
  }

  TraceWriterForTesting* last_writer() const { return last_writer_; }

 private:
  TraceWriterForTesting* last_writer_ = nullptr;
};

template <typename T>
inline void WriteAt(std::vector<uint8_t>& buf, size_t off, const T& v) {
  ASSERT_LE(off + sizeof(T), buf.size());
  std::memcpy(buf.data() + off, &v, sizeof(T));
}

inline void WriteFixedCharArray(std::vector<uint8_t>& buf,
                                size_t off,
                                size_t len,
                                const std::string& s) {
  std::memset(buf.data() + off, 0, len);
  const size_t n = std::min(len, s.size());
  std::memcpy(buf.data() + off, s.data(), n);
}

ParsedSample MakeSchedSwitchSample(uint32_t cpu,
                                   uint64_t ts,
                                   int32_t common_pid,
                                   const char* prev_comm,
                                   int32_t prev_pid,
                                   int32_t prev_prio,
                                   int64_t prev_state,
                                   const char* next_comm,
                                   int32_t next_pid,
                                   int32_t next_prio) {
  ParsedSample s{};
  s.common.cpu = cpu;
  s.common.timestamp = ts;

  s.raw_data.assign(kSchedSwitchSize, 0);

  // --- Common header ---
  WriteAt<uint16_t>(s.raw_data, 0, kSchedSwitchId);
  WriteAt<uint8_t>(s.raw_data, 2, 0);
  WriteAt<uint8_t>(s.raw_data, 3, 0);
  WriteAt<int32_t>(s.raw_data, 4, common_pid);
  // --- Payload ---
  WriteFixedCharArray(s.raw_data, 8, 16, prev_comm);
  WriteAt<int32_t>(s.raw_data, 24, prev_pid);
  WriteAt<int32_t>(s.raw_data, 28, prev_prio);
  WriteAt<int64_t>(s.raw_data, 32, prev_state);
  WriteFixedCharArray(s.raw_data, 40, 16, next_comm);
  WriteAt<int32_t>(s.raw_data, 56, next_pid);
  WriteAt<int32_t>(s.raw_data, 60, next_prio);

  return s;
}

ParsedSample MakeSchedWakingSample(uint32_t cpu,
                                   uint64_t ts,
                                   int32_t common_pid,
                                   std::string comm,
                                   int32_t pid,
                                   int32_t prio,
                                   int32_t success,
                                   int32_t target_cpu) {
  ParsedSample s{};
  s.common.timestamp = ts;
  s.common.cpu = cpu;

  s.raw_data.resize(kSchedWakingSize);

  // --- Common header ---
  WriteAt<uint16_t>(s.raw_data, 0, kSchedWakingId);
  WriteAt<uint8_t>(s.raw_data, 2, 0);
  WriteAt<uint8_t>(s.raw_data, 3, 0);
  WriteAt<int32_t>(s.raw_data, 4, common_pid);
  // --- Payload ---
  WriteFixedCharArray(s.raw_data, 8, 16, comm);
  WriteAt<int32_t>(s.raw_data, 24, pid);
  WriteAt<int32_t>(s.raw_data, 28, prio);
  WriteAt<int32_t>(s.raw_data, 32, success);
  WriteAt<int32_t>(s.raw_data, 36, target_cpu);

  return s;
}

ParsedSample MakePrintEventSample(uint32_t cpu,
                                  uint64_t ts,
                                  int32_t common_pid,
                                  uint64_t ip,
                                  std::string message) {
  ParsedSample s{};
  s.common.timestamp = ts;
  s.common.cpu = cpu;

  s.raw_data.resize(16 + message.size() + 1);
  WriteAt<uint16_t>(s.raw_data, 0, kFtracePrintId);
  WriteAt<uint8_t>(s.raw_data, 2, 0);
  WriteAt<uint8_t>(s.raw_data, 3, 0);
  WriteAt<int32_t>(s.raw_data, 4, common_pid);
  WriteAt<uint64_t>(s.raw_data, 8, ip);
  WriteFixedCharArray(s.raw_data, 16, message.size(), message);

  return s;
}

ParsedSample MakeFailingSample(uint16_t eventId) {
  // creating a sample whose size is shorter such that
  // the event id will match, but parsing will fail.
  ParsedSample s{};
  s.common.timestamp = 1308020252351000ULL;
  s.common.cpu = 0;
  s.raw_data.resize(16);
  WriteAt<uint16_t>(s.raw_data, 0, eventId);
  return s;
}

class FtraceEventManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    endpoint_ = std::make_unique<ProducerEndpointForTesting>();
    table_ = GetTable("synthetic");
    protos::gen::PerfEventConfig cfg;
    auto event_config = CreateEventConfig(cfg);
    ASSERT_TRUE(event_config.has_value());
    event_config_.emplace(std::move(event_config.value()));
  }

  BufferID buffer_id_ = 0;
  std::unique_ptr<ProducerEndpointForTesting> endpoint_;
  perfetto::ProtoTranslationTable* table_;
  std::optional<EventConfig> event_config_;
};

TEST_F(FtraceEventManagerTest, ConstructAndFlushDoesNotCrash) {
  auto writer =
      endpoint_->CreateTraceWriter(BufferID(0), BufferExhaustedPolicy::kStall);
  FtraceEventManager mgr(table_, &event_config_.value(), writer.get(), true,
                         endpoint_.get(), BufferID(0));
  // No samples -> flush should be fine.
  mgr.Flush();
}

TEST_F(FtraceEventManagerTest, EmptyRawSample) {
  auto writer =
      endpoint_->CreateTraceWriter(BufferID(0), BufferExhaustedPolicy::kStall);
  FtraceEventManager mgr(table_, &event_config_.value(), writer.get(), true,
                         endpoint_.get(), BufferID(0));
  ParsedSample s{};
  s.raw_data.clear();
  auto status = mgr.ProcessSample(s);
  ASSERT_EQ(status, FtraceParseStatus::FTRACE_STATUS_UNSPECIFIED);
}

TEST_F(FtraceEventManagerTest, DropsUnknownEventId) {
  auto writer =
      endpoint_->CreateTraceWriter(BufferID(0), BufferExhaustedPolicy::kStall);
  FtraceEventManager mgr(table_, &event_config_.value(), writer.get(), true,
                         endpoint_.get(), BufferID(0));
  ParsedSample s{};
  s.common.cpu = 0;
  s.common.timestamp = 123;
  // raw_data starts with uint16_t ftrace_event_id.
  uint16_t unknown_id = 0xFFFF;
  s.raw_data.resize(sizeof(uint16_t));
  std::memcpy(s.raw_data.data(), &unknown_id, sizeof(uint16_t));
  auto status = mgr.ProcessSample(s);
  ASSERT_EQ(status, FtraceParseStatus::FTRACE_STATUS_INVALID_EVENT);
}

TEST_F(FtraceEventManagerTest, DropsShortCompactSchedSwitch) {
  auto writer =
      endpoint_->CreateTraceWriter(BufferID(0), BufferExhaustedPolicy::kStall);
  FtraceEventManager mgr(table_, &event_config_.value(), writer.get(), true,
                         endpoint_.get(), BufferID(0));
  // Shorter compact sched switch
  ParsedSample s2 = MakeFailingSample(kSchedSwitchId);
  auto status = mgr.ProcessSample(s2);
  ASSERT_EQ(status, FtraceParseStatus::FTRACE_STATUS_SHORT_COMPACT_EVENT);
}

TEST_F(FtraceEventManagerTest, DropsShortCompactSchedWaking) {
  auto writer =
      endpoint_->CreateTraceWriter(BufferID(0), BufferExhaustedPolicy::kStall);
  FtraceEventManager mgr(table_, &event_config_.value(), writer.get(), true,
                         endpoint_.get(), BufferID(0));
  // Shorter compact sched waking
  ParsedSample s3 = MakeFailingSample(kSchedWakingId);
  auto status = mgr.ProcessSample(s3);
  ASSERT_EQ(status, FtraceParseStatus::FTRACE_STATUS_SHORT_COMPACT_EVENT);
}

TEST_F(FtraceEventManagerTest, DropsPraseFailedFtraceEvent) {
  auto writer =
      endpoint_->CreateTraceWriter(BufferID(0), BufferExhaustedPolicy::kStall);
  FtraceEventManager mgr(table_, &event_config_.value(), writer.get(), true,
                         endpoint_.get(), BufferID(0));
  ParsedSample s1 = MakeFailingSample(kOtherEventId);
  auto status = mgr.ProcessSample(s1);
  ASSERT_EQ(status, FtraceParseStatus::FTRACE_STATUS_INVALID_EVENT);
}

TEST_F(FtraceEventManagerTest, SchedTest) {
  auto tw =
      endpoint_->CreateTraceWriter(BufferID(0), BufferExhaustedPolicy::kStall);
  FtraceEventManager mgr(table_, &event_config_.value(), tw.get(),
                         false /*compact sched*/, endpoint_.get(), BufferID(0));
  uint32_t cpu = 0;

  ParsedSample sample_switch =
      MakeSchedSwitchSample(cpu, 1308020252356549ULL, 4321, "sleep", 100, 120,
                            0, "kworker", 200, 120);
  auto status = mgr.ProcessSample(sample_switch);
  ASSERT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);

  ParsedSample sample_waking = MakeSchedWakingSample(
      cpu, 1308020252352573ULL, 4321, "my_task", 100, 120, 1, 1);
  status = mgr.ProcessSample(sample_waking);
  ASSERT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);
  mgr.Flush();

  {
    auto writer = endpoint_->last_writer();
    auto packets = writer->GetAllTracePackets();
    ASSERT_EQ(1u, packets.size());
    auto const& bundle = packets[0].ftrace_events();
    EXPECT_FALSE(bundle.lost_events());
    ASSERT_EQ(2u, bundle.event().size());
    EXPECT_TRUE(bundle.has_previous_bundle_end_timestamp());
    EXPECT_EQ(0u, bundle.previous_bundle_end_timestamp());
    EXPECT_EQ(1308020252356549ULL, bundle.event()[0].timestamp());
    EXPECT_EQ(1308020252352573ULL, bundle.event()[1].timestamp());
  }
}

TEST_F(FtraceEventManagerTest, SchedCompactTest) {
  auto tw =
      endpoint_->CreateTraceWriter(BufferID(0), BufferExhaustedPolicy::kStall);
  FtraceEventManager mgr(table_, &event_config_.value(), tw.get(), true,
                         endpoint_.get(), BufferID(0));
  uint32_t cpu = 0;

  ParsedSample sample_switch1 =
      MakeSchedSwitchSample(cpu, 1308020252356549ULL, 4321, "sleep", 100, 120,
                            0, "kworker", 200, 120);
  auto status = mgr.ProcessSample(sample_switch1);
  ASSERT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);

  ParsedSample sample_switch2 =
      MakeSchedSwitchSample(cpu, 1308020252351567ULL, 4321, "kworker", 200, 120,
                            0, "rcuop/0", 300, 120);
  status = mgr.ProcessSample(sample_switch2);
  ASSERT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);

  ParsedSample sample_waking = MakeSchedWakingSample(
      cpu, 1308020252352573ULL, 4321, "my_task", 100, 120, 1, 0);
  status = mgr.ProcessSample(sample_waking);
  ASSERT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);

  mgr.Flush();

  {
    auto writer = endpoint_->last_writer();
    auto packets = writer->GetAllTracePackets();

    ASSERT_EQ(1u, packets.size());
    auto const& bundle = packets[0].ftrace_events();
    EXPECT_FALSE(bundle.lost_events());
    ASSERT_EQ(0u, bundle.event().size());
    EXPECT_TRUE(bundle.has_previous_bundle_end_timestamp());
    EXPECT_EQ(0u, bundle.previous_bundle_end_timestamp());

    const auto& compact_sched = bundle.compact_sched();
    EXPECT_EQ(2u, compact_sched.switch_timestamp().size());
    EXPECT_EQ(2u, compact_sched.switch_prev_state().size());
    EXPECT_EQ(2u, compact_sched.switch_next_pid().size());
    EXPECT_EQ(2u, compact_sched.switch_next_prio().size());
    EXPECT_EQ(2u, compact_sched.switch_next_comm_index().size());

    EXPECT_EQ(1u, compact_sched.waking_timestamp().size());
    EXPECT_EQ(1u, compact_sched.waking_pid().size());
    EXPECT_EQ(1u, compact_sched.waking_prio().size());
    EXPECT_EQ(1u, compact_sched.waking_target_cpu().size());
    EXPECT_EQ(1u, compact_sched.waking_comm_index().size());

    // 3 unique interned next_comm strings:
    EXPECT_EQ(3u, compact_sched.intern_table().size());
  }
}

TEST_F(FtraceEventManagerTest, FtraceAndSchedCompactSameBundleTest) {
  auto tw =
      endpoint_->CreateTraceWriter(BufferID(0), BufferExhaustedPolicy::kStall);
  FtraceEventManager mgr(table_, &event_config_.value(), tw.get(), true,
                         endpoint_.get(), BufferID(0));
  uint32_t cpu = 0;

  ParsedSample sample_print1 = MakePrintEventSample(
      cpu, 1308020252351000ULL, 4371, 1, "hello ftrace print");
  auto status = mgr.ProcessSample(sample_print1);
  ASSERT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);

  ParsedSample sample_switch1 =
      MakeSchedSwitchSample(cpu, 1308020252352000ULL, 4321, "sleep", 100, 120,
                            0, "kworker", 200, 120);
  status = mgr.ProcessSample(sample_switch1);
  ASSERT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);

  ParsedSample sample_waking = MakeSchedWakingSample(
      cpu, 1308020252353000ULL, 4321, "my_task", 100, 120, 1, 0);
  status = mgr.ProcessSample(sample_waking);
  ASSERT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);

  ParsedSample sample_print2 = MakePrintEventSample(
      cpu, 1308020252354000ULL, 4371, 1, "hello ftrace print");
  status = mgr.ProcessSample(sample_print2);
  ASSERT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);

  ParsedSample sample_switch2 =
      MakeSchedSwitchSample(cpu, 1308020252355000ULL, 4321, "kworker", 200, 120,
                            0, "rcuop/0", 300, 120);
  status = mgr.ProcessSample(sample_switch2);
  ASSERT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);

  mgr.Flush();

  auto writer = endpoint_->last_writer();
  auto packets = writer->GetAllTracePackets();
  ASSERT_EQ(1u, packets.size());
  auto const& bundle = packets[0].ftrace_events();
  EXPECT_TRUE(bundle.has_previous_bundle_end_timestamp());
  EXPECT_EQ(0u, bundle.previous_bundle_end_timestamp());
  EXPECT_FALSE(bundle.lost_events());

  {
    // 2 print events
    ASSERT_EQ(2u, bundle.event().size());
    // 3 compact sched
    const auto& compact_sched = bundle.compact_sched();
    EXPECT_EQ(2u, compact_sched.switch_timestamp().size());
    EXPECT_EQ(2u, compact_sched.switch_prev_state().size());
    EXPECT_EQ(2u, compact_sched.switch_next_pid().size());
    EXPECT_EQ(2u, compact_sched.switch_next_prio().size());
    EXPECT_EQ(2u, compact_sched.switch_next_comm_index().size());

    EXPECT_EQ(1u, compact_sched.waking_timestamp().size());
    EXPECT_EQ(1u, compact_sched.waking_pid().size());
    EXPECT_EQ(1u, compact_sched.waking_prio().size());
    EXPECT_EQ(1u, compact_sched.waking_target_cpu().size());
    EXPECT_EQ(1u, compact_sched.waking_comm_index().size());

    // 3 unique interned next_comm strings:
    EXPECT_EQ(3u, compact_sched.intern_table().size());
  }
}

TEST_F(FtraceEventManagerTest, MultipleBundlesTest) {
  auto tw =
      endpoint_->CreateTraceWriter(BufferID(0), BufferExhaustedPolicy::kStall);
  FtraceEventManager mgr(table_, &event_config_.value(), tw.get(), false,
                         endpoint_.get(), BufferID(0));
  uint32_t cpu = 0;

  ParsedSample sample_print1 = MakePrintEventSample(
      cpu, 1308020252351000ULL, 4371, 1, "hello ftrace print");
  auto status = mgr.ProcessSample(sample_print1);
  ASSERT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);

  // set next bundle's lost_events to true for testing.
  mgr.OnEventsLost(cpu);

  ParsedSample sample_switch1 =
      MakeSchedSwitchSample(cpu, 1308020252352000ULL, 4321, "sleep", 100, 120,
                            0, "kworker", 200, 120);
  status = mgr.ProcessSample(sample_switch1);
  ASSERT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);

  ParsedSample sample_waking = MakeSchedWakingSample(
      cpu, 1308020252353000ULL, 4321, "my_task", 100, 120, 1, 0);
  status = mgr.ProcessSample(sample_waking);
  ASSERT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);

  ParsedSample sample_print2 = MakePrintEventSample(
      cpu, 1308020252354000ULL, 4371, 1, "hello ftrace print");
  status = mgr.ProcessSample(sample_print2);
  ASSERT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);

  mgr.Flush(cpu, false);

  ParsedSample sample_print3 = MakePrintEventSample(
      cpu, 1308020252355000ULL, 4371, 1, "hello ftrace print");
  status = mgr.ProcessSample(sample_print3);
  ASSERT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);

  ParsedSample sample_switch2 =
      MakeSchedSwitchSample(cpu, 1308020252355000ULL, 4321, "kworker", 200, 120,
                            0, "rcuop/0", 300, 120);
  status = mgr.ProcessSample(sample_switch2);
  ASSERT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);

  mgr.Flush();

  auto writer = endpoint_->last_writer();
  auto packets = writer->GetAllTracePackets();
  ASSERT_EQ(3u, packets.size());

  // bundle 1
  auto const& bundle_1 = packets[0].ftrace_events();
  EXPECT_TRUE(bundle_1.has_previous_bundle_end_timestamp());
  EXPECT_EQ(0u, bundle_1.previous_bundle_end_timestamp());
  EXPECT_FALSE(bundle_1.lost_events());
  EXPECT_EQ(1u, bundle_1.event().size());

  // bundle 2
  auto const& bundle_2 = packets[1].ftrace_events();
  EXPECT_TRUE(bundle_2.has_previous_bundle_end_timestamp());
  EXPECT_EQ(1308020252351000ULL, bundle_2.previous_bundle_end_timestamp());
  EXPECT_TRUE(bundle_2.lost_events());  // true
  EXPECT_EQ(3u, bundle_2.event().size());

  // bundle 3
  auto const& bundle_3 = packets[2].ftrace_events();
  EXPECT_TRUE(bundle_3.has_previous_bundle_end_timestamp());
  EXPECT_EQ(1308020252354000ULL, bundle_3.previous_bundle_end_timestamp());
  EXPECT_FALSE(bundle_3.lost_events());
  EXPECT_EQ(2u, bundle_3.event().size());
}

TEST_F(FtraceEventManagerTest, MultipleCpusTest) {
  auto tw =
      endpoint_->CreateTraceWriter(BufferID(0), BufferExhaustedPolicy::kStall);
  FtraceEventManager mgr(table_, &event_config_.value(), tw.get(), true,
                         endpoint_.get(), BufferID(0));

  ParsedSample sample_print0 = MakePrintEventSample(
      0, 1308020252351000ULL, 4370, 1, "hello ftrace print");
  auto status = mgr.ProcessSample(sample_print0);
  ASSERT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);
  auto cpu_writer0 = endpoint_->last_writer();

  ParsedSample sample_print1 = MakePrintEventSample(
      1, 1308020252351001ULL, 4371, 1, "hello ftrace print");
  status = mgr.ProcessSample(sample_print1);
  ASSERT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);
  auto cpu_writer1 = endpoint_->last_writer();

  ParsedSample sample_print2 = MakePrintEventSample(
      2, 1308020252351002ULL, 4372, 1, "hello ftrace print");
  status = mgr.ProcessSample(sample_print2);
  ASSERT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);
  auto cpu_writer2 = endpoint_->last_writer();

  ParsedSample sample_switch0 =
      MakeSchedSwitchSample(0, 1308020252352000ULL, 4320, "kworker", 200, 120,
                            0, "rcuop/0", 300, 120);
  status = mgr.ProcessSample(sample_switch0);
  ASSERT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);

  ParsedSample sample_switch1 =
      MakeSchedSwitchSample(1, 1308020252352001ULL, 4321, "kworker", 200, 120,
                            0, "rcuop/0", 300, 120);
  status = mgr.ProcessSample(sample_switch1);
  ASSERT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);

  ParsedSample sample_switch2 =
      MakeSchedSwitchSample(2, 1308020252352002ULL, 4321, "kworker", 200, 120,
                            0, "rcuop/0", 300, 120);
  status = mgr.ProcessSample(sample_switch2);
  ASSERT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);

  mgr.Flush();

  {
    auto packets0 = cpu_writer0->GetAllTracePackets();
    ASSERT_EQ(1u, packets0.size());
    auto const& bundle0 = packets0[0].ftrace_events();
    EXPECT_TRUE(bundle0.has_previous_bundle_end_timestamp());
    EXPECT_EQ(0u, bundle0.previous_bundle_end_timestamp());
    EXPECT_FALSE(bundle0.lost_events());
    ASSERT_EQ(1u, bundle0.event().size());
    EXPECT_EQ(1308020252351000ULL, bundle0.event()[0].timestamp());
    const auto& compact_sched = bundle0.compact_sched();
    EXPECT_EQ(1u, compact_sched.switch_timestamp().size());
    EXPECT_EQ(1u, compact_sched.intern_table().size());
  }

  {
    auto packets1 = cpu_writer1->GetAllTracePackets();
    ASSERT_EQ(1u, packets1.size());
    auto const& bundle1 = packets1[0].ftrace_events();
    EXPECT_TRUE(bundle1.has_previous_bundle_end_timestamp());
    EXPECT_EQ(0u, bundle1.previous_bundle_end_timestamp());
    EXPECT_FALSE(bundle1.lost_events());
    ASSERT_EQ(1u, bundle1.event().size());
    EXPECT_EQ(1308020252351001ULL, bundle1.event()[0].timestamp());
    const auto& compact_sched = bundle1.compact_sched();
    EXPECT_EQ(1u, compact_sched.switch_timestamp().size());
    EXPECT_EQ(1u, compact_sched.intern_table().size());
  }

  {
    auto packets2 = cpu_writer2->GetAllTracePackets();
    ASSERT_EQ(1u, packets2.size());
    auto const& bundle2 = packets2[0].ftrace_events();
    EXPECT_TRUE(bundle2.has_previous_bundle_end_timestamp());
    EXPECT_EQ(0u, bundle2.previous_bundle_end_timestamp());
    EXPECT_FALSE(bundle2.lost_events());
    ASSERT_EQ(1u, bundle2.event().size());
    EXPECT_EQ(1308020252351002ULL, bundle2.event()[0].timestamp());
    const auto& compact_sched = bundle2.compact_sched();
    EXPECT_EQ(1u, compact_sched.switch_timestamp().size());
    EXPECT_EQ(1u, compact_sched.intern_table().size());
  }
}

TEST_F(FtraceEventManagerTest, LostEventsTest) {
  auto tw =
      endpoint_->CreateTraceWriter(BufferID(0), BufferExhaustedPolicy::kStall);
  FtraceEventManager mgr(table_, &event_config_.value(), tw.get(), false,
                         endpoint_.get(), BufferID(0));
  uint32_t cpu = 0;

  ParsedSample sample_print1 = MakePrintEventSample(
      cpu, 1308020252351000ULL, 4371, 1, "hello ftrace print");
  auto status = mgr.ProcessSample(sample_print1);
  ASSERT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);

  mgr.OnEventsLost(cpu);
  mgr.Flush();

  auto writer = endpoint_->last_writer();
  auto packets = writer->GetAllTracePackets();
  ASSERT_EQ(2u, packets.size());

  auto const& bundle1 = packets[0].ftrace_events();
  EXPECT_TRUE(bundle1.has_previous_bundle_end_timestamp());
  EXPECT_EQ(0u, bundle1.previous_bundle_end_timestamp());
  EXPECT_EQ(1u, bundle1.event().size());
  EXPECT_EQ(1308020252351000ULL, bundle1.event()[0].timestamp());
  auto const& bundle2 = packets[1].ftrace_events();
  EXPECT_EQ(0u, bundle2.event().size());
  EXPECT_TRUE(bundle2.lost_events());
}

TEST_F(FtraceEventManagerTest, ParseErrorTest) {
  auto writer =
      endpoint_->CreateTraceWriter(BufferID(0), BufferExhaustedPolicy::kStall);
  FtraceEventManager mgr(table_, &event_config_.value(), writer.get(), true,
                         endpoint_.get(), BufferID(0));

  // FTRACE_STATUS_INVALID_EVENT
  ParsedSample s{};
  uint16_t unknown_id = 0xFFFF;
  s.raw_data.resize(sizeof(uint16_t));
  std::memcpy(s.raw_data.data(), &unknown_id, sizeof(uint16_t));
  mgr.ProcessSample(s);

  // FTRACE_STATUS_INVALID_EVENT
  ParsedSample s1 = MakeFailingSample(kOtherEventId);
  mgr.ProcessSample(s1);

  // FTRACE_STATUS_SHORT_COMPACT_EVENT
  ParsedSample s2 = MakeFailingSample(kSchedSwitchId);
  mgr.ProcessSample(s2);

  // FTRACE_STATUS_SHORT_COMPACT_EVENT
  ParsedSample s3 = MakeFailingSample(kSchedWakingId);
  mgr.ProcessSample(s3);

  mgr.Flush();

  // 2 invalid event, 2 short compact event
  auto packets = endpoint_->last_writer()->GetAllTracePackets();
  ASSERT_EQ(packets.size(), 1u);

  protos::gen::FtraceEventBundle bundle = packets[0].ftrace_events();
  using Bundle = protos::gen::FtraceEventBundle;
  using Error = Bundle::FtraceError;
  using ErrorStats = perfetto::protos::gen::FtraceParseStatus;
  using testing::ElementsAre;
  using testing::Property;
  EXPECT_THAT(
      bundle,
      Property(
          &Bundle::error,
          ElementsAre(
              Property(&Error::status, ErrorStats::FTRACE_STATUS_INVALID_EVENT),
              Property(&Error::status, ErrorStats::FTRACE_STATUS_INVALID_EVENT),
              Property(&Error::status,
                       ErrorStats::FTRACE_STATUS_SHORT_COMPACT_EVENT),
              Property(&Error::status,
                       ErrorStats::FTRACE_STATUS_SHORT_COMPACT_EVENT))));
}

}  // namespace
}  // namespace perfetto::profiling
