/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/android_sdk/perfetto_sdk_for_jni/tracing_sdk.h"

#include <sys/types.h>

#include <cstdarg>
#include <mutex>

#include "perfetto/public/abi/producer_abi.h"
#include "perfetto/public/data_source.h"
#include "perfetto/public/pb_msg.h"
#include "perfetto/public/producer.h"
#include "perfetto/public/protos/trace/trace_packet.pzc.h"
#include "perfetto/public/protos/trace/track_event/track_descriptor.pzc.h"
#include "perfetto/public/protos/trace/track_event/track_event.pzc.h"
#include "perfetto/public/te_macros.h"
#include "perfetto/public/track_event.h"

namespace perfetto {
namespace sdk_for_jni {
void register_perfetto(bool backend_in_process) {
  static std::once_flag registration;
  std::call_once(registration, [backend_in_process]() {
    struct PerfettoProducerInitArgs args = PERFETTO_PRODUCER_INIT_ARGS_INIT();
    args.backends = backend_in_process ? PERFETTO_BACKEND_IN_PROCESS
                                       : PERFETTO_BACKEND_SYSTEM;
    args.shmem_size_hint_kb = 1024;
    PerfettoProducerInit(args);
    PerfettoTeInit();
  });
}

void trace_event(int type,
                 const PerfettoTeCategory* perfettoTeCategory,
                 const char* name,
                 Extra* extra) {
  bool enabled = PERFETTO_UNLIKELY(PERFETTO_ATOMIC_LOAD_EXPLICIT(
      perfettoTeCategory->enabled, PERFETTO_MEMORY_ORDER_RELAXED));
  if (enabled) {
    extra->push_extra(nullptr);
    PerfettoTeHlEmitImpl(perfettoTeCategory->impl, type,
                         type == PERFETTO_TE_TYPE_COUNTER ? nullptr : name,
                         extra->get());
    extra->clear_extras();
  }
}

// Max interned-string proto fields per event (matches the Java-side cap).
static constexpr int kMaxInternedFields = 16;

// Emits TrackDescriptor packets for any track levels not yet seen on this
// instance's sequence. Each level's parent is the level above it (the first
// level's parent is the root uuid the Java side derived the chain from).
static void emit_unseen_track_descriptors(struct PerfettoTeLlIterator* ctx,
                                          int32_t track_count,
                                          const uint64_t* track_uuids,
                                          const uint64_t* track_parent_uuids,
                                          const char* const* track_names,
                                          bool track_name_static,
                                          bool track_is_counter) {
  for (int32_t i = 0; i < track_count; i++) {
    if (PerfettoTeLlTrackSeen(ctx->impl.incr, track_uuids[i])) {
      continue;
    }
    struct PerfettoDsRootTracePacket desc_packet;
    PerfettoTeLlPacketBegin(ctx, &desc_packet);
    perfetto_protos_TracePacket_set_sequence_flags(
        &desc_packet.msg,
        perfetto_protos_TracePacket_SEQ_NEEDS_INCREMENTAL_STATE);
    struct perfetto_protos_TrackDescriptor desc;
    perfetto_protos_TracePacket_begin_track_descriptor(&desc_packet.msg, &desc);
    // The leaf is a counter track when requested; ancestors are always named.
    if (track_is_counter && i == track_count - 1) {
      PerfettoTeCounterTrackFillDesc(&desc, track_names[i],
                                     track_parent_uuids[i], track_uuids[i],
                                     track_name_static);
    } else {
      PerfettoTeNamedTrackFillDesc(&desc, track_names[i], /*id=*/0,
                                   track_parent_uuids[i], track_uuids[i],
                                   track_name_static);
    }
    perfetto_protos_TracePacket_end_track_descriptor(&desc_packet.msg, &desc);
    PerfettoTeLlPacketEnd(ctx, &desc_packet);
  }
}

void emit_track_event(const PerfettoTeCategory* cat,
                      int32_t type,
                      const char* name,
                      const void* body,
                      size_t body_size,
                      bool set_track_uuid,
                      uint64_t leaf_track_uuid,
                      int32_t track_count,
                      const uint64_t* track_uuids,
                      const uint64_t* track_parent_uuids,
                      const char* const* track_names,
                      bool track_name_static,
                      bool track_is_counter,
                      int32_t interned_count,
                      const int32_t* interned_field_ids,
                      const int32_t* interned_type_ids,
                      const char* const* interned_strs) {
  bool enabled = PERFETTO_UNLIKELY(PERFETTO_ATOMIC_LOAD_EXPLICIT(
      cat->enabled, PERFETTO_MEMORY_ORDER_RELAXED));
  if (!enabled) {
    return;
  }

  // SLICE_END and COUNTER events carry no name, matching the HL path.
  const char* event_name =
      (type == PERFETTO_TE_TYPE_SLICE_END || type == PERFETTO_TE_TYPE_COUNTER)
          ? nullptr
          : name;

  // The LL ABI takes a non-const category; it only reads `impl`/`enabled`.
  PerfettoTeCategory* mut_cat = const_cast<PerfettoTeCategory*>(cat);
  struct PerfettoTeTimestamp timestamp = PerfettoTeGetTimestamp();

  for (struct PerfettoTeLlIterator ctx =
           PerfettoTeLlBeginSlowPath(mut_cat, timestamp);
       ctx.impl.ds.tracer != nullptr;
       PerfettoTeLlNext(mut_cat, timestamp, &ctx)) {
    if (track_count > 0) {
      emit_unseen_track_descriptors(&ctx, track_count, track_uuids,
                                    track_parent_uuids, track_names,
                                    track_name_static, track_is_counter);
    }

    struct PerfettoDsRootTracePacket trace_packet;
    PerfettoTeLlPacketBegin(&ctx, &trace_packet);
    PerfettoTeLlWriteTimestamp(&trace_packet.msg, &timestamp);
    perfetto_protos_TracePacket_set_sequence_flags(
        &trace_packet.msg,
        perfetto_protos_TracePacket_SEQ_NEEDS_INCREMENTAL_STATE);

    // Interned-string proto fields (addFieldWithInterning): intern per instance
    // and remember the iids to reference from the track_event below. Interned
    // strings use the standard { iid = 1, name = 2 } entry layout.
    uint64_t name_iid = 0;
    uint64_t interned_iids[kMaxInternedFields];
    {
      struct PerfettoTeLlInternContext intern_ctx;
      PerfettoTeLlInternContextInit(&intern_ctx, ctx.impl.incr,
                                    &trace_packet.msg);
      PerfettoTeLlInternRegisteredCat(&intern_ctx, mut_cat);
      if (event_name) {
        name_iid = PerfettoTeLlInternEventName(&intern_ctx, event_name);
      }
      for (int32_t i = 0; i < interned_count; i++) {
        const char* str = interned_strs[i];
        bool seen;
        interned_iids[i] = PerfettoTeLlIntern(ctx.impl.incr, interned_type_ids[i],
                                              str, strlen(str), &seen);
        if (!seen) {
          PerfettoTeLlInternContextStartIfNeeded(&intern_ctx);
          struct PerfettoPbMsg entry;
          PerfettoPbMsgBeginNested(&intern_ctx.interned.msg, &entry,
                                   interned_type_ids[i]);
          PerfettoPbMsgAppendType0Field(&entry, /*iid=*/1, interned_iids[i]);
          PerfettoPbMsgAppendCStrField(&entry, /*name=*/2, str);
          PerfettoPbMsgEndNested(&intern_ctx.interned.msg);
        }
      }
      PerfettoTeLlInternContextDestroy(&intern_ctx);
    }

    {
      struct perfetto_protos_TrackEvent te_msg;
      perfetto_protos_TracePacket_begin_track_event(&trace_packet.msg, &te_msg);
      perfetto_protos_TrackEvent_set_type(
          &te_msg, static_cast<enum perfetto_protos_TrackEvent_Type>(type));
      PerfettoTeLlWriteRegisteredCat(&te_msg, mut_cat);
      if (event_name) {
        PerfettoTeLlWriteInternedEventName(&te_msg, name_iid);
      }
      if (set_track_uuid) {
        perfetto_protos_TrackEvent_set_track_uuid(&te_msg, leaf_track_uuid);
      }
      // Append the Java-encoded TrackEvent body (debug args, non-interned proto
      // fields, ...) verbatim into the track_event submessage.
      if (body_size) {
        PerfettoPbMsgAppendBytes(&te_msg.msg,
                                 static_cast<const uint8_t*>(body), body_size);
      }
      // Reference the interned strings by iid (top-level track_event fields).
      for (int32_t i = 0; i < interned_count; i++) {
        PerfettoPbMsgAppendType0Field(&te_msg.msg, interned_field_ids[i],
                                      interned_iids[i]);
      }
      perfetto_protos_TracePacket_end_track_event(&trace_packet.msg, &te_msg);
    }

    PerfettoTeLlPacketEnd(&ctx, &trace_packet);
  }
}

uint64_t get_process_track_uuid() {
  return PerfettoTeProcessTrackUuid();
}

uint64_t get_thread_track_uuid(pid_t tid) {
  // Cating a signed pid_t to unsigned
  return PerfettoTeProcessTrackUuid() ^ PERFETTO_STATIC_CAST(uint64_t, tid);
}

Extra::Extra() {}

void Extra::push_extra(PerfettoTeHlExtra* ptr) {
  extras_.push_back(ptr);
}

void Extra::pop_extra() {
  extras_.pop_back();
}

void Extra::clear_extras() {
  extras_.clear();
}

void Extra::delete_extra(Extra* ptr) {
  delete ptr;
}

Category::Category(const std::string& name) : Category(name, {}) {}

Category::Category(const std::string& name,
                   const std::vector<std::string>& tags)
    : category_({&perfetto_atomic_false, {}, {}, 0}), name_(name), tags_(tags) {
  for (const auto& tag : tags_) {
    tags_data_.push_back(tag.data());
  }
}

Category::~Category() {
  unregister_category();
}

void Category::register_category() {
  if (category_.impl)
    return;

  category_.desc = {name_.c_str(), name_.c_str(), tags_data_.data(),
                    tags_data_.size()};

  PerfettoTeCategoryRegister(&category_);
  PerfettoTePublishCategories();
}

void Category::unregister_category() {
  if (!category_.impl)
    return;

  PerfettoTeCategoryUnregister(&category_);
  PerfettoTePublishCategories();
}

bool Category::is_category_enabled() {
  return PERFETTO_UNLIKELY(PERFETTO_ATOMIC_LOAD_EXPLICIT(
      (category_).enabled, PERFETTO_MEMORY_ORDER_RELAXED));
}

void Category::delete_category(Category* ptr) {
  delete ptr;
}

Flow::Flow() : flow_{} {}

void Flow::set_process_flow(uint64_t id) {
  flow_.header.type = PERFETTO_TE_HL_EXTRA_TYPE_FLOW;
  PerfettoTeFlow ret = PerfettoTeProcessScopedFlow(id);
  flow_.id = ret.id;
}

void Flow::set_process_terminating_flow(uint64_t id) {
  flow_.header.type = PERFETTO_TE_HL_EXTRA_TYPE_TERMINATING_FLOW;
  PerfettoTeFlow ret = PerfettoTeProcessScopedFlow(id);
  flow_.id = ret.id;
}

void Flow::delete_flow(Flow* ptr) {
  delete ptr;
}

NamedTrack::NamedTrack(uint64_t id,
                       uint64_t parent_uuid,
                       const std::string& name,
                       bool is_name_static)
    : name_(name),
      track_{{PERFETTO_TE_HL_EXTRA_TYPE_NAMED_TRACK},
             name_.data(),
             id,
             parent_uuid,
             is_name_static} {}

void NamedTrack::delete_track(NamedTrack* ptr) {
  delete ptr;
}

RegisteredTrack::RegisteredTrack(uint64_t id,
                                 uint64_t parent_uuid,
                                 const std::string& name,
                                 bool is_counter,
                                 bool is_name_static)
    : registered_track_{},
      track_{{PERFETTO_TE_HL_EXTRA_TYPE_REGISTERED_TRACK},
             &registered_track_.impl},
      name_(name),
      id_(id),
      parent_uuid_(parent_uuid),
      is_counter_(is_counter),
      is_name_static_(is_name_static) {
  register_track();
}

RegisteredTrack::~RegisteredTrack() {
  unregister_track();
}

void RegisteredTrack::register_track() {
  if (registered_track_.impl.descriptor)
    return;

  if (is_counter_) {
    PerfettoTeCounterTrackRegister(&registered_track_, name_.data(),
                                   parent_uuid_, is_name_static_);
  } else {
    PerfettoTeNamedTrackRegister(&registered_track_, name_.data(), id_,
                                 parent_uuid_, is_name_static_);
  }
}

void RegisteredTrack::unregister_track() {
  if (!registered_track_.impl.descriptor)
    return;
  PerfettoTeRegisteredTrackUnregister(&registered_track_);
}

void RegisteredTrack::delete_track(RegisteredTrack* ptr) {
  delete ptr;
}

Proto::Proto() : proto_({{PERFETTO_TE_HL_EXTRA_TYPE_PROTO_FIELDS}, nullptr}) {}

void Proto::add_field(PerfettoTeHlProtoField* ptr) {
  if (!fields_.empty()) {
    fields_.pop_back();
  }

  fields_.push_back(ptr);
  fields_.push_back(nullptr);
  proto_.fields = fields_.data();
}

void Proto::clear_fields() {
  fields_.clear();
  proto_.fields = nullptr;
}

void Proto::delete_proto(Proto* ptr) {
  delete ptr;
}

ProtoFieldNested::ProtoFieldNested()
    : field_({{PERFETTO_TE_HL_PROTO_TYPE_NESTED, 0}, nullptr}) {}

void ProtoFieldNested::add_field(PerfettoTeHlProtoField* ptr) {
  if (!fields_.empty()) {
    fields_.pop_back();
  }

  fields_.push_back(ptr);
  fields_.push_back(nullptr);
  field_.fields = fields_.data();
}

void ProtoFieldNested::set_id(uint32_t id) {
  fields_.clear();
  field_.header.id = id;
  field_.fields = nullptr;
}

void ProtoFieldNested::delete_field(ProtoFieldNested* ptr) {
  delete ptr;
}

Session::Session(bool is_backend_in_process, void* buf, size_t len) {
  session_ = PerfettoTracingSessionCreate(is_backend_in_process
                                              ? PERFETTO_BACKEND_IN_PROCESS
                                              : PERFETTO_BACKEND_SYSTEM);

  PerfettoTracingSessionSetup(session_, buf, len);

  PerfettoTracingSessionStartBlocking(session_);
}

Session::~Session() {
  PerfettoTracingSessionStopBlocking(session_);
  PerfettoTracingSessionDestroy(session_);
}

bool Session::FlushBlocking(uint32_t timeout_ms) {
  return PerfettoTracingSessionFlushBlocking(session_, timeout_ms);
}

void Session::StopBlocking() {
  PerfettoTracingSessionStopBlocking(session_);
}

std::vector<uint8_t> Session::ReadBlocking() {
  std::vector<uint8_t> data;
  PerfettoTracingSessionReadTraceBlocking(
      session_,
      [](struct PerfettoTracingSessionImpl*, const void* trace_data,
         size_t size, bool, void* user_arg) {
        auto& dst = *static_cast<std::vector<uint8_t>*>(user_arg);
        auto* src = static_cast<const uint8_t*>(trace_data);
        dst.insert(dst.end(), src, src + size);
      },
      &data);
  return data;
}

void Session::delete_session(Session* ptr) {
  delete ptr;
}

void activate_trigger(const char* name, uint32_t ttl_ms) {
  const char* names[] = {name, nullptr};
  PerfettoProducerActivateTriggers(names, ttl_ms);
}
}  // namespace sdk_for_jni
}  // namespace perfetto
