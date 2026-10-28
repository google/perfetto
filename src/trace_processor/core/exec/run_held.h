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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_RUN_HELD_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_RUN_HELD_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/core/exec/buffer_pool.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"

// What a step's state holds for one run (see OperatorState): a Held<T> for
// anything with storage, a RunValue<T> for a plain value. Each registers with
// the state it is in, so the executor ends its run when the step's stream
// ends.

namespace perfetto::trace_processor::core::exec {

// Storage of up to this many bytes is kept across runs, so a pipeline run
// over and over on small inputs allocates nothing after its first run.
// Anything larger is given back as soon as the run ends.
inline constexpr uint64_t kRunKeptBytes = 64 * 1024;

template <typename T>
void EndRunOf(FlexVector<T>* v) {
  if (v->capacity() * sizeof(T) > kRunKeptBytes) {
    *v = FlexVector<T>();
  } else {
    v->clear();
  }
}

template <typename T>
void EndRunOf(std::vector<T>* v) {
  if (v->capacity() * sizeof(T) > kRunKeptBytes) {
    std::vector<T>().swap(*v);
  } else {
    v->clear();
  }
}

inline void EndRunOf(BitVector* v) {
  if (v->size() / 8 > kRunKeptBytes) {
    *v = BitVector();
  } else {
    v->clear();
  }
}

template <typename K, typename V>
void EndRunOf(base::FlatHashMap<K, V>* map) {
  map->Clear();
}

inline void EndRunOf(RowBatch* batch) {
  batch->Reset();
}

inline void EndRunOf(ColumnBuffer* buffer) {
  *buffer = ColumnBuffer();
}

template <typename T, size_t N>
void EndRunOf(std::array<T, N>*) {}

template <typename T>
void EndRunOf(std::unique_ptr<T>* held);

// Any other type ends its own run.
template <typename T>
void EndRunOf(T* held) {
  held->EndRun();
}

template <typename T>
void EndRunOf(std::unique_ptr<T>* held) {
  if (*held) {
    EndRunOf(held->get());
  }
}

// A list sized when a state is made, such as one entry for each aggregate:
// each entry ends its own run, and the list keeps its size.
template <typename T>
class RunEach : public std::vector<T> {
 public:
  void EndRun() {
    for (T& entry : *this) {
      EndRunOf(&entry);
    }
  }
};

// A T held for one run.
template <typename T>
class Held final : public T, public RunHeld {
 public:
  template <typename... Args>
  explicit Held(OperatorState* owner, Args&&... args)
      : T(std::forward<Args>(args)...), RunHeld(owner) {}
  using T::operator=;

  void EndRun() override { EndRunOf(static_cast<T*>(this)); }
};

// A status is put back to ok by the state itself, with no call.
template <>
class Held<base::Status> final : public base::Status {
 public:
  explicit Held(OperatorState* owner) {
    owner->registry().statuses.push_back(this);
  }
  Held(const Held&) = delete;
  Held& operator=(const Held&) = delete;
  using base::Status::operator=;
};

// Storage a step uses within one call only, kept between calls for its
// capacity alone (rule 2 of OperatorState): each call writes whatever it then
// reads, and it is bounded by a batch, so a run leaves nothing in it to end.
template <typename T>
class CallScratch final : public T {
 public:
  CallScratch() = default;
  using T::operator=;
};

// A plain value held for one run: it is `initial` again once the run ends.
template <typename T>
class RunValue final {
 public:
  static_assert(std::is_trivially_copyable_v<T> &&
                (sizeof(T) == 1 || sizeof(T) == 4 || sizeof(T) == 8));

  explicit RunValue(OperatorState* owner, T initial = T()) : value_(initial) {
    OperatorState::HeldValue held{&value_, 0, sizeof(T)};
    memcpy(&held.initial, &initial, sizeof(T));
    owner->registry().values.push_back(held);
  }
  RunValue(const RunValue&) = delete;
  RunValue& operator=(const RunValue&) = delete;

  operator T&() { return value_; }
  operator const T&() const { return value_; }
  T& operator*() { return value_; }
  const T& operator*() const { return value_; }
  T* operator->() { return &value_; }
  RunValue& operator=(T value) {
    value_ = value;
    return *this;
  }
  template <typename U>
  RunValue& operator+=(U delta) {
    value_ += delta;
    return *this;
  }
  template <typename U>
  RunValue& operator-=(U delta) {
    value_ -= delta;
    return *this;
  }
  T& operator++() { return ++value_; }
  T operator++(int) { return value_++; }

 private:
  T value_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_RUN_HELD_H_
