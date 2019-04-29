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

#ifndef SRC_TRACE_PROCESSOR_TABLE_H_
#define SRC_TRACE_PROCESSOR_TABLE_H_

#include <sqlite3.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "perfetto/base/optional.h"
#include "src/trace_processor/query_constraints.h"

namespace perfetto {
namespace trace_processor {

class TraceStorage;

// Abstract base class representing a SQLite virtual table. Implements the
// common bookeeping required across all tables and allows subclasses to
// implement a friendlier API than that required by SQLite.
class Table : public sqlite3_vtab {
 public:
  using Factory =
      std::function<std::unique_ptr<Table>(sqlite3*, const TraceStorage*)>;

  // Allowed types for columns in a table.
  enum ColumnType {
    kString = 1,
    kUint = 2,
    kLong = 3,
    kInt = 4,
    kDouble = 5,
    kUnknown = 6,
  };

  // Describes a column of this table.
  class Column {
   public:
    Column(size_t idx, std::string name, ColumnType type, bool hidden = false);

    size_t index() const { return index_; }
    const std::string& name() const { return name_; }
    ColumnType type() const { return type_; }
    bool hidden() const { return hidden_; }

   private:
    size_t index_ = 0;
    std::string name_;
    ColumnType type_ = ColumnType::kString;
    bool hidden_ = false;
  };

  // When set it logs all BestIndex and Filter actions on the console.
  static bool debug;

  // Public for unique_ptr destructor calls.
  virtual ~Table();

  // Abstract base class representing an SQLite Cursor. Presents a friendlier
  // API for subclasses to implement.
  class Cursor : public sqlite3_vtab_cursor {
   public:
    Cursor(Table* table);
    virtual ~Cursor();

    // Methods to be implemented by derived table classes.

    // Called to intialise the cursor with the constraints of the query.
    virtual int Filter(const QueryConstraints& qc, sqlite3_value**) = 0;

    // Called to forward the cursor to the next row in the table.
    virtual int Next() = 0;

    // Called to check if the cursor has reached eof. Column will be called iff
    // this method returns true.
    virtual int Eof() = 0;

    // Used to extract the value from the column at index |N|.
    virtual int Column(sqlite3_context* context, int N) = 0;

    // Optional methods to implement.
    virtual int RowId(sqlite3_int64*);

   protected:
    Cursor(Cursor&) = delete;
    Cursor& operator=(const Cursor&) = delete;

    Cursor(Cursor&&) noexcept = default;
    Cursor& operator=(Cursor&&) = default;

   private:
    friend class Table;

    Table* table_ = nullptr;
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
    const std::vector<size_t> primary_keys() { return primary_keys_; }

   private:
    // The names and types of the columns of the table.
    std::vector<Column> columns_;

    // The primary keys of the table given by an offset into |columns|.
    std::vector<size_t> primary_keys_;
  };

 protected:
  // Populated by a BestIndex call to allow subclasses to tweak SQLite's
  // handling of sets of constraints.
  struct BestIndexInfo {
    bool order_by_consumed = false;
    uint32_t estimated_cost = 0;
    std::vector<bool> omit;
  };

  struct TableDescriptor {
    Table::Factory factory;
    const TraceStorage* storage = nullptr;
    std::string name;
    sqlite3_module module = {};
  };

  Table();

