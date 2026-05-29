/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/tools/trace_replay/producer_worker.h"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/tracing.h"

#include "src/tools/trace_replay/replay_file.h"

namespace perfetto {
namespace trace_replay {

namespace {

// Cap. The producer worker pre-instantiates ReplayDS<0..kMaxBufs-1> at compile
// time; if the trace forwards a buffer index >= kMaxBufs we refuse to run.
constexpr int kMaxBufs = 32;

// State shared between the OnStart callbacks of each ReplayDS instance and the
// main thread of the producer worker.
struct WorkerState {
  // Per-buffer slice of records, grouped by original sequence_id.
  // [buffer_idx][seq_id] = vector<rec>
  std::map<uint32_t, std::map<uint32_t, std::vector<ReplayRecord>>>
      records_by_buf;

  // Set of buffers actually used by this process — exposed on stdout so the
  // worker's main() can register only what it needs.
  std::set<uint32_t> used_buffers;

  // Anchor time for the replay: captured on the first OnStart of any ds.
  std::atomic<int64_t> t0_ns{0};
  std::once_flag t0_once;

  // Worker thread bookkeeping.
  std::mutex mtx;
  std::condition_variable cv;
  std::vector<std::thread> threads;
  int active_workers = 0;  // protected by mtx
  int total_started = 0;   // protected by mtx (monotonic)

  bool stop_requested = false;  // set by OnStop()
};

WorkerState* g_state = nullptr;

// One DataSource<T> per buffer index. The SDK keys static state by T, so we
// need distinct types — hence the template parameter.
template <int BufIdx>
class ReplayDS : public DataSource<ReplayDS<BufIdx>> {
 public:
  // Replay packets can be large (e.g. ftrace bundles), and the SDK's TLS
  // TraceWriter would otherwise silently drop chunks when the SHM fills up.
  // Stall instead — we want to faithfully push every recorded byte and
  // measure traced's response.
  static constexpr BufferExhaustedPolicy kBufferExhaustedPolicy =
      BufferExhaustedPolicy::kStall;

