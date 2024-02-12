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

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/ref_counted.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/db/column.h"
#include "src/trace_processor/db/column/data_layer.h"
#include "src/trace_processor/db/column_storage.h"
#include "src/trace_processor/db/column_storage_overlay.h"
#include "src/trace_processor/db/table.h"

namespace perfetto::trace_processor {

// Represents a table of data with named, strongly typed columns. Only used
// where the schema of the table is decided at runtime.
class RuntimeTable : public Table {
 public:
  using NullIntStorage = ColumnStorage<std::optional<int64_t>>;
  using IntStorage = ColumnStorage<int64_t>;
  using StringStorage = ColumnStorage<StringPool::Id>;
  using NullDoubleStorage = ColumnStorage<std::optional<double>>;
  using DoubleStorage = ColumnStorage<double>;
  using VariantStorage = std::variant<uint32_t,
                                      IntStorage,
                                      NullIntStorage,
                                      StringStorage,
                                      DoubleStorage,
                                      NullDoubleStorage>;
  class Builder {
   public:
    Builder(StringPool* pool, std::vector<std::string> col_names);

    base::Status AddNull(uint32_t idx);
    base::Status AddInteger(uint32_t idx, int64_t res);
    base::Status AddFloat(uint32_t idx, double res);
    base::Status AddText(uint32_t idx, const char* ptr);

    base::StatusOr<std::unique_ptr<RuntimeTable>> Build(uint32_t rows) &&;

   private:
    StringPool* string_pool_ = nullptr;
    std::vector<std::string> col_names_;
    std::vector<std::unique_ptr<VariantStorage>> storage_;
  };

  explicit RuntimeTable(StringPool*,
                        uint32_t row_count,
                        std::vector<ColumnLegacy>,
                        std::vector<ColumnStorageOverlay>,
                        std::vector<RefPtr<column::DataLayer>> storage_layers,
                        std::vector<RefPtr<column::DataLayer>> null_layers,
                        std::vector<RefPtr<column::DataLayer>> overlay_layers);
  ~RuntimeTable() override;

  RuntimeTable(RuntimeTable&&) = default;
  RuntimeTable& operator=(RuntimeTable&&) = default;

  const Table::Schema& schema() const { return schema_; }

 private:
  std::vector<std::string> col_names_;
  std::vector<std::unique_ptr<VariantStorage>> storage_;
  Table::Schema schema_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_DB_RUNTIME_TABLE_H_
