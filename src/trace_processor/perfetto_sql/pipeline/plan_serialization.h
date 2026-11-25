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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PLAN_SERIALIZATION_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PLAN_SERIALIZATION_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/span.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/util/arena.h"
#include "src/trace_processor/perfetto_sql/pipeline/catalog.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_key.h"

namespace perfetto::trace_processor::pipeline {

// A plan whose SQL sources have been moved out into dataframe arguments: the
// plan reads its i-th SQL source as dataframe argument i, and `args[i]` is the
// SQL building it.
struct PlanWithDataframeArgs {
  LogicalPlan plan;
  std::vector<std::string> args;
};
PlanWithDataframeArgs MoveSqlSourcesToDataframeArgs(LogicalPlan plan);

// -----------------------------------------------------------------------------
// Internal plan serialization and binding API
// -----------------------------------------------------------------------------

// Writes an optimized plan as bytes. When `key` is non-null, appends an HMAC
// stamp so untrusted carriers (such as SQL) can authenticate it.
std::string SerializePlan(const LogicalPlan& plan,
                          const PlanKey* key = nullptr);

// Rebuilds a plan written by SerializePlan(plan, nullptr).
LogicalPlan DeserializePlanUnstamped(std::string_view bytes);

// Rebuilds a plan written by SerializePlan(plan, &key), verifying the stamp
// without resolving its catalog dataframes yet.
base::StatusOr<LogicalPlan> DeserializePlanBytes(std::string_view bytes,
                                                 const PlanKey& key);

// Clones an arena-owned LogicalPlan by round-tripping through PlanCodec.
LogicalPlan ClonePlan(const LogicalPlan& plan);

// Rebuilds a plan written by SerializePlan, binding the dataframes it names
// to those in `catalog` now. Anyone can write bytes into SQL, so bytes without
// the stamp of `key` are refused, as is a plan whose dataframes have since
// changed.
base::StatusOr<BoundPlan> DeserializePlan(std::string_view,
                                          const PlanKey& key,
                                          const Catalog&);

// Binds the dataframes a plan compiled in this process names, which it never
// wrote, to those in `catalog`.
base::StatusOr<BoundPlan> ResolveCompiledPlan(LogicalPlan, const Catalog&);

// Binds each scan of a dataframe argument to `args[i]`, the dataframe the
// plan is passed as its i-th argument when it runs, and types the scan's
// columns as the dataframe does. A null argument is a relation with no rows.
// Anyone can pass dataframes to a plan in SQL, so each must have the columns
// the plan reads from it.
base::Status BindDataframeArgs(
    BoundPlan* bound,
    base::Span<const dataframe::Dataframe* const> args,
    StringPool* pool);

// Unified internal binding entry point: resolves any catalog-named scans
// against `catalog` (checking table stamps against `key` when non-null) and
// attaches any positional dataframe arguments from `args`.
base::StatusOr<BoundPlan> BindPlan(
    LogicalPlan plan,
    const Catalog* catalog,
    const PlanKey* key,
    base::Span<const dataframe::Dataframe* const> args,
    StringPool* pool);

// -----------------------------------------------------------------------------
// Plan encoding
// -----------------------------------------------------------------------------

// Moves a plan to or from bytes. Each operation describes its payload once,
// in PlanOperation::Serialize, which a codec runs either way: writing, each
// call appends the field it is given; reading, it fills the field in. Only
// stamped bytes are read, so a read trusts them. Fields are in the host's byte
// order: a plan is only read by the process which wrote it.
class PlanCodec {
 public:
  // Writes into `out` without mutating `plan`.
  PlanCodec(const LogicalPlan* plan, const PlanKey* key, std::string* out)
      : plan_(const_cast<LogicalPlan*>(plan)), key_(key), out_(out) {}
  // Reads `in`, which SerializePlan wrote.
  PlanCodec(LogicalPlan* plan, const PlanKey* key, std::string_view in)
      : plan_(plan), key_(key), in_(in) {}

  bool reading() const { return !out_; }
  bool has_key() const { return key_ != nullptr; }
  LogicalPlan& plan() { return *plan_; }
  const PlanKey& key() const { return *key_; }

  template <typename T>
  void Pod(T* value) {
    static_assert(kCopiedAsBytes<T>);
    Bytes(value, sizeof(T));
  }
  template <typename T>
  void Vec(std::vector<T>* values) {
    static_assert(kCopiedAsBytes<T>);
    values->resize(Count(values->size()));
    Bytes(values->data(), values->size() * sizeof(T));
  }
  template <typename T>
  void Vec(core::ArenaSpan<T>* values) {
    static_assert(kCopiedAsBytes<T>);
    uint32_t size = Count(values->size());
    if (reading()) {
      *values = plan_->arena().NewSpan<T>(size);
    }
    Bytes(values->begin(), size * sizeof(T));
  }
  // Sizes a list whose values are written one at a time. When reading, it makes
  // as many empty values as were written, for the reader to fill in.
  template <typename T>
  void Resize(core::ArenaSpan<T>* values) {
    uint32_t size = Count(values->size());
    if (reading()) {
      *values = plan_->arena().NewSpan<T>(size);
      for (T& value : *values) {
        new (&value) T();
      }
    }
  }
  // The size of a list: `size` when writing, the size written when reading.
  uint32_t Count(size_t size);
  // When reading, the string is made in the plan's arena.
  void Str(std::string_view* value);
  // When reading, borrows directly from the input buffer without copying into
  // the plan's arena.
  void BorrowStr(std::string_view* value);

 private:
  // Appends `size` bytes from `data` or reads them into it.
  void Bytes(void* data, size_t size);

  LogicalPlan* plan_;
  const PlanKey* key_;
  std::string* out_ = nullptr;
  std::string_view in_;
};

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PLAN_SERIALIZATION_H_
