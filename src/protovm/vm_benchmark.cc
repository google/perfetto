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

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/protozero/field.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "src/base/test/utils.h"
#include "src/protovm/vm.h"

namespace perfetto {
namespace protovm {
namespace {

constexpr size_t kMemoryLimitBytes = 10 * 1024 * 1024;

void BenchmarkArgs(benchmark::internal::Benchmark* b) {
  bool is_benchmark_functional_only =
      getenv("BENCHMARK_FUNCTIONAL_TEST_ONLY") != nullptr;
  if (is_benchmark_functional_only) {
    b->Iterations(1);
  }
}

protozero::ConstBytes ExtractProgram(const std::string& trace_bytes) {
  protos::pbzero::Trace::Decoder trace(trace_bytes);
  for (auto packet_it = trace.packet(); packet_it; ++packet_it) {
    protos::pbzero::TracePacket::Decoder packet(*packet_it);
    if (!packet.has_protovms()) {
      continue;
    }
    protos::pbzero::TracePacket::ProtoVms::Decoder protovms(packet.protovms());
    if (auto instance_it = protovms.instance(); instance_it) {
      protos::pbzero::TracePacket::ProtoVms::Instance::Decoder instance(
          *instance_it);
      PERFETTO_CHECK(instance.program().data != nullptr &&
                     instance.program().size > 0);
      return instance.program();
    }
  }
  PERFETTO_FATAL("Trace has no program");
}

std::vector<protozero::ConstBytes> ExtractPatches(
    const std::string& trace_bytes) {
  std::vector<protozero::ConstBytes> patches;
  protos::pbzero::Trace::Decoder trace(trace_bytes);
  for (auto packet_it = trace.packet(); packet_it; ++packet_it) {
    protos::pbzero::TracePacket::Decoder packet(*packet_it);
    if (!packet.has_protovms()) {
      patches.push_back(*packet_it);
    }
  }

  PERFETTO_CHECK(!patches.empty());
  return patches;
}

void SurfaceFlingerTrace(benchmark::State& state) {
  std::string trace;
  PERFETTO_CHECK(base::ReadFile(
      base::GetTestDataPath("test/data/protovm_surfaceflinger.pftrace"),
      &trace));

  protozero::ConstBytes program = ExtractProgram(trace);

  std::vector<protozero::ConstBytes> patches = ExtractPatches(trace);

  for (auto _ : state) {
    Vm vm{program, kMemoryLimitBytes};
    for (const auto& patch : patches) {
      auto status = vm.ApplyPatch(patch);
      PERFETTO_CHECK(status.IsOk());
    }
    benchmark::DoNotOptimize(vm);
    benchmark::ClobberMemory();
  }
}

BENCHMARK(SurfaceFlingerTrace)->Apply(BenchmarkArgs);

}  // namespace
}  // namespace protovm
}  // namespace perfetto
