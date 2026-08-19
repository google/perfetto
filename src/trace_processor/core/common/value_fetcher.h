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

namespace perfetto::trace_processor::core {

// Fetches values from an arbitrary indexed source. The meaning of the index in
// each of the *Value methods varies depending on where this class is used.
//
// Called a handful of times per query execution, once per filter value, so the
// dispatch costs nothing next to running the query.
class ValueFetcher {
 public:
  // The values are SQLite's own type tags, so a SQLite-backed fetcher returns
  // its source type without translating it. The static_asserts that hold them
  // together live with that fetcher, where the SQLite headers are in scope.
  enum class Type : uint8_t {
    kInt64 = 1,
    kDouble = 2,
    kString = 3,
    kBytes = 4,
    kNull = 5,
  };

  virtual ~ValueFetcher();

  // Scalars. The caller knows the value at the index is one.
  virtual Type GetValueType(uint32_t) = 0;
  virtual int64_t GetInt64Value(uint32_t) = 0;
  virtual double GetDoubleValue(uint32_t) = 0;
  virtual const char* GetStringValue(uint32_t) = 0;

  // Iterators, for a value that is a list. The caller knows the value at the
  // index is one. Init returns whether there is a first element, Next whether
  // there is another.
  virtual bool IteratorInit(uint32_t) = 0;
  virtual bool IteratorNext(uint32_t) = 0;
};

// Stands in where a fetcher is required but no filter values exist, as when
// iterating a dataframe with no filters at all.
class ErrorValueFetcher final : public ValueFetcher {
 public:
  ~ErrorValueFetcher() override;

  Type GetValueType(uint32_t) override;
  int64_t GetInt64Value(uint32_t) override;
  double GetDoubleValue(uint32_t) override;
  const char* GetStringValue(uint32_t) override;
  bool IteratorInit(uint32_t) override;
  bool IteratorNext(uint32_t) override;
};

}  // namespace perfetto::trace_processor::core

#endif  // SRC_TRACE_PROCESSOR_CORE_COMMON_VALUE_FETCHER_H_
