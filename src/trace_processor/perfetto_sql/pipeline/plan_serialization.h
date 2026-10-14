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
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/perfetto_sql/pipeline/catalog.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"

namespace perfetto::trace_processor::pipeline {

// -----------------------------------------------------------------------------
// Plan serialization API
// -----------------------------------------------------------------------------

// The most columns a pipeline can output.
inline constexpr uint32_t kMaxPipelineColumns = 256;

// Writes an optimized plan as bytes, so that SQL can carry it and run it later
// without compiling the pipeline again.
std::string SerializePlan(const LogicalPlan&);

// Rebuilds a plan written by SerializePlan, looking its dataframes up in
// `catalog` again. Anyone can write bytes into SQL, so anything SerializePlan
// could not have written is refused, as is a plan whose dataframes have since
// changed.
base::StatusOr<LogicalPlan> DeserializePlan(std::string_view, const Catalog&);

// Points each scan of a dataframe argument at `args[i]`, the dataframe the
// plan is passed as its i-th argument when it runs, and types the scan's
// columns as the dataframe does. A null argument is a relation with no rows.
// Anyone can pass dataframes to a plan in SQL, so each must have the columns
// the plan reads from it.
base::Status BindDataframeArgs(
    LogicalPlan& plan,
    const std::vector<const dataframe::Dataframe*>& args,
    StringPool* pool);

// -----------------------------------------------------------------------------
// Byte encoding
// -----------------------------------------------------------------------------

class Writer {
 public:
  void U8(uint8_t value);
  void U32(uint32_t value);
  void Size(size_t value);
  void Str(std::string_view value);

  // Out of bounds if `id` is not available.
  void Position(const Available& available, ColumnId id);

  std::string Take();

 private:
  void Append(const void* data, size_t size);

  std::string out_;
};

// Failures are latched: ok() remains false after any invalid read. Callers
// must discard the partially decoded plan on failure.
class Reader {
 public:
  explicit Reader(std::string_view in);

  uint8_t U8();
  uint32_t U32();

  // At most the bytes left, so a corrupt count cannot allocate much.
  uint32_t Count();
  std::string Str();

  ColumnId Position(const Available& available);
  void Fail();

  bool ok() const;
  bool done() const;

 private:
  void Read(void* out, size_t size);

  std::string_view in_;
  size_t at_ = 0;
  bool ok_ = true;
};

void WriteType(Writer* w, const std::optional<core::StorageType>& type);
std::optional<core::StorageType> ReadType(Reader* r);

// -----------------------------------------------------------------------------
// Plan encoding
// -----------------------------------------------------------------------------

// Writes stages in input-to-output order. Operations update Available from
// input to output column IDs as their payloads are written.
class PlanWriter {
 public:
  explicit PlanWriter(const LogicalPlan& plan);
  std::string Write();

  const LogicalPlan& plan() const { return plan_; }
  Writer& writer() { return w_; }

 private:
  // Borrowed for the duration of encoding.
  const LogicalPlan& plan_;
  Writer w_;
};

// Reconstructs logical IDs from encoded column positions. Operation decoders
// append nodes to plan_ and update Available to match their output.
class PlanReader {
 public:
  explicit PlanReader(Reader& r);

  LogicalPlan Read();
  ColumnId AddColumn(std::string name, std::optional<core::StorageType> type);

  template <typename T>
  PlanNodeId AddNode(T operation, std::vector<PlanNodeId> children = {}) {
    return plan_.AddNode(std::move(operation), std::move(children));
  }

  const LogicalPlan& plan() const { return plan_; }
  Reader& reader() { return r_; }

 private:
  // Borrowed reader; its error state covers the whole plan.
  Reader& r_;
  LogicalPlan plan_;
};

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PLAN_SERIALIZATION_H_
