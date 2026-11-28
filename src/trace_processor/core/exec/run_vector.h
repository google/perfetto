/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_RUN_VECTOR_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_RUN_VECTOR_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/murmur_hash.h"
#include "perfetto/ext/base/span.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/operator.h"

namespace perfetto::trace_processor::core::exec {

// Values a step's state holds for a run, in memory owned by its Context. The
// state holds only a pointer to the values, so it needs no destructor. Growing
// goes through Context::GrowRun(), which every element type shares. Every
// method which can grow the vector takes the state it belongs to, its `owner`.
//
// A RunVector is one of a state's run values: it lives in the state itself, in
// a span made with Context::NewRunSpan(), or in another RunVector of the same
// state. When the state's run ends, the vector is treated as `kKeep` says (see
// RunKeep); memory it no longer uses is the Context's to reuse or free. Copying
// a RunVector copies the pointer, not the values, so only one of the copies may
// be changed afterwards.
template <typename T, RunKeep kKeep = RunKeep::kSmall>
class RunVector {
 public:
  static_assert(std::is_trivially_copyable_v<T> &&
                    std::is_trivially_destructible_v<T>,
                "a run's memory is copied as bytes and never destroyed");

  PERFETTO_ALWAYS_INLINE T* data() const {
    return static_cast<T*>(header_.data);
  }
  PERFETTO_ALWAYS_INLINE T* begin() const { return data(); }
  PERFETTO_ALWAYS_INLINE T* end() const { return data() + header_.size; }
  PERFETTO_ALWAYS_INLINE uint32_t size() const {
    return static_cast<uint32_t>(header_.size);
  }
  PERFETTO_ALWAYS_INLINE uint32_t capacity() const {
    return static_cast<uint32_t>(header_.capacity);
  }
  PERFETTO_ALWAYS_INLINE bool empty() const { return header_.size == 0; }
  PERFETTO_ALWAYS_INLINE T& operator[](uint64_t i) const {
    PERFETTO_DCHECK(i < header_.size);
    return data()[i];
  }
  PERFETTO_ALWAYS_INLINE T& back() const {
    PERFETTO_DCHECK(header_.size > 0);
    return data()[header_.size - 1];
  }
  base::Span<const T> span() const { return {data(), end()}; }

  PERFETTO_ALWAYS_INLINE void push_back(OperatorState& owner, T value) {
    if (PERFETTO_UNLIKELY(header_.size == header_.capacity)) {
      Grow(owner, header_.size + 1);
    }
    data()[header_.size++] = value;
  }

  // Adds `count` copies of `value`.
  PERFETTO_ALWAYS_INLINE void push_back_multiple(OperatorState& owner,
                                                 T value,
                                                 uint64_t count) {
    uint64_t size = header_.size + count;
    if (PERFETTO_UNLIKELY(size > header_.capacity)) {
      Grow(owner, size);
    }
    // Fill through a local pointer, since a store through data() could alias
    // the size.
    T* begin = data() + header_.size;
    std::fill(begin, begin + count, value);
    header_.size = size;
  }

  // Resizes to `size`, leaving any added values uninitialized.
  PERFETTO_ALWAYS_INLINE void resize(OperatorState& owner, uint64_t size) {
    if (PERFETTO_UNLIKELY(size > header_.capacity)) {
      Grow(owner, size);
    }
    header_.size = size;
  }

  // Makes room for `capacity` values in total without changing the size,
  // growing as appending would.
  PERFETTO_ALWAYS_INLINE void reserve(OperatorState& owner, uint64_t capacity) {
    if (PERFETTO_UNLIKELY(capacity > header_.capacity)) {
      Grow(owner, capacity);
    }
  }

  // Appends `count` uninitialized values in the room reserve() made, and
  // returns a pointer to them.
  PERFETTO_ALWAYS_INLINE T* append_reserved(uint64_t count) {
    PERFETTO_DCHECK(header_.size + count <= header_.capacity);
    T* added = data() + header_.size;
    header_.size += count;
    return added;
  }

  // Replaces the contents with `count` copies of `value`.
  void assign(OperatorState& owner, uint64_t count, T value) {
    header_.size = 0;
    push_back_multiple(owner, value, count);
  }

  void pop_back() {
    PERFETTO_DCHECK(header_.size > 0);
    --header_.size;
  }

  void clear() { header_.size = 0; }

 private:
  PERFETTO_NO_INLINE void Grow(OperatorState& owner, uint64_t min_capacity) {
    PERFETTO_CHECK(min_capacity <= UINT32_MAX);
    owner.context().GrowRun(owner, &header_, min_capacity, sizeof(T), kKeep);
  }

