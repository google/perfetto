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
#include "src/trace_processor/importers/proto/statsd_module.h"

#include "perfetto/ext/base/string_utils.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/perfetto/trace/statsd/statsd_atom.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "src/trace_processor/importers/common/async_track_set_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/proto/packet_sequence_state.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/util/descriptors.h"

#include "src/trace_processor/importers/proto/atoms.descriptor.h"

namespace perfetto {
namespace trace_processor {
namespace {

constexpr const char* kAtomProtoName = ".android.os.statsd.Atom";

using BoundInserter = ArgsTracker::BoundInserter;

class InserterDelegate : public util::ProtoToArgsParser::Delegate {
 public:
  InserterDelegate(BoundInserter& inserter, TraceStorage& storage)
      : inserter_(inserter), storage_(storage) {}
  ~InserterDelegate() override = default;

  using Key = util::ProtoToArgsParser::Key;

  void AddInteger(const Key& key, int64_t value) override {
    StringId flat_key_id =
        storage_.InternString(base::StringView(key.flat_key));
    StringId key_id = storage_.InternString(base::StringView(key.key));
    Variadic variadic_val = Variadic::Integer(value);
    inserter_.AddArg(flat_key_id, key_id, variadic_val);
  }

  void AddUnsignedInteger(const Key& key, uint64_t value) override {
    StringId flat_key_id =
        storage_.InternString(base::StringView(key.flat_key));
    StringId key_id = storage_.InternString(base::StringView(key.key));
    Variadic variadic_val = Variadic::UnsignedInteger(value);
    inserter_.AddArg(flat_key_id, key_id, variadic_val);
  }

  void AddString(const Key& key, const protozero::ConstChars& value) override {
    StringId flat_key_id =
        storage_.InternString(base::StringView(key.flat_key));
    StringId key_id = storage_.InternString(base::StringView(key.key));
    Variadic variadic_val = Variadic::String(storage_.InternString(value));
    inserter_.AddArg(flat_key_id, key_id, variadic_val);
  }

  void AddString(const Key& key, const std::string& value) override {
    StringId flat_key_id =
        storage_.InternString(base::StringView(key.flat_key));
    StringId key_id = storage_.InternString(base::StringView(key.key));
    Variadic variadic_val =
        Variadic::String(storage_.InternString(base::StringView(value)));
    inserter_.AddArg(flat_key_id, key_id, variadic_val);
  }

  void AddDouble(const Key& key, double value) override {
    StringId flat_key_id =
        storage_.InternString(base::StringView(key.flat_key));
    StringId key_id = storage_.InternString(base::StringView(key.key));
    Variadic variadic_val = Variadic::Real(value);
    inserter_.AddArg(flat_key_id, key_id, variadic_val);
  }

  void AddPointer(const Key& key, const void* value) override {
    StringId flat_key_id =
        storage_.InternString(base::StringView(key.flat_key));
    StringId key_id = storage_.InternString(base::StringView(key.key));
    Variadic variadic_val =
        Variadic::Pointer(reinterpret_cast<uintptr_t>(value));
    inserter_.AddArg(flat_key_id, key_id, variadic_val);
  }

  void AddBoolean(const Key& key, bool value) override {
    StringId flat_key_id =
        storage_.InternString(base::StringView(key.flat_key));
    StringId key_id = storage_.InternString(base::StringView(key.key));
    Variadic variadic_val = Variadic::Boolean(value);
    inserter_.AddArg(flat_key_id, key_id, variadic_val);
  }

  bool AddJson(const Key&, const protozero::ConstChars&) override {
    PERFETTO_FATAL("Unexpected JSON value when parsing statsd data");
  }

  void AddNull(const Key& key) override {
    StringId flat_key_id =
        storage_.InternString(base::StringView(key.flat_key));
    StringId key_id = storage_.InternString(base::StringView(key.key));
    Variadic variadic_val = Variadic::Null();
    inserter_.AddArg(flat_key_id, key_id, variadic_val);
  }

  size_t GetArrayEntryIndex(const std::string& array_key) override {
    base::ignore_result(array_key);
    return 0;
  }

  size_t IncrementArrayEntryIndex(const std::string& array_key) override {
    base::ignore_result(array_key);
    return 0;
  }

  PacketSequenceStateGeneration* seq_state() override { return nullptr; }

 protected:
  InternedMessageView* GetInternedMessageView(uint32_t field_id,
                                              uint64_t iid) override {
    base::ignore_result(field_id);
    base::ignore_result(iid);
    return nullptr;
  }

