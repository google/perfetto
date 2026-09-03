// Copyright (C) 2022 The Android Open Source Project
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

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/protozero/proto_ring_buffer.h"
#include "perfetto/protozero/proto_utils.h"
#include "src/base/test/utils.h"

namespace {

std::string LoadTestTrace() {
  std::string trace_data;
  static const char kTestTrace[] = "test/data/example_android_trace_30s.pb";
  perfetto::base::ReadFile(perfetto::base::GetTestDataPath(kTestTrace),
                           &trace_data);
  PERFETTO_CHECK(!trace_data.empty());
  return trace_data;
}

// Stands in for the read(2)/recv(2) that brings bytes in from the outside
// world: like those, it fills a buffer chosen by the caller. That is the one
// copy no scheme avoids; the benchmarks below differ only in whether the buffer
// belongs to the transport or to the ring buffer.
PERFETTO_NO_INLINE void ProduceBytes(void* dst, const void* src, size_t size) {
  memcpy(dst, src, size);
}

// Ingestion as the transports used to do it: read into a staging buffer of the
// transport's own, then hand those bytes to Append(), which copies them again.
void BM_ProtoRingBufferIngestStaged(benchmark::State& state) {
  const std::string trace_data = LoadTestTrace();
  const auto read_size = static_cast<size_t>(state.range(0));
  std::vector<uint8_t> staging(read_size);

  protozero::ProtoRingBuffer buffer;
  size_t offset = 0, bytes_ingested = 0, total_packet_size = 0;
  for (auto _ : state) {
    size_t n = std::min(read_size, trace_data.size() - offset);
    ProduceBytes(staging.data(), trace_data.data() + offset, n);
    buffer.Append(staging.data(), n);
    // The trace holds whole messages only, so by the time we get back to the
    // start the buffer has drained and the wrap is seamless.
    offset = (offset + n) % trace_data.size();
    bytes_ingested += n;
    for (;;) {
      auto msg = buffer.ReadMessage();
      if (!msg.valid())
        break;
      total_packet_size += msg.len;
    }
  }
  benchmark::DoNotOptimize(total_packet_size);
  state.SetBytesProcessed(static_cast<int64_t>(bytes_ingested));
}

// Ingestion as they do it now: read straight into the ring buffer, so the bytes
// are copied exactly once.
void BM_ProtoRingBufferIngestZeroCopy(benchmark::State& state) {
  const std::string trace_data = LoadTestTrace();
  const auto read_size = static_cast<size_t>(state.range(0));

  protozero::ProtoRingBuffer buffer;
  size_t offset = 0, bytes_ingested = 0, total_packet_size = 0;
  for (auto _ : state) {
    size_t n = std::min(read_size, trace_data.size() - offset);
    auto write = buffer.BeginWrite(n);
    ProduceBytes(write.data(), trace_data.data() + offset, n);
    write.EndWrite(n);
    offset = (offset + n) % trace_data.size();
    bytes_ingested += n;
    for (;;) {
      auto msg = buffer.ReadMessage();
      if (!msg.valid())
        break;
      total_packet_size += msg.len;
    }
  }
  benchmark::DoNotOptimize(total_packet_size);
  state.SetBytesProcessed(static_cast<int64_t>(bytes_ingested));
}

// unixd used to tokenize each message out of a per-connection ProtoRingBuffer,
// re-serialize its TraceProcessorRpcStream preamble, and push preamble +
// payload through a second ProtoRingBuffer inside Rpc, so every message was
// copied and tokenized twice. It now reads straight into Rpc's tokenizer.
void RunDispatch(benchmark::State& state, bool reframe) {
  namespace pu = protozero::proto_utils;
  const std::string trace_data = LoadTestTrace();
  constexpr size_t kReadSize = 4096;

  protozero::ProtoRingBuffer conn_buf, rpc_buf;
  size_t offset = 0, bytes_ingested = 0, total_packet_size = 0;
  for (auto _ : state) {
    size_t n = std::min(kReadSize, trace_data.size() - offset);
    auto write = conn_buf.BeginWrite(n);
    ProduceBytes(write.data(), trace_data.data() + offset, n);
    write.EndWrite(n);
    offset = (offset + n) % trace_data.size();
    bytes_ingested += n;
    for (;;) {
      auto msg = conn_buf.ReadMessage();
      if (!msg.valid())
        break;
      if (!reframe) {
        total_packet_size += msg.len;
        continue;
      }
      uint8_t preamble[16];
      uint8_t* end = preamble;
      end = pu::WriteVarInt(pu::MakeTagLengthDelimited(1), end);
      end = pu::WriteVarInt(msg.len, end);
      rpc_buf.Append(preamble, static_cast<size_t>(end - preamble));
      rpc_buf.Append(msg.start, msg.len);
      for (;;) {
        auto inner = rpc_buf.ReadMessage();
        if (!inner.valid())
          break;
        total_packet_size += inner.len;
      }
    }
  }
  benchmark::DoNotOptimize(total_packet_size);
  state.SetBytesProcessed(static_cast<int64_t>(bytes_ingested));
}

void BM_ProtoRingBufferDispatchReframed(benchmark::State& state) {
  RunDispatch(state, /*reframe=*/true);
}

void BM_ProtoRingBufferDispatchDirect(benchmark::State& state) {
  RunDispatch(state, /*reframe=*/false);
}

}  // namespace

