// Copyright (C) 2025 The Android Open Source Project
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

#include "perfetto/base/build_config.h"

#include <fstream>
#include <vector>

#if PERFETTO_BUILDFLAG(PERFETTO_ZLIB)
#include <zlib.h>
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_ZSTD)
#include <lib/zstd.h>
#endif

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/tracing/core/trace_packet.h"
#include "protos/perfetto/trace/trace.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"

namespace perfetto {
namespace {

// Load real trace data from test file
std::vector<uint8_t> LoadTestTrace(const std::string& filename) {
  std::string path = "test/data/" + filename;
  std::string contents;
  base::ReadFile(path, &contents);
  return std::vector<uint8_t>(contents.begin(), contents.end());
}

// Convert raw trace bytes to TracePackets for compression
std::vector<TracePacket> ParseTracePackets(const std::vector<uint8_t>& data) {
  protos::gen::Trace trace;
  if (!trace.ParseFromArray(data.data(), data.size())) {
    return {};
  }

  std::vector<TracePacket> packets;
  for (const auto& packet_proto : trace.packet()) {
    std::vector<uint8_t> buf = packet_proto.SerializeAsArray();
    Slice slice = Slice::Allocate(buf.size());
    memcpy(slice.own_data(), buf.data(), buf.size());

    TracePacket packet;
    packet.AddSlice(std::move(slice));
    packets.push_back(std::move(packet));
  }
  return packets;
}

// Copy packets for benchmarking (since compression modifies the vector)
std::vector<TracePacket> CopyPackets(const std::vector<TracePacket>& packets) {
  std::vector<TracePacket> result;
  result.reserve(packets.size());
  for (const auto& packet : packets) {
    TracePacket copy;
    for (const Slice& slice : packet.slices()) {
      Slice new_slice = Slice::Allocate(slice.size);
      memcpy(new_slice.own_data(), slice.start, slice.size);
      copy.AddSlice(std::move(new_slice));
    }
    result.push_back(std::move(copy));
  }
  return result;
}

#if PERFETTO_BUILDFLAG(PERFETTO_ZLIB)
// Compress using zlib at specified level
size_t CompressZlib(const std::vector<TracePacket>& packets, int level) {
  z_stream stream{};
  deflateInit(&stream, level);

  std::vector<uint8_t> output(1024 * 1024);  // 1MB initial buffer
  stream.next_out = output.data();
  stream.avail_out = static_cast<uInt>(output.size());

  for (const TracePacket& packet : packets) {
    for (const Slice& slice : packet.slices()) {
      stream.next_in =
          const_cast<Bytef*>(static_cast<const Bytef*>(slice.start));
      stream.avail_in = static_cast<uInt>(slice.size);
      while (stream.avail_in > 0) {
        if (stream.avail_out == 0) {
          output.resize(output.size() * 2);
          stream.next_out = output.data() + stream.total_out;
          stream.avail_out =
              static_cast<uInt>(output.size() - stream.total_out);
        }
        deflate(&stream, Z_NO_FLUSH);
      }
    }
  }

  while (deflate(&stream, Z_FINISH) != Z_STREAM_END) {
    output.resize(output.size() * 2);
    stream.next_out = output.data() + stream.total_out;
    stream.avail_out = static_cast<uInt>(output.size() - stream.total_out);
  }

  size_t compressed_size = stream.total_out;
  deflateEnd(&stream);

  return compressed_size;
}
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_ZSTD)
// Compress using zstd at specified level
size_t CompressZstd(const std::vector<TracePacket>& packets, int level) {
  // Collect all data into a single buffer first
  std::vector<uint8_t> input_buffer;
  for (const TracePacket& packet : packets) {
    for (const Slice& slice : packet.slices()) {
      input_buffer.insert(
          input_buffer.end(), static_cast<const uint8_t*>(slice.start),
          static_cast<const uint8_t*>(slice.start) + slice.size);
    }
  }

  size_t max_compressed_size = ZSTD_compressBound(input_buffer.size());
  std::vector<uint8_t> output(max_compressed_size);

  size_t compressed_size =
      ZSTD_compress(output.data(), output.size(), input_buffer.data(),
                    input_buffer.size(), level);

  if (ZSTD_isError(compressed_size)) {
    return 0;
  }

  return compressed_size;
}
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_ZLIB)
static void BM_CompressZlib(benchmark::State& state) {
  int level = static_cast<int>(state.range(0));

  // Load real trace data once (use pointer to avoid exit-time destructor)
  static std::vector<uint8_t>* trace_data =
      new std::vector<uint8_t>(LoadTestTrace("wattson_tk4_pcmark.pb"));
  static std::vector<TracePacket>* packets =
      new std::vector<TracePacket>(ParseTracePackets(*trace_data));

  if (packets->empty()) {
    state.SkipWithError("Failed to load test trace");
    return;
  }

  size_t compressed_size = 0;
  for (auto _ : state) {
    auto packets_copy = CopyPackets(*packets);
    compressed_size = CompressZlib(packets_copy, level);
    benchmark::DoNotOptimize(compressed_size);
  }

  // Report compression stats
  size_t original_size = 0;
  for (const auto& p : *packets) {
    original_size += p.size();
  }

  state.counters["original_mb"] =
      benchmark::Counter(static_cast<double>(original_size) / (1024 * 1024),
                         benchmark::Counter::kDefaults);
  state.counters["compressed_mb"] =
      benchmark::Counter(static_cast<double>(compressed_size) / (1024 * 1024),
                         benchmark::Counter::kDefaults);
  state.counters["ratio"] = benchmark::Counter(
      static_cast<double>(original_size) / static_cast<double>(compressed_size),
      benchmark::Counter::kDefaults);
  state.counters["bytes_per_sec"] = benchmark::Counter(
      static_cast<double>(original_size), benchmark::Counter::kIsRate);
}
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_ZSTD)
static void BM_CompressZstd(benchmark::State& state) {
  int level = static_cast<int>(state.range(0));

  // Load real trace data once (use pointer to avoid exit-time destructor)
  static std::vector<uint8_t>* trace_data =
      new std::vector<uint8_t>(LoadTestTrace("wattson_tk4_pcmark.pb"));
  static std::vector<TracePacket>* packets =
      new std::vector<TracePacket>(ParseTracePackets(*trace_data));

  if (packets->empty()) {
    state.SkipWithError("Failed to load test trace");
    return;
  }

  size_t compressed_size = 0;
  for (auto _ : state) {
    auto packets_copy = CopyPackets(*packets);
    compressed_size = CompressZstd(packets_copy, level);
    benchmark::DoNotOptimize(compressed_size);
  }

  // Report compression stats
  size_t original_size = 0;
  for (const auto& p : *packets) {
    original_size += p.size();
  }

  state.counters["original_mb"] =
      benchmark::Counter(static_cast<double>(original_size) / (1024 * 1024),
                         benchmark::Counter::kDefaults);
  state.counters["compressed_mb"] =
      benchmark::Counter(static_cast<double>(compressed_size) / (1024 * 1024),
                         benchmark::Counter::kDefaults);
  state.counters["ratio"] = benchmark::Counter(
      static_cast<double>(original_size) / static_cast<double>(compressed_size),
      benchmark::Counter::kDefaults);
  state.counters["bytes_per_sec"] = benchmark::Counter(
      static_cast<double>(original_size), benchmark::Counter::kIsRate);
}
#endif

// Benchmark different compression levels
#if PERFETTO_BUILDFLAG(PERFETTO_ZLIB)
BENCHMARK(BM_CompressZlib)->Arg(1)->Arg(6)->Arg(9);
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_ZSTD)
BENCHMARK(BM_CompressZstd)->Arg(1)->Arg(3)->Arg(6)->Arg(9)->Arg(19);
#endif

}  // namespace
}  // namespace perfetto
