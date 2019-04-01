/*
 * Copyright (C) 2018 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_ARGS_TABLE_H_
#define SRC_TRACE_PROCESSOR_ARGS_TABLE_H_

#include "src/trace_processor/storage_table.h"
#include "src/trace_processor/trace_storage.h"

namespace perfetto {
namespace trace_processor {

class ArgsTable : public StorageTable {
 public:
  using VariadicType = TraceStorage::Args::Variadic::Type;

  static void RegisterTable(sqlite3* db, const TraceStorage* storage);

  ArgsTable(sqlite3*, const TraceStorage*);

  // StorageTable implementation.
  StorageSchema CreateStorageSchema() override;
  uint32_t RowCount() override;
  int BestIndex(const QueryConstraints&, BestIndexInfo*) override;

 private:
  class ValueColumn final : public StorageColumn {
   public:
    ValueColumn(std::string col_name,
                VariadicType type,
                const TraceStorage* storage);

    void ReportResult(sqlite3_context* ctx, uint32_t row) const override;

    Bounds BoundFilter(int op, sqlite3_value* sqlite_val) const override;

    void Filter(int op, sqlite3_value* value, FilteredRowIndex*) const override;

    Comparator Sort(const QueryConstraints::OrderBy& ob) const override;

    bool HasOrdering() const override { return false; }

    Table::ColumnType GetType() const override {
      switch (type_) {
        case VariadicType::kInt:
          return Table::ColumnType::kLong;
        case VariadicType::kReal:
          return Table::ColumnType::kDouble;
        case VariadicType::kString:
          return Table::ColumnType::kString;
      }
      PERFETTO_FATAL("Not reached");  // For gcc
    }

   private:
    int CompareRefsAsc(uint32_t f, uint32_t s) const;

    TraceStorage::Args::Variadic::Type type_;
    const TraceStorage* storage_ = nullptr;
  };

  const TraceStorage* const storage_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_ARGS_TABLE_H_
