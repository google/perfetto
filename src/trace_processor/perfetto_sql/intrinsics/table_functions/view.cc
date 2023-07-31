/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/view.h"

namespace perfetto {
namespace trace_processor {

ViewStaticTableFunction::ViewStaticTableFunction(const View* view,
                                                 const char* name)
    : view_(view), name_(name) {}

ViewStaticTableFunction::~ViewStaticTableFunction() = default;

base::Status ViewStaticTableFunction::ValidateConstraints(
    const QueryConstraints&) {
  return base::OkStatus();
}

base::Status ViewStaticTableFunction::ComputeTable(
    const std::vector<Constraint>& cs,
    const std::vector<Order>& ob,
    const BitVector& cols_used,
    std::unique_ptr<Table>& table) {
  table.reset(new Table(view_->Query(cs, ob, cols_used)));
  return base::OkStatus();
}

Table::Schema ViewStaticTableFunction::CreateSchema() {
  return view_->schema();
}

std::string ViewStaticTableFunction::TableName() {
  return name_;
}

uint32_t ViewStaticTableFunction::EstimateRowCount() {
  return view_->EstimateRowCount();
}

}  // namespace trace_processor
}  // namespace perfetto
