/*
 * Copyright (C) 2022 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_DB_COLUMN_STORAGE_OVERLAY_H_
#define SRC_TRACE_PROCESSOR_DB_COLUMN_STORAGE_OVERLAY_H_

#include <stdint.h>

#include <memory>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/optional.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/containers/bit_vector_iterators.h"
#include "src/trace_processor/containers/row_map.h"

namespace perfetto {
namespace trace_processor {

// Contains indices which can be used to lookup data in one or more
// ColumnStorages.
//
// Implemented as a thin wrapper around RowMap so much of the documentation
// from RowMap also applies to this class.
class ColumnStorageOverlay {
 public:
  // Input type.
  using InputRow = uint32_t;
  using OutputIndex = uint32_t;

  // Allows efficient iteration over the rows of a ColumnStorageOverlay.
  class Iterator {
   public:
    Iterator(RowMap::Iterator it) : it_(std::move(it)) {}

    Iterator(Iterator&&) noexcept = default;
    Iterator& operator=(Iterator&&) = default;

    // Forwards the iterator to the next row of the ColumnStorageOverlay.
    void Next() { return it_.Next(); }

    // Returns if the iterator is still valid.
    operator bool() const { return it_; }

    // Returns the index pointed to by this iterator.
    OutputIndex index() const { return it_.index(); }

    // Returns the row of the index the iterator points to.
    InputRow row() const { return it_.row(); }

   private:
    RowMap::Iterator it_;
  };

  // Creates an empty ColumnStorageOverlay.
  // By default this will be implemented using a range.
  ColumnStorageOverlay() : ColumnStorageOverlay(0) {}

  // Creates a |ColumnStorageOverlay| containing all rows between 0 and |size|.
  explicit ColumnStorageOverlay(uint32_t size)
      : ColumnStorageOverlay(0, size) {}

  // Creates a |ColumnStorageOverlay| containing all rows between |start| and
  // |end|.
  explicit ColumnStorageOverlay(uint32_t start, uint32_t end)
      : ColumnStorageOverlay(RowMap(start, end)) {}

  // Creates a |ColumnStorageOverlay| containing all rows corresponding to set
  // bits in |bv|.
  explicit ColumnStorageOverlay(BitVector bv)
      : ColumnStorageOverlay(RowMap(std::move(bv))) {}

  // Creates a |ColumnStorageOverlay| containing all rows in |rows|.
  explicit ColumnStorageOverlay(std::vector<uint32_t> rows)
      : ColumnStorageOverlay(RowMap(std::move(rows))) {}

  ColumnStorageOverlay(const ColumnStorageOverlay&) noexcept = delete;
  ColumnStorageOverlay& operator=(const ColumnStorageOverlay&) = delete;

  ColumnStorageOverlay(ColumnStorageOverlay&&) noexcept = default;
  ColumnStorageOverlay& operator=(ColumnStorageOverlay&&) = default;

  // Creates a copy of the ColumnStorageOverlay.
  // We have an explicit copy function because ColumnStorageOverlay can hold
  // onto large chunks of memory and we want to be very explicit when making a
  // copy to avoid accidental leaks and copies.
  ColumnStorageOverlay Copy() const {
    return ColumnStorageOverlay(row_map_.Copy());
  }

  // Returns the size of the ColumnStorageOverlay; that is the number of
  // indices in the ColumnStorageOverlay.
  uint32_t size() const { return row_map_.size(); }

  // Returns whether this ColumnStorageOverlay is empty.
  bool empty() const { return size() == 0; }

  // Returns the index at the given |row|.
  OutputIndex Get(uint32_t row) const { return row_map_.Get(row); }

  // Returns the first row of the given |index| in the ColumnStorageOverlay.
  base::Optional<InputRow> RowOf(OutputIndex index) const {
    return row_map_.RowOf(index);
  }

  // Performs an ordered insert of the index into the current
  // ColumnStorageOverlay (precondition: this ColumnStorageOverlay is ordered
  // based on the indices it contains).
  //
  // See RowMap::Insert for more information on this function.
  void Insert(OutputIndex index) { return row_map_.Insert(index); }

  // Updates this ColumnStorageOverlay by 'picking' the indices given by
  // |picker|.
  //
  // See RowMap::SelectRows for more information on this function.
  ColumnStorageOverlay SelectRows(const RowMap& selector) const {
    return ColumnStorageOverlay(row_map_.SelectRows(selector));
  }

  // Clears this ColumnStorageOverlay by resetting it to a newly constructed
  // state.
  void Clear() { *this = ColumnStorageOverlay(); }

  // Filters the current ColumnStorageOverlay into the RowMap given by |out|
  // based on the return value of |p(idx)|.
  //
  // Precondition: |out| should be sorted by the indices inside it (this is
  // required to keep this method efficient). This is automatically true if the
  // mode of |out| is Range or BitVector but needs to be enforced if the mode is
  // IndexVector.
  //
  // Specifically, the setup for each of the variables is as follows:
  //  this: contains the indices passed to p to filter.
  //  out : contains indicies into |this| and will be filtered down to only
  //        contain indicies where p returns true.
  //  p   : takes an index given by |this| and returns whether the index should
  //        be retained in |out|.
  //
  // Concretely, the algorithm being invoked looks like (but more efficient
  // based on the mode of |this| and |out|):
  // for (idx : out)
  //   this_idx = (*this)[idx]
  //   if (!p(this_idx))
  //     out->Remove(idx)
  template <typename Predicate>
  void FilterInto(RowMap* out, Predicate p) const {
    PERFETTO_DCHECK(size() >= out->size());

    if (out->empty()) {
      // If the output ColumnStorageOverlay is empty, we don't need to do
      // anything.
      return;
    }

    if (out->size() == 1) {
      // If the output ColumnStorageOverlay has a single entry, just lookup
      // that entry and see if we should keep it.
      if (!p(Get(out->Get(0))))
        out->Clear();
      return;
    }

    // TODO(lalitm): investigate whether we should have another fast path for
    // cases where |out| has only a few entries so we can scan |out| instead of
    // scanning |this|.

    // Ideally, we'd always just scan |out| and keep the indices in |this| which
    // meet |p|. However, if |this| is a BitVector, we end up needing expensive
    // |IndexOfNthSet| calls (as we need to convert the row to an index before
    // passing it to |p|).
    switch (row_map_.mode_) {
      case RowMap::Mode::kRange: {
        auto ip = [this, p](uint32_t row) { return p(row_map_.GetRange(row)); };
        out->Filter(ip);
        break;
      }
      case RowMap::Mode::kBitVector: {
        FilterIntoScanSelfBv(out, p);
        break;
      }
      case RowMap::Mode::kIndexVector: {
        auto ip = [this, p](uint32_t row) {
          return p(row_map_.GetIndexVector(row));
        };
        out->Filter(ip);
        break;
      }
    }
  }

  template <typename Comparator = bool(uint32_t, uint32_t)>
  void StableSort(std::vector<uint32_t>* out, Comparator c) const {
    return row_map_.StableSort(out, c);
  }

  // Returns the iterator over the rows in this ColumnStorageOverlay.
  Iterator IterateRows() const { return Iterator(row_map_.IterateRows()); }

 private:
  explicit ColumnStorageOverlay(RowMap rm) : row_map_(std::move(rm)) {}

  // Filters the current ColumnStorageOverlay into |out| by performing a full
  // scan on |row_map.bit_vector_|. See |FilterInto| for a full breakdown of the
  // semantics of this function.
  template <typename Predicate>
  void FilterIntoScanSelfBv(RowMap* out, Predicate p) const {
    auto it = row_map_.bit_vector_.IterateSetBits();
    switch (out->mode_) {
      case RowMap::Mode::kRange: {
        // TODO(lalitm): investigate whether we can reuse the data inside
        // out->bit_vector_ at some point.
        BitVector bv(out->end_index_, false);
        for (auto out_it = bv.IterateAllBits(); it; it.Next(), out_it.Next()) {
          uint32_t ordinal = it.ordinal();
          if (ordinal < out->start_index_)
            continue;
          if (ordinal >= out->end_index_)
            break;

          if (p(it.index())) {
            out_it.Set();
          }
        }
        *out = RowMap(std::move(bv));
        break;
      }
      case RowMap::Mode::kBitVector: {
        auto out_it = out->bit_vector_.IterateAllBits();
        for (; out_it; it.Next(), out_it.Next()) {
          PERFETTO_DCHECK(it);
          if (out_it.IsSet() && !p(it.index()))
            out_it.Clear();
        }
        break;
      }
      case RowMap::Mode::kIndexVector: {
        PERFETTO_DCHECK(std::is_sorted(out->index_vector_.begin(),
                                       out->index_vector_.end()));
        auto fn = [&p, &it](uint32_t i) {
          while (it.ordinal() < i) {
            it.Next();
            PERFETTO_DCHECK(it);
          }
          PERFETTO_DCHECK(it.ordinal() == i);
          return !p(it.index());
        };
        auto iv_it = std::remove_if(out->index_vector_.begin(),
                                    out->index_vector_.end(), fn);
        out->index_vector_.erase(iv_it, out->index_vector_.end());
        break;
      }
    }
  }

  RowMap row_map_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_DB_COLUMN_STORAGE_OVERLAY_H_
