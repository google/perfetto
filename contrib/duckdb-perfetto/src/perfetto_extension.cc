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

// PROTOTYPE: DuckDB extension embedding Perfetto's TraceProcessor.
//
// This is the only translation unit which includes DuckDB headers. It is
// built with exceptions and RTTI (DuckDB's C++ API requires both); everything
// that talks to TraceProcessor lives in trace_session.cc and is built with
// Perfetto's usual flags. Errors cross that boundary as base::Status, never as
// exceptions.
//
// Surface:
//   ATTACH 'x.pftrace' AS t (TYPE perfetto);       read-only catalog over TP
//   FROM t.slice WHERE ...                         projection + filter
//                                                  pushdown into TP
//   FROM PERFETTO(t) ( <PerfettoSQL> )             block executed entirely
//                                                  inside TP
//   FROM perfetto_query(source, sql)               what blocks desugar to;
//                                                  also the bulk entry point
//                                                  (paths, globs, lists)

#include <glob.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/parser_extension.hpp"
#include "duckdb/parser/simplified_token.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/storage/database_size.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "duckdb/transaction/transaction_manager.hpp"

#include "contrib/duckdb-perfetto/src/block_rewriter.h"
#include "contrib/duckdb-perfetto/src/trace_session.h"

namespace duckdb {
namespace perfetto_ext {
namespace {

using ::perfetto::duckdb_ext::BlockRewriter;
using ::perfetto::duckdb_ext::ColumnInfo;
using ::perfetto::duckdb_ext::ColumnType;
using ::perfetto::duckdb_ext::Cursor;
using ::perfetto::duckdb_ext::OuterToken;
using ::perfetto::duckdb_ext::RewriteResult;
using ::perfetto::duckdb_ext::RowSink;
using ::perfetto::duckdb_ext::Trace;
using ::perfetto::trace_processor::SqlValue;
using PStatus = ::perfetto::base::Status;

constexpr const char* kCatalogType = "perfetto";

// Rows of the first trace inspected to infer column types of an arbitrary
// PerfettoSQL query.
constexpr size_t kTypeInferenceRows = 2048;

LogicalType ToLogicalType(ColumnType t) {
  switch (t) {
    case ColumnType::kInt64:
      return LogicalType::BIGINT;
    case ColumnType::kDouble:
      return LogicalType::DOUBLE;
    case ColumnType::kString:
      return LogicalType::VARCHAR;
    case ColumnType::kBytes:
      return LogicalType::BLOB;
  }
  return LogicalType::VARCHAR;
}

std::string QuoteIdent(const std::string& s) {
  return "\"" + StringUtil::Replace(s, "\"", "\"\"") + "\"";
}

std::string QuoteLiteral(const std::string& s) {
  return "'" + StringUtil::Replace(s, "'", "''") + "'";
}

std::string JoinAnd(const std::vector<std::string>& preds) {
  std::string res;
  for (const std::string& p : preds)
    res += (res.empty() ? "" : " AND ") + p;
  return res;
}

// Writes TP cells into a DuckDB output chunk.
class VectorSink : public RowSink {
 public:
  // |out_col[i]| is the output vector for TP column i; |types[i]| its type.
  VectorSink(DataChunk& output,
             const std::vector<idx_t>& out_col,
             const std::vector<ColumnType>& types)
      : output_(output), out_col_(out_col), types_(types) {}

  PStatus Write(size_t row, uint32_t col, const SqlValue& v) override {
    Vector& vec = output_.data[out_col_[col]];
    if (v.is_null()) {
      FlatVector::SetNull(vec, row, true);
      return PStatus();
    }
    switch (types_[col]) {
      case ColumnType::kInt64:
        if (v.type == SqlValue::kLong) {
          FlatVector::GetData<int64_t>(vec)[row] = v.long_value;
          return PStatus();
        }
        if (v.type == SqlValue::kDouble &&
            static_cast<double>(static_cast<int64_t>(v.double_value)) ==
                v.double_value) {
          FlatVector::GetData<int64_t>(vec)[row] =
              static_cast<int64_t>(v.double_value);
          return PStatus();
        }
        return Mismatch(col, "BIGINT");
      case ColumnType::kDouble:
        if (v.type == SqlValue::kDouble) {
          FlatVector::GetData<double>(vec)[row] = v.double_value;
          return PStatus();
        }
        if (v.type == SqlValue::kLong) {
          FlatVector::GetData<double>(vec)[row] =
              static_cast<double>(v.long_value);
          return PStatus();
        }
        return Mismatch(col, "DOUBLE");
      case ColumnType::kString:
      case ColumnType::kBytes: {
        auto* data = FlatVector::GetData<string_t>(vec);
        if (v.type == SqlValue::kString) {
          data[row] = StringVector::AddStringOrBlob(vec, v.string_value);
        } else if (v.type == SqlValue::kBytes) {
          data[row] = StringVector::AddStringOrBlob(
              vec, static_cast<const char*>(v.bytes_value), v.bytes_count);
        } else {
          data[row] = StringVector::AddStringOrBlob(
              vec, ::perfetto::duckdb_ext::ToText(v));
        }
        return PStatus();
      }
    }
    return PStatus();
  }

