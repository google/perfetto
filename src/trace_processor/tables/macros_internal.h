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

#ifndef SRC_TRACE_PROCESSOR_TABLES_MACROS_INTERNAL_H_
#define SRC_TRACE_PROCESSOR_TABLES_MACROS_INTERNAL_H_

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <type_traits>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/trace_processor/ref_counted.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/containers/row_map.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/db/column.h"
#include "src/trace_processor/db/column/data_layer.h"
#include "src/trace_processor/db/column/selector_overlay.h"
#include "src/trace_processor/db/column_storage.h"
#include "src/trace_processor/db/column_storage_overlay.h"
#include "src/trace_processor/db/table.h"

namespace perfetto::trace_processor::macros_internal {

// We define this class to allow the table macro below to compile without
// needing templates; in reality none of the methods will be called because the
// pointer to this class will always be null.
class RootParentTable : public Table {
 public:
  struct Row {
   public:
    explicit Row(std::nullptr_t = nullptr) {}

    const char* type() const { return type_; }

   protected:
    const char* type_ = nullptr;
  };
  // This class only exists to allow typechecking to work correctly in Insert
  // below. If we had C++17 and if constexpr, we could statically verify that
  // this was never created but for now, we still need to define it to satisfy
  // the typechecker.
  struct IdAndRow {
    uint32_t id;
  };
  struct RowNumber {
    static uint32_t row_number() { PERFETTO_FATAL("Should not be called"); }
  };
  static IdAndRow Insert(const Row&) { PERFETTO_FATAL("Should not be called"); }

 private:
  explicit RootParentTable(std::nullptr_t);
};

// The parent class for all macro generated tables.
// This class is used to extract common code from the macro tables to reduce
// code size.
class MacroTable : public Table {
 public:
  // We don't want a move or copy constructor because we store pointers to
  // fields of macro tables which will be invalidated if we move/copy them.
  MacroTable(const MacroTable&) = delete;
  MacroTable& operator=(const MacroTable&) = delete;

  MacroTable(MacroTable&&) = delete;
  MacroTable& operator=(MacroTable&&) noexcept = delete;

 protected:
  // Constructors for tables created by the regular constructor.
  PERFETTO_NO_INLINE explicit MacroTable(StringPool* pool,
                                         std::vector<ColumnLegacy> columns,
                                         const MacroTable* parent)
      : Table(pool, 0u, std::move(columns), EmptyOverlaysFromParent(parent)),
        allow_inserts_(true),
        parent_(parent) {}

  // Constructor for tables created by SelectAndExtendParent.
  MacroTable(StringPool* pool,
             std::vector<ColumnLegacy> columns,
             const MacroTable& parent,
             const RowMap& parent_overlay)
      : Table(pool,
              parent_overlay.size(),
              std::move(columns),
              SelectedOverlaysFromParent(parent, parent_overlay)),
        allow_inserts_(false),
        parent_(&parent) {}

  ~MacroTable() override;

  PERFETTO_NO_INLINE void UpdateOverlaysAfterParentInsert() {
    CopyLastInsertFrom(parent_->overlays());
  }

  PERFETTO_NO_INLINE void UpdateSelfOverlayAfterInsert() {
    IncrementRowCountAndAddToLastOverlay();
  }

  PERFETTO_NO_INLINE static std::vector<ColumnLegacy>
  CopyColumnsFromParentOrAddRootColumns(MacroTable* self,
                                        const MacroTable* parent) {
    std::vector<ColumnLegacy> columns;
    if (parent) {
      for (const ColumnLegacy& col : parent->columns()) {
        columns.emplace_back(col, col.index_in_table(), col.overlay_index());
      }
    } else {
      columns.emplace_back(ColumnLegacy::IdColumn(0, 0));
      columns.emplace_back("type", &self->type_, ColumnLegacy::kNonNull, 1, 0);
    }
    return columns;
  }

  PERFETTO_NO_INLINE void OnConstructionCompletedRegularConstructor(
      std::initializer_list<RefPtr<column::DataLayer>> storage_layers,
      std::initializer_list<RefPtr<column::DataLayer>> null_layers) {
    std::vector<RefPtr<column::DataLayer>> overlay_layers(
        OverlayCount(parent_) + 1);
    for (uint32_t i = 0; i < overlay_layers.size() - 1; ++i) {
      PERFETTO_CHECK(overlays()[i].row_map().IsBitVector());
      overlay_layers[i].reset(new column::SelectorOverlay(
          overlays()[i].row_map().GetIfBitVector()));
    }
    Table::OnConstructionCompleted(storage_layers, null_layers,
                                   std::move(overlay_layers));
  }

  template <typename T>
  PERFETTO_NO_INLINE static void AddColumnToVector(
      std::vector<ColumnLegacy>& columns,
      const char* name,
      ColumnStorage<T>* storage,
      uint32_t flags,
      uint32_t column_index,
      uint32_t overlay_index) {
    columns.emplace_back(name, storage, flags, column_index, overlay_index);
  }

