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

#include <cstdint>
#include <random>
#include <vector>

#include "src/trace_processor/util/graph.h"

namespace perfetto::trace_processor::util {
namespace {

constexpr uint32_t kNodes = 1 << 20;

// Like a Java heap: every object is owned by an earlier one, and also holds
// two references to objects anywhere in the heap.
Digraph HeapLike() {
  std::minstd_rand rng(42);
  std::vector<uint32_t> sources;
  std::vector<uint32_t> targets;
  for (uint32_t n = 1; n < kNodes; ++n) {
    sources.push_back(static_cast<uint32_t>(rng() % n));
    targets.push_back(n);
  }
  for (uint32_t n = 0; n < kNodes; ++n) {
    for (int i = 0; i < 2; ++i) {
      sources.push_back(n);
      targets.push_back(static_cast<uint32_t>(rng() % kNodes));
    }
  }
  return Digraph::FromEdges(kNodes, sources, targets);
}

// One long path, as a linked list in a heap is.
Digraph Chain() {
  std::vector<uint32_t> sources;
  std::vector<uint32_t> targets;
  for (uint32_t n = 1; n < kNodes; ++n) {
    sources.push_back(n - 1);
    targets.push_back(n);
  }
  return Digraph::FromEdges(kNodes, sources, targets);
}

void BM_DominatorTreeHeapLike(benchmark::State& state) {
  Digraph graph = HeapLike();
  for (auto _ : state) {
    benchmark::DoNotOptimize(DominatorTree(graph, 0));
  }
}
BENCHMARK(BM_DominatorTreeHeapLike)->Unit(benchmark::kMillisecond);

void BM_DominatorTreeChain(benchmark::State& state) {
  Digraph graph = Chain();
  for (auto _ : state) {
    benchmark::DoNotOptimize(DominatorTree(graph, 0));
  }
}
BENCHMARK(BM_DominatorTreeChain)->Unit(benchmark::kMillisecond);

}  // namespace
}  // namespace perfetto::trace_processor::util
