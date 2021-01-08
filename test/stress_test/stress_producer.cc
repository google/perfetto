/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include <math.h>
#include <stdint.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <list>
#include <random>
#include <thread>

#include "perfetto/base/time.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/tracing.h"

#include "protos/perfetto/config/stress_test_config.gen.h"
#include "protos/perfetto/trace/test_event.pbzero.h"

using StressTestConfig = perfetto::protos::gen::StressTestConfig;

namespace perfetto {
namespace {

StressTestConfig* g_cfg;

class StressTestDataSource : public DataSource<StressTestDataSource> {
 public:
  constexpr static BufferExhaustedPolicy kBufferExhaustedPolicy =
      BufferExhaustedPolicy::kStall;

  void OnSetup(const SetupArgs& args) override;
  void OnStart(const StartArgs&) override;
  void OnStop(const StopArgs&) override;

 private:
  class Worker {
   public:
    explicit Worker(uint32_t id) : id_(id) {}
    void Start();
    void Stop();
    ~Worker() { Stop(); }

   private:
    void WorkerMain(uint32_t worker_id);
    void FillPayload(const StressTestConfig::WriterTiming&,
                     uint32_t seq,
                     uint32_t nesting,
                     protos::pbzero::TestEvent::TestPayload*);

    const uint32_t id_;
    std::thread thread_;
    std::atomic<bool> quit_;
    std::minstd_rand0 rnd_seq_;

    // Use a different engine for the generation of random value, keep rnd_seq_
    // dedicated to generating deterministic sequences.
    std::minstd_rand0 rnd_gen_;
  };

  std::list<Worker> workers_;
};

// Called before the tracing session starts.
void StressTestDataSource::OnSetup(const SetupArgs&) {
  for (uint32_t i = 0; i < std::max(g_cfg->num_threads(), 1u); ++i)
    workers_.emplace_back(i);
}

// Called when the tracing session starts.
void StressTestDataSource::OnStart(const StartArgs&) {
  for (auto& worker : workers_)
    worker.Start();
}

// Called when the tracing session ends.
void StressTestDataSource::OnStop(const StopArgs&) {
  for (auto& worker : workers_)
    worker.Stop();
  workers_.clear();
}

void StressTestDataSource::Worker::Start() {
  quit_.store(false);
  thread_ = std::thread(&StressTestDataSource::Worker::WorkerMain, this, id_);
}

void StressTestDataSource::Worker::Stop() {
  if (!thread_.joinable() || quit_)
    return;
  PERFETTO_DLOG("Stopping worker %u", id_);
  quit_.store(true);
  thread_.join();
}

void StressTestDataSource::Worker::WorkerMain(uint32_t worker_id) {
  PERFETTO_DLOG("Worker %u starting", worker_id);
  rnd_seq_ = std::minstd_rand0(0);
  int64_t t_start = base::GetBootTimeNs().count();
  int64_t num_msgs = 0;

  const int64_t max_msgs = g_cfg->max_events()
                               ? static_cast<int64_t>(g_cfg->max_events())
                               : INT64_MAX;
  bool is_last = false;
  while (!is_last) {
    is_last = quit_ || ++num_msgs >= max_msgs;

    const int64_t now = base::GetBootTimeNs().count();
    const auto elapsed_ms = static_cast<uint64_t>((now - t_start) / 1000000);

    const auto* timings = &g_cfg->steady_state_timings();
    if (g_cfg->burst_period_ms() &&
        elapsed_ms % g_cfg->burst_period_ms() >
            (g_cfg->burst_period_ms() - g_cfg->burst_duration_ms())) {
      timings = &g_cfg->burst_timings();
    }
    std::normal_distribution<> rate_dist{timings->rate_mean(),
                                         timings->rate_stddev()};

    double period_ns = 1e9 / rate_dist(rnd_gen_);
    period_ns = isnan(period_ns) || period_ns == 0.0 ? 1 : period_ns;
    double expected_msgs = static_cast<double>(now - t_start) / period_ns;
    int64_t delay_ns = 0;
    if (static_cast<int64_t>(expected_msgs) < num_msgs)
      delay_ns = static_cast<int64_t>(period_ns);
    std::this_thread::sleep_for(
        std::chrono::nanoseconds(static_cast<int64_t>(delay_ns)));

    StressTestDataSource::Trace([&](StressTestDataSource::TraceContext ctx) {
      const uint32_t seq = static_cast<uint32_t>(rnd_seq_());
      auto packet = ctx.NewTracePacket();
      packet->set_timestamp(static_cast<uint64_t>(now));
      auto* test_event = packet->set_for_testing();
      test_event->set_seq_value(seq);
      test_event->set_counter(static_cast<uint64_t>(num_msgs));
      if (is_last)
        test_event->set_is_last(true);

      FillPayload(*timings, seq, g_cfg->nesting(), test_event->set_payload());
    });  // Trace().

  }  // while (!quit)
  PERFETTO_DLOG("Worker done");
}

void StressTestDataSource::Worker::FillPayload(
    const StressTestConfig::WriterTiming& timings,
    uint32_t seq,
    uint32_t nesting,
    protos::pbzero::TestEvent::TestPayload* payload) {
  // Write the payload in two halves, optionally with some delay in the
  // middle.
  std::normal_distribution<> msg_size_dist{timings.payload_mean(),
                                           timings.payload_stddev()};
  auto payload_size =
      static_cast<uint32_t>(std::max(std::round(msg_size_dist(rnd_gen_)), 0.0));
  std::string buf;
  buf.resize(payload_size / 2);
  for (size_t i = 0; i < buf.size(); ++i) {
    buf[i] = static_cast<char>(33 + ((seq + i) % 64));  // Stay ASCII.
  }
  payload->add_str(buf);
  payload->set_remaining_nesting_depth(nesting);
  if (timings.payload_write_time_ms() > 0) {
    std::this_thread::sleep_for(
        std::chrono::milliseconds(timings.payload_write_time_ms()));
  }

  if (nesting > 0)
    FillPayload(timings, seq, nesting - 1, payload->add_nested());

  payload->add_str(buf);
}
}  // namespace

PERFETTO_DECLARE_DATA_SOURCE_STATIC_MEMBERS(StressTestDataSource);
PERFETTO_DEFINE_DATA_SOURCE_STATIC_MEMBERS(StressTestDataSource);

}  // namespace perfetto

int main() {
  perfetto::TracingInitArgs args;
  args.backends = perfetto::kSystemBackend;

  std::string config_blob;
  if (isatty(fileno(stdin)))
    PERFETTO_LOG("Reading StressTestConfig proto from stdin");
  perfetto::base::ReadFileStream(stdin, &config_blob);

  StressTestConfig cfg;
  perfetto::g_cfg = &cfg;
  if (config_blob.empty() || !cfg.ParseFromString(config_blob))
    PERFETTO_FATAL("A StressTestConfig blob must be passed into stdin");

  if (cfg.shmem_page_size_kb())
    args.shmem_page_size_hint_kb = cfg.shmem_page_size_kb();
  if (cfg.shmem_size_kb())
    args.shmem_page_size_hint_kb = cfg.shmem_size_kb();

  perfetto::Tracing::Initialize(args);
  perfetto::DataSourceDescriptor dsd;
  dsd.set_name("perfetto.stress_test");
  perfetto::StressTestDataSource::Register(dsd);

  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(30));
  }
}