  RunHeader header_{nullptr, 0, 0};
};

// A bit vector a step's state holds for a run, stored as a RunVector is.
class RunBits {
 public:
  uint64_t size() const { return size_; }
  bool empty() const { return size_ == 0; }

  PERFETTO_ALWAYS_INLINE bool is_set(uint64_t i) const {
    PERFETTO_DCHECK(i < size_);
    return (words_[i / 64] >> (i % 64)) & 1;
  }
  PERFETTO_ALWAYS_INLINE void set(uint64_t i) {
    PERFETTO_DCHECK(i < size_);
    words_[i / 64] |= uint64_t{1} << (i % 64);
  }
  PERFETTO_ALWAYS_INLINE void clear(uint64_t i) {
    PERFETTO_DCHECK(i < size_);
    words_[i / 64] &= ~(uint64_t{1} << (i % 64));
  }
  PERFETTO_ALWAYS_INLINE void push_back(OperatorState& owner, bool value) {
    if (size_ % 64 == 0) {
      words_.push_back(owner, 0);
    }
    words_[size_ / 64] |= uint64_t{value} << (size_ % 64);
    ++size_;
  }
  // Resizes to `size` bits, setting any added bits to `value`.
  void resize(OperatorState& owner, uint64_t size, bool value);

  uint64_t CountSetBits() const;

  void clear() {
    words_.clear();
    size_ = 0;
  }

 private:
  RunVector<uint64_t> words_;
  uint64_t size_ = 0;
};

// A hash map from 64-bit keys to 32-bit numbers, held for a run as a RunVector
// is. It is laid out and probed like base::FlatHashMap, which a state cannot
// hold because it owns its memory. A tag byte from each key's hash is stored
// apart from the keys and values, so a probe reads a key only when its tag
// matches. Like FlatHashMap::Clear(), ending a run keeps the slots, however
// many there are.
class RunMap {
 public:
  uint32_t size() const { return size_; }

  // The number `key` maps to, or null.
  PERFETTO_ALWAYS_INLINE uint32_t* Find(uint64_t key) const {
    uint64_t hash = base::MurmurHashValue(key);
    uint8_t tag = Tag(hash);
    for (uint32_t step = 0; step < max_probe_;) {
      size_t at = base::QuadraticProbe::Calc(hash, step++, tags_.size());
      uint8_t found = tags_[at];
      if (!found) {
        return nullptr;
      }
      if (found == tag && keys_[at] == key) {
        return &values_[at];
      }
    }
    return nullptr;
  }

  // Returns the number `key` maps to, first mapping it to `value` if it maps to
  // nothing.
  PERFETTO_ALWAYS_INLINE uint32_t FindOrInsert(OperatorState& owner,
                                               uint64_t key,
                                               uint32_t value) {
    if (PERFETTO_UNLIKELY(uint64_t{size_} * 4 >= uint64_t{tags_.size()} * 3)) {
      Grow(owner);
    }
    uint64_t hash = base::MurmurHashValue(key);
    uint8_t tag = Tag(hash);
    for (uint32_t step = 0;;) {
      size_t at = base::QuadraticProbe::Calc(hash, step++, tags_.size());
      uint8_t found = tags_[at];
      if (!found) {
        tags_[at] = tag;
        keys_[at] = key;
        values_[at] = value;
        ++size_;
        max_probe_ = std::max(max_probe_, step);
        return value;
      }
      if (found == tag && keys_[at] == key) {
        return values_[at];
      }
    }
  }

  // Maps `key`, which maps to nothing, to `value`.
  void Insert(OperatorState& owner, uint64_t key, uint32_t value) {
    PERFETTO_DCHECK(!Find(key));
    FindOrInsert(owner, key, value);
  }

  void clear();

 private:
  // The top byte of the hash. It is never 0, since 0 marks an empty slot.
  PERFETTO_ALWAYS_INLINE static uint8_t Tag(uint64_t hash) {
    auto tag = static_cast<uint8_t>(hash >> 56);
    return tag ? tag : 1;
  }
  // Grows the slots, as FlatHashMap does, keeping what is mapped.
  void Grow(OperatorState& owner);

  RunVector<uint8_t, RunKeep::kAllZeroed> tags_;
  RunVector<uint64_t, RunKeep::kAll> keys_;
  RunVector<uint32_t, RunKeep::kAll> values_;
  uint32_t size_ = 0;
  // The most slots any insertion probed.
  uint32_t max_probe_ = 0;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_RUN_VECTOR_H_
