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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PLAN_KEY_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PLAN_KEY_H_

#include <cstdint>
#include <string_view>

namespace perfetto::trace_processor::pipeline {

// A secret which stamps the plans a connection writes into SQL, so that when
// one is read back it is known to be one the connection wrote. SQL cannot see
// the key, so it cannot forge a stamp.
class PlanKey {
 public:
  // What a stamp is of. Each is stamped differently, so that a stamp of one
  // can never pass as a stamp of another.
  enum class Use : uint8_t {
    kPlan,
    kTable,
  };

  PlanKey(uint64_t k0, uint64_t k1) : k0_(k0), k1_(k1) {}

  // A key no one else knows.
  static PlanKey Random();

  // The stamp of `data`, for `use`.
  uint64_t Stamp(Use use, std::string_view data) const;

  // SipHash-2-4 of `data`, exposed to test against the reference vectors.
  static uint64_t SipHash(uint64_t k0, uint64_t k1, std::string_view data);

 private:
  uint64_t k0_;
  uint64_t k1_;
};

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PLAN_KEY_H_
