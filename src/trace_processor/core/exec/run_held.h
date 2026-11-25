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
// the HeldState it is in, so the executor ends its run when the step's stream
// ends.

namespace perfetto::trace_processor::core::exec {

// A state which keeps what it holds for a run in Held<T> and RunValue<T>
// members. The Context ends the state's run by ending each of them.
class HeldState : public OperatorState {
 public:
  HeldState() = default;
  virtual ~HeldState();

  // Ends the run of everything the state holds. Called by the Context, never
  // by the step.
  void EndRun() {
    if (held_) {
      EndHeldRuns();
    }
  }

  // Whether the state holds anything for a run, which is settled once it is
  // made.
  bool holds() const { return held_ != nullptr; }

 private:
  friend class PipelineUse;
  template <typename T>
  friend class RunValue;
  template <typename T>
  friend class Held;

  // Something held for a run which ends it with a call made for its type
  // (see Held).
  struct HeldObject {
    void* where;
    void (*end_run)(void*);
  };
  // A plain value held for a run: where it is, and the bytes it starts each
  // run with. Put back with a store, so ending a run calls nothing for it.
  struct HeldValue {
    void* where;
    uint64_t initial;
    uint32_t size;
  };
  // Everything the state holds for a run. Behind a pointer, made on the
  // first registration, so a state holding nothing stays small.
  struct Registry {
    std::vector<HeldObject> held;
    std::vector<HeldValue> values;
  };

  Registry& registry() {
    if (!held_) {
      held_ = std::make_unique<Registry>();
    }
    return *held_;
  }
  void EndHeldRuns();
  void AddHeld(void* where, void (*end_run)(void*));

  std::unique_ptr<Registry> held_;
};

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

// A T held for one run. It registers itself with the state when made, so the
// executor can end its run without the step's help: ending a run lets go of
// what the run left, keeping only small allocations for the next run.
template <typename T>
class Held final : public T {
 public:
  template <typename... Args>
  explicit Held(HeldState* owner, Args&&... args)
      : T(std::forward<Args>(args)...) {
    owner->AddHeld(static_cast<T*>(this), &EndRun);
  }
  Held(const Held&) = delete;
  Held& operator=(const Held&) = delete;
  using T::operator=;

 private:
  static void EndRun(void* held) { EndRunOf(static_cast<T*>(held)); }
};

// Storage a step uses within one call only, kept between calls for its
// capacity alone (rule 2 of OperatorState), and never cleared for it, even by
// a rewind. A call must not depend on what an earlier call left, including
// that it is empty: write it, or clear it, before reading it.
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

  explicit RunValue(HeldState* owner, T initial = T()) : value_(initial) {
    HeldState::HeldValue held{&value_, 0, sizeof(T)};
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