 private:
  PStatus Mismatch(uint32_t col, const char* type) {
    return ::perfetto::base::ErrStatus(
        "column %u: value does not fit the inferred %s type; add a CAST in the "
        "PerfettoSQL query",
        col, type);
  }

  DataChunk& output_;
  const std::vector<idx_t>& out_col_;
  const std::vector<ColumnType>& types_;
};

std::shared_ptr<Trace> LoadTraceOrThrow(const std::string& path) {
  auto t = Trace::Load(path);
  if (!t.ok()) {
    throw IOException("perfetto: failed to load %s: %s", path,
                      t.status().message());
  }
  return std::move(*t);
}

[[noreturn]] void ThrowReadOnly() {
  throw BinderException(
      "perfetto databases are read-only; create PerfettoSQL tables from a "
      "PERFETTO block instead");
}

std::unique_ptr<Cursor> OpenCursorOrThrow(std::shared_ptr<Trace> trace,
                                          const std::string& sql) {
  auto c = Cursor::Open(std::move(trace), sql);
  if (!c.ok())
    throw InvalidInputException("perfetto: %s", c.status().message());
  return std::move(*c);
}

// ---------------------------------------------------------------------------
// Catalog: ATTACH 'x.pftrace' AS t (TYPE perfetto)
// ---------------------------------------------------------------------------

struct ScanBindData : public TableFunctionData {
  std::shared_ptr<Trace> trace;
  std::string table;
  std::vector<ColumnInfo> columns;
  // PerfettoSQL predicates pushed down from DuckDB, ANDed together.
  std::vector<std::string> where;

  unique_ptr<FunctionData> Copy() const override {
    auto res = make_uniq<ScanBindData>();
    res->trace = trace;
    res->table = table;
    res->columns = columns;
    res->where = where;
    return std::move(res);
  }
  bool Equals(const FunctionData& other) const override {
    auto& o = other.Cast<ScanBindData>();
    return trace == o.trace && table == o.table && where == o.where;
  }
};

TableFunction PerfettoScanFunction();

class PerfettoTableEntry : public TableCatalogEntry {
 public:
  PerfettoTableEntry(Catalog& catalog,
                     SchemaCatalogEntry& schema,
                     CreateTableInfo& info,
                     std::shared_ptr<Trace> trace,
                     std::vector<ColumnInfo> columns)
      : TableCatalogEntry(catalog, schema, info),
        trace_(std::move(trace)),
        columns_(std::move(columns)) {}

  const std::vector<ColumnInfo>& columns_info() const { return columns_; }

  unique_ptr<BaseStatistics> GetStatistics(ClientContext&, column_t) override {
    return nullptr;
  }

  TableFunction GetScanFunction(ClientContext&,
                                unique_ptr<FunctionData>& bind_data) override {
    auto data = make_uniq<ScanBindData>();
    data->trace = trace_;
    data->table = name;
    data->columns = columns_;
    bind_data = std::move(data);
    return PerfettoScanFunction();
  }

  TableStorageInfo GetStorageInfo(ClientContext&) override {
    return TableStorageInfo();
  }

 private:
  std::shared_ptr<Trace> trace_;
  std::vector<ColumnInfo> columns_;
};

bool SameColumns(const std::vector<ColumnInfo>& a,
                 const std::vector<ColumnInfo>& b) {
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].name != b[i].name || a[i].type != b[i].type)
      return false;
  }
  return true;
}

// The single schema ("main") of a perfetto catalog. Table entries are created
// lazily from TP's own catalog, so tables created later inside PERFETTO
// blocks (e.g. by INCLUDE PERFETTO MODULE) show up automatically.
class PerfettoSchemaEntry : public SchemaCatalogEntry {
 public:
  PerfettoSchemaEntry(Catalog& catalog,
                      CreateSchemaInfo& info,
                      std::shared_ptr<Trace> trace)
      : SchemaCatalogEntry(catalog, info), trace_(std::move(trace)) {}

