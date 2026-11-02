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

#include "src/trace_processor/perfetto_sql/pipeline/plan_key.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <string_view>

#include "perfetto/base/compiler.h"
#include "perfetto/base/endian.h"

namespace perfetto::trace_processor::pipeline {
namespace {

uint64_t Rotl(uint64_t x, int bits) {
  return (x << bits) | (x >> (64 - bits));
}

struct SipState {
  PERFETTO_NO_INLINE void Round() {
    v0 += v1;
    v1 = Rotl(v1, 13);
    v1 ^= v0;
    v0 = Rotl(v0, 32);
    v2 += v3;
    v3 = Rotl(v3, 16);
    v3 ^= v2;
    v0 += v3;
    v3 = Rotl(v3, 21);
    v3 ^= v0;
    v2 += v1;
    v1 = Rotl(v1, 17);
    v1 ^= v2;
    v2 = Rotl(v2, 32);
  }

  void Compress(uint64_t m) {
    v3 ^= m;
    Round();
    Round();
    v0 ^= m;
  }

  uint64_t v0;
  uint64_t v1;
  uint64_t v2;
  uint64_t v3;
};

}  // namespace

PlanKey PlanKey::Random() {
  std::random_device device;
  auto word = [&device] {
    return static_cast<uint64_t>(device()) << 32 | device();
  };
  uint64_t k0 = word();
  return PlanKey(k0, word());
}

uint64_t PlanKey::Stamp(Use use, std::string_view data) const {
  std::string in(1, static_cast<char>(use));
  in.append(data);
  return SipHash(k0_, k1_, in);
}

uint64_t PlanKey::SipHash(uint64_t k0, uint64_t k1, std::string_view data) {
  SipState s{k0 ^ 0x736f6d6570736575ull, k1 ^ 0x646f72616e646f6dull,
             k0 ^ 0x6c7967656e657261ull, k1 ^ 0x7465646279746573ull};
  size_t whole = data.size() & ~size_t{7};
  PERFETTO_NO_VECTORIZE_LOOP
  for (size_t i = 0; i < whole; i += 8) {
    s.Compress(base::LoadLE<uint64_t>(data.data() + i));
  }
  // SipHash reads words little endian.
  uint64_t last = 0;
  memcpy(&last, data.data() + whole, data.size() - whole);
  s.Compress(base::LEToHost(last) | static_cast<uint64_t>(data.size()) << 56);
  s.v2 ^= 0xff;
  PERFETTO_NO_VECTORIZE_LOOP
  for (int i = 0; i < 4; ++i) {
    s.Round();
  }
  return s.v0 ^ s.v1 ^ s.v2 ^ s.v3;
}

}  // namespace perfetto::trace_processor::pipeline
