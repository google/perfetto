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

#ifndef SRC_TRACE_PROCESSOR_CONTAINERS_ROW_MAP_H_
#define SRC_TRACE_PROCESSOR_CONTAINERS_ROW_MAP_H_

#include <stdint.h>

#include <memory>
#include <optional>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/containers/bit_vector_iterators.h"

namespace perfetto {
namespace trace_processor {

// Those structs enable overloading std::visit, which makes code a lot
// cleaner. For details go to
// https://en.cppreference.com/w/cpp/utility/variant/visit
template <class... Ts>
struct overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

// Stores a list of row indicies in a space efficient manner. One or more
// columns can refer to the same RowMap. The RowMap defines the access pattern
// to iterate on rows.
//
// Naming convention:
//
// As both the input and output of RowMap is a uint32_t, it can be quite
// confusing to reason about what parameters/return values of the functions
// of RowMap actually means. To help with this, we define a strict convention
// of naming.
//
// row:     input - that is, rows are what are passed into operator[]; named as
//          such because a "row" number in a table is converted to an index to
//          lookup in the backing vectors.
// index:   output - that is, indices are what are returned from operator[];
//          named as such because an "index" is what's used to lookup data
//          from the backing vectors.
//
// Implementation details:
//
// Behind the scenes, this class is impelemented using one of three backing
// data-structures:
// 1. A start and end index (internally named 'range')
// 1. BitVector
// 2. std::vector<uint32_t> (internally named IndexVector).
//
// Generally the preference for data structures is range > BitVector >
// std::vector<uint32>; this ordering is based mainly on memory efficiency as we
// expect RowMaps to be large.
//
// However, BitVector and std::vector<uint32_t> allow things which are not
// possible with the data-structures preferred to them:
//  * a range (as the name suggests) can only store a compact set of indices
//  with no holes. A BitVector works around this limitation by storing a 1 at an
//  index where that row is part of the RowMap and 0 otherwise.
//  * as soon as ordering or duplicate rows come into play, we cannot use a
//   BitVector anymore as ordering/duplicate row information cannot be captured
//   by a BitVector.
//
// For small, sparse RowMaps, it is possible that a std::vector<uint32_t> is
// more efficient than a BitVector; in this case, we will make a best effort
// switch to it but the cases where this happens is not precisely defined.
class RowMap {
 public:
  using InputRow = uint32_t;
  using OutputIndex = uint32_t;

  struct Range {
    Range(OutputIndex start_index, OutputIndex end_index)
        : start(start_index), end(end_index) {}
    Range() : start(0), end(0) {}

    OutputIndex start = 0;  // This is an inclusive index.
    OutputIndex end = 0;    // This is an exclusive index.

    uint32_t size() const {
      PERFETTO_DCHECK(end >= start);
      return end - start;
    }
  };

  // Allows efficient iteration over the rows of a RowMap.
  //
  // Note: you should usually prefer to use the methods on RowMap directly (if
  // they exist for the task being attempted) to avoid the lookup for the mode
  // of the RowMap on every method call.
  class Iterator {
   public:
    explicit Iterator(const RowMap* rm);

    Iterator(Iterator&&) noexcept = default;
    Iterator& operator=(Iterator&&) = default;

    // Forwards the iterator to the next row of the RowMap.
    void Next() {
      std::visit(
          overloaded{[this](const Range&) { ++ordinal_; },
                     [this](const BitVector&) { set_bits_it_->Next(); },
                     [this](const std::vector<OutputIndex>&) { ++ordinal_; }},
          rm_->data_);
    }

    // Returns if the iterator is still valid.
    operator bool() const {
      return std::visit(
          overloaded{[this](const Range& r) { return ordinal_ < r.end; },
                     [this](const BitVector&) { return bool(*set_bits_it_); },
                     [this](const std::vector<OutputIndex>& vec) {
                       return ordinal_ < vec.size();
                     }},
          rm_->data_);
    }

    // Returns the index pointed to by this iterator.
    OutputIndex index() const {
      return std::visit(
          overloaded{[this](const Range&) { return ordinal_; },
                     [this](const BitVector&) { return set_bits_it_->index(); },
                     [this](const std::vector<OutputIndex>& vec) {
                       return vec[ordinal_];
                     }},
          rm_->data_);
    }

