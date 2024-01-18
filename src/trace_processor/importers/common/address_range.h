/*
 * Copyright (C) 2024 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_ADDRESS_RANGE_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_ADDRESS_RANGE_H_

#include <algorithm>
#include <cstdint>
#include <map>
#include <tuple>
#include <utility>

#include "perfetto/base/logging.h"

namespace perfetto {
namespace trace_processor {

// A range in the form [start, end), i.e. start is inclusive and end is
// exclusive.
// Note: This means that you can not have a range containing int64_max
class AddressRange {
 public:
  constexpr AddressRange() : AddressRange(0, 0) {}

  constexpr AddressRange(uint64_t start, uint64_t end)
      : start_(start), end_(end) {
    PERFETTO_CHECK(start <= end);
  }

  // Checks whether the given `addr` lies withing this range.
  constexpr bool Contains(uint64_t addr) const {
    return start_ <= addr && addr < end_;
  }

  // Checks whether the given `other` range is fully contained in this range.
  constexpr bool Contains(const AddressRange& other) const {
    return start_ <= other.start_ && other.end_ <= end_;
  }

  // Computes the intersection of the two ranges, that is, it returns a range
  // with all the points in common between the two.
  constexpr AddressRange IntersectWith(const AddressRange& other) const {
    auto start = std::max(start_, other.start_);
    auto end = std::min(end_, other.end_);
    return start < end ? AddressRange(start, end) : AddressRange();
  }

  // Checks whether there is any overlap between the two ranges, that it, if
  // there exists a point such that Contains(point) would return true for both
  // ranges.
  constexpr bool Overlaps(const AddressRange& other) const {
    return start_ < other.end_ && other.start_ < end_;
  }

  // Two ranges are the same is their respective limits are the same, that is A
  // contains A and B contains A
  constexpr bool operator==(const AddressRange& other) const {
    return start_ == other.start_ && end_ == other.end_;
  }
  constexpr bool operator!=(const AddressRange& other) const {
    return !(*this == other);
  }

  // Start of range, inclusive
  constexpr uint64_t start() const { return start_; }
  // Start of range, exclusive
  constexpr uint64_t end() const { return end_; }

  constexpr uint64_t length() const { return end_ - start_; }
  constexpr uint64_t size() const { return end_ - start_; }

  // Check whether the length is zero, that is no point will is contained by
  // this range.
  constexpr bool empty() const { return length() == 0; }

 private:
  uint64_t start_;
  uint64_t end_;
};

// Maps AddressRange instances to a given value. These AddressRange instances
// (basically the keys of the map)  will never overlap, as insertions of
// overlapping ranges will always fail.
template <typename Value>
class AddressRangeMap {
 public:
  struct CompareByEnd {
    // Allow heterogeneous lookups (https://abseil.io/tips/144)
    using is_transparent = void;
    // Keeps ranges sorted by end address
    bool operator()(const AddressRange& lhs, const AddressRange& rhs) const {
      return lhs.end() < rhs.end();
    }

    // Overload to implement PC lookup via upper_bound.
    bool operator()(const AddressRange& lhs, uint64_t pc) const {
      return lhs.end() < pc;
    }

    // Overload to implement PC lookup via upper_bound.
    bool operator()(uint64_t pc, const AddressRange& rhs) const {
      return pc < rhs.end();
    }
  };

  using Impl = std::map<AddressRange, Value, CompareByEnd>;

  using value_type = typename Impl::value_type;
  using iterator = typename Impl::iterator;
  using const_iterator = typename Impl::const_iterator;
  using size_type = typename Impl::size_type;

  // Fails if the new range overlaps with any existing one.
  template <typename... Args>
  std::pair<iterator, bool> Emplace(AddressRange range, Args&&... args) {
    auto it = ranges_.upper_bound(range.start());
    if (it != ranges_.end() && range.end() > it->first.start()) {
      return {it, false};
    }
    return {ranges_.emplace_hint(
                it, std::piecewise_construct, std::forward_as_tuple(range),
                std::forward_as_tuple(std::forward<Args>(args)...)),
            true};
  }

  // Finds the map entry that fully contains the given `range` or `end()` if not
  // such entry can be found.
  // ATTENTION: `range` can not be empty. Strictly speaking any range contains
  // the empty range but that would mean we need to return all the ranges here.
  // So we chose to just ban that case.
  iterator FindRangeThatContains(AddressRange range) {
    PERFETTO_CHECK(!range.empty());
    auto it = Find(range.start());
    if (it != end() && it->first.end() >= range.end()) {
      return it;
    }
    return end();
  }

  // Finds the range that contains a given address.
  iterator Find(uint64_t address) {
    auto it = ranges_.upper_bound(address);
    if (it != ranges_.end() && address >= it->first.start()) {
      return it;
    }
    return end();
  }

  // Finds the range that contains a given address.
  const_iterator Find(uint64_t address) const {
    auto it = ranges_.upper_bound(address);
    if (it != ranges_.end() && address >= it->first.start()) {
      return it;
    }
    return end();
  }

  // std::map like methods

  bool empty() const { return ranges_.empty(); }
  bool size() const { return ranges_.size(); }
  iterator begin() { return ranges_.begin(); }
  const_iterator begin() const { return ranges_.begin(); }
  iterator end() { return ranges_.end(); }
  const_iterator end() const { return ranges_.end(); }
  iterator erase(const_iterator pos) { return ranges_.erase(pos); }

  // Emplaces a new value into the map by first deleting all overlapping
  // intervals. It takes an optional (set to nullptr to ignore) callback `cb`
  // that will be called for each deleted map entry.
  // ATTENTION: `range` can not be empty. Supporting it would complicate things
  // too much for a not needed use case.
  template <typename Callback, typename... Args>
  void DeleteOverlapsAndEmplace(Callback cb,
                                AddressRange range,
                                Args&&... args) {
    PERFETTO_CHECK(!range.empty());
    auto it = ranges_.upper_bound(range.start());
    PERFETTO_DCHECK(it == ranges_.end() || range.start() < it->first.end());

    while (it != ranges_.end() && range.end() > it->first.start()) {
      cb(*it);
      it = ranges_.erase(it);
    }

    ranges_.emplace_hint(it, std::piecewise_construct,
                         std::forward_as_tuple(range),
                         std::forward_as_tuple(std::forward<Args>(args)...));
  }

  // Same as above but without a callback.
  template <typename Callback, typename... Args>
  void DeleteOverlapsAndEmplace(AddressRange range, Args&&... args) {
    struct NoOp {
      void operator()(std::pair<const AddressRange, Value>&) {}
    };
    DeleteOverlapsAndEmplace(NoOp(), range, std::forward<Args>(args)...);
  }

 private:
  // Invariant: There are no overlapping ranges.
  // Which makes lookups O(log N). Also, ranges are sorted by end which makes
  // point lookups trivial using upper_bound()
  Impl ranges_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_ADDRESS_RANGE_H_