  void OnSetup(const DataSourceBase::SetupArgs&) override {}
  void OnStart(const DataSourceBase::StartArgs&) override {
    if (!g_state)
      return;
    // Anchor t0 once per process.
    std::call_once(g_state->t0_once, [] {
      g_state->t0_ns.store(base::GetWallTimeNs().count(),
                           std::memory_order_relaxed);
    });

    auto bit = g_state->records_by_buf.find(static_cast<uint32_t>(BufIdx));
    if (bit == g_state->records_by_buf.end())
      return;

    // Spawn one thread per original sequence_id targeting this buffer.
    for (auto& kv : bit->second) {
      uint32_t orig_seq_id = kv.first;
      // Move the records into the thread; we don't need them anymore in
      // g_state once the thread owns them.
      auto records = std::move(kv.second);
      {
        std::lock_guard<std::mutex> lk(g_state->mtx);
        g_state->active_workers++;
        g_state->total_started++;
      }
      g_state->threads.emplace_back([orig_seq_id,
                                     records = std::move(records)]() mutable {
        (void)orig_seq_id;  // SDK assigns its own sequence_id.
        const int64_t t0 = g_state->t0_ns.load(std::memory_order_relaxed);
        for (const auto& r : records) {
          // Sleep until t0 + rel_ts_ns (boot time).
          int64_t deadline_ns = t0 + static_cast<int64_t>(r.rel_ts_ns);
          for (;;) {
            int64_t now = base::GetWallTimeNs().count();
            int64_t delta = deadline_ns - now;
            if (delta <= 0)
              break;
            // Bail out promptly if the consumer asked us to stop.
            {
              std::lock_guard<std::mutex> lk(g_state->mtx);
              if (g_state->stop_requested)
                break;
            }
            // Cap the sleep at 50ms to remain responsive to stop.
            int64_t cap_ns = 50 * 1000 * 1000;
            std::this_thread::sleep_for(
                std::chrono::nanoseconds(delta > cap_ns ? cap_ns : delta));
          }
          {
            std::lock_guard<std::mutex> lk(g_state->mtx);
            if (g_state->stop_requested)
              break;
          }
          // Emit the recorded TracePacket body via AppendRawProtoBytes.
          ReplayDS<BufIdx>::Trace(
              [&r](typename ReplayDS<BufIdx>::TraceContext ctx) {
                auto packet = ctx.NewTracePacket();
                packet->AppendRawProtoBytes(r.bytes.data(), r.bytes.size());
              });
        }
        // Final flush of this thread's writer.
        ReplayDS<BufIdx>::Trace(
            [](typename ReplayDS<BufIdx>::TraceContext ctx) { ctx.Flush(); });
        {
          std::lock_guard<std::mutex> lk(g_state->mtx);
          if (--g_state->active_workers == 0)
            g_state->cv.notify_all();
        }
      });
    }
    // Free the now-empty inner map.
    g_state->records_by_buf.erase(bit);
  }
  void OnStop(const DataSourceBase::StopArgs&) override {
    if (!g_state)
      return;
    std::lock_guard<std::mutex> lk(g_state->mtx);
    g_state->stop_requested = true;
    g_state->cv.notify_all();
  }
};

template <int N>
void RegisterAll(const std::set<uint32_t>& used) {
  if (used.count(static_cast<uint32_t>(N))) {
    DataSourceDescriptor dsd;
    dsd.set_name("replay.buf" + std::to_string(N));
    ReplayDS<N>::Register(dsd);
  }
  if constexpr (N + 1 < kMaxBufs)
    RegisterAll<N + 1>(used);
}

}  // namespace

int RunProducerWorker(const ProducerWorkerOptions& opts) {
  WorkerState state;
  g_state = &state;

  uint32_t num_buffers = 0;
  std::vector<ReplayRecord> all_records;
  auto st = ReadReplayFile(opts.replay_file, &num_buffers, &all_records);
  if (!st.ok()) {
    PERFETTO_ELOG("ReadReplayFile failed: %s", st.c_message());
    return 1;
  }
  for (auto& r : all_records) {
    if (r.buffer_idx >= kMaxBufs) {
      PERFETTO_ELOG("Record references buffer %u, exceeds kMaxBufs=%d",
                    r.buffer_idx, kMaxBufs);
      return 1;
    }
    state.used_buffers.insert(r.buffer_idx);
    uint32_t b = r.buffer_idx;
    uint32_t s = r.orig_seq_id;
    state.records_by_buf[b][s].push_back(std::move(r));
  }

  PERFETTO_LOG("Worker: records=%zu  buffers_used=%zu  pid=%d",
               all_records.size(), state.used_buffers.size(),
               static_cast<int>(getpid()));

  TracingInitArgs args;
  args.backends = kSystemBackend;
  // Big SMB so we can buffer bursts of large replayed packets without
  // stalling on every chunk.
  args.shmem_size_hint_kb = 8 * 1024;  // 8 MB
  args.shmem_page_size_hint_kb = 32;   // 32 KB pages
  Tracing::Initialize(args);

  RegisterAll<0>(state.used_buffers);

  // Signal the parent that we have registered.
  if (opts.ready_fd >= 0) {
    const char ch = 'R';
    ssize_t r = write(opts.ready_fd, &ch, 1);
    (void)r;
    close(opts.ready_fd);
  }

  // Wait until either OnStop fired, or every worker we ever spawned has
  // exited. The total_started>0 guard avoids returning before OnStart had a
  // chance to spawn any worker thread at all.
  {
    std::unique_lock<std::mutex> lk(state.mtx);
    state.cv.wait(lk, [&] {
      return state.stop_requested ||
             (state.total_started > 0 && state.active_workers == 0);
    });
  }

  for (auto& t : state.threads) {
    if (t.joinable())
      t.join();
  }

  PERFETTO_LOG("Worker pid=%d done", static_cast<int>(getpid()));
  return 0;
}

}  // namespace trace_replay
}  // namespace perfetto

// Allocate static storage for ReplayDS<0..31>.
#define DEF_REPLAY_DS(N)                      \
  PERFETTO_DEFINE_DATA_SOURCE_STATIC_MEMBERS( \
      perfetto::trace_replay::ReplayDS<N>)
DEF_REPLAY_DS(0);
DEF_REPLAY_DS(1);
DEF_REPLAY_DS(2);
DEF_REPLAY_DS(3);
DEF_REPLAY_DS(4);
DEF_REPLAY_DS(5);
DEF_REPLAY_DS(6);
DEF_REPLAY_DS(7);
DEF_REPLAY_DS(8);
DEF_REPLAY_DS(9);
DEF_REPLAY_DS(10);
DEF_REPLAY_DS(11);
DEF_REPLAY_DS(12);
DEF_REPLAY_DS(13);
DEF_REPLAY_DS(14);
DEF_REPLAY_DS(15);
DEF_REPLAY_DS(16);
DEF_REPLAY_DS(17);
DEF_REPLAY_DS(18);
DEF_REPLAY_DS(19);
DEF_REPLAY_DS(20);
DEF_REPLAY_DS(21);
DEF_REPLAY_DS(22);
DEF_REPLAY_DS(23);
DEF_REPLAY_DS(24);
DEF_REPLAY_DS(25);
DEF_REPLAY_DS(26);
DEF_REPLAY_DS(27);
DEF_REPLAY_DS(28);
DEF_REPLAY_DS(29);
DEF_REPLAY_DS(30);
DEF_REPLAY_DS(31);
#undef DEF_REPLAY_DS