  optional_ptr<CatalogEntry> GetTable(const std::string& raw_name) {
    std::string table = StringUtil::Lower(raw_name);
    auto cols = trace_->DescribeTable(table);
    if (!cols.ok())
      throw InvalidInputException("perfetto: %s", cols.status().message());
    if (cols->empty())
      return nullptr;
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(table);
    if (it != entries_.end() && SameColumns(it->second->columns_info(), *cols))
      return it->second.get();
    CreateTableInfo info(catalog.GetName(), name, table);
    for (const ColumnInfo& c : *cols)
      info.columns.AddColumn(ColumnDefinition(c.name, ToLogicalType(c.type)));
    auto entry = make_uniq<PerfettoTableEntry>(catalog, *this, info, trace_,
                                               std::move(*cols));
    auto* ptr = entry.get();
    if (it != entries_.end()) {
      // The table changed shape (e.g. re-created by a PERFETTO block). Plans
      // may still reference the old entry, so keep it alive.
      stale_.push_back(std::move(it->second));
      it->second = std::move(entry);
    } else {
      entries_.emplace(table, std::move(entry));
    }
    return ptr;
  }

  void Scan(ClientContext&,
            CatalogType type,
            const std::function<void(CatalogEntry&)>& callback) override {
    Scan(type, callback);
  }

  void Scan(CatalogType type,
            const std::function<void(CatalogEntry&)>& callback) override {
    if (type != CatalogType::TABLE_ENTRY)
      return;
    auto tables = trace_->ListTables();
    if (!tables.ok())
      throw InvalidInputException("perfetto: %s", tables.status().message());
    for (const std::string& t : *tables) {
      if (auto entry = GetTable(t))
        callback(*entry);
    }
  }

  optional_ptr<CatalogEntry> LookupEntry(
      CatalogTransaction,
      const EntryLookupInfo& lookup_info) override {
    if (lookup_info.GetCatalogType() != CatalogType::TABLE_ENTRY)
      return nullptr;
    return GetTable(lookup_info.GetEntryName());
  }

  optional_ptr<CatalogEntry> CreateIndex(CatalogTransaction,
                                         CreateIndexInfo&,
                                         TableCatalogEntry&) override {
    ThrowReadOnly();
  }
  optional_ptr<CatalogEntry> CreateFunction(CatalogTransaction,
                                            CreateFunctionInfo&) override {
    ThrowReadOnly();
  }
  optional_ptr<CatalogEntry> CreateTable(CatalogTransaction,
                                         BoundCreateTableInfo&) override {
    ThrowReadOnly();
  }
  optional_ptr<CatalogEntry> CreateView(CatalogTransaction,
                                        CreateViewInfo&) override {
    ThrowReadOnly();
  }
  optional_ptr<CatalogEntry> CreateSequence(CatalogTransaction,
                                            CreateSequenceInfo&) override {
    ThrowReadOnly();
  }
  optional_ptr<CatalogEntry> CreateTableFunction(
      CatalogTransaction,
      CreateTableFunctionInfo&) override {
    ThrowReadOnly();
  }
  optional_ptr<CatalogEntry> CreateCopyFunction(
      CatalogTransaction,
      CreateCopyFunctionInfo&) override {
    ThrowReadOnly();
  }
  optional_ptr<CatalogEntry> CreatePragmaFunction(
      CatalogTransaction,
      CreatePragmaFunctionInfo&) override {
    ThrowReadOnly();
  }
  optional_ptr<CatalogEntry> CreateCollation(CatalogTransaction,
                                             CreateCollationInfo&) override {
    ThrowReadOnly();
  }
  optional_ptr<CatalogEntry> CreateType(CatalogTransaction,
                                        CreateTypeInfo&) override {
    ThrowReadOnly();
  }
  void DropEntry(ClientContext&, DropInfo&) override { ThrowReadOnly(); }
  void Alter(CatalogTransaction, AlterInfo&) override { ThrowReadOnly(); }

 private:
  std::shared_ptr<Trace> trace_;
  std::mutex mu_;
  std::map<std::string, unique_ptr<PerfettoTableEntry>> entries_;
  std::vector<unique_ptr<PerfettoTableEntry>> stale_;
};