  // Called by derived classes to register themselves with the SQLite db.
  // |read_write| specifies whether the table can also be written to.
  // |requires_args| should be true if the table requires arguments in order to
  // be instantiated.
  // Note: this function is inlined here because we use the TTable template to
  // devirtualise the function calls.
  template <typename TTable>
  static void Register(sqlite3* db,
                       const TraceStorage* storage,
                       const std::string& table_name,
                       bool read_write = false,
                       bool requires_args = false) {
    using TCursor = typename TTable::Cursor;

    std::unique_ptr<TableDescriptor> desc(new TableDescriptor());
    desc->storage = storage;
    desc->factory = GetFactory<TTable>();
    desc->name = table_name;
    sqlite3_module* module = &desc->module;
    memset(module, 0, sizeof(*module));

    auto create_fn = [](sqlite3* xdb, void* arg, int argc,
                        const char* const* argv, sqlite3_vtab** tab, char**) {
      const TableDescriptor* xdesc = static_cast<const TableDescriptor*>(arg);
      auto table = xdesc->factory(xdb, xdesc->storage);
      table->name_ = xdesc->name;

      auto opt_schema = table->Init(argc, argv);
      if (!opt_schema.has_value()) {
        PERFETTO_ELOG("Failed to create schema (table %s)",
                      xdesc->name.c_str());
        return SQLITE_ERROR;
      }

      const auto& schema = opt_schema.value();
      auto create_stmt = schema.ToCreateTableStmt();
      PERFETTO_DLOG("Create table statement: %s", create_stmt.c_str());

      int res = sqlite3_declare_vtab(xdb, create_stmt.c_str());
      if (res != SQLITE_OK)
        return res;

      // Freed in xDisconnect().
      table->schema_ = std::move(schema);
      *tab = table.release();

      return SQLITE_OK;
    };
    auto destroy_fn = [](sqlite3_vtab* t) {
      delete static_cast<TTable*>(t);
      return SQLITE_OK;
    };

    module->xCreate = create_fn;
    module->xConnect = create_fn;
    module->xDisconnect = destroy_fn;
    module->xDestroy = destroy_fn;
    module->xOpen = [](sqlite3_vtab* t, sqlite3_vtab_cursor** c) {
      return static_cast<TTable*>(t)->OpenInternal(c);
    };
    module->xClose = [](sqlite3_vtab_cursor* c) {
      delete static_cast<TCursor*>(c);
      return SQLITE_OK;
    };
    module->xBestIndex = [](sqlite3_vtab* t, sqlite3_index_info* i) {
      return static_cast<TTable*>(t)->BestIndexInternal(i);
    };
    module->xFilter = [](sqlite3_vtab_cursor* c, int i, const char* s, int a,
                         sqlite3_value** v) {
      const auto& qc =
          static_cast<Cursor*>(c)->table_->ParseConstraints(i, s, a);
      return static_cast<TCursor*>(c)->Filter(qc, v);
    };
    module->xNext = [](sqlite3_vtab_cursor* c) {
      return static_cast<TCursor*>(c)->Next();
    };
    module->xEof = [](sqlite3_vtab_cursor* c) {
      return static_cast<TCursor*>(c)->Eof();
    };
    module->xColumn = [](sqlite3_vtab_cursor* c, sqlite3_context* a, int b) {
      return static_cast<TCursor*>(c)->Column(a, b);
    };
    module->xRowid = [](sqlite3_vtab_cursor* c, sqlite3_int64* r) {
      return static_cast<TCursor*>(c)->RowId(r);
    };
    module->xFindFunction =
        [](sqlite3_vtab* t, int, const char* name,
           void (**fn)(sqlite3_context*, int, sqlite3_value**), void** args) {
          return static_cast<TTable*>(t)->FindFunction(name, fn, args);
        };

    if (read_write) {
      module->xUpdate = [](sqlite3_vtab* t, int a, sqlite3_value** v,
                           sqlite3_int64* r) {
        return static_cast<TTable*>(t)->Update(a, v, r);
      };
    }

    int res = sqlite3_create_module_v2(
        db, table_name.c_str(), module, desc.release(),
        [](void* arg) { delete static_cast<TableDescriptor*>(arg); });
    PERFETTO_CHECK(res == SQLITE_OK);

    // Register virtual tables into an internal 'perfetto_tables' table. This is
    // used for iterating through all the tables during a database export. Note
    // that virtual tables requiring arguments aren't registered because they
    // can't be automatically instantiated for exporting.
    if (!requires_args) {
      char* insert_sql = sqlite3_mprintf(
          "INSERT INTO perfetto_tables(name) VALUES('%q')", table_name.c_str());
      char* error = nullptr;
      sqlite3_exec(db, insert_sql, 0, 0, &error);
      sqlite3_free(insert_sql);
      if (error) {
        PERFETTO_ELOG("Error registering table: %s", error);
        sqlite3_free(error);
      }
    }
  }

  // Methods to be implemented by derived table classes.
  virtual base::Optional<Schema> Init(int argc, const char* const* argv) = 0;
  virtual std::unique_ptr<Cursor> CreateCursor() = 0;
  virtual int BestIndex(const QueryConstraints& qc, BestIndexInfo* info) = 0;

  // Optional metods to implement.
  using FindFunctionFn = void (**)(sqlite3_context*, int, sqlite3_value**);
  virtual int FindFunction(const char* name, FindFunctionFn fn, void** args);

  // At registration time, the function should also pass true for |read_write|.
  virtual int Update(int, sqlite3_value**, sqlite3_int64*);

  void SetErrorMessage(char* error) {
    sqlite3_free(zErrMsg);
    zErrMsg = error;
  }

  const Schema& schema() const { return schema_; }
  const std::string& name() const { return name_; }

 private:
  template <typename TableType>
  static Factory GetFactory() {
    return [](sqlite3* db, const TraceStorage* storage) {
      return std::unique_ptr<Table>(new TableType(db, storage));
    };
  }

  static void RegisterInternal(sqlite3* db,
                               const TraceStorage*,
                               const std::string& name,
                               bool read_write,
                               bool requires_args,
                               Factory);

  const QueryConstraints& ParseConstraints(int idxNum,
                                           const char* idxStr,
                                           int argc);

  // Overriden functions from sqlite3_vtab.
  int OpenInternal(sqlite3_vtab_cursor**);
  int BestIndexInternal(sqlite3_index_info*);

  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;

  std::string name_;
  Schema schema_;

  QueryConstraints qc_cache_;
  int qc_hash_ = 0;
  int best_index_num_ = 0;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TABLE_H_
