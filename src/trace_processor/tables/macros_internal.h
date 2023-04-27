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

#include <type_traits>

#include "perfetto/ext/base/small_vector.h"
#include "src/trace_processor/db/table.h"
#include "src/trace_processor/db/typed_column.h"

namespace perfetto {
namespace trace_processor {
namespace macros_internal {

// We define this class to allow the table macro below to compile without
// needing templates; in reality none of the methods will be called because the
// pointer to this class will always be null.
class RootParentTable : public Table {
 public:
  struct Row {
   public:
    Row(std::nullptr_t = nullptr) {}

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
    uint32_t row_number() { PERFETTO_FATAL("Should not be called"); }
  };
  IdAndRow Insert(const Row&) { PERFETTO_FATAL("Should not be called"); }

 private:
  explicit RootParentTable(std::nullptr_t);
};

// The parent class for all macro generated tables.
// This class is used to extract common code from the macro tables to reduce
// code size.
class MacroTable : public Table {
 protected:
  // Constructors for tables created by the regular constructor.
  MacroTable(StringPool* pool, const Table* parent = nullptr)
      : Table(pool), allow_inserts_(true), parent_(parent) {
    if (!parent) {
      overlays_.emplace_back();
      columns_.emplace_back(Column::IdColumn(this, 0, 0));
      columns_.emplace_back(
          Column("type", &type_, Column::kNonNull, this, 1, 0));
      return;
    }

    overlays_.resize(parent->overlays().size() + 1);
    for (const Column& col : parent->columns()) {
      columns_.emplace_back(col, this, col.index_in_table(),
                            col.overlay_index());
    }
  }

  // Constructor for tables created by SelectAndExtendParent.
  MacroTable(StringPool* pool,
             const Table& parent,
             const RowMap& parent_overlay)
      : Table(pool), allow_inserts_(false) {
    row_count_ = parent_overlay.size();
    for (const auto& rm : parent.overlays()) {
      overlays_.emplace_back(rm.SelectRows(parent_overlay));
      PERFETTO_DCHECK(overlays_.back().size() == row_count_);
    }
    overlays_.emplace_back(ColumnStorageOverlay(row_count_));

    for (const Column& col : parent.columns()) {
      columns_.emplace_back(col, this, col.index_in_table(),
                            col.overlay_index());
    }
  }
  ~MacroTable() override;

  // We don't want a move or copy constructor because we store pointers to
  // fields of macro tables which will be invalidated if we move/copy them.
  MacroTable(const MacroTable&) = delete;
  MacroTable& operator=(const MacroTable&) = delete;

  MacroTable(MacroTable&&) = delete;
  MacroTable& operator=(MacroTable&&) noexcept = delete;

  void UpdateOverlaysAfterParentInsert() {
    // Add the last inserted row in each of the parent row maps to the
    // corresponding row map in the child.
    for (uint32_t i = 0; i < parent_->overlays().size(); ++i) {
      const ColumnStorageOverlay& parent_rm = parent_->overlays()[i];
      overlays_[i].Insert(parent_rm.Get(parent_rm.size() - 1));
    }
  }

  void UpdateSelfOverlayAfterInsert() {
    // Also add the index of the new row to the identity row map and increment
    // the size.
    overlays_.back().Insert(row_count_++);
  }

  std::vector<ColumnStorageOverlay> FilterAndApplyToOverlays(
      const std::vector<Constraint>& cs,
      RowMap::OptimizeFor optimize_for) const {
    RowMap rm = FilterToRowMap(cs, optimize_for);
    std::vector<ColumnStorageOverlay> overlays;
    overlays.reserve(overlays_.size());
    for (uint32_t i = 0; i < overlays_.size(); ++i) {
      overlays.emplace_back(overlays_[i].SelectRows(rm));
    }
    return overlays;
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
  const Table* parent_ = nullptr;
};

// Abstract iterator class for macro tables.
// Extracted to allow sharing with view code.
template <typename Iterator,
          typename MacroTable,
          typename RowNumber,
          typename ConstRowReference>
class AbstractConstIterator {
 public:
  explicit operator bool() const { return its_[0]; }

  Iterator& operator++() {
    for (ColumnStorageOverlay::Iterator& it : its_) {
      it.Next();
    }
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
                                 std::vector<ColumnStorageOverlay> overlays)
      : overlays_(std::move(overlays)), table_(table) {
    static_assert(std::is_base_of<Table, MacroTable>::value,
                  "Template param should be a subclass of Table.");

    for (const auto& rm : overlays_) {
      its_.emplace_back(rm.IterateRows());
    }
  }

  // Must not be modified as |its_| contains pointers into this vector.
  std::vector<ColumnStorageOverlay> overlays_;
  std::vector<ColumnStorageOverlay::Iterator> its_;

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

}  // namespace macros_internal
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TABLES_MACROS_INTERNAL_H_
