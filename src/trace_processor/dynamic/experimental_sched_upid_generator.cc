/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "src/trace_processor/dynamic/experimental_sched_upid_generator.h"

namespace perfetto {
namespace trace_processor {

ExperimentalSchedUpidGenerator::ExperimentalSchedUpidGenerator(
    const tables::SchedSliceTable& sched,
    const tables::ThreadTable& thread)
    : sched_slice_table_(&sched), thread_table_(&thread) {}
ExperimentalSchedUpidGenerator::~ExperimentalSchedUpidGenerator() = default;

Table::Schema ExperimentalSchedUpidGenerator::CreateSchema() {
  Table::Schema schema = tables::SchedSliceTable::Schema();
  schema.columns.emplace_back(
      Table::Schema::Column{"upid", SqlValue::Type::kLong, false /* is_id */,
                            false /* is_sorted */, false /* is_hidden */});
  return schema;
}

std::string ExperimentalSchedUpidGenerator::TableName() {
  return "experimental_sched_upid";
}

uint32_t ExperimentalSchedUpidGenerator::EstimateRowCount() {
  return sched_slice_table_->row_count();
}

util::Status ExperimentalSchedUpidGenerator::ValidateConstraints(
    const QueryConstraints&) {
  return util::OkStatus();
}

std::unique_ptr<Table> ExperimentalSchedUpidGenerator::ComputeTable(
    const std::vector<Constraint>&,
    const std::vector<Order>&) {
  if (!upid_column_) {
    upid_column_.reset(new NullableVector<uint32_t>(ComputeUpidColumn()));
  }
  return std::unique_ptr<Table>(new Table(sched_slice_table_->ExtendWithColumn(
      "upid", upid_column_.get(),
      TypedColumn<base::Optional<uint32_t>>::default_flags())));
}

NullableVector<uint32_t> ExperimentalSchedUpidGenerator::ComputeUpidColumn() {
  NullableVector<uint32_t> upid;
  for (uint32_t i = 0; i < sched_slice_table_->row_count(); ++i) {
    upid.Append(thread_table_->upid()[sched_slice_table_->utid()[i]]);
  }
  return upid;
}

}  // namespace trace_processor
}  // namespace perfetto