class PerfettoCatalog : public Catalog {
 public:
  PerfettoCatalog(AttachedDatabase& db, std::shared_ptr<Trace> trace)
      : Catalog(db), trace_(std::move(trace)) {
    CreateSchemaInfo info;
    info.schema = DEFAULT_SCHEMA;
    info.internal = true;
    schema_ = make_uniq<PerfettoSchemaEntry>(*this, info, trace_);
  }

  const std::shared_ptr<Trace>& trace() const { return trace_; }

  void Initialize(bool) override {}
  string GetCatalogType() override { return kCatalogType; }

  optional_ptr<SchemaCatalogEntry> LookupSchema(
      CatalogTransaction,
      const EntryLookupInfo& lookup,
      OnEntryNotFound if_not_found) override {
    const string& n = lookup.GetEntryName();
    if (n.empty() || n == DEFAULT_SCHEMA)
      return schema_.get();
    if (if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
      throw BinderException("perfetto catalog %s has only a main schema",
                            GetName());
    }
    return nullptr;
  }

  void ScanSchemas(ClientContext&,
                   std::function<void(SchemaCatalogEntry&)> callback) override {
    callback(*schema_);
  }

  optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction,
                                          CreateSchemaInfo&) override {
    ThrowReadOnly();
  }
  void DropSchema(ClientContext&, DropInfo&) override { ThrowReadOnly(); }
  PhysicalOperator& PlanCreateTableAs(ClientContext&,
                                      PhysicalPlanGenerator&,
                                      LogicalCreateTable&,
                                      PhysicalOperator&) override {
    ThrowReadOnly();
  }
  PhysicalOperator& PlanInsert(ClientContext&,
                               PhysicalPlanGenerator&,
                               LogicalInsert&,
                               optional_ptr<PhysicalOperator>) override {
    ThrowReadOnly();
  }
  PhysicalOperator& PlanDelete(ClientContext&,
                               PhysicalPlanGenerator&,
                               LogicalDelete&,
                               PhysicalOperator&) override {
    ThrowReadOnly();
  }
  PhysicalOperator& PlanUpdate(ClientContext&,
                               PhysicalPlanGenerator&,
                               LogicalUpdate&,
                               PhysicalOperator&) override {
    ThrowReadOnly();
  }
  using Catalog::PlanDelete;
  using Catalog::PlanUpdate;

  DatabaseSize GetDatabaseSize(ClientContext&) override {
    return DatabaseSize();
  }
  bool InMemory() override { return true; }
  string GetDBPath() override { return trace_->path(); }

 private:
  std::shared_ptr<Trace> trace_;
  unique_ptr<PerfettoSchemaEntry> schema_;
};

class PerfettoTransactionManager : public TransactionManager {
 public:
  explicit PerfettoTransactionManager(AttachedDatabase& db)
      : TransactionManager(db) {}

  Transaction& StartTransaction(ClientContext& context) override {
    auto t = make_uniq<Transaction>(*this, context);
    auto& ref = *t;
    std::lock_guard<std::mutex> lock(mu_);
    transactions_[&ref] = std::move(t);
    return ref;
  }
  ErrorData CommitTransaction(ClientContext&, Transaction& t) override {
    std::lock_guard<std::mutex> lock(mu_);
    transactions_.erase(&t);
    return ErrorData();
  }
  void RollbackTransaction(Transaction& t) override {
    std::lock_guard<std::mutex> lock(mu_);
    transactions_.erase(&t);
  }
  void Checkpoint(ClientContext&, bool) override {}

 private:
  std::mutex mu_;
  std::map<Transaction*, unique_ptr<Transaction>> transactions_;
};

unique_ptr<Catalog> PerfettoAttach(optional_ptr<StorageExtensionInfo>,
                                   ClientContext&,
                                   AttachedDatabase& db,
                                   const string&,
                                   AttachInfo& info,
                                   AttachOptions&) {
  return make_uniq<PerfettoCatalog>(db, LoadTraceOrThrow(info.path));
}

unique_ptr<TransactionManager> PerfettoCreateTransactionManager(
    optional_ptr<StorageExtensionInfo>,
    AttachedDatabase& db,
    Catalog&) {
  return make_uniq<PerfettoTransactionManager>(db);
}

