/*
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

#ifndef SRC_TRACE_PROCESSOR_STORAGE_SCHEMA_H_
#define SRC_TRACE_PROCESSOR_STORAGE_SCHEMA_H_

#include <algorithm>

#include "src/trace_processor/filtered_row_index.h"
#include "src/trace_processor/sqlite_utils.h"
#include "src/trace_processor/storage_columns.h"
#include "src/trace_processor/storage_cursor.h"
#include "src/trace_processor/table.h"
#include "src/trace_processor/trace_storage.h"

namespace perfetto {
namespace trace_processor {

// Defines the schema for a table which is backed by concrete storage (i.e. does
// not generate data on the fly).
// Used by all tables which are backed by data in TraceStorage.
class StorageSchema {
 public:
  StorageSchema();
  StorageSchema(std::vector<std::unique_ptr<StorageColumn>> columns);

  Table::Schema ToTableSchema(std::vector<std::string> primary_keys);

  size_t ColumnIndexFromName(const std::string& name);

  const StorageColumn& GetColumn(size_t idx) const { return *(columns_[idx]); }

  std::vector<std::unique_ptr<StorageColumn>>* mutable_columns() {
    return &columns_;
  }

 private:
  std::vector<std::unique_ptr<StorageColumn>> columns_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_STORAGE_SCHEMA_H_
