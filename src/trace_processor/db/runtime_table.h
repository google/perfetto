/*
 * Copyright (C) 2023 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_DB_RUNTIME_TABLE_H_
#define SRC_TRACE_PROCESSOR_DB_RUNTIME_TABLE_H_

#include <stdint.h>

#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

#include "src/trace_processor/db/table.h"

namespace perfetto {
namespace trace_processor {

// Represents a table of data with named, strongly typed columns. Only used
// where the schema of the table is decided at runtime.
class RuntimeTable : public Table {
 public:
  using IntStorage = ColumnStorage<std::optional<int64_t>>;
  using StringStorage = ColumnStorage<StringPool::Id>;
  using DoubleStorage = ColumnStorage<std::optional<double>>;
  using VariantStorage =
      std::variant<uint32_t, IntStorage, StringStorage, DoubleStorage>;

  RuntimeTable(StringPool* pool, std::vector<std::string> col_names);
  ~RuntimeTable() override;

  base::Status AddNull(uint32_t idx);

  base::Status AddInteger(uint32_t idx, int64_t res);

  base::Status AddFloat(uint32_t idx, double res);

  base::Status AddText(uint32_t idx, const char* ptr);

  base::Status AddColumnsAndOverlays(uint32_t rows);

 private:
  std::vector<std::string> col_names_;
  std::vector<std::unique_ptr<VariantStorage>> storage_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_DB_RUNTIME_TABLE_H_