    // Returns the row of the index the iterator points to.
    InputRow row() const {
      return std::visit(
          overloaded{
              [this](const Range& r) { return ordinal_ - r.start; },
              [this](const BitVector&) { return set_bits_it_->ordinal(); },
              [this](const std::vector<OutputIndex>&) { return ordinal_; }},
          rm_->data_);
    }

   private:
    Iterator(const Iterator&) = delete;
    Iterator& operator=(const Iterator&) = delete;

    // Ordinal will not be used for BitVector based RowMap.
    uint32_t ordinal_ = 0;
    // Not nullptr for BitVector based RowMap.
    std::unique_ptr<BitVector::SetBitsIterator> set_bits_it_;

    const RowMap* rm_ = nullptr;
  };

  // Enum to allow users of RowMap to decide whether they want to optimize for
  // memory usage or for speed of lookups.
  enum class OptimizeFor {
    kMemory,
    kLookupSpeed,
  };

  // Creates an empty RowMap.
  // By default this will be implemented using a range.
  RowMap();

  // Creates a RowMap containing the range of indices between |start| and |end|
  // i.e. all indices between |start| (inclusive) and |end| (exclusive).
  RowMap(OutputIndex start,
         OutputIndex end,
         OptimizeFor optimize_for = OptimizeFor::kMemory);

  // Creates a RowMap backed by a BitVector.
  explicit RowMap(BitVector bit_vector);

  // Creates a RowMap backed by an std::vector<uint32_t>.
  explicit RowMap(std::vector<OutputIndex> vec);

  RowMap(const RowMap&) noexcept = delete;
  RowMap& operator=(const RowMap&) = delete;

  RowMap(RowMap&&) noexcept = default;
  RowMap& operator=(RowMap&&) = default;

  // Creates a RowMap containing just |index|.
  // By default this will be implemented using a range.
  static RowMap SingleRow(OutputIndex index) {
    return RowMap(index, index + 1);
  }

  // Creates a copy of the RowMap.
  // We have an explicit copy function because RowMap can hold onto large chunks
  // of memory and we want to be very explicit when making a copy to avoid
  // accidental leaks and copies.
  RowMap Copy() const;

  // Returns the size of the RowMap; that is the number of indices in the
  // RowMap.
  uint32_t size() const {
    return std::visit(
        overloaded{[](const Range& r) { return r.size(); },
                   [](const BitVector& bv) { return bv.CountSetBits(); },
                   [](const std::vector<OutputIndex>& vec) {
                     return static_cast<uint32_t>(vec.size());
                   }},
        data_);
  }

  // Returns whether this rowmap is empty.
  bool empty() const { return size() == 0; }

  // Returns the index at the given |row|.
  OutputIndex Get(InputRow row) const {
    return std::visit(
        overloaded{[row](const Range r) { return GetRange(r, row); },
                   [row](const BitVector& bv) { return GetBitVector(bv, row); },
                   [row](const std::vector<OutputIndex>& vec) {
                     return GetIndexVector(vec, row);
                   }},
        data_);
  }

  // Returns whether the RowMap contains the given index.
  bool Contains(OutputIndex index) const {
    return std::visit(overloaded{[index](const Range& r) {
                                   return index >= r.start && index < r.end;
                                 },
                                 [index](const BitVector& bv) {
                                   return index < bv.size() && bv.IsSet(index);
                                 },
                                 [index](const std::vector<OutputIndex>& vec) {
                                   return std::find(vec.begin(), vec.end(),
                                                    index) != vec.end();
                                 }},
                      data_);
  }

  // Returns the first row of the given |index| in the RowMap.
  std::optional<InputRow> RowOf(OutputIndex index) const {
    return std::visit(
        overloaded{[index](const Range& r) -> std::optional<InputRow> {
                     if (index < r.start || index >= r.end)
                       return std::nullopt;
                     return index - r.start;
                   },
                   [index](const BitVector& bv) {
                     return index < bv.size() && bv.IsSet(index)
                                ? std::make_optional(bv.CountSetBits(index))
                                : std::nullopt;
                   },
                   [index](const std::vector<OutputIndex>& vec) {
                     auto it = std::find(vec.begin(), vec.end(), index);
                     return it != vec.end()
                                ? std::make_optional(static_cast<InputRow>(
                                      std::distance(vec.begin(), it)))
                                : std::nullopt;
                   }},
        data_);
  }  // namespace trace_processor