// Resolves the name of an attached perfetto catalog (or "" for "the only
// one") to its trace. Returns nullptr if |name| is not such a catalog.
std::shared_ptr<Trace> FindAttachedTrace(ClientContext& context,
                                         const std::string& name) {
  auto& manager = DatabaseManager::Get(context);
  if (!name.empty()) {
    auto db = manager.GetDatabase(context, name);
    if (!db || db->GetCatalog().GetCatalogType() != kCatalogType)
      return nullptr;
    return db->GetCatalog().Cast<PerfettoCatalog>().trace();
  }
  std::shared_ptr<Trace> found;
  for (auto& db : manager.GetDatabases(context)) {
    if (db->GetCatalog().GetCatalogType() != kCatalogType)
      continue;
    if (found) {
      throw BinderException(
          "perfetto: no database name given, but more than one perfetto "
          "database is attached");
    }
    found = db->GetCatalog().Cast<PerfettoCatalog>().trace();
  }
  if (!found) {
    throw BinderException(
        "perfetto: no database name given, and no perfetto database is "
        "attached: ATTACH 'trace' AS t (TYPE perfetto)");
  }
  return found;
}

// ---------------------------------------------------------------------------
// perfetto_scan: the scan behind t.<table>.
// ---------------------------------------------------------------------------

struct ScanGlobalState : public GlobalTableFunctionState {
  std::unique_ptr<Cursor> cursor;
  std::vector<idx_t> out_col;
  std::vector<ColumnType> types;
};

// Translates a DuckDB filter on a perfetto table into a PerfettoSQL predicate.
// Only simple column-vs-constant forms are translated; everything else stays
// in DuckDB, so this never changes query semantics.
std::optional<std::string> TranslateFilter(const LogicalGet& get,
                                           const ScanBindData& bind,
                                           const Expression& expr) {
  auto column = [&](const Expression& e)
      -> std::optional<std::pair<std::string, ColumnType>> {
    if (e.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF)
      return std::nullopt;
    auto& ref = e.Cast<BoundColumnRefExpression>();
    if (ref.binding.table_index != get.table_index)
      return std::nullopt;
    const auto& ids = get.GetColumnIds();
    if (ref.binding.column_index >= ids.size())
      return std::nullopt;
    column_t col = ids[ref.binding.column_index].GetPrimaryIndex();
    if (col >= bind.columns.size())
      return std::nullopt;
    return std::make_pair(QuoteIdent(bind.columns[col].name),
                          bind.columns[col].type);
  };
  auto literal = [](const Expression& e,
                    ColumnType type) -> std::optional<std::string> {
    if (e.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT)
      return std::nullopt;
    const Value& v = e.Cast<BoundConstantExpression>().value;
    if (v.IsNull())
      return std::nullopt;
    switch (type) {
      case ColumnType::kInt64:
        if (!v.type().IsIntegral())
          return std::nullopt;
        return v.ToString();
      case ColumnType::kDouble:
        if (v.type().id() != LogicalTypeId::DOUBLE)
          return std::nullopt;
        return StringUtil::Format("%.17g", v.GetValue<double>());
      case ColumnType::kString:
        if (v.type().id() != LogicalTypeId::VARCHAR)
          return std::nullopt;
        return QuoteLiteral(StringValue::Get(v));
      case ColumnType::kBytes:
        return std::nullopt;
    }
    return std::nullopt;
  };

  switch (expr.GetExpressionClass()) {
    case ExpressionClass::BOUND_COMPARISON: {
      auto& cmp = expr.Cast<BoundComparisonExpression>();
      ExpressionType op = cmp.GetExpressionType();
      const Expression* col_side = cmp.left.get();
      const Expression* const_side = cmp.right.get();
      if (!column(*col_side)) {
        std::swap(col_side, const_side);
        op = FlipComparisonExpression(op);
      }
      auto col = column(*col_side);
      if (!col)
        return std::nullopt;
      auto lit = literal(*const_side, col->second);
      if (!lit)
        return std::nullopt;
      const char* sql_op = nullptr;
      switch (op) {
        case ExpressionType::COMPARE_EQUAL:
          sql_op = "=";
          break;
        case ExpressionType::COMPARE_NOTEQUAL:
          sql_op = "!=";
          break;
        case ExpressionType::COMPARE_LESSTHAN:
          sql_op = "<";
          break;
        case ExpressionType::COMPARE_GREATERTHAN:
          sql_op = ">";
          break;
        case ExpressionType::COMPARE_LESSTHANOREQUALTO:
          sql_op = "<=";
          break;
        case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
          sql_op = ">=";
          break;
        default:
          return std::nullopt;
      }
      return col->first + " " + sql_op + " " + *lit;
    }
    case ExpressionClass::BOUND_OPERATOR: {
      auto& op = expr.Cast<BoundOperatorExpression>();
      if (op.children.empty())
        return std::nullopt;
      auto col = column(*op.children[0]);
      if (!col)
        return std::nullopt;
      switch (op.GetExpressionType()) {
        case ExpressionType::OPERATOR_IS_NULL:
          return col->first + " IS NULL";
        case ExpressionType::OPERATOR_IS_NOT_NULL:
          return col->first + " IS NOT NULL";
        case ExpressionType::COMPARE_IN: {
          std::string list;
          for (size_t i = 1; i < op.children.size(); ++i) {
            auto lit = literal(*op.children[i], col->second);
            if (!lit)
              return std::nullopt;
            list += (i > 1 ? ", " : "") + *lit;
          }
          return col->first + " IN (" + list + ")";
        }
        default:
          return std::nullopt;
      }
    }
    default:
      return std::nullopt;
  }
}