  static uint32_t OverlayCount(const MacroTable* parent) {
    return parent ? static_cast<uint32_t>(parent->overlays().size()) : 0;
  }

  // Stores whether inserts are allowed into this macro table; by default
  // inserts are allowed but they are disallowed when a parent table is extended
  // with |ExtendParent|; the rationale for this is that extensions usually
  // happen in dynamic tables and they should not be allowed to insert rows into
  // the real (static) tables.
  bool allow_inserts_ = true;

  // Stores the most specific "derived" type of this row in the table.
  //
  // For example, suppose a row is inserted into the gpu_slice table. This will
  // also cause a row to be inserted into the slice table. For users querying
  // the slice table, they will want to know the "real" type of this slice (i.e.
  // they will want to see that the type is gpu_slice). This sparse vector
  // stores precisely the real type.
  //
  // Only relevant for parentless tables. Will be empty and unreferenced by
  // tables with parents.
  ColumnStorage<StringPool::Id> type_;

 private:
  PERFETTO_NO_INLINE static std::vector<ColumnStorageOverlay>
  EmptyOverlaysFromParent(const MacroTable* parent) {
    std::vector<ColumnStorageOverlay> overlays(
        parent ? parent->overlays().size() : 0);
    for (auto& overlay : overlays) {
      overlay = ColumnStorageOverlay(BitVector());
    }
    overlays.emplace_back();
    return overlays;
  }
  PERFETTO_NO_INLINE static std::vector<ColumnStorageOverlay>
  SelectedOverlaysFromParent(const macros_internal::MacroTable& parent,
                             const RowMap& rm) {
    std::vector<ColumnStorageOverlay> overlays;
    for (const auto& overlay : parent.overlays()) {
      overlays.emplace_back(overlay.SelectRows(rm));
      PERFETTO_DCHECK(overlays.back().size() == rm.size());
    }
    overlays.emplace_back(rm.size());
    return overlays;
  }

  const MacroTable* parent_ = nullptr;
};

// Abstract iterator class for macro tables.
// Extracted to allow sharing with view code.
template <typename Iterator,
          typename MacroTable,
          typename RowNumber,
          typename ConstRowReference>
class AbstractConstIterator {
 public:
  explicit operator bool() const { return bool(iterator_); }

  Iterator& operator++() {
    ++iterator_;
    return *this_it();
  }

  // Returns a RowNumber for the current row.
  RowNumber row_number() const {
    return RowNumber(this_it()->CurrentRowNumber());
  }

  // Returns a ConstRowReference to the current row.
  ConstRowReference row_reference() const {
    return ConstRowReference(table_, this_it()->CurrentRowNumber());
  }

 protected:
  explicit AbstractConstIterator(const MacroTable* table,
                                 Table::Iterator iterator)
      : iterator_(std::move(iterator)), table_(table) {
    static_assert(std::is_base_of<Table, MacroTable>::value,
                  "Template param should be a subclass of Table.");
  }

  Table::Iterator iterator_;
  const MacroTable* table_;

 private:
  Iterator* this_it() { return static_cast<Iterator*>(this); }
  const Iterator* this_it() const { return static_cast<const Iterator*>(this); }
};

// Abstract RowNumber class for macro tables.
// Extracted to allow sharing with view code.
template <typename MacroTable,
          typename ConstRowReference,
          typename RowReference = void>
class AbstractRowNumber {
 public:
  // Converts this RowNumber to a RowReference for the given |table|.
  template <
      typename RR = RowReference,
      typename = typename std::enable_if<!std::is_same<RR, void>::value>::type>
  RR ToRowReference(MacroTable* table) const {
    return RR(table, row_number_);
  }

  // Converts this RowNumber to a ConstRowReference for the given |table|.
  ConstRowReference ToRowReference(const MacroTable& table) const {
    return ConstRowReference(&table, row_number_);
  }

  // Converts this object to the underlying int value.
  uint32_t row_number() const { return row_number_; }

  // Allows sorting + storage in a map/set.
  bool operator<(const AbstractRowNumber& other) const {
    return row_number_ < other.row_number_;
  }

 protected:
  explicit AbstractRowNumber(uint32_t row_number) : row_number_(row_number) {}

 private:
  uint32_t row_number_ = 0;
};

// Abstract ConstRowReference class for macro tables.
// Extracted to allow sharing with view code.
template <typename MacroTable, typename RowNumber>
class AbstractConstRowReference {
 public:
  // Converts this RowReference to a RowNumber object which is more memory
  // efficient to store.
  RowNumber ToRowNumber() { return RowNumber(row_number_); }

 protected:
  AbstractConstRowReference(const MacroTable* table, uint32_t row_number)
      : table_(table), row_number_(row_number) {}

  const MacroTable* table_ = nullptr;
  uint32_t row_number_ = 0;
};

}  // namespace perfetto::trace_processor::macros_internal

#endif  // SRC_TRACE_PROCESSOR_TABLES_MACROS_INTERNAL_H_
