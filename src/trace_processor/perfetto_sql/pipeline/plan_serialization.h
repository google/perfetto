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
std::string SerializePlan(LogicalPlan);

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
// Plan encoding
// -----------------------------------------------------------------------------

// Moves a plan to or from bytes. Each operation describes its payload once,
// in PlanOperation::Serialize, which a codec runs either way: writing, each
// call takes the field it is given; reading, it fills the field in, checking
// it as it goes. A read fails on anything a writer could not have written,
// and the failure latches: later fields read as zero.
class PlanCodec {
 public:
  virtual ~PlanCodec();

  bool reading() const { return reading_; }
  LogicalPlan& plan() { return *plan_; }

  virtual void U8(uint8_t*) = 0;
  virtual void U32(uint32_t*) = 0;
  virtual void U64(uint64_t*) = 0;
  virtual void F64(double*) = 0;
  virtual void Str(std::string*) = 0;
  // A name, as its id in the plan's names.
  virtual void Name(NameId*) = 0;
  // A column, as its position among `available`.
  virtual void Column(const Available& available, ColumnId*) = 0;
  // The size of a list: `size` when writing; when reading, a size no larger
  // than the bytes left, so a corrupt one cannot allocate much.
  virtual uint32_t Count(size_t size) = 0;
  // Fails a read unless `ok`. Whatever a writer writes passes.
  virtual void Check(bool ok) = 0;
  // False once a read has failed, after which ids read are not to be used.
  virtual bool ok() const = 0;

  void Bool(bool* value);
  void Columns(const Available& available, std::vector<ColumnId>* columns);
  void Type(PlanType* type);
  // A column the operation defines, of `type`: written as its name; read, it
  // is added to the plan.
  void NewColumn(ColumnId* id, const PlanType& type);
  // An enum, which a read checks is no greater than `max`.
  template <typename E>
  void Enum(E* value, E max) {
    auto raw = static_cast<uint8_t>(*value);
    U8(&raw);
    Check(raw <= static_cast<uint8_t>(max));
    *value = static_cast<E>(raw);
  }

 protected:
  PlanCodec(LogicalPlan* plan, bool reading) : plan_(plan), reading_(reading) {}

 private:
  LogicalPlan* plan_;
  bool reading_;
};

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PLAN_SERIALIZATION_H_