void ScanPushdownComplexFilter(ClientContext&,
                               LogicalGet& get,
                               FunctionData* bind_data,
                               vector<unique_ptr<Expression>>& filters) {
  auto& bind = bind_data->Cast<ScanBindData>();
  for (idx_t i = 0; i < filters.size();) {
    if (auto sql = TranslateFilter(get, bind, *filters[i])) {
      bind.where.push_back(*sql);
      filters.erase_at(i);
    } else {
      ++i;
    }
  }
}

unique_ptr<GlobalTableFunctionState> ScanInitGlobal(
    ClientContext&,
    TableFunctionInitInput& input) {
  auto& bind = input.bind_data->Cast<ScanBindData>();
  auto state = make_uniq<ScanGlobalState>();
  std::string select;
  for (idx_t i = 0; i < input.column_ids.size(); ++i) {
    column_t col = input.column_ids[i];
    if (!select.empty())
      select += ", ";
    if (col < bind.columns.size()) {
      select += QuoteIdent(bind.columns[col].name);
      state->types.push_back(bind.columns[col].type);
    } else {
      // Row-id / virtual column (e.g. for COUNT(*)).
      select += "NULL";
      state->types.push_back(ColumnType::kInt64);
    }
    state->out_col.push_back(i);
  }
  std::string sql = "SELECT " + (select.empty() ? std::string("1") : select) +
                    " FROM " + QuoteIdent(bind.table);
  if (!bind.where.empty())
    sql += " WHERE " + JoinAnd(bind.where);
  state->cursor = OpenCursorOrThrow(bind.trace, sql);
  return std::move(state);
}

void ScanFunction(ClientContext&, TableFunctionInput& data, DataChunk& output) {
  auto& state = data.global_state->Cast<ScanGlobalState>();
  if (state.cursor->done()) {
    output.SetCardinality(0);
    return;
  }
  VectorSink sink(output, state.out_col, state.types);
  auto n = state.cursor->Read(STANDARD_VECTOR_SIZE, &sink);
  if (!n.ok())
    throw InvalidInputException("perfetto: %s", n.status().message());
  output.SetCardinality(*n);
}

InsertionOrderPreservingMap<string> ScanToString(
    TableFunctionToStringInput& input) {
  InsertionOrderPreservingMap<string> result;
  auto& bind = input.bind_data->Cast<ScanBindData>();
  result["Table"] = bind.table;
  if (!bind.where.empty())
    result["PerfettoSQL Filters"] = JoinAnd(bind.where);
  return result;
}

TableFunction PerfettoScanFunction() {
  TableFunction f("perfetto_scan", {}, ScanFunction, nullptr, ScanInitGlobal);
  f.projection_pushdown = true;
  f.pushdown_complex_filter = ScanPushdownComplexFilter;
  f.to_string = ScanToString;
  return f;
}

// ---------------------------------------------------------------------------
// perfetto_query(source, sql): runs PerfettoSQL inside TP. PERFETTO blocks
// desugar to this.
// ---------------------------------------------------------------------------

struct Source {
  std::string label;
  std::shared_ptr<Trace> attached;  // Null for a path loaded on demand.
};

struct QueryBindData : public TableFunctionData {
  std::vector<Source> sources;
  std::string sql;
  bool trace_column = false;
  std::vector<ColumnType> types;
  std::vector<idx_t> out_col;  // TP column -> output column.

  // Cursor for sources[0] opened at bind time to discover the schema; handed
  // to the first scanning thread so the query is not executed twice.
  mutable std::mutex mu;
  mutable std::unique_ptr<Cursor> first;

  unique_ptr<FunctionData> Copy() const override {
    auto res = make_uniq<QueryBindData>();
    res->sources = sources;
    res->sql = sql;
    res->trace_column = trace_column;
    res->types = types;
    res->out_col = out_col;
    return std::move(res);
  }
  bool Equals(const FunctionData& other) const override {
    return this == &other;
  }
};