 private:
  BoundInserter& inserter_;
  TraceStorage& storage_;
};

}  // namespace

using perfetto::protos::pbzero::StatsdAtom;
using perfetto::protos::pbzero::TracePacket;

PoolAndDescriptor::PoolAndDescriptor(const uint8_t* data,
                                     size_t size,
                                     const char* name) {
  pool_.AddFromFileDescriptorSet(data, size);
  std::optional<uint32_t> opt_idx = pool_.FindDescriptorIdx(name);
  if (opt_idx.has_value()) {
    descriptor_ = &pool_.descriptors()[opt_idx.value()];
  }
}

PoolAndDescriptor::~PoolAndDescriptor() = default;

StatsdModule::StatsdModule(TraceProcessorContext* context)
    : context_(context),
      pool_(kAtomsDescriptor.data(), kAtomsDescriptor.size(), kAtomProtoName),
      args_parser_(*(pool_.pool())) {
  RegisterForField(TracePacket::kStatsdAtomFieldNumber, context);
}

StatsdModule::~StatsdModule() = default;

ModuleResult StatsdModule::TokenizePacket(const TracePacket::Decoder& decoder,
                                          TraceBlobView* /*packet*/,
                                          int64_t packet_timestamp,
                                          PacketSequenceState* state,
                                          uint32_t field_id) {
  if (field_id != TracePacket::kStatsdAtomFieldNumber) {
    return ModuleResult::Ignored();
  }
  const auto& atoms_wrapper = StatsdAtom::Decoder(decoder.statsd_atom());
  auto it_timestamps = atoms_wrapper.timestamp_nanos();
  for (auto it = atoms_wrapper.atom(); it; ++it) {
    int64_t atom_timestamp;

    if (it_timestamps) {
      atom_timestamp = *it_timestamps++;
    } else {
      context_->storage->IncrementStats(stats::atom_timestamp_missing);
      atom_timestamp = packet_timestamp;
    }

    protozero::HeapBuffered<TracePacket> forged;

    forged->set_timestamp(static_cast<uint64_t>(atom_timestamp));

    auto* statsd = forged->set_statsd_atom();
    statsd->AppendBytes(StatsdAtom::kAtomFieldNumber, (*it).data, (*it).size);

    std::vector<uint8_t> vec = forged.SerializeAsArray();
    TraceBlob blob = TraceBlob::CopyFrom(vec.data(), vec.size());

    context_->sorter->PushTracePacket(atom_timestamp,
                                      state->current_generation(),
                                      TraceBlobView(std::move(blob)));
  }

  return ModuleResult::Handled();
}

void StatsdModule::ParseTracePacketData(const TracePacket::Decoder& decoder,
                                        int64_t ts,
                                        const TracePacketData&,
                                        uint32_t field_id) {
  if (field_id != TracePacket::kStatsdAtomFieldNumber) {
    return;
  }
  const auto& atoms_wrapper = StatsdAtom::Decoder(decoder.statsd_atom());
  auto it = atoms_wrapper.atom();
  // There should be exactly one atom per trace packet at this point.
  // If not something has gone wrong in tokenization above.
  PERFETTO_CHECK(it);
  ParseAtom(ts, *it++);
  PERFETTO_CHECK(!it);
}

void StatsdModule::ParseAtom(int64_t ts, protozero::ConstBytes nested_bytes) {
  // nested_bytes is an Atom proto. We (deliberately) don't generate
  // decoding code for every kind of atom (or the parent Atom proto)
  // and instead use the descriptor to parse the args/name.

  // Atom is a giant oneof of all the possible 'kinds' of atom so here
  // we use the protozero decoder implementation to grab the first
  // field id which we we use to look up the field name:
  protozero::ProtoDecoder nested_decoder(nested_bytes);
  protozero::Field field = nested_decoder.ReadField();
  uint32_t nested_field_id = 0;
  if (field.valid()) {
    nested_field_id = field.id();
  }
  StringId atom_name = GetAtomName(nested_field_id);

  AsyncTrackSetTracker::TrackSetId track_set = InternAsyncTrackSetId();
  TrackId track = context_->async_track_set_tracker->Scoped(track_set, ts, 0);
  std::optional<SliceId> opt_slice =
      context_->slice_tracker->Scoped(ts, track, kNullStringId, atom_name, 0);
  if (!opt_slice) {
    return;
  }
  SliceId slice = opt_slice.value();
  auto inserter = context_->args_tracker->AddArgsTo(slice);
  InserterDelegate delegate(inserter, *context_->storage.get());
  base::Status result = args_parser_.ParseMessage(
      nested_bytes, kAtomProtoName, nullptr /* parse all fields */, delegate);
  if (!result.ok()) {
    PERFETTO_ELOG("%s", result.c_message());
    context_->storage->IncrementStats(stats::atom_unknown);
  }
}

StringId StatsdModule::GetAtomName(uint32_t atom_field_id) {
  StringId* cached_name = atom_names_.Find(atom_field_id);
  if (cached_name == nullptr) {
    if (pool_.descriptor() == nullptr) {
      context_->storage->IncrementStats(stats::atom_unknown);
      return context_->storage->InternString("Could not load atom descriptor");
    }

    const auto& fields = pool_.descriptor()->fields();
    const auto& field_it = fields.find(atom_field_id);
    if (field_it == fields.end()) {
      context_->storage->IncrementStats(stats::atom_unknown);
      return context_->storage->InternString("Unknown atom");
    }

    const FieldDescriptor& field = field_it->second;
    StringId name =
        context_->storage->InternString(base::StringView(field.name()));
    atom_names_[atom_field_id] = name;
    return name;
  }
  return *cached_name;
}

AsyncTrackSetTracker::TrackSetId StatsdModule::InternAsyncTrackSetId() {
  if (!track_set_id_) {
    StringId name = context_->storage->InternString("Statsd Atoms");
    track_set_id_ =
        context_->async_track_set_tracker->InternGlobalTrackSet(name);
  }
  return track_set_id_.value();
}

}  // namespace trace_processor
}  // namespace perfetto
