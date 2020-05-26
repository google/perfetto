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

#include "src/trace_processor/sqlite/db_sqlite_table.h"

#include "src/trace_processor/sqlite/sqlite_utils.h"

namespace perfetto {
namespace trace_processor {

namespace {

FilterOp SqliteOpToFilterOp(int sqlite_op) {
  switch (sqlite_op) {
    case SQLITE_INDEX_CONSTRAINT_EQ:
    case SQLITE_INDEX_CONSTRAINT_IS:
      return FilterOp::kEq;
    case SQLITE_INDEX_CONSTRAINT_GT:
      return FilterOp::kGt;
    case SQLITE_INDEX_CONSTRAINT_LT:
      return FilterOp::kLt;
    case SQLITE_INDEX_CONSTRAINT_ISNOT:
    case SQLITE_INDEX_CONSTRAINT_NE:
      return FilterOp::kNe;
    case SQLITE_INDEX_CONSTRAINT_GE:
      return FilterOp::kGe;
    case SQLITE_INDEX_CONSTRAINT_LE:
      return FilterOp::kLe;
    case SQLITE_INDEX_CONSTRAINT_ISNULL:
      return FilterOp::kIsNull;
    case SQLITE_INDEX_CONSTRAINT_ISNOTNULL:
      return FilterOp::kIsNotNull;
    case SQLITE_INDEX_CONSTRAINT_LIKE:
      return FilterOp::kLike;
    default:
      PERFETTO_FATAL("Currently unsupported constraint");
  }
}

SqlValue SqliteValueToSqlValue(sqlite3_value* sqlite_val) {
  auto col_type = sqlite3_value_type(sqlite_val);
  SqlValue value;
  switch (col_type) {
    case SQLITE_INTEGER:
      value.type = SqlValue::kLong;
      value.long_value = sqlite3_value_int64(sqlite_val);
      break;
    case SQLITE_TEXT:
      value.type = SqlValue::kString;
      value.string_value =
          reinterpret_cast<const char*>(sqlite3_value_text(sqlite_val));
      break;
    case SQLITE_FLOAT:
      value.type = SqlValue::kDouble;
      value.double_value = sqlite3_value_double(sqlite_val);
      break;
    case SQLITE_BLOB:
      value.type = SqlValue::kBytes;
      value.bytes_value = sqlite3_value_blob(sqlite_val);
      value.bytes_count = static_cast<size_t>(sqlite3_value_bytes(sqlite_val));
      break;
    case SQLITE_NULL:
      value.type = SqlValue::kNull;
      break;
  }
  return value;
}

}  // namespace

DbSqliteTable::DbSqliteTable(sqlite3*, const Table* table) : table_(table) {}
DbSqliteTable::~DbSqliteTable() = default;

void DbSqliteTable::RegisterTable(sqlite3* db,
                                  const Table* table,
                                  const std::string& name) {
  SqliteTable::Register<DbSqliteTable, const Table*>(db, table, name);
}

util::Status DbSqliteTable::Init(int, const char* const*, Schema* schema) {
  std::vector<SqliteTable::Column> schema_cols;
  for (uint32_t i = 0; i < table_->GetColumnCount(); ++i) {
    const auto& col = table_->GetColumn(i);
    schema_cols.emplace_back(i, col.name(), col.type());
  }
  // TODO(lalitm): this is hardcoded to be the id column but change this to be
  // more generic in the future.
  const auto* col = table_->GetColumnByName("id");
  if (!col) {
    PERFETTO_FATAL(
        "id column not found in %s. Currently all db Tables need to contain an "
        "id column; this constraint will be relaxed in the future.",
        name().c_str());
  }

  std::vector<size_t> primary_keys;
  primary_keys.emplace_back(col->index_in_table());

  *schema = Schema(std::move(schema_cols), std::move(primary_keys));
  return util::OkStatus();
}

int DbSqliteTable::BestIndex(const QueryConstraints& qc, BestIndexInfo* info) {
  // TODO(lalitm): investigate SQLITE_INDEX_SCAN_UNIQUE for id columns.
  auto cost_and_rows = EstimateCost(*table_, qc);
  info->estimated_cost = cost_and_rows.cost;
  info->estimated_rows = cost_and_rows.rows;
  return SQLITE_OK;
}

int DbSqliteTable::ModifyConstraints(QueryConstraints* qc) {
  using C = QueryConstraints::Constraint;

  // Reorder constraints to consider the constraints on columns which are
  // cheaper to filter first.
  auto* cs = qc->mutable_constraints();
  std::sort(cs->begin(), cs->end(), [this](const C& a, const C& b) {
    uint32_t a_idx = static_cast<uint32_t>(a.column);
    uint32_t b_idx = static_cast<uint32_t>(b.column);
    const auto& a_col = table_->GetColumn(a_idx);
    const auto& b_col = table_->GetColumn(b_idx);

    // Id columns are always very cheap to filter on so try and get them
    // first.
    if (a_col.IsId() && !b_col.IsId())
      return true;

    // Sorted columns are also quite cheap to filter so order them after
    // any id columns.
    if (a_col.IsSorted() && !b_col.IsSorted())
      return true;

    // TODO(lalitm): introduce more orderings here based on empirical data.
    return false;
  });

  // Remove any order by constraints which also have an equality constraint.
  auto* ob = qc->mutable_order_by();
  {
    auto p = [&cs](const QueryConstraints::OrderBy& o) {
      auto inner_p = [&o](const QueryConstraints::Constraint& c) {
        return c.column == o.iColumn && sqlite_utils::IsOpEq(c.op);
      };
      return std::any_of(cs->begin(), cs->end(), inner_p);
    };
    auto remove_it = std::remove_if(ob->begin(), ob->end(), p);
    ob->erase(remove_it, ob->end());
  }

  // Go through the order by constraints in reverse order and eliminate
  // constraints until the first non-sorted column or the first order by in
  // descending order.
  {
    auto p = [this](const QueryConstraints::OrderBy& o) {
      const auto& col = table_->GetColumn(static_cast<uint32_t>(o.iColumn));
      return o.desc || !col.IsSorted();
    };
    auto first_non_sorted_it = std::find_if(ob->rbegin(), ob->rend(), p);
    auto pop_count = std::distance(ob->rbegin(), first_non_sorted_it);
    ob->resize(ob->size() - static_cast<uint32_t>(pop_count));
  }

  return SQLITE_OK;
}

DbSqliteTable::QueryCost DbSqliteTable::EstimateCost(
    const Table& table,
    const QueryConstraints& qc) {
  // Currently our cost estimation algorithm is quite simplistic but is good
  // enough for the simplest cases.
  // TODO(lalitm): replace hardcoded constants with either more heuristics
  // based on the exact type of constraint or profiling the queries themselves.

  // We estimate the fixed cost of set-up and tear-down of a query in terms of
  // the number of rows scanned.
  constexpr double kFixedQueryCost = 1000.0;

  // Setup the variables for estimating the number of rows we will have at the
  // end of filtering. Note that |current_row_count| should always be at least 1
  // unless we are absolutely certain that we will return no rows as otherwise
  // SQLite can make some bad choices.
  uint32_t current_row_count = table.size();

  // If the table is empty, any constraint set only pays the fixed cost. Also we
  // can return 0 as the row count as we are certain that we will return no
  // rows.
  if (current_row_count == 0)
    return QueryCost{kFixedQueryCost, 0};

  // Setup the variables for estimating the cost of filtering.
  double filter_cost = 0.0;
  const auto& cs = qc.constraints();
  for (const auto& c : cs) {
    if (current_row_count < 2)
      break;
    const auto& col = table.GetColumn(static_cast<uint32_t>(c.column));
    if (sqlite_utils::IsOpEq(c.op) && col.IsId()) {
      // If we have an id equality constraint, it's a bit expensive to find
      // the exact row but it filters down to a single row.
      filter_cost += 100;
      current_row_count = 1;
    } else if (sqlite_utils::IsOpEq(c.op)) {
      // If there is only a single equality constraint, we have special logic
      // to sort by that column and then binary search if we see the constraint
      // set often. Model this by dividing but the log of the number of rows as
      // a good approximation. Otherwise, we'll need to do a full table scan.
      filter_cost += cs.size() == 1
                         ? (2 * current_row_count) / log2(current_row_count)
                         : current_row_count;

      // We assume that an equalty constraint will cut down the number of rows
      // by approximate log of the number of rows.
      double estimated_rows = current_row_count / log2(current_row_count);
      current_row_count = std::max(static_cast<uint32_t>(estimated_rows), 1u);
    } else {
      // Otherwise, we will need to do a full table scan and we estimate we will
      // maybe (at best) halve the number of rows.
      filter_cost += current_row_count;
      current_row_count = std::max(current_row_count / 2u, 1u);
    }
  }

  // Now, to figure out the cost of sorting, multiply the final row count
  // by |qc.order_by().size()| * log(row count). This should act as a crude
  // estimation of the cost.
  double sort_cost =
      qc.order_by().size() * current_row_count * log2(current_row_count);

  // The cost of iterating rows is more expensive than filtering the rows
  // so multiply by an appropriate factor.
  double iteration_cost = current_row_count * 2.0;

  // To get the final cost, add up all the individual components.
  double final_cost =
      kFixedQueryCost + filter_cost + sort_cost + iteration_cost;
  return QueryCost{final_cost, current_row_count};
}

std::unique_ptr<SqliteTable::Cursor> DbSqliteTable::CreateCursor() {
  return std::unique_ptr<Cursor>(new Cursor(this));
}

DbSqliteTable::Cursor::Cursor(DbSqliteTable* table)
    : SqliteTable::Cursor(table), initial_db_table_(table->table_) {}

int DbSqliteTable::Cursor::Filter(const QueryConstraints& qc,
                                  sqlite3_value** argv,
                                  FilterHistory history) {
  // Clear out the iterator before filtering to ensure the destructor is run
  // before the table's destructor.
  iterator_ = base::nullopt;

  if (history == FilterHistory::kSame && qc.constraints().size() == 1 &&
      sqlite_utils::IsOpEq(qc.constraints().front().op)) {
    // If we've seen the same constraint set with a single equality constraint
    // more than |kRepeatedThreshold| times, we assume we will see it more
    // in the future and thus cache a table sorted on the column. That way,
    // future equality constraints can binary search for the value instead of
    // doing a full table scan.
    constexpr uint32_t kRepeatedThreshold = 3;
    if (!sorted_cache_table_ && repeated_cache_count_++ > kRepeatedThreshold) {
      const auto& c = qc.constraints().front();
      uint32_t col = static_cast<uint32_t>(c.column);
      sorted_cache_table_ = initial_db_table_->Sort({Order{col, false}});
    }
  } else {
    sorted_cache_table_ = base::nullopt;
    repeated_cache_count_ = 0;
  }

  // We reuse this vector to reduce memory allocations on nested subqueries.
  constraints_.resize(qc.constraints().size());
  for (size_t i = 0; i < qc.constraints().size(); ++i) {
    const auto& cs = qc.constraints()[i];
    uint32_t col = static_cast<uint32_t>(cs.column);

    FilterOp op = SqliteOpToFilterOp(cs.op);
    SqlValue value = SqliteValueToSqlValue(argv[i]);

    constraints_[i] = Constraint{col, op, value};
  }

  // We reuse this vector to reduce memory allocations on nested subqueries.
  orders_.resize(qc.order_by().size());
  for (size_t i = 0; i < qc.order_by().size(); ++i) {
    const auto& ob = qc.order_by()[i];
    uint32_t col = static_cast<uint32_t>(ob.iColumn);
    orders_[i] = Order{col, static_cast<bool>(ob.desc)};
  }

  // Try and use the sorted cache table (if it exists) to speed up the sorting.
  // Otherwise, just use the original table.
  auto* source =
      sorted_cache_table_ ? &*sorted_cache_table_ : &*initial_db_table_;
  db_table_ = source->Filter(constraints_).Sort(orders_);
  iterator_ = db_table_->IterateRows();

  return SQLITE_OK;
}

int DbSqliteTable::Cursor::Next() {
  iterator_->Next();
  return SQLITE_OK;
}

int DbSqliteTable::Cursor::Eof() {
  return !*iterator_;
}

int DbSqliteTable::Cursor::Column(sqlite3_context* ctx, int raw_col) {
  uint32_t column = static_cast<uint32_t>(raw_col);
  SqlValue value = iterator_->Get(column);
  switch (value.type) {
    case SqlValue::Type::kLong:
      sqlite3_result_int64(ctx, value.long_value);
      break;
    case SqlValue::Type::kDouble:
      sqlite3_result_double(ctx, value.double_value);
      break;
    case SqlValue::Type::kString: {
      // We can say kSqliteStatic here because all strings are expected to
      // come from the string pool and thus will be valid for the lifetime
      // of trace processor.
      sqlite3_result_text(ctx, value.string_value, -1,
                          sqlite_utils::kSqliteStatic);
      break;
    }
    case SqlValue::Type::kBytes: {
      // We can say kSqliteStatic here because for our iterator will hold
      // onto the pointer as long as we don't call Next() but that only
      // happens with Next() is called on the Cursor itself at which point
      // SQLite no longer cares about the bytes pointer.
      sqlite3_result_blob(ctx, value.bytes_value,
                          static_cast<int>(value.bytes_count),
                          sqlite_utils::kSqliteStatic);
      break;
    }
    case SqlValue::Type::kNull:
      sqlite3_result_null(ctx);
      break;
  }
  return SQLITE_OK;
}

}  // namespace trace_processor
}  // namespace perfetto
