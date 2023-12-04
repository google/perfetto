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

#ifndef SRC_TRACE_PROCESSOR_SQLITE_SQLITE_TABLE_H_
#define SRC_TRACE_PROCESSOR_SQLITE_SQLITE_TABLE_H_

#include <sqlite3.h>

#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/db/table.h"
#include "src/trace_processor/sqlite/query_constraints.h"

namespace perfetto {
namespace trace_processor {

class SqliteEngine;
class TypedSqliteTableBase;

// Abstract base class representing a SQLite virtual table. Implements the
// common bookeeping required across all tables and allows subclasses to
// implement a friendlier API than that required by SQLite.
class SqliteTable : public sqlite3_vtab {
 public:
  // Custom opcodes used by subclasses of SqliteTable.
  // Stored here as we need a central repository of opcodes to prevent clashes
  // between different sub-classes.
  enum CustomFilterOpcode {
    kSourceGeqOpCode = SQLITE_INDEX_CONSTRAINT_FUNCTION + 1,
  };
  // Describes a column of this table.
  class Column {
   public:
    Column(size_t idx,
           std::string name,
           SqlValue::Type type,
           bool hidden = false);

    size_t index() const { return index_; }
    const std::string& name() const { return name_; }
    SqlValue::Type type() const { return type_; }

    bool hidden() const { return hidden_; }
    void set_hidden(bool hidden) { hidden_ = hidden; }

   private:
    size_t index_ = 0;
    std::string name_;
    SqlValue::Type type_ = SqlValue::Type::kNull;
    bool hidden_ = false;
  };

  // Abstract base class representing an SQLite Cursor. Presents a friendlier
  // API for subclasses to implement.
  class BaseCursor : public sqlite3_vtab_cursor {
   public:
    // Enum for the history of calls to Filter.
    enum class FilterHistory : uint32_t {
      // Indicates that constraint set passed is the different to the
      // previous Filter call.
      kDifferent = 0,

      // Indicates that the constraint set passed is the same as the previous
      // Filter call.
      // This can be useful for subclasses to perform optimizations on repeated
      // nested subqueries.
      kSame = 1,
    };

    explicit BaseCursor(SqliteTable* table);
    virtual ~BaseCursor();

    // Methods to be implemented by derived table classes.
    // Note: these methods are intentionally not virtual for performance
    // reasons. As these methods are not defined, there will be compile errors
    // thrown if any of these methods are missing.

    // Called to intialise the cursor with the constraints of the query.
    base::Status Filter(const QueryConstraints& qc,
                        sqlite3_value**,
                        FilterHistory);

    // Called to forward the cursor to the next row in the table.
    base::Status Next();

    // Called to check if the cursor has reached eof. Column will be called iff
    // this method returns true.
    bool Eof();

    // Used to extract the value from the column at index |N|.
    base::Status Column(sqlite3_context* context, int N);

    SqliteTable* table() const { return table_; }

   protected:
    BaseCursor(BaseCursor&) = delete;
    BaseCursor& operator=(const BaseCursor&) = delete;

    BaseCursor(BaseCursor&&) noexcept = default;
    BaseCursor& operator=(BaseCursor&&) = default;

   private:
    SqliteTable* table_ = nullptr;
  };

  // The schema of the table. Created by subclasses to allow the table class to
  // do filtering and inform SQLite about the CREATE table statement.
  class Schema {
   public:
    Schema();
    Schema(std::vector<Column>, std::vector<size_t> primary_keys);

    // This class is explicitly copiable.
    Schema(const Schema&);
    Schema& operator=(const Schema& t);

    std::string ToCreateTableStmt() const;

    const std::vector<Column>& columns() const { return columns_; }
    std::vector<Column>* mutable_columns() { return &columns_; }

    const std::vector<size_t> primary_keys() { return primary_keys_; }

   private:
    // The names and types of the columns of the table.
    std::vector<Column> columns_;

    // The primary keys of the table given by an offset into |columns|.
    std::vector<size_t> primary_keys_;
  };

  enum TableType {
    // A table which automatically exists in the main schema and cannot be
    // created with CREATE VIRTUAL TABLE.
    // Note: the name value here matches the naming in the vtable docs of
    // SQLite.
    kEponymousOnly,

    // A table which must be explicitly created using a CREATE VIRTUAL TABLE
    // statement (i.e. does exist automatically).
    kExplicitCreate,
  };

  // Public for unique_ptr destructor calls.
  virtual ~SqliteTable();

  // When set it logs all BestIndex and Filter actions on the console.
  static bool debug;

