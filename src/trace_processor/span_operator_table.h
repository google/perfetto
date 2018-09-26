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

  // Represents possible values of a SQLite joined table.
  struct Value {
    Table::ColumnType type;
    std::string text_value;
    uint64_t ulong_value;
    uint32_t uint_value;
  };

  SpanOperatorTable(sqlite3*, const TraceStorage*);

  static void RegisterTable(sqlite3* db, const TraceStorage* storage);

  // Table implementation.
  Table::Schema CreateSchema(int argc, const char* const* argv) override;
  std::unique_ptr<Table::Cursor> CreateCursor() override;
  int BestIndex(const QueryConstraints& qc, BestIndexInfo* info) override;

 private:
  static constexpr uint8_t kReservedColumns = Column::kJoinValue + 1;

  // Contains the definition of the child tables.
  struct TableDefinition {
    std::string name;
    std::vector<Table::Column> cols;
    std::string join_col_name;
  };

  // State used when filtering on the span table.
  class FilterState {
   public:
    FilterState(SpanOperatorTable*, ScopedStmt t1_stmt, ScopedStmt t2_stmt);

    int Initialize();
    int Next();
    int Eof();
    int Column(sqlite3_context* context, int N);

   private:
    // Details of a row of one of the child tables.
    struct Span {
      uint64_t ts = 0;
      uint64_t dur = 0;
      std::vector<Value> values;  // One for each column.
    };

    // Details of the state of retrieval from a table across all join values.
    struct TableState {
      uint64_t latest_ts = std::numeric_limits<uint64_t>::max();
      size_t col_count = 0;
      ScopedStmt stmt;

      // The rows of the table indexed by the values of join column.
      // TODO(lalitm): see how we can expand this past int64_t.
      std::map<int64_t, Span> spans;
    };

    // A span which has data from both tables associated with it.
    struct IntersectingSpan {
      uint64_t ts = 0;
      uint64_t dur = 0;
      int64_t join_val = 0;
      Span t1_span;
      Span t2_span;
    };

    // Computes the next value from the child tables.
    int ExtractNext(bool pull_t1);

    // Add an intersecting span to the queue if the two child spans intersect
    // at any point in time.
    bool MaybeAddIntersectingSpan(int64_t join_value,
                                  Span t1_span,
                                  Span t2_span);

    // Reports to SQLite the value given by |value| based on its type.
    void ReportSqliteResult(sqlite3_context* context,
                            SpanOperatorTable::Value value);

    TableState t1_;
    TableState t2_;

    bool children_have_more_ = true;
    std::deque<IntersectingSpan> intersecting_spans_;

    SpanOperatorTable* const table_;
  };

  // Cursor on the span table.
  class Cursor : public Table::Cursor {
   public:
    Cursor(SpanOperatorTable*, sqlite3* db);
    ~Cursor() override;

    // Methods to be implemented by derived table classes.
    int Filter(const QueryConstraints& qc, sqlite3_value** argv) override;
    int Next() override;
    int Eof() override;
    int Column(sqlite3_context* context, int N) override;

   private:
    int PrepareRawStmt(const QueryConstraints& qc,
                       sqlite3_value** argv,
                       const TableDefinition& def,
                       bool is_t1,
                       sqlite3_stmt**);

    sqlite3* const db_;
    SpanOperatorTable* const table_;
    std::unique_ptr<FilterState> filter_state_;
  };

  // Converts a joined column index into an index on the columns of the child
  // tables.
  // Returns a (bool, index) pair with the bool indicating whether the index is
  // into table 1 and the index being the offset into the relevant table's
  // columns.
  std::pair<bool, size_t> GetTableAndColumnIndex(int joined_column_idx);

  TableDefinition t1_defn_;
  TableDefinition t2_defn_;
  std::string join_col_;

  sqlite3* const db_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SPAN_OPERATOR_TABLE_H_
