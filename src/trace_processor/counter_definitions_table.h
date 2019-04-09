/*
 * Copyright (C) 2019 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_COUNTER_DEFINITIONS_TABLE_H_
#define SRC_TRACE_PROCESSOR_COUNTER_DEFINITIONS_TABLE_H_

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "src/trace_processor/storage_table.h"

namespace perfetto {
namespace trace_processor {

class CounterDefinitionsTable : public StorageTable {
 public:
  static void RegisterTable(sqlite3* db, const TraceStorage* storage);

  CounterDefinitionsTable(sqlite3*, const TraceStorage*);

  // StorageTable implementation.
  StorageSchema CreateStorageSchema() override;
  uint32_t RowCount() override;
  int BestIndex(const QueryConstraints&, BestIndexInfo*) override;

  class RefColumn final : public StorageColumn {
   public:
    RefColumn(std::string col_name,
              const std::deque<int64_t>* refs,
              const std::deque<RefType>* types,
              const TraceStorage* storage);

    void ReportResult(sqlite3_context* ctx, uint32_t row) const override;

    Bounds BoundFilter(int op, sqlite3_value* sqlite_val) const override;

    void Filter(int op, sqlite3_value* value, FilteredRowIndex*) const override;

    Comparator Sort(const QueryConstraints::OrderBy& ob) const override;

    bool HasOrdering() const override { return false; }

    Table::ColumnType GetType() const override {
      return Table::ColumnType::kLong;
    }

   private:
    int CompareRefsAsc(uint32_t f, uint32_t s) const;

    const std::deque<int64_t>* refs_;
    const std::deque<RefType>* types_;
    const TraceStorage* storage_ = nullptr;
  };

 private:
  uint32_t EstimateCost(const QueryConstraints&);

  std::vector<const char*> ref_types_;
  const TraceStorage* const storage_;
};
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_COUNTER_DEFINITIONS_TABLE_H_