bool IsGlob(const std::string& s) {
  return s.find_first_of("*?[") != std::string::npos;
}

void ExpandSource(ClientContext& context,
                  const std::string& s,
                  std::vector<Source>* out) {
  if (auto t = FindAttachedTrace(context, s)) {
    out->push_back({s, t});
    return;
  }
  if (!IsGlob(s)) {
    out->push_back({s, nullptr});
    return;
  }
  glob_t g{};
  int res = glob(s.c_str(), 0, nullptr, &g);
  if (res == 0) {
    for (size_t i = 0; i < g.gl_pathc; ++i)
      out->push_back({g.gl_pathv[i], nullptr});
  }
  globfree(&g);
  if (res != 0 && res != GLOB_NOMATCH)
    throw IOException("perfetto: glob failed for %s", s);
}

unique_ptr<FunctionData> QueryBind(ClientContext& context,
                                   TableFunctionBindInput& input,
                                   vector<LogicalType>& return_types,
                                   vector<string>& names) {
  auto bind = make_uniq<QueryBindData>();
  const Value& src = input.inputs[0];
  std::vector<std::string> raw;
  if (src.type().id() == LogicalTypeId::LIST) {
    for (const Value& v : ListValue::GetChildren(src))
      raw.push_back(v.ToString());
  } else {
    raw.push_back(src.ToString());
  }
  bind->sql = input.inputs[1].ToString();

  bool multi = raw.size() > 1;
  for (const std::string& r : raw) {
    ExpandSource(context, r, &bind->sources);
    multi |= IsGlob(r);
  }
  if (bind->sources.empty())
    throw InvalidInputException("perfetto_query: no traces matched");
  bind->trace_column = multi;
  auto it = input.named_parameters.find("trace_column");
  if (it != input.named_parameters.end())
    bind->trace_column = BooleanValue::Get(it->second);

  const Source& s0 = bind->sources[0];
  bind->first = OpenCursorOrThrow(
      s0.attached ? s0.attached : LoadTraceOrThrow(s0.label), bind->sql);
  auto types = bind->first->InferTypes(kTypeInferenceRows);
  if (!types.ok())
    throw InvalidInputException("perfetto: %s", types.status().message());
  bind->types = std::move(*types);

  if (bind->trace_column) {
    names.push_back("trace");
    return_types.push_back(LogicalType::VARCHAR);
  }
  const auto& cols = bind->first->column_names();
  for (size_t i = 0; i < cols.size(); ++i) {
    bind->out_col.push_back(names.size());
    names.push_back(cols[i]);
    return_types.push_back(ToLogicalType(bind->types[i]));
  }
  return std::move(bind);
}

struct QueryGlobalState : public GlobalTableFunctionState {
  std::atomic<size_t> next_source{0};
  idx_t max_threads = 1;
  idx_t MaxThreads() const override { return max_threads; }
};

struct QueryLocalState : public LocalTableFunctionState {
  std::unique_ptr<Cursor> cursor;
};

unique_ptr<GlobalTableFunctionState> QueryInitGlobal(
    ClientContext&,
    TableFunctionInitInput& input) {
  auto state = make_uniq<QueryGlobalState>();
  state->max_threads = input.bind_data->Cast<QueryBindData>().sources.size();
  return std::move(state);
}

unique_ptr<LocalTableFunctionState> QueryInitLocal(ExecutionContext&,
                                                   TableFunctionInitInput&,
                                                   GlobalTableFunctionState*) {
  return make_uniq<QueryLocalState>();
}

