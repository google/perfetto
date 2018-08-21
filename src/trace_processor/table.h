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

namespace perfetto {
namespace trace_processor {

class QueryConstraints;
class TraceStorage;

// Abstract base class representing a SQLite virtual table. Implements the
// common bookeeping required across all tables and allows subclasses to
// implement a friendlier API than that required by SQLite.
class Table : public sqlite3_vtab {
 public:
  using Factory =
      std::function<std::unique_ptr<Table>(const TraceStorage*,
                                           const std::string& name)>;

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

  Table();

  // Called by derived classes to register themselves with the SQLite db.
  template <typename T>
  static void Register(sqlite3* db,
                       const TraceStorage* storage,
                       const std::string& create_statement) {
    RegisterInternal(db, storage, create_statement, GetFactory<T>());
  }

  // Methods to be implemented by derived table classes.
  virtual std::unique_ptr<Cursor> CreateCursor() = 0;
  virtual int BestIndex(const QueryConstraints& qc, BestIndexInfo* info) = 0;

  // Optional metods to implement.
  using FindFunctionFn = void (**)(sqlite3_context*, int, sqlite3_value**);
  virtual int FindFunction(const char* name, FindFunctionFn fn, void** args);

 private:
  template <typename TableType>
  static Factory GetFactory() {
    return [](const TraceStorage* storage, const std::string& name) {
      auto table = std::unique_ptr<Table>(new TableType(storage));
      table->name_ = name;
      return table;
    };
  }

  static void RegisterInternal(sqlite3* db,
                               const TraceStorage*,
                               const std::string& create,
                               Factory);

  // Overriden functions from sqlite3_vtab.
  int OpenInternal(sqlite3_vtab_cursor**);
  int BestIndexInternal(sqlite3_index_info*);

  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;

  std::string name_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TABLE_H_
