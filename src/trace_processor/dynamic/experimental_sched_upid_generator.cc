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
namespace tables {
#define PERFETTO_TP_SCHED_UPID_TABLE_DEF(NAME, PARENT, C)     \
  NAME(ExperimentalSchedUpidTable, "experimental_sched_upid") \
  PARENT(PERFETTO_TP_SCHED_SLICE_TABLE_DEF, C)                \
  C(base::Optional<UniquePid>, upid)

PERFETTO_TP_TABLE(PERFETTO_TP_SCHED_UPID_TABLE_DEF);

ExperimentalSchedUpidTable::~ExperimentalSchedUpidTable() = default;
}  // namespace tables

ExperimentalSchedUpidGenerator::ExperimentalSchedUpidGenerator(
    const tables::SchedSliceTable& sched,
    const tables::ThreadTable& thread)
    : sched_slice_table_(&sched), thread_table_(&thread) {}
ExperimentalSchedUpidGenerator::~ExperimentalSchedUpidGenerator() = default;

Table::Schema ExperimentalSchedUpidGenerator::CreateSchema() {
  return tables::ExperimentalSchedUpidTable::ComputeStaticSchema();
}

std::string ExperimentalSchedUpidGenerator::TableName() {
  return tables::ExperimentalSchedUpidTable::Name();
}

uint32_t ExperimentalSchedUpidGenerator::EstimateRowCount() {
  return sched_slice_table_->row_count();
}

base::Status ExperimentalSchedUpidGenerator::ValidateConstraints(
    const QueryConstraints&) {
  return base::OkStatus();
}

base::Status ExperimentalSchedUpidGenerator::ComputeTable(
    const std::vector<Constraint>&,
    const std::vector<Order>&,
    const BitVector&,
    std::unique_ptr<Table>& table_return) {
  if (!sched_upid_table_) {
    sched_upid_table_ = tables::ExperimentalSchedUpidTable::ExtendParent(
        *sched_slice_table_, ComputeUpidColumn());
  }
  table_return.reset(new Table(sched_upid_table_->Copy()));
  return base::OkStatus();
}

ColumnStorage<base::Optional<UniquePid>>
ExperimentalSchedUpidGenerator::ComputeUpidColumn() {
  ColumnStorage<base::Optional<UniquePid>> upid;
  for (uint32_t i = 0; i < sched_slice_table_->row_count(); ++i) {
    upid.Append(thread_table_->upid()[sched_slice_table_->utid()[i]]);
  }
  return upid;
}

}  // namespace trace_processor
}  // namespace perfetto
