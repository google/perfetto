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
    kUlong = 2,
    kUint = 3,
    kInt = 4,
  };

  // Describes a column of this table.
  class Column {
   public:
    Column(size_t index,
           std::string name,
           ColumnType type,
           bool hidden = false);

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
    virtual ~Cursor();

    // Methods to be implemented by derived table classes.
    virtual int Filter(const QueryConstraints& qc, sqlite3_value** argv) = 0;
    virtual int Next() = 0;
    virtual int Eof() = 0;
    virtual int Column(sqlite3_context* context, int N) = 0;

    // Optional methods to implement.
    virtual int RowId(sqlite3_int64*);

   private:
    friend class Table;

    // Overriden functions from sqlite3_vtab_cursor.
    int FilterInternal(int num, const char* idxStr, int argc, sqlite3_value**);
  };

 protected:
  // Populated by a BestIndex call to allow subclasses to tweak SQLite's
  // handling of sets of constraints.
  struct BestIndexInfo {
    bool order_by_consumed = false;
    uint32_t estimated_cost = 0;
    std::vector<bool> omit;
  };

  // The schema of the table. Created by subclasses to allow the table class to
  // do filtering and inform SQLite about the CREATE table statement.
  class Schema {
   public:
    Schema();
    Schema(std::vector<Column>, std::vector<size_t> primary_keys);

    // This class is explicitly copiable.
    Schema(const Schema&) noexcept;
    Schema& operator=(const Schema& t);

    std::string ToCreateTableStmt();

    const std::vector<Column>& columns() { return columns_; }
    const std::vector<size_t> primary_keys() { return primary_keys_; }

   private:
    // The names and types of the columns of the table.
    std::vector<Column> columns_;

    // The primary keys of the table given by an offset into |columns|.
    std::vector<size_t> primary_keys_;
  };

  Table();

  // Called by derived classes to register themselves with the SQLite db.
  template <typename T>
  static void Register(sqlite3* db,
                       const TraceStorage* storage,
                       const std::string& name,
                       bool read_write = false) {
    RegisterInternal(db, storage, name, read_write, GetFactory<T>());
  }

  // Methods to be implemented by derived table classes.
  virtual Schema CreateSchema(int argc, const char* const* argv) = 0;
  virtual std::unique_ptr<Cursor> CreateCursor() = 0;
  virtual int BestIndex(const QueryConstraints& qc, BestIndexInfo* info) = 0;

  // Optional metods to implement.
  using FindFunctionFn = void (**)(sqlite3_context*, int, sqlite3_value**);
  virtual int FindFunction(const char* name, FindFunctionFn fn, void** args);

  // At registration time, the function should also pass true for |read_write|.
  virtual int Update(int, sqlite3_value**, sqlite3_int64*);

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
                               Factory);

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