void QueryFunction(ClientContext&,
                   TableFunctionInput& data,
                   DataChunk& output) {
  auto& bind = data.bind_data->Cast<QueryBindData>();
  auto& global = data.global_state->Cast<QueryGlobalState>();
  auto& local = data.local_state->Cast<QueryLocalState>();
  for (;;) {
    if (!local.cursor) {
      size_t idx = global.next_source.fetch_add(1);
      if (idx >= bind.sources.size()) {
        output.SetCardinality(0);
        return;
      }
      if (idx == 0) {
        std::lock_guard<std::mutex> lock(bind.mu);
        local.cursor = std::move(bind.first);
      }
      if (!local.cursor) {
        // A later source, or a re-execution of a prepared statement.
        const Source& s = bind.sources[idx];
        local.cursor = OpenCursorOrThrow(
            s.attached ? s.attached : LoadTraceOrThrow(s.label), bind.sql);
      }
    }
    VectorSink sink(output, bind.out_col, bind.types);
    auto n = local.cursor->Read(STANDARD_VECTOR_SIZE, &sink);
    if (!n.ok()) {
      throw InvalidInputException("perfetto: %s: %s", local.cursor->label(),
                                  n.status().message());
    }
    if (bind.trace_column) {
      auto* labels = FlatVector::GetData<string_t>(output.data[0]);
      for (size_t r = 0; r < *n; ++r) {
        labels[r] =
            StringVector::AddString(output.data[0], local.cursor->label());
      }
    }
    if (local.cursor->done())
      local.cursor.reset();
    if (*n > 0) {
      output.SetCardinality(*n);
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// Parser: PERFETTO(t) ( <PerfettoSQL> )
// ---------------------------------------------------------------------------

// The outer query is DuckDB SQL, so it is tokenized by DuckDB itself.
std::vector<OuterToken> TokenizeDuckDbSql(const std::string& sql) {
  std::vector<OuterToken> res;
  for (const SimplifiedToken& t : Parser::Tokenize(sql)) {
    OuterToken::Kind kind;
    switch (t.type) {
      case SimplifiedTokenType::SIMPLIFIED_TOKEN_IDENTIFIER:
      case SimplifiedTokenType::SIMPLIFIED_TOKEN_KEYWORD:
        kind = OuterToken::Kind::kWord;
        break;
      case SimplifiedTokenType::SIMPLIFIED_TOKEN_OPERATOR:
        kind = OuterToken::Kind::kOperator;
        break;
      case SimplifiedTokenType::SIMPLIFIED_TOKEN_COMMENT:
        continue;
      default:
        kind = OuterToken::Kind::kLiteral;
        break;
    }
    res.push_back({t.start, kind});
  }
  return res;
}

// Uses parser_override (rather than the fallback parse_function) because the
// fallback only sees statements after DuckDB has split the input on ';',
// which would cut multi-statement PerfettoSQL blocks in half.
struct PerfettoParserInfo : public ParserExtensionInfo {
  BlockRewriter rewriter;
};

class PerfettoParserExtension : public ParserExtension {
 public:
  PerfettoParserExtension() {
    parser_override = Override;
    parser_info = make_shared_ptr<PerfettoParserInfo>();
  }

  static ParserOverrideResult Override(ParserExtensionInfo* info,
                                       const string& query,
                                       ParserOptions& options) {
    auto& rewriter = static_cast<PerfettoParserInfo*>(info)->rewriter;
    RewriteResult r = rewriter.Rewrite(query, TokenizeDuckDbSql);
    if (r.status == RewriteResult::kNoBlocks)
      return ParserOverrideResult();
    if (r.status == RewriteResult::kError) {
      ParserException e(optional_idx(r.error_offset), "%s", r.error);
      return ParserOverrideResult(e);
    }
    try {
      // The rewritten query contains no blocks, so this recursive parse falls
      // straight through to DuckDB's own parser.
      Parser parser(options);
      parser.ParseQuery(r.query);
      return ParserOverrideResult(std::move(parser.statements));
    } catch (std::exception& e) {
      return ParserOverrideResult(e);
    }
  }
};

void LoadInternal(ExtensionLoader& loader) {
  auto& config = DBConfig::GetConfig(loader.GetDatabaseInstance());

  auto storage = make_shared_ptr<StorageExtension>();
  storage->attach = PerfettoAttach;
  storage->create_transaction_manager = PerfettoCreateTransactionManager;
  StorageExtension::Register(config, kCatalogType, std::move(storage));

  ParserExtension::Register(config, PerfettoParserExtension());
  // parser_override hooks are ignored unless this is enabled. "strict" (as
  // opposed to "fallback") surfaces our errors (e.g. an unterminated block)
  // instead of DuckDB's generic syntax error; queries without blocks still
  // fall through to DuckDB's parser.
  config.SetOptionByName("allow_parser_override_extension", Value("strict"));

  TableFunction query("perfetto_query",
                      {LogicalType::ANY, LogicalType::VARCHAR}, QueryFunction,
                      QueryBind, QueryInitGlobal, QueryInitLocal);
  query.named_parameters["trace_column"] = LogicalType::BOOLEAN;
  loader.RegisterFunction(query);
}

}  // namespace
}  // namespace perfetto_ext
}  // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(perfetto, loader) {
  duckdb::perfetto_ext::LoadInternal(loader);
}
}