// 4096 is what the socket transports read; 1MB is what traceconv reads.
BENCHMARK(BM_ProtoRingBufferIngestStaged)->Arg(4096)->Arg(1024 * 1024);
BENCHMARK(BM_ProtoRingBufferIngestZeroCopy)->Arg(4096)->Arg(1024 * 1024);
BENCHMARK(BM_ProtoRingBufferDispatchReframed);
BENCHMARK(BM_ProtoRingBufferDispatchDirect);

static void BM_ProtoRingBufferReadLargeChunks(benchmark::State& state) {
  std::string trace_data;
  static const char kTestTrace[] = "test/data/example_android_trace_30s.pb";
  perfetto::base::ReadFile(perfetto::base::GetTestDataPath(kTestTrace),
                           &trace_data);
  PERFETTO_CHECK(!trace_data.empty());

  size_t total_packet_size = 0;
  protozero::ProtoRingBuffer buffer;
  for (auto _ : state) {
    protozero::ProtoRingBuffer::Message msg = buffer.ReadMessage();
    if (msg.valid()) {
      total_packet_size += msg.len;
    } else {
      state.PauseTiming();
      buffer.Append(trace_data.data(), trace_data.size());
      state.ResumeTiming();
    }
  }
  benchmark::DoNotOptimize(total_packet_size);
}

BENCHMARK(BM_ProtoRingBufferReadLargeChunks);

static void BM_ProtoRingBufferRead(benchmark::State& state) {
  std::string trace_data;
  static const char kTestTrace[] = "test/data/example_android_trace_30s.pb";
  perfetto::base::ReadFile(perfetto::base::GetTestDataPath(kTestTrace),
                           &trace_data);
  PERFETTO_CHECK(!trace_data.empty());

  constexpr size_t kChunkSize = 1024 * 1024 * 3;

  size_t offset = 0;
  size_t total_packet_size = 0;
  protozero::ProtoRingBuffer buffer;
  for (auto _ : state) {
    protozero::ProtoRingBuffer::Message msg = buffer.ReadMessage();
    if (msg.valid()) {
      total_packet_size += msg.len;
    } else {
      state.PauseTiming();
      size_t sz = std::min(kChunkSize, trace_data.size() - offset);
      buffer.Append(trace_data.data() + offset, sz);
      offset = (offset + sz) % trace_data.size();
      state.ResumeTiming();
    }
  }
  benchmark::DoNotOptimize(total_packet_size);
}

BENCHMARK(BM_ProtoRingBufferRead);
