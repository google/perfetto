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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PACKET_SEQUENCE_STATE_GENERATION_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PACKET_SEQUENCE_STATE_GENERATION_H_

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <tuple>
#include <type_traits>
#include <unordered_map>

#include "perfetto/public/compiler.h"
#include "perfetto/trace_processor/ref_counted.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/util/interned_message_view.h"

#include "protos/perfetto/trace/trace_packet_defaults.pbzero.h"
#include "protos/perfetto/trace/track_event/track_event.pbzero.h"

namespace perfetto {
namespace trace_processor {

using InternedMessageMap =
    std::unordered_map<uint64_t /*iid*/, InternedMessageView>;
using InternedFieldMap =
    std::unordered_map<uint32_t /*field_id*/, InternedMessageMap>;

class PacketSequenceState;
class TraceProcessorContext;

class StackProfileSequenceState;
class ProfilePacketSequenceState;
class V8SequenceState;

using InternedDataTrackers = std::tuple<StackProfileSequenceState,
                                        ProfilePacketSequenceState,
                                        V8SequenceState>;

class PacketSequenceStateGeneration : public RefCounted {
 public:
  // Base class to add custom sequence state. This state is keep per sequence
  // and per incremental state interval, that is, each time incremental state is
  // reset a new instance is created but not each time `TracePacketDefaults` are
  // updated. Note that this means that different
  // `PacketSequenceStateGeneration` instances might point to the same
  // `InternedDataTracker` (because they only differ in their
  // `TracePacketDefaults`).
  //
  // ATTENTION: You should not create instances of these classes yourself but
  // use the `PacketSequenceStateGeneration::GetOrCreate<>' method instead.
  class InternedDataTracker : public RefCounted {
   public:
    virtual ~InternedDataTracker();

   protected:
    template <uint32_t FieldId, typename MessageType>
    typename MessageType::Decoder* LookupInternedMessage(uint64_t iid) {
      return generation_->LookupInternedMessage<FieldId, MessageType>(iid);
    }

    InternedMessageView* GetInternedMessageView(uint32_t field_id,
                                                uint64_t iid) {
      return generation_->GetInternedMessageView(field_id, iid);
    }

    template <typename T>
    std::remove_cv_t<T>* GetOrCreate() {
      return generation_->GetOrCreate<T>();
    }

    PacketSequenceState* state() const { return generation_->state(); }

   private:
    friend PacketSequenceStateGeneration;
    // Called when the a new generation is created as a result of
    // `TracePacketDefaults` being updated.
    void set_generation(PacketSequenceStateGeneration* generation) {
      generation_ = generation;
    }

    // Note: A `InternedDataTracker` instance can be linked to multiple
    // `PacketSequenceStateGeneration` instances (when there are multiple
    // `TracePacketDefaults` in the same interning context). `generation_` will
    // point to the latest one. We keep this member private to prevent misuse /
    // confusion around this fact. Instead subclasses should access the public
    // methods of this class to get any interned data.
    PacketSequenceStateGeneration* generation_ = nullptr;
  };

  // Returns |nullptr| if the message with the given |iid| was not found (also
  // records a stat in this case).
  template <uint32_t FieldId, typename MessageType>
  typename MessageType::Decoder* LookupInternedMessage(uint64_t iid);

  InternedMessageView* GetInternedMessageView(uint32_t field_id, uint64_t iid);
  // Returns |nullptr| if no defaults were set.
  InternedMessageView* GetTracePacketDefaultsView() {
    if (!trace_packet_defaults_)
      return nullptr;
    return &trace_packet_defaults_.value();
  }

  // Returns |nullptr| if no defaults were set.
  protos::pbzero::TracePacketDefaults::Decoder* GetTracePacketDefaults() {
    InternedMessageView* view = GetTracePacketDefaultsView();
    if (!view)
      return nullptr;
    return view->GetOrCreateDecoder<protos::pbzero::TracePacketDefaults>();
  }

  // Returns |nullptr| if no TrackEventDefaults were set.
  protos::pbzero::TrackEventDefaults::Decoder* GetTrackEventDefaults() {
    auto* packet_defaults_view = GetTracePacketDefaultsView();
    if (packet_defaults_view) {
      auto* track_event_defaults_view =
          packet_defaults_view
              ->GetOrCreateSubmessageView<protos::pbzero::TracePacketDefaults,
                                          protos::pbzero::TracePacketDefaults::
                                              kTrackEventDefaultsFieldNumber>();
      if (track_event_defaults_view) {
        return track_event_defaults_view
            ->GetOrCreateDecoder<protos::pbzero::TrackEventDefaults>();
      }
    }
    return nullptr;
  }

  PacketSequenceState* state() const { return state_; }
  size_t generation_index() const { return generation_index_; }

  // Extension point for custom sequence state. To add new per sequence state
  // just subclass ´PacketSequenceStateGeneration´ and get your sequence bound
  // instance by calling this method.
  template <typename T>
  std::remove_cv_t<T>* GetOrCreate();

 private:
  friend class PacketSequenceState;

  // Helper to find the index in a tuple of a given type. Lookups are done
  // ignoring cv qualifiers. If no index is found size of the tuple is returned.
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

  PacketSequenceStateGeneration(PacketSequenceState* state,
                                size_t generation_index)
      : state_(state), generation_index_(generation_index) {}

  PacketSequenceStateGeneration(PacketSequenceState* state,
                                size_t generation_index,
                                PacketSequenceStateGeneration* prev_gen,
                                TraceBlobView defaults);

  TraceProcessorContext* GetContext() const;

  void InternMessage(uint32_t field_id, TraceBlobView message);

  void SetTracePacketDefaults(TraceBlobView defaults) {
    // Defaults should only be set once per generation.
    PERFETTO_DCHECK(!trace_packet_defaults_);
    trace_packet_defaults_ = InternedMessageView(std::move(defaults));
  }

  PacketSequenceState* state_;
  size_t generation_index_;
  InternedFieldMap interned_data_;
  std::optional<InternedMessageView> trace_packet_defaults_;
  std::array<RefPtr<InternedDataTracker>,
             std::tuple_size_v<InternedDataTrackers>>
      trackers_;
};

template <typename T>
std::remove_cv_t<T>* PacketSequenceStateGeneration::GetOrCreate() {
  constexpr size_t index = FindUniqueType<InternedDataTrackers, T>();
  static_assert(index < std::tuple_size_v<InternedDataTrackers>, "Not found");
  auto& ptr = trackers_[index];
  if (PERFETTO_UNLIKELY(ptr.get() == nullptr)) {
    ptr.reset(new T(GetContext()));
    ptr->set_generation(this);
  }

  return static_cast<std::remove_cv_t<T>*>(ptr.get());
}

template <uint32_t FieldId, typename MessageType>
typename MessageType::Decoder*
PacketSequenceStateGeneration::LookupInternedMessage(uint64_t iid) {
  auto* interned_message_view = GetInternedMessageView(FieldId, iid);
  if (!interned_message_view)
    return nullptr;

  return interned_message_view->template GetOrCreateDecoder<MessageType>();
}

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PACKET_SEQUENCE_STATE_GENERATION_H_
