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

#ifndef SRC_TRACE_PROCESSOR_SPAN_OPERATOR_TABLE_H_
#define SRC_TRACE_PROCESSOR_SPAN_OPERATOR_TABLE_H_

#include <sqlite3.h>
#include <array>
#include <deque>
#include <limits>
#include <map>
#include <memory>

#include "src/trace_processor/scoped_db.h"
#include "src/trace_processor/table.h"

namespace perfetto {
namespace trace_processor {

// Implements the SPAN JOIN operation between two tables on a particular column.
//
// Span:
// A span is a row with a timestamp and a duration. It can is used to model
// operations which run for a particular *span* of time.
//
// We draw spans like so (time on the x-axis):
// start of span->[ time where opertion is running ]<- end of span
//
// Multiple spans can happen in parallel:
// [      ]
//    [        ]
//   [                    ]
//  [ ]
//
// The above for example, models scheduling activity on a 4-core computer for a
// short period of time.
//
// Span join:
// The span join operation can be thought of as the intersection of span tables.
// That is, the join table has a span for each pair of spans in the child tables
// where the spans overlap. Because many spans are possible in parallel, an
// extra metadata column (labelled the "join column") is used to distinguish
// between the spanned tables.
//
// For a given join key suppose these were the two span tables:
// Table 1:   [        ]              [      ]         [ ]
// Table 2:          [      ]            [  ]           [      ]
// Output :          [ ]                 [  ]           []
//
// All other columns apart from timestamp (ts), duration (dur) and the join key
// are passed through unchanged.
class SpanOperatorTable : public Table {
 public:
  // Columns of the span operator table.
  enum Column {
    kTimestamp = 0,
    kDuration = 1,
    kJoinValue = 2,
    // All other columns are dynamic depending on the joined tables.
  };

  SpanOperatorTable(sqlite3*, const TraceStorage*);

  static void RegisterTable(sqlite3* db, const TraceStorage* storage);

  // Table implementation.
  Table::Schema CreateSchema(int argc, const char* const* argv) override;
  std::unique_ptr<Table::Cursor> CreateCursor(const QueryConstraints&,
                                              sqlite3_value**) override;
  int BestIndex(const QueryConstraints& qc, BestIndexInfo* info) override;

 private:
  static constexpr uint8_t kReservedColumns = Column::kJoinValue + 1;

  enum ChildTable {
    kFirst = 0,
    kSecond = 1,
  };

  // Contains the definition of the child tables.
  struct TableDefinition {
    std::string name;
    std::vector<Table::Column> cols;
    std::string join_col_name;
  };

  // Cursor on the span table.
  class Cursor : public Table::Cursor {
   public:
    Cursor(SpanOperatorTable*, sqlite3* db);
    ~Cursor() override;

    int Initialize(const QueryConstraints& qc, sqlite3_value** argv);
    int Next() override;
    int Eof() override;
    int Column(sqlite3_context* context, int N) override;

   private:
    // Details of the state of retrieval from a table across all join values.
    struct TableState {
      ScopedStmt stmt;
      uint64_t ts_start = std::numeric_limits<uint64_t>::max();
      uint64_t ts_end = std::numeric_limits<uint64_t>::max();
      int64_t join_val = std::numeric_limits<int64_t>::max();
    };

    // Steps the cursor forward for the given table and updates the state
    // for that table.
    int StepForTable(ChildTable table);

    void ReportSqliteResult(sqlite3_context* context,
                            sqlite3_stmt* stmt,
                            size_t index);

    int PrepareRawStmt(const QueryConstraints& qc,
                       sqlite3_value** argv,
                       const TableDefinition& def,
                       ChildTable table,
                       sqlite3_stmt**);

    TableState t1_;
    TableState t2_;
    ChildTable next_stepped_table_ = ChildTable::kFirst;

    sqlite3* const db_;
    SpanOperatorTable* const table_;
  };

  std::vector<std::string> ComputeSqlConstraintVector(
      const QueryConstraints& qc,
      sqlite3_value** argv,
      ChildTable table);

  // Converts a joined column index into an index on the columns of the child
  // tables.
  // Returns a (table, index) pair with the table indicating whether the index
  // is into table 1 or 2 and the index being the offset into the relevant
  // table's columns.
  std::pair<SpanOperatorTable::ChildTable, size_t> GetTableAndColumnIndex(
      int joined_column_idx);

  TableDefinition t1_defn_;
  TableDefinition t2_defn_;
  std::string join_col_;

  sqlite3* const db_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SPAN_OPERATOR_TABLE_H_
