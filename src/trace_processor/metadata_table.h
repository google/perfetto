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

#ifndef SRC_TRACE_PROCESSOR_METADATA_TABLE_H_
#define SRC_TRACE_PROCESSOR_METADATA_TABLE_H_

#include "src/trace_processor/metadata.h"
#include "src/trace_processor/storage_table.h"
#include "src/trace_processor/trace_storage.h"

namespace perfetto {
namespace trace_processor {

class MetadataTable : public StorageTable {
 public:
  MetadataTable(sqlite3*, const TraceStorage*);

  static void RegisterTable(sqlite3* db, const TraceStorage* storage);

  // StorageTable implementation.
  StorageSchema CreateStorageSchema() override;
  uint32_t RowCount() override;
  int BestIndex(const QueryConstraints&, BestIndexInfo*) override;

 private:
  // Returns the stringified key enum name from metadata::kNames.
  class MetadataKeyNameAccessor : public Accessor<NullTermStringView> {
   public:
    MetadataKeyNameAccessor(const std::deque<metadata::KeyIDs>* keys);
    ~MetadataKeyNameAccessor() override;

    uint32_t Size() const override {
      return static_cast<uint32_t>(keys_->size());
    }

    NullTermStringView Get(uint32_t idx) const override {
      return NullTermStringView(metadata::kNames[(*keys_)[idx]]);
    }

   private:
    const std::deque<metadata::KeyIDs>* keys_;
  };

  // Returns the stringified metadata type, "single" for scalar, "multi" for
  // repeated.
  class MetadataKeyTypeAccessor : public Accessor<NullTermStringView> {
   public:
    MetadataKeyTypeAccessor(const std::deque<metadata::KeyIDs>* keys);
    ~MetadataKeyTypeAccessor() override;

    uint32_t Size() const override {
      return static_cast<uint32_t>(keys_->size());
    }

    NullTermStringView Get(uint32_t idx) const override {
      switch (metadata::kKeyTypes[(*keys_)[idx]]) {
        case metadata::KeyType::kSingle:
          return NullTermStringView("single");
          break;
        case metadata::KeyType::kMulti:
          return NullTermStringView("multi");
          break;
      }
      PERFETTO_FATAL("unsupported metadata type");  // for gcc
    }

   private:
    const std::deque<metadata::KeyIDs>* keys_;
  };

  // Returns values from Variadic storage. Only supports columns of
  // type Variadic::Type::kInt or Variadic::Type::kString.
  //
  // Based on ArgsTable::ValueColumn.
  class ValueColumn final : public StorageColumn {
   public:
    ValueColumn(std::string col_name,
                Variadic::Type type,
                const TraceStorage* storage);

    void ReportResult(sqlite3_context* ctx, uint32_t row) const override;
    Bounds BoundFilter(int op, sqlite3_value* sqlite_val) const override;
    void Filter(int op, sqlite3_value* value, FilteredRowIndex*) const override;
    Comparator Sort(const QueryConstraints::OrderBy& ob) const override;

    bool HasOrdering() const override { return false; }

    SqlValue::Type GetType() const override {
      if (type_ == Variadic::Type::kInt)
        return SqlValue::Type::kLong;
      if (type_ == Variadic::Type::kString)
        return SqlValue::Type::kString;
      PERFETTO_FATAL("Unimplemented metadata value type.");
    }

   private:
    int CompareRefsAsc(uint32_t f, uint32_t s) const;

    Variadic::Type type_;
    const TraceStorage* storage_ = nullptr;
  };

  const TraceStorage* const storage_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_METADATA_TABLE_H_
