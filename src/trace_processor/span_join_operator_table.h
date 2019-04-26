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

#ifndef SRC_TRACE_PROCESSOR_SPAN_JOIN_OPERATOR_TABLE_H_
#define SRC_TRACE_PROCESSOR_SPAN_JOIN_OPERATOR_TABLE_H_

#include <sqlite3.h>
#include <array>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

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
class SpanJoinOperatorTable : public Table {
 public:
  // Columns of the span operator table.
  enum Column {
    kTimestamp = 0,
    kDuration = 1,
    kPartition = 2,
    // All other columns are dynamic depending on the joined tables.
  };

  enum class PartitioningType {
    kNoPartitioning = 0,
    kSamePartitioning = 1,
    kMixedPartitioning = 2
  };

  // Parsed version of a table descriptor.
  struct TableDescriptor {
    static base::Optional<TableDescriptor> Parse(
        const std::string& raw_descriptor);

    bool IsPartitioned() const { return !partition_col.empty(); }

    std::string name;
    std::string partition_col;
  };

  // Contains the definition of the child tables.
  class TableDefinition {
   public:
    TableDefinition() = default;

    TableDefinition(std::string name,
                    std::string partition_col,
                    std::vector<Table::Column> cols,
                    bool emit_shadow_slices,
                    uint32_t ts_idx,
                    uint32_t dur_idx,
                    uint32_t partition_idx);

    const std::string& name() const { return name_; }
    const std::string& partition_col() const { return partition_col_; }
    const std::vector<Table::Column>& columns() const { return cols_; }

    bool emit_shadow_slices() const { return emit_shadow_slices_; }
    uint32_t ts_idx() const { return ts_idx_; }
    uint32_t dur_idx() const { return dur_idx_; }
    uint32_t partition_idx() const { return partition_idx_; }

    bool IsPartitioned() const { return !partition_col_.empty(); }

   private:
    std::string name_;
    std::string partition_col_;
    std::vector<Table::Column> cols_;
    bool emit_shadow_slices_;
    uint32_t ts_idx_ = std::numeric_limits<uint32_t>::max();
    uint32_t dur_idx_ = std::numeric_limits<uint32_t>::max();
    uint32_t partition_idx_ = std::numeric_limits<uint32_t>::max();
  };

  class Query {
   public:
    struct StepRet {
      enum Code {
        kRow,
        kEof,
        kError,
      };

      StepRet(Code c, int e = SQLITE_OK) : code(c), err_code(e) {}

      bool is_row() const { return code == Code::kRow; }
      bool is_eof() const { return code == Code::kEof; }
      bool is_err() const { return code == Code::kError; }

      Code code = Code::kEof;
      int err_code = SQLITE_OK;
    };

    Query(SpanJoinOperatorTable*, const TableDefinition*, sqlite3* db);
    virtual ~Query();

    Query(Query&) = delete;
    Query& operator=(const Query&) = delete;

    Query(Query&&) noexcept = default;
    Query& operator=(Query&&) = default;

    int Initialize(const QueryConstraints& qc, sqlite3_value** argv);

    StepRet Step();
    StepRet StepToNextPartition();
    StepRet StepToPartition(int64_t target_partition);
    StepRet StepUntil(int64_t timestamp);

    void ReportSqliteResult(sqlite3_context* context, size_t index);

    int64_t ts_start() const { return ts_start_; }
    int64_t ts_end() const { return ts_end_; }
    int64_t partition() const { return partition_; }

    const TableDefinition* definition() const { return defn_; }

    bool Eof() const { return cursor_eof_ && mode_ == Mode::kRealSlice; }
    bool IsPartitioned() const { return defn_->IsPartitioned(); }
    bool IsRealSlice() const { return mode_ == Mode::kRealSlice; }

    bool IsFullPartitionShadowSlice() const {
      return mode_ == Mode::kShadowSlice && ts_start_ == 0 &&
             ts_end_ == std::numeric_limits<int64_t>::max();
    }

