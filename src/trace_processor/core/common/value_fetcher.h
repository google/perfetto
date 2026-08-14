/*
 * Copyright (C) 2025 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_CORE_COMMON_VALUE_FETCHER_H_
#define SRC_TRACE_PROCESSOR_CORE_COMMON_VALUE_FETCHER_H_

#include <cstdint>

#include "perfetto/base/logging.h"

namespace perfetto::trace_processor::core {

// Fetches filter values from an arbitrary indexed source.
struct ValueFetcher {
  // These values match SQLite's fundamental type tags so SQLite-backed
  // fetchers can return their source type without translating it.
  using Type = uint8_t;
  static constexpr Type kInt64 = 1;
  static constexpr Type kDouble = 2;
  static constexpr Type kString = 3;
  static constexpr Type kBytes = 4;
  static constexpr Type kNull = 5;

  virtual ~ValueFetcher();

  virtual int64_t GetInt64Value(uint32_t) const = 0;
  virtual double GetDoubleValue(uint32_t) const = 0;
  virtual const char* GetStringValue(uint32_t) const = 0;
  virtual Type GetValueType(uint32_t) const = 0;

  virtual bool IteratorInit(uint32_t) = 0;
  virtual bool IteratorNext(uint32_t) = 0;
};

struct ErrorValueFetcher final : ValueFetcher {
  ~ErrorValueFetcher() override;
  int64_t GetInt64Value(uint32_t) const override {
    PERFETTO_FATAL("Dummy implementation; should not be called");
  }
  double GetDoubleValue(uint32_t) const override {
    PERFETTO_FATAL("Dummy implementation; should not be called");
  }
  const char* GetStringValue(uint32_t) const override {
    PERFETTO_FATAL("Dummy implementation; should not be called");
  }
  Type GetValueType(uint32_t) const override {
    PERFETTO_FATAL("Dummy implementation; should not be called");
  }
  bool IteratorInit(uint32_t) override { PERFETTO_FATAL("Unsupported"); }
  bool IteratorNext(uint32_t) override { PERFETTO_FATAL("Unsupported"); }
};

}  // namespace perfetto::trace_processor::core

#endif  // SRC_TRACE_PROCESSOR_CORE_COMMON_VALUE_FETCHER_H_