 protected:
  // Populated by a BestIndex call to allow subclasses to tweak SQLite's
  // handling of sets of constraints.
  struct BestIndexInfo {
    // Contains bools which indicate whether SQLite should omit double checking
    // the constraint at that index.
    //
    // If there are no constraints, SQLite will be told it can omit checking for
    // the whole query.
    std::vector<bool> sqlite_omit_constraint;

    // Indicates that SQLite should not double check the result of the order by
    // clause.
    //
    // If there are no order by clauses, this value will be ignored and SQLite
    // will be told that it can omit double checking (i.e. this value will
    // implicitly be taken to be true).
    bool sqlite_omit_order_by = false;

    // Stores the estimated cost of this query.
    double estimated_cost = 0;

    // Estimated row count.
    int64_t estimated_rows = 0;
  };

  SqliteTable();

  // Methods to be implemented by derived table classes.
  virtual base::Status Init(int argc, const char* const* argv, Schema*) = 0;
  virtual std::unique_ptr<BaseCursor> CreateCursor() = 0;
  virtual int BestIndex(const QueryConstraints& qc, BestIndexInfo* info) = 0;

  // Optional metods to implement.
  using FindFunctionFn = void (*)(sqlite3_context*, int, sqlite3_value**);
  virtual base::Status ModifyConstraints(QueryConstraints* qc);
  virtual int FindFunction(const char* name, FindFunctionFn* fn, void** args);

  // At registration time, the function should also pass true for |read_write|.
  virtual base::Status Update(int, sqlite3_value**, sqlite3_int64*);

  bool ReadConstraints(int idxNum, const char* idxStr, int argc);

  const Schema& schema() const { return schema_; }
  const std::string& module_name() const { return module_name_; }
  const std::string& name() const { return name_; }

 private:
  template <typename, typename>
  friend class TypedSqliteTable;
  friend class TypedSqliteTableBase;

  SqliteTable(const SqliteTable&) = delete;
  SqliteTable& operator=(const SqliteTable&) = delete;

  // The engine class this table is registered with. Used for restoring/saving
  // the table.
  SqliteEngine* engine_ = nullptr;

  // This name of the table. For tables created using CREATE VIRTUAL TABLE, this
  // will be the name of the table specified by the query. For automatically
  // created tables, this will be the same as the module name registered.
  std::string name_;

  // The module name is the name that will be registered. This is
  // differs from the table name (|name_|) where the table was created using
  // CREATE VIRTUAL TABLE.
  std::string module_name_;

  Schema schema_;

  QueryConstraints qc_cache_;
  int qc_hash_ = 0;
  int best_index_num_ = 0;
};

class TypedSqliteTableBase : public SqliteTable {
 protected:
  struct BaseModuleArg {
    sqlite3_module module;
    SqliteEngine* engine;
    TableType table_type;
  };

  ~TypedSqliteTableBase() override;

  static int xDestroy(sqlite3_vtab*);
  static int xDestroyFatal(sqlite3_vtab*);

  static int xConnectRestoreTable(sqlite3* xdb,
                                  void* arg,
                                  int argc,
                                  const char* const* argv,
                                  sqlite3_vtab** tab,
                                  char** pzErr);
  static int xDisconnectSaveTable(sqlite3_vtab*);

  static int xOpen(sqlite3_vtab*, sqlite3_vtab_cursor**);
  static int xBestIndex(sqlite3_vtab*, sqlite3_index_info*);

  static base::Status DeclareAndAssignVtab(std::unique_ptr<SqliteTable> table,
                                           sqlite3_vtab** tab);

  base::Status InitInternal(SqliteEngine* engine,
                            int argc,
                            const char* const* argv);

  int SetStatusAndReturn(base::Status status) {
    if (!status.ok()) {
      sqlite3_free(zErrMsg);
      zErrMsg = sqlite3_mprintf("%s", status.c_message());
      return SQLITE_ERROR;
    }
    return SQLITE_OK;
  }
};

template <typename SubTable, typename Context>
class TypedSqliteTable : public TypedSqliteTableBase {
 public:
  struct ModuleArg : public BaseModuleArg {
    Context context;
  };

  static std::unique_ptr<ModuleArg> CreateModuleArg(SqliteEngine* engine,
                                                    Context ctx,
                                                    TableType table_type,
                                                    bool updatable) {
    auto arg = std::make_unique<ModuleArg>();
    arg->module = CreateModule(table_type, updatable);
    arg->engine = engine;
    arg->table_type = table_type;
    arg->context = std::move(ctx);
    return arg;
  }