    int64_t CursorPartition() const {
      PERFETTO_DCHECK(defn_->IsPartitioned());
      auto partition_idx = static_cast<int>(defn_->partition_idx());
      return sqlite3_column_int64(stmt_.get(), partition_idx);
    }

   private:
    enum Mode {
      kRealSlice,
      kShadowSlice,
    };

    int PrepareRawStmt();
    std::string CreateSqlQuery(const std::vector<std::string>& cs) const;

    int64_t CursorTs() const {
      auto ts_idx = static_cast<int>(defn_->ts_idx());
      return sqlite3_column_int64(stmt_.get(), ts_idx);
    }

    int64_t CursorDur() const {
      auto dur_idx = static_cast<int>(defn_->dur_idx());
      return sqlite3_column_int64(stmt_.get(), dur_idx);
    }

    std::string sql_query_;
    ScopedStmt stmt_;

    int64_t ts_start_ = 0;
    int64_t ts_end_ = 0;
    int64_t partition_ = std::numeric_limits<int64_t>::lowest();

    bool cursor_eof_ = false;
    Mode mode_ = Mode::kRealSlice;

    const TableDefinition* defn_ = nullptr;
    sqlite3* db_ = nullptr;
    SpanJoinOperatorTable* table_ = nullptr;
  };

  // Base class for a cursor on the span table.
  class Cursor : public Table::Cursor {
   public:
    Cursor(SpanJoinOperatorTable*, sqlite3* db);
    ~Cursor() override = default;

    int Filter(const QueryConstraints& qc, sqlite3_value** argv) override;
    int Column(sqlite3_context* context, int N) override;
    int Next() override;
    int Eof() override;

   private:
    Cursor(Cursor&) = delete;
    Cursor& operator=(const Cursor&) = delete;

    Cursor(Cursor&&) noexcept = default;
    Cursor& operator=(Cursor&&) = default;

    bool IsOverlappingSpan();
    Query::StepRet StepUntilRealSlice();

    Query t1_;
    Query t2_;
    Query* next_stepped_ = nullptr;

    SpanJoinOperatorTable* table_;
  };

  SpanJoinOperatorTable(sqlite3*, const TraceStorage*);

  static void RegisterTable(sqlite3* db, const TraceStorage* storage);

  // Table implementation.
  base::Optional<Table::Schema> Init(int, const char* const*) override;
  std::unique_ptr<Table::Cursor> CreateCursor() override;
  int BestIndex(const QueryConstraints& qc, BestIndexInfo* info) override;

 private:
  // Identifier for a column by index in a given table.
  struct ColumnLocator {
    const TableDefinition* defn;
    size_t col_index;
  };

  bool IsLeftJoin() const { return name() == "span_left_join"; }
  bool IsOuterJoin() const { return name() == "span_outer_join"; }

  const std::string& partition_col() const {
    return t1_defn_.IsPartitioned() ? t1_defn_.partition_col()
                                    : t2_defn_.partition_col();
  }

  base::Optional<TableDefinition> CreateTableDefinition(
      const TableDescriptor& desc,
      bool emit_shadow_slices);

  std::vector<std::string> ComputeSqlConstraintsForDefinition(
      const TableDefinition& defn,
      const QueryConstraints& qc,
      sqlite3_value** argv);

  std::string GetNameForGlobalColumnIndex(const TableDefinition& defn,
                                          int global_column);

  void CreateSchemaColsForDefn(const TableDefinition& defn,
                               std::vector<Table::Column>* cols);

  TableDefinition t1_defn_;
  TableDefinition t2_defn_;
  PartitioningType partitioning_;
  std::unordered_map<size_t, ColumnLocator> global_index_to_column_locator_;

  sqlite3* const db_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SPAN_JOIN_OPERATOR_TABLE_H_
