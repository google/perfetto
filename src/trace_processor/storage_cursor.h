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

#ifndef SRC_TRACE_PROCESSOR_STORAGE_CURSOR_H_
#define SRC_TRACE_PROCESSOR_STORAGE_CURSOR_H_

#include "src/trace_processor/row_iterators.h"
#include "src/trace_processor/storage_columns.h"
#include "src/trace_processor/table.h"

namespace perfetto {
namespace trace_processor {

// A cursor which abstracts common patterns found in storage backed tables. It
// takes a strategy to iterate through rows and a column reporter for each
// column to implement the Cursor interface.
class StorageCursor final : public Table::Cursor {
 public:
  StorageCursor(std::unique_ptr<RowIterator>,
                std::vector<std::unique_ptr<StorageColumn>>*);

  // Implementation of Table::Cursor.
  int Next() override;
  int Eof() override;
  int Column(sqlite3_context*, int N) override;

 private:
  std::unique_ptr<RowIterator> iterator_;
  std::vector<std::unique_ptr<StorageColumn>>* columns_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_STORAGE_CURSOR_H_
