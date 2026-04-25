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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_INCREMENTAL_STATE_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_INCREMENTAL_STATE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "perfetto/public/compiler.h"
#include "perfetto/trace_processor/ref_counted.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/proto/track_event_thread_descriptor.h"
#include "src/trace_processor/util/interned_message_view.h"

namespace perfetto::trace_processor {

using InternedMessageMap =
    std::unordered_map<uint64_t /*iid*/, InternedMessageView>;
using InternedFieldMap =
    std::unordered_map<uint32_t /*field_id*/, InternedMessageMap>;

class TraceProcessorContext;
class IncrementalState;

class StackProfileSequenceState;
class ProfilePacketSequenceState;
class V8SequenceState;
struct AndroidKernelWakelockState;
struct AndroidCpuPerUidState;
class TrackEventSequenceState;

using CustomStateClasses = std::tuple<StackProfileSequenceState,
                                      ProfilePacketSequenceState,
                                      V8SequenceState,
                                      AndroidKernelWakelockState,
                                      AndroidCpuPerUidState,
                                      TrackEventSequenceState>;

// Defines an optional dependency for a `CustomState` class, which will be
// passed as an argument to its constructor. The default leaves `Tracker` as
// `void`, meaning the CustomState is constructed with only
// `TraceProcessorContext*`. Specialise to add a single extra Tracker* arg.
template <typename T>
struct CustomStateTraits {
  using Tracker = void;
};

// Base class for extension state attached to a packet sequence's incremental
// state interval. One instance per CustomState subclass per
// `IncrementalState`; owned uniquely by the IncrementalState (NOT
// RefCounted). The back-pointer to its owning IncrementalState is set once at
// construction and is stable for the entire lifetime of the CustomState,
// since the IncrementalState owns it.
//
// ATTENTION: do not create instances directly — use
// `IncrementalState::GetCustomState<>` or
// `PacketSequenceStateGeneration::GetCustomState<>`.
class CustomState {
 public:
  virtual ~CustomState();

  // Hook called when the packet sequence experiences packet loss. Override to
  // return true if this CustomState's contents should be discarded — the slot
  // in the IncrementalState's array is cleared, and the next call to
  // `GetCustomState<T>` will lazy-create a fresh instance.
  virtual bool ClearOnPacketLoss() const { return false; }

 protected:
  template <uint32_t FieldId, typename MessageType>
  typename MessageType::Decoder* LookupInternedMessage(uint64_t iid);
  InternedMessageView* GetInternedMessageView(uint32_t field_id, uint64_t iid);

  template <typename T, typename... Args>
  std::remove_cv_t<T>* GetCustomState(Args... args);

  const TrackEventThreadDescriptor& thread_descriptor() const;

 private:
  friend class IncrementalState;
  void set_incremental(IncrementalState* incremental) {
    incremental_ = incremental;
  }

  IncrementalState* incremental_ = nullptr;
};

// Holds the per-incremental-state-interval state for one packet sequence: the
// interned-data table, the persistent thread descriptor, and the array of
// `CustomState` extensions. RefCounted because a single `IncrementalState` is
// shared across all `PacketSequenceStateGeneration` instances within the same
// interval (one per `trace_packet_defaults` slice). A new `IncrementalState`
// is constructed when `SEQ_INCREMENTAL_STATE_CLEARED` arrives.
class IncrementalState : public RefCounted {
 public:
  explicit IncrementalState(TraceProcessorContext* context)
      : context_(context) {}

  // Helper for `OnIncrementalStateCleared`: construct a successor that
  // inherits the persistent thread descriptor from the previous interval.
  static RefPtr<IncrementalState> CreateSuccessor(
      TraceProcessorContext* context,
      TrackEventThreadDescriptor thread_descriptor) {
    auto incr = RefPtr<IncrementalState>(new IncrementalState(context));
    incr->thread_descriptor_ = std::move(thread_descriptor);
    return incr;
  }

  TrackEventThreadDescriptor& thread_descriptor() { return thread_descriptor_; }
  const TrackEventThreadDescriptor& thread_descriptor() const {
    return thread_descriptor_;
  }