 private:
  static constexpr sqlite3_module CreateModule(TableType table_type,
                                               bool updatable) {
    sqlite3_module module;
    memset(&module, 0, sizeof(sqlite3_module));
    switch (table_type) {
      case TableType::kEponymousOnly:
        // Neither xCreate nor xDestroy should ever be called for
        // eponymous-only tables.
        module.xCreate = nullptr;
        module.xDestroy = &xDestroyFatal;

        // xConnect and xDisconnect will automatically be called with
        // |module_name| == |name|.
        module.xConnect = &xCreate;
        module.xDisconnect = &xDestroy;
        break;
      case TableType::kExplicitCreate:
        // xConnect and xDestroy will be called when the table is CREATE-ed and
        // DROP-ed respectively.
        module.xCreate = &xCreate;
        module.xDestroy = &xDestroy;

        // xConnect and xDisconnect can be called at any time.
        module.xConnect = &xConnectRestoreTable;
        module.xDisconnect = &xDisconnectSaveTable;
        break;
    }
    module.xOpen = &xOpen;
    module.xClose = &xClose;
    module.xBestIndex = &xBestIndex;
    module.xFindFunction = &xFindFunction;
    module.xFilter = &xFilter;
    module.xNext = &xNext;
    module.xEof = &xEof;
    module.xColumn = &xColumn;
    module.xRowid = &xRowid;
    if (updatable) {
      module.xUpdate = &xUpdate;
    }
    return module;
  }

  static int xCreate(sqlite3* xdb,
                     void* arg,
                     int argc,
                     const char* const* argv,
                     sqlite3_vtab** tab,
                     char** pzErr) {
    auto* xdesc = static_cast<ModuleArg*>(arg);
    std::unique_ptr<SubTable> table(new SubTable(xdb, &*xdesc->context));
    SubTable* table_ptr = table.get();
    base::Status status = table->InitInternal(xdesc->engine, argc, argv);
    if (!status.ok()) {
      *pzErr = sqlite3_mprintf("%s", status.c_message());
      return SQLITE_ERROR;
    }
    status = DeclareAndAssignVtab(std::move(table), tab);
    if (!status.ok()) {
      *pzErr = sqlite3_mprintf("%s", status.c_message());
      return SQLITE_ERROR;
    }
    xdesc->engine->OnSqliteTableCreated(table_ptr->name(), xdesc->table_type);
    return SQLITE_OK;
  }
  static int xClose(sqlite3_vtab_cursor* c) {
    delete static_cast<typename SubTable::Cursor*>(c);
    return SQLITE_OK;
  }
  static int xFindFunction(sqlite3_vtab* t,
                           int,
                           const char* name,
                           void (**fn)(sqlite3_context*, int, sqlite3_value**),
                           void** args) {
    return static_cast<SubTable*>(t)->FindFunction(name, fn, args);
  }
  static int xFilter(sqlite3_vtab_cursor* vc,
                     int i,
                     const char* s,
                     int a,
                     sqlite3_value** v) {
    auto* cursor = static_cast<typename SubTable::Cursor*>(vc);
    bool is_cached = cursor->table()->ReadConstraints(i, s, a);
    auto history = is_cached ? BaseCursor::FilterHistory::kSame
                             : BaseCursor::FilterHistory::kDifferent;
    auto* table = static_cast<SubTable*>(cursor->table());
    return table->SetStatusAndReturn(
        cursor->Filter(cursor->table()->qc_cache_, v, history));
  }
  static int xNext(sqlite3_vtab_cursor* c) {
    auto* cursor = static_cast<typename SubTable::Cursor*>(c);
    auto* table = static_cast<SubTable*>(cursor->table());
    return table->SetStatusAndReturn(cursor->Next());
  }
  static int xEof(sqlite3_vtab_cursor* c) {
    return static_cast<int>(static_cast<typename SubTable::Cursor*>(c)->Eof());
  }
  static int xColumn(sqlite3_vtab_cursor* c, sqlite3_context* a, int b) {
    auto* cursor = static_cast<typename SubTable::Cursor*>(c);
    auto* table = static_cast<SubTable*>(cursor->table());
    return table->SetStatusAndReturn(cursor->Column(a, b));
  }
  static int xRowid(sqlite3_vtab_cursor*, sqlite3_int64*) {
    return SQLITE_ERROR;
  }
  static int xUpdate(sqlite3_vtab* t,
                     int a,
                     sqlite3_value** v,
                     sqlite3_int64* r) {
    auto* table = static_cast<SubTable*>(t);
    return table->SetStatusAndReturn(table->Update(a, v, r));
  }
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SQLITE_SQLITE_TABLE_H_
