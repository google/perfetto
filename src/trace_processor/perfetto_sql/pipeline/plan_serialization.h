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
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/perfetto_sql/pipeline/catalog.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_key.h"

namespace perfetto::trace_processor::pipeline {

// -----------------------------------------------------------------------------
// Plan serialization API
// -----------------------------------------------------------------------------

// The most columns a pipeline can output.
inline constexpr uint32_t kMaxPipelineColumns = 256;

// Writes an optimized plan as bytes, stamped with `key`, so that SQL can carry
// it and run it later without compiling the pipeline again.
std::string SerializePlan(LogicalPlan, const PlanKey& key);

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
    BoundPlan* plan,
    const std::vector<const dataframe::Dataframe*>& args,
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
  // Writes into `out`.
  PlanCodec(LogicalPlan* plan, const PlanKey* key, std::string* out)
      : plan_(plan), key_(key), out_(out) {}
  // Reads `in`, which SerializePlan wrote.
  PlanCodec(LogicalPlan* plan, const PlanKey* key, std::string_view in)
      : plan_(plan), key_(key), in_(in) {}

  bool reading() const { return !out_; }
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
  // The size of a list: `size` when writing, the size written when reading.
  uint32_t Count(size_t size);
  void Str(std::string* value);

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