  // Returns |nullptr| if the message with the given |iid| was not found (also
  // records a stat in this case).
  template <uint32_t FieldId, typename MessageType>
  typename MessageType::Decoder* LookupInternedMessage(uint64_t iid) {
    auto* view = GetInternedMessageView(FieldId, iid);
    if (!view)
      return nullptr;
    return view->template GetOrCreateDecoder<MessageType>();
  }
  InternedMessageView* GetInternedMessageView(uint32_t field_id, uint64_t iid);

  // Lazy-creates the requested CustomState if absent; returns the cached
  // instance otherwise.
  template <typename T, typename... Args>
  std::remove_cv_t<T>* GetCustomState(Args... args);

 private:
  friend class PacketSequenceStateGeneration;

  using CustomStateArray = std::array<std::unique_ptr<CustomState>,
                                      std::tuple_size_v<CustomStateClasses>>;

  // Helper to find the index in a tuple of a given type. Lookups are done
  // ignoring cv qualifiers. If no index is found, the size of the tuple is
  // returned.
  //
  // ATTENTION: Duplicate types in the tuple will trigger a compiler error.
  template <typename Tuple, typename Type, size_t index = 0>
  static constexpr size_t FindUniqueType() {
    constexpr size_t kSize = std::tuple_size_v<Tuple>;
    if constexpr (index < kSize) {
      using TypeAtIndex = typename std::tuple_element<index, Tuple>::type;
      if constexpr (std::is_same_v<std::remove_cv_t<Type>,
                                   std::remove_cv_t<TypeAtIndex>>) {
        static_assert(FindUniqueType<Tuple, Type, index + 1>() == kSize,
                      "Duplicate types.");
        return index;
      } else {
        return FindUniqueType<Tuple, Type, index + 1>();
      }
    } else {
      return kSize;
    }
  }

  // Add an interned message into the table. Called by
  // `PacketSequenceStateBuilder` (via `PacketSequenceStateGeneration`).
  void InternMessage(uint32_t field_id, TraceBlobView message);

  // Called from `PacketSequenceStateGeneration::OnPacketLoss` to clear any
  // CustomState whose `ClearOnPacketLoss()` opts in.
  void ClearCustomStatesForPacketLoss() {
    for (auto& cs : custom_state_) {
      if (cs && cs->ClearOnPacketLoss())
        cs.reset();
    }
  }

  TraceProcessorContext* const context_;
  InternedFieldMap interned_data_;
  TrackEventThreadDescriptor thread_descriptor_;
  CustomStateArray custom_state_;
};

template <typename T, typename... Args>
std::remove_cv_t<T>* IncrementalState::GetCustomState(Args... args) {
  constexpr size_t index = FindUniqueType<CustomStateClasses, T>();
  static_assert(index < std::tuple_size_v<CustomStateClasses>, "Not found");
  auto& ptr = custom_state_[index];
  if (PERFETTO_UNLIKELY(!ptr)) {
    if constexpr (std::is_void_v<typename CustomStateTraits<T>::Tracker>) {
      static_assert(sizeof...(args) == 0,
                    "This custom state does not take any arguments.");
      ptr.reset(new T(context_));
    } else {
      static_assert(sizeof...(args) == 1,
                    "This custom state takes exactly one argument.");
      using ArgType =
          std::decay_t<std::tuple_element_t<0, std::tuple<Args...>>>;
      using ExpectedType = typename CustomStateTraits<T>::Tracker*;
      static_assert(std::is_same_v<ArgType, ExpectedType>,
                    "Argument must be a pointer to the Tracker defined in "
                    "CustomStateTraits.");
      ptr.reset(new T(context_, std::forward<Args>(args)...));
    }
    ptr->set_incremental(this);
  }
  return static_cast<std::remove_cv_t<T>*>(ptr.get());
}

template <uint32_t FieldId, typename MessageType>
typename MessageType::Decoder* CustomState::LookupInternedMessage(
    uint64_t iid) {
  return incremental_->LookupInternedMessage<FieldId, MessageType>(iid);
}

template <typename T, typename... Args>
std::remove_cv_t<T>* CustomState::GetCustomState(Args... args) {
  return incremental_->GetCustomState<T>(std::forward<Args>(args)...);
}

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_INCREMENTAL_STATE_H_
