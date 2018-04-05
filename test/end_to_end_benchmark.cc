// Copyright (C) 2018 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <gtest/gtest.h>
#include <random>

#include "benchmark/benchmark.h"
#include "perfetto/base/time.h"
#include "perfetto/traced/traced.h"
#include "perfetto/tracing/core/trace_config.h"
#include "perfetto/tracing/core/trace_packet.h"
#include "src/base/test/test_task_runner.h"
#include "test/task_runner_thread.h"
#include "test/task_runner_thread_delegates.h"
#include "test/test_helper.h"

#include "perfetto/trace/trace_packet.pb.h"
#include "perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {

namespace {

bool IsBenchmarkFunctionalOnly() {
  return getenv("BENCHMARK_FUNCTIONAL_TEST_ONLY") != nullptr;
}

void BenchmarkCommon(benchmark::State& state) {
  base::TestTaskRunner task_runner;

  TestHelper helper(&task_runner);
  helper.StartServiceIfRequired();

  FakeProducer* producer = helper.ConnectFakeProducer();
  helper.ConnectConsumer();

  // Setup the TraceConfig for the consumer.
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(512);

  // Create the buffer for ftrace.
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.perfetto.FakeProducer");
  ds_config->set_target_buffer(0);

  // The parameters for the producer.
  static constexpr uint32_t kRandomSeed = 42;
  size_t message_count = state.range(0);
  size_t message_bytes = state.range(1);
  size_t mb_per_s = state.range(2);

  size_t messages_per_s = mb_per_s * 1024 * 1024 / message_bytes;
  size_t time_for_messages_ms =
      10000 + (messages_per_s == 0 ? 0 : message_count * 1000 / messages_per_s);

  // Setup the test to use a random number generator.
  ds_config->mutable_for_testing()->set_seed(kRandomSeed);
  ds_config->mutable_for_testing()->set_message_count(message_count);
  ds_config->mutable_for_testing()->set_message_size(message_bytes);
  ds_config->mutable_for_testing()->set_max_messages_per_second(messages_per_s);

  helper.StartTracing(trace_config);

  bool is_first_packet = true;
  std::minstd_rand0 rnd_engine(kRandomSeed);
  auto on_consumer_data = [&is_first_packet,
                           &rnd_engine](const protos::TracePacket& packet) {
    ASSERT_TRUE(packet.has_for_testing());
    if (is_first_packet) {
      rnd_engine = std::minstd_rand0(packet.for_testing().seq_value());
      is_first_packet = false;
    } else {
      ASSERT_EQ(packet.for_testing().seq_value(), rnd_engine());
    }
  };

  uint64_t wall_start_ns = base::GetWallTimeNs().count();
  uint64_t service_start_ns = helper.service_thread()->GetThreadCPUTimeNs();
  uint64_t producer_start_ns = helper.producer_thread()->GetThreadCPUTimeNs();
  uint64_t iterations = 0;
  for (auto _ : state) {
    auto cname = "produced.and.committed." + std::to_string(iterations++);
    auto on_produced_and_committed = task_runner.CreateCheckpoint(cname);
    producer->ProduceEventBatch(helper.WrapTask(on_produced_and_committed));
    task_runner.RunUntilCheckpoint(cname, time_for_messages_ms);
  }
  uint64_t service_ns =
      helper.service_thread()->GetThreadCPUTimeNs() - service_start_ns;
  uint64_t producer_ns =
      helper.producer_thread()->GetThreadCPUTimeNs() - producer_start_ns;
  uint64_t wall_ns = base::GetWallTimeNs().count() - wall_start_ns;

  state.counters["Pro CPU"] = benchmark::Counter(100.0 * producer_ns / wall_ns);
  state.counters["Ser CPU"] = benchmark::Counter(100.0 * service_ns / wall_ns);
  state.counters["Ser ns/m"] =
      benchmark::Counter(1.0 * service_ns / message_count);

  // Read back the buffer just to check correctness.
  auto on_readback_complete = task_runner.CreateCheckpoint("readback.complete");
  helper.ReadData(on_consumer_data, on_readback_complete);
  task_runner.RunUntilCheckpoint("readback.complete");
  state.SetBytesProcessed(iterations * message_bytes * message_count);
}

void SaturateCpuArgs(benchmark::internal::Benchmark* b) {
  int min_message_count = 16;
  int max_message_count = IsBenchmarkFunctionalOnly() ? 1024 : 1024 * 1024;
  int min_payload = 8;
  int max_payload = IsBenchmarkFunctionalOnly() ? 256 : 2048;
  for (int count = min_message_count; count <= max_message_count; count *= 2) {
    for (int bytes = min_payload; bytes <= max_payload; bytes *= 2) {
      b->Args({count, bytes, 0 /* speed */});
    }
  }
}

void ConstantRateArgs(benchmark::internal::Benchmark* b) {
  int message_count = IsBenchmarkFunctionalOnly() ? 2 * 1024 : 128 * 1024;
  int min_speed = IsBenchmarkFunctionalOnly() ? 64 : 8;
  int max_speed = IsBenchmarkFunctionalOnly() ? 128 : 128;
  for (int speed = min_speed; speed <= max_speed; speed *= 2) {
    b->Args({message_count, 128, speed});
    b->Args({message_count, 256, speed});
  }
}
}  // namespace

static void BM_EndToEnd_SaturateCpu(benchmark::State& state) {
  BenchmarkCommon(state);
}

BENCHMARK(BM_EndToEnd_SaturateCpu)
    ->Unit(benchmark::kMicrosecond)
    ->UseRealTime()
    ->Apply(SaturateCpuArgs);

static void BM_EndToEnd_ConstantRate(benchmark::State& state) {
  BenchmarkCommon(state);
}

BENCHMARK(BM_EndToEnd_ConstantRate)
    ->Unit(benchmark::kMicrosecond)
    ->UseRealTime()
    ->Apply(ConstantRateArgs);
}  // namespace perfetto
