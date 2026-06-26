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

// tracing-v2 Task 02 — v2 SharedRingBuffer REASSEMBLY-CORRECTNESS stress test.
//
// Task-2 / the RFC measured *loss* ("did the buffer drop?"). This is the other
// half the RFC flags as unproven: *correctness* ("when a message is delivered,
// are the bytes EXACTLY what was written?"). Loss is fine and expected here;
// CORRUPTION is the bug we are hunting — the descheduled-writer / needs_rewrite
// race, fragmentation across 252 B chunk boundaries, wrap-around, and CAS
// contention between many writers and one reader.
//
// Method: every message carries deterministic, self-describing content
//   [u32 seq][u32 len][ pattern(writer, seq, offset) ... ]
// so the reader can, for any delivered message, recompute exactly what should
// have been written and compare byte-for-byte. We then run an adversarial matrix
// (small buffers -> constant wrap; SPIN reader -> laps writers mid-fragment ->
// needs_rewrite; large messages -> many-chunk fragmentation; 4..16 writers ->
// CAS contention). Every config uses a fixed seed and is fully reproducible.
//
// Exit code 0 iff ZERO content/size/ordering mismatches across all configs.
// Host-only test; no device needed.

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "src/tracing/v2/shared_ring_buffer.h"

namespace perfetto {
namespace {

// Deterministic byte at (writer, seq, offset) — recomputable by the verifier.
static inline uint8_t Pat(uint32_t w, uint32_t q, uint32_t i) {
  uint32_t x = w * 2654435761u ^ q * 2246822519u ^ i * 3266489917u;
  x ^= x >> 15; x *= 2654435761u; x ^= x >> 13;
  return static_cast<uint8_t>(x & 0xFFu);
}

// Tiny deterministic PRNG (splitmix32) so message sizes/jitter are reproducible.
struct Rng {
  uint32_t s;
  uint32_t next() {
    s += 0x9e3779b9u;
    uint32_t z = s;
    z = (z ^ (z >> 16)) * 0x21f0aaadu;
    z = (z ^ (z >> 15)) * 0x735a2d97u;
    return z ^ (z >> 15);
  }
  uint32_t range(uint32_t lo, uint32_t hi) { return lo + next() % (hi - lo + 1); }
};

struct Config {
  const char* name;
  uint32_t writers;
  uint32_t msgs_per_writer;
  uint32_t chunks;     // buffer size in 256 B chunks
  uint32_t max_size;   // max message bytes (>= 8)
  bool spin_reader;    // true = adversarial lapping reader
  uint32_t seed;
};

struct Result {
  uint64_t written = 0, received = 0, multichunk_ok = 0;
  uint64_t content_mismatch = 0, size_mismatch = 0, reorder = 0, too_short = 0;
  bool ok() const { return content_mismatch == 0 && size_mismatch == 0 &&
                           reorder == 0 && too_short == 0; }
};

Result RunConfig(const Config& cfg) {
  const size_t buf_size = SharedRingBuffer::kRingBufferHeaderSize +
                          static_cast<size_t>(cfg.chunks) * SharedRingBuffer::kChunkSize;
  std::vector<uint8_t> backing(buf_size, 0);
  SharedRingBuffer rb(backing.data(), buf_size);
  PERFETTO_CHECK(rb.is_valid());

  std::atomic<bool> writers_done{false};
  std::atomic<uint32_t> writers_remaining{cfg.writers};
  std::atomic<uint64_t> total_written{0};

  // --- writers: deterministic, self-describing messages ---
  auto writer_fn = [&](uint32_t widx) {
    const uint32_t wid = widx + 1;  // WriterID must be > 0
    auto w = rb.CreateWriter(static_cast<WriterID>(wid));
    Rng rng{cfg.seed * 0x85ebca6bu + wid};
    std::vector<uint8_t> msg(cfg.max_size);
    uint64_t wrote = 0;
    for (uint32_t q = 0; q < cfg.msgs_per_writer; q++) {
      uint32_t len = rng.range(8, cfg.max_size);
      memcpy(&msg[0], &q, 4);
      memcpy(&msg[4], &len, 4);
      for (uint32_t i = 8; i < len; i++) msg[i] = Pat(wid, q, i);
      w.BeginWrite();
      w.WriteBytes(msg.data(), static_cast<uint16_t>(len > 0xFFFF ? 0xFFFF : len));
      w.EndWrite();
      wrote++;
      if ((rng.next() & 0x7) == 0) std::this_thread::yield();  // jitter interleavings
    }
    total_written.fetch_add(wrote, std::memory_order_relaxed);
    // Last writer to finish signals the reader loop it can drain + exit.
    if (writers_remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
      writers_done.store(true, std::memory_order_release);
  };

  std::vector<std::thread> ts;
  for (uint32_t i = 0; i < cfg.writers; i++) ts.emplace_back(writer_fn, i);

  // --- reader: drain + verify every delivered message byte-for-byte ---
  Result r;
  SharedRingBuffer::Reader reader(&rb);
  std::vector<int64_t> last_seq(cfg.writers + 2, -1);
  auto verify = [&]() {
    for (auto& m : reader.completed_messages()) {
      r.received++;
      WriterID w = m.writer_id;
      const std::string& d = m.data;
      if (d.size() < 8 || w < 1 || w >= last_seq.size()) { r.too_short++; continue; }
      uint32_t q, len;
      memcpy(&q, d.data(), 4);
      memcpy(&len, d.data() + 4, 4);
      // ordering within a writer: seq must strictly increase (gaps = drops = ok).
      if (static_cast<int64_t>(q) <= last_seq[w]) { r.reorder++; continue; }
      last_seq[w] = q;
      uint32_t want = len > 0xFFFF ? 0xFFFF : len;
      if (d.size() != want) { r.size_mismatch++; continue; }
      bool bad = false;
      for (uint32_t i = 8; i < want; i++)
        if (static_cast<uint8_t>(d[i]) != Pat(w, q, i)) { bad = true; break; }
      if (bad) r.content_mismatch++;
      else if (want > SharedRingBuffer::kChunkPayloadSize) r.multichunk_ok++;
    }
    reader.ClearCompletedMessages();
  };

  bool done = false;
  while (true) {
    while (reader.ReadOneChunk()) verify();
    if (done) break;
    if (writers_done.load(std::memory_order_acquire)) { done = true; continue; }
    if (!cfg.spin_reader) std::this_thread::sleep_for(std::chrono::microseconds(50));
    // spin reader: no sleep -> constantly laps writers mid-write (needs_rewrite).
  }

  for (auto& t : ts) t.join();  // writers already finished (they set writers_done)
  // final unthrottled drain of whatever remains
  while (reader.ReadOneChunk()) verify();
  verify();

  r.written = total_written.load();
  return r;
}

}  // namespace
}  // namespace perfetto

int main(int argc, char** argv) {
  using perfetto::Config;
  using perfetto::Result;
  using perfetto::RunConfig;

  setvbuf(stdout, nullptr, _IONBF, 0);  // unbuffered: partial results survive a kill

  // Adversarial matrix. Each row exercises a distinct hazard; all fixed-seed.
  // Counts are sized so the (slow) SPIN/lap configs still churn the needs_rewrite
  // path heavily but finish in seconds — correctness needs many laps, not millions.
  std::vector<Config> configs = {
      {"single-chunk/batch", 4, 4000, 2048, 252, false, 1},
      {"fragmented/batch", 4, 3000, 2048, 4096, false, 2},
      {"fragmented/SPIN-lap", 8, 1200, 1024, 4096, true, 3},
      {"tiny-buf/wrap/SPIN", 8, 2500, 256, 1024, true, 4},
      {"16w-contention/SPIN", 16, 1000, 1024, 2048, true, 5},
      {"big-msg/wrap/SPIN", 6, 600, 512, 8192, true, 6},
  };
  if (argc > 1) {  // optional: run one named subset (substring match)
    std::vector<Config> sub;
    for (auto& c : configs)
      if (strstr(c.name, argv[1])) sub.push_back(c);
    if (!sub.empty()) configs = sub;
  }

  printf("v2 SharedRingBuffer reassembly-correctness stress\n");
  printf("%-22s %9s %9s %9s | %8s %8s %8s %8s  %s\n", "config", "written",
         "recv", "multiChk", "content", "size", "reorder", "short", "verdict");
  bool all_ok = true;
  for (auto& cfg : configs) {
    auto t0 = std::chrono::steady_clock::now();
    Result r = RunConfig(cfg);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    bool ok = r.ok();
    all_ok = all_ok && ok;
    double drop = r.written ? 100.0 * (1.0 - double(r.received) / double(r.written)) : 0;
    printf("%-22s %9" PRIu64 " %9" PRIu64 " %9" PRIu64 " | %8" PRIu64 " %8" PRIu64
           " %8" PRIu64 " %8" PRIu64 "  %s (drop %.1f%%, %.1fs)\n",
           cfg.name, r.written, r.received, r.multichunk_ok, r.content_mismatch,
           r.size_mismatch, r.reorder, r.too_short, ok ? "PASS" : "**FAIL**", drop, secs);
  }
  printf("\n%s — corruption is the failure; drops are expected and fine.\n",
         all_ok ? "ALL PASS: every delivered message was byte-exact"
                : "FAIL: reassembly produced corrupted/misordered messages");
  return all_ok ? 0 : 1;
}