  // Performs an ordered insert of the index into the current RowMap
  // (precondition: this RowMap is ordered based on the indices it contains).
  //
  // Example:
  // this = [1, 5, 10, 11, 20]
  // Insert(10)  // this = [1, 5, 10, 11, 20]
  // Insert(12)  // this = [1, 5, 10, 11, 12, 20]
  // Insert(21)  // this = [1, 5, 10, 11, 12, 20, 21]
  // Insert(2)   // this = [1, 2, 5, 10, 11, 12, 20, 21]
  //
  // Speecifically, this means that it is only valid to call Insert on a RowMap
  // which is sorted by the indices it contains; this is automatically true when
  // the RowMap is in range or BitVector mode but is a required condition for
  // IndexVector mode.
  void Insert(OutputIndex index) {
    std::visit(
        overloaded{[this, index](Range& r) {
                     if (index == r.end) {
                       // Fast path: if we're just appending to the end
                       // of the range, we can stay in range mode and
                       // just bump the end index.
                       r.end++;
                       return;
                     }

                     // Slow path: the insert is somewhere else other
                     // than the end. This means we need to switch to
                     // using a BitVector instead.
                     BitVector bv;
                     bv.Resize(r.start, false);
                     bv.Resize(r.end, true);
                     InsertIntoBitVector(bv, index);
                     data_ = std::move(bv);
                   },
                   [index](BitVector& bv) { InsertIntoBitVector(bv, index); },
                   [index](std::vector<OutputIndex>& vec) {
                     PERFETTO_DCHECK(std::is_sorted(vec.begin(), vec.end()));
                     auto it = std::upper_bound(vec.begin(), vec.end(), index);
                     vec.insert(it, index);
                   }},
        data_);
  }

  // Updates this RowMap by 'picking' the indices given by |picker|.
  // This is easiest to explain with an example; suppose we have the following
  // RowMaps:
  // this  : [0, 1, 4, 10, 11]
  // picker: [0, 3, 4, 4, 2]
  //
  // After calling Apply(picker), we now have the following:
  // this  : [0, 10, 11, 11, 4]
  //
  // Conceptually, we are performing the following algorithm:
  // RowMap rm = Copy()
  // for (p : picker)
  //   rm[i++] = this[p]
  // return rm;
  RowMap SelectRows(const RowMap& selector) const {
    uint32_t size = selector.size();

    // If the selector is empty, just return an empty RowMap.
    if (size == 0u)
      return RowMap();

    // If the selector is just picking a single row, just return that row
    // without any additional overhead.
    if (size == 1u)
      return RowMap::SingleRow(Get(selector.Get(0)));

    // For all other cases, go into the slow-path.
    return SelectRowsSlow(selector);
  }

  // Intersects the range [start_index, end_index) with |this| writing the
  // result into |this|. By "intersect", we mean to keep only the indices
  // present in both this RowMap and in the Range [start_index, end_index). The
  // order of the preserved indices will be the same as |this|.
  //
  // Conceptually, we are performing the following algorithm:
  // for (idx : this)
  //   if (start_index <= idx && idx < end_index)
  //     continue;
  //   Remove(idx)
  void Intersect(const RowMap& second);

  // Intersects this RowMap with |index|. If this RowMap contained |index|, then
  // it will *only* contain |index|. Otherwise, it will be empty.
  void IntersectExact(OutputIndex index) {
    if (Contains(index)) {
      *this = RowMap(index, index + 1);
    } else {
      Clear();
    }
  }

  // Clears this RowMap by resetting it to a newly constructed state.
  void Clear() { *this = RowMap(); }

  template <typename Comparator = bool(uint32_t, uint32_t)>
  void StableSort(std::vector<uint32_t>* out, Comparator c) const {
    std::visit(
        overloaded{
            [out, c](const Range& r) {
              std::stable_sort(out->begin(), out->end(),
                               [r, c](uint32_t a, uint32_t b) {
                                 return c(GetRange(r, a), GetRange(r, b));
                               });
            },
            [out, c](const BitVector& bv) {
              std::stable_sort(
                  out->begin(), out->end(), [&bv, c](uint32_t a, uint32_t b) {
                    return c(GetBitVector(bv, a), GetBitVector(bv, b));
                  });
            },
            [out, c](const std::vector<OutputIndex>& vec) {
              std::stable_sort(
                  out->begin(), out->end(), [vec, c](uint32_t a, uint32_t b) {
                    return c(GetIndexVector(vec, a), GetIndexVector(vec, b));
                  });
            }},
        data_);
  }

