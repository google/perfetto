// Copyright (C) 2019 The Android Open Source Project
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

#include <random>

#include <benchmark/benchmark.h>

#include <cstdint>

#include "perfetto/base/compiler.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"

namespace perfetto::base {
namespace {

// Chains of calls each returning a Status or StatusOr, as code propagating
// errors through layers does. Kept out of line so each return is a real one.
PERFETTO_NO_INLINE Status Leaf(int64_t i) {
  benchmark::DoNotOptimize(i);
  return i < 0 ? ErrStatus("negative: %d", static_cast<int>(i)) : OkStatus();
}

PERFETTO_NO_INLINE Status Middle(int64_t i) {
  RETURN_IF_ERROR(Leaf(i));
  RETURN_IF_ERROR(Leaf(i + 1));
  return OkStatus();
}

PERFETTO_NO_INLINE Status Top(int64_t i) {
  RETURN_IF_ERROR(Middle(i));
  RETURN_IF_ERROR(Middle(i + 2));
  return OkStatus();
}

PERFETTO_NO_INLINE StatusOr<int64_t> LeafOr(int64_t i) {
  benchmark::DoNotOptimize(i);
  if (i < 0) {
    return ErrStatus("negative: %d", static_cast<int>(i));
  }
  return i * 2;
}

PERFETTO_NO_INLINE StatusOr<int64_t> MiddleOr(int64_t i) {
  ASSIGN_OR_RETURN(int64_t a, LeafOr(i));
  ASSIGN_OR_RETURN(int64_t b, LeafOr(i + 1));
  return a + b;
}

PERFETTO_NO_INLINE StatusOr<int64_t> TopOr(int64_t i) {
  ASSIGN_OR_RETURN(int64_t a, MiddleOr(i));
  ASSIGN_OR_RETURN(int64_t b, MiddleOr(i + 2));
  return a + b;
}

void BM_StatusOkChain(benchmark::State& state) {
  int64_t i = 0;
  for (auto _ : state) {
    Status status = Top(i++);
    benchmark::DoNotOptimize(status.ok());
  }
}
BENCHMARK(BM_StatusOkChain);

void BM_StatusErrorChain(benchmark::State& state) {
  for (auto _ : state) {
    Status status = Top(-10);
    benchmark::DoNotOptimize(status.ok());
  }
}
BENCHMARK(BM_StatusErrorChain);

void BM_StatusOrOkChain(benchmark::State& state) {
  int64_t i = 0;
  for (auto _ : state) {
    StatusOr<int64_t> value = TopOr(i++);
    benchmark::DoNotOptimize(value.ok());
  }
}
BENCHMARK(BM_StatusOrOkChain);

void BM_StatusOrErrorChain(benchmark::State& state) {
  for (auto _ : state) {
    StatusOr<int64_t> value = TopOr(-10);
    benchmark::DoNotOptimize(value.ok());
  }
}
BENCHMARK(BM_StatusOrErrorChain);

}  // namespace
}  // namespace perfetto::base