  // Filters the indices in |out| by keeping those which meet |p|.
  template <typename Predicate = bool(OutputIndex)>
  void Filter(Predicate p) {
    std::visit(overloaded{[p, this](Range& r) { data_ = FilterRange(p, r); },
                          [p](BitVector& bv) {
                            for (auto it = bv.IterateSetBits(); it; it.Next()) {
                              if (!p(it.index()))
                                it.Clear();
                            }
                          },
                          [p](std::vector<OutputIndex>& vec) {
                            auto ret = std::remove_if(
                                vec.begin(), vec.end(),
                                [p](uint32_t i) { return !p(i); });
                            vec.erase(ret, vec.end());
                          }},
               data_);
  }

  // Returns the iterator over the rows in this RowMap.
  Iterator IterateRows() const { return Iterator(this); }

  // Returns if the RowMap is internally represented using a range.
  bool IsRange() const { return std::holds_alternative<Range>(data_); }

  // Returns if the RowMap is internally represented using a BitVector.
  bool IsBitVector() const { return std::holds_alternative<BitVector>(data_); }

  // Returns if the RowMap is internally represented using an index vector.
  bool IsIndexVector() const {
    return std::holds_alternative<std::vector<OutputIndex>>(data_);
  }

 private:
  using Variant = std::variant<Range, BitVector, std::vector<OutputIndex>>;

  explicit RowMap(Range);

  explicit RowMap(Variant);

  // TODO(lalitm): remove this when the coupling between RowMap and
  // ColumnStorage Selector is broken (after filtering is moved out of here).
  friend class ColumnStorageOverlay;

  template <typename Predicate>
  static Variant FilterRange(Predicate p, Range r) {
    uint32_t count = r.size();

    // Optimization: if we are only going to scan a few indices, it's not
    // worth the haslle of working with a BitVector.
    constexpr uint32_t kSmallRangeLimit = 2048;
    bool is_small_range = count < kSmallRangeLimit;

    // Optimization: weif the cost of a BitVector is more than the highest
    // possible cost an index vector could have, use the index vector.
    uint32_t bit_vector_cost = BitVector::ApproxBytesCost(r.end);
    uint32_t index_vector_cost_ub = sizeof(uint32_t) * count;

    // If either of the conditions hold which make it better to use an
    // index vector, use it instead. Alternatively, if we are optimizing for
    // lookup speed, we also want to use an index vector.
    if (is_small_range || index_vector_cost_ub <= bit_vector_cost) {
      // Try and strike a good balance between not making the vector too
      // big and good performance.
      std::vector<uint32_t> iv(std::min(kSmallRangeLimit, count));

      uint32_t out_i = 0;
      for (uint32_t i = 0; i < count; ++i) {
        // If we reach the capacity add another small set of indices.
        if (PERFETTO_UNLIKELY(out_i == iv.size()))
          iv.resize(iv.size() + kSmallRangeLimit);

        // We keep this branch free by always writing the index but only
        // incrementing the out index if the return value is true.
        bool value = p(i + r.start);
        iv[out_i] = i + r.start;
        out_i += value;
      }

      // Make the vector the correct size and as small as possible.
      iv.resize(out_i);
      iv.shrink_to_fit();

      return std::move(iv);
    }

    // Otherwise, create a bitvector which spans the full range using
    // |p| as the filler for the bits between start and end.
    return BitVector::Range(r.start, r.end, p);
  }

  PERFETTO_ALWAYS_INLINE static OutputIndex GetRange(Range r, InputRow row) {
    return r.start + row;
  }
  PERFETTO_ALWAYS_INLINE static OutputIndex GetBitVector(const BitVector& bv,
                                                         uint32_t row) {
    return bv.IndexOfNthSet(row);
  }
  PERFETTO_ALWAYS_INLINE static OutputIndex GetIndexVector(
      const std::vector<OutputIndex> vec,
      uint32_t row) {
    return vec[row];
  }

  RowMap SelectRowsSlow(const RowMap& selector) const;

  static void InsertIntoBitVector(BitVector& bv, OutputIndex row) {
    if (row == bv.size()) {
      bv.AppendTrue();
      return;
    }
    if (row > bv.size())
      bv.Resize(row + 1, false);
    bv.Set(row);
  }

  Variant data_;
  OptimizeFor optimize_for_ = OptimizeFor::kMemory;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_CONTAINERS_ROW_MAP_H_
