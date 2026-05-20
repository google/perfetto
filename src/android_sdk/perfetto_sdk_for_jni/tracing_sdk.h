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

#ifndef SRC_ANDROID_SDK_PERFETTO_SDK_FOR_JNI_TRACING_SDK_H_
#define SRC_ANDROID_SDK_PERFETTO_SDK_FOR_JNI_TRACING_SDK_H_

#include <stdint.h>

#include <sys/types.h>

#include <string>
#include <vector>

#include "perfetto/public/tracing_session.h"
#include "perfetto/public/track_event.h"

// Macro copied from
// https://source.corp.google.com/h/googleplex-android/platform/superproject/main/+/main:system/libbase/include/android-base/macros.h;l=45;drc=bd641075ad60ed703baf59f63a9153d96d96b98e
// A macro to disallow the copy constructor and operator= functions
// This must be placed in the private: declarations for a class.
#define DISALLOW_COPY_AND_ASSIGN(TypeName) \
  TypeName(const TypeName&) = delete;      \
  void operator=(const TypeName&) = delete

/**
 * The objects declared here are intended to be managed by Java.
 * This means the Java Garbage Collector is responsible for freeing the
 * underlying native resources.
 *
 * The static methods prefixed with `delete_` are special. They are designed to
 * be invoked by Java through the `NativeAllocationRegistry` when the
 * corresponding Java object becomes unreachable.  These methods act as
 * callbacks to ensure proper deallocation of native resources.
 */
namespace perfetto {
namespace sdk_for_jni {
/**
 * @brief Initializes the global perfetto instance.
 * @param backend_in_process use in-process or system backend
 */
void register_perfetto(bool backend_in_process = false);

/**
 * @brief Emits a track event through the Low Level track event ABI.
 *
 * Drives the public LL ABI (PerfettoTeLl*): it walks the active data source
 * instances and serializes the TrackEvent with protozero. The LL ABI keeps
 * ownership of the parts that must stay native -- category and event-name
 * interning, incremental-state resets (sequence defaults, clock snapshot,
 * thread/process descriptors) and per-instance fan-out.
 *
 * `body`/`body_size`, when non-empty, is an opaque, already-encoded sequence of
 * TrackEvent proto fields (debug annotations, flows, proto fields, ...)
 * produced on the Java side with ProtoWriter; it is appended verbatim into the
 * `track_event` submessage. Bare events pass an empty body.
 *
 * The event can be placed on a nested track. The track chain is passed
 * pre-resolved (uuids computed on the Java side): `track_uuids[i]` /
 * `track_parent_uuids[i]` / `track_names[i]` describe each named level from the
 * root down, `leaf_track_uuid` is the uuid the event is attached to. For each
 * level not yet seen on a sequence (PerfettoTeLlTrackSeen), its TrackDescriptor
 * is emitted once. `track_count == 0` means the event uses the default track.
 *
 * @param cat The (registered) category of the event.
 * @param type The PerfettoTeType of the event (slice begin/end, instant, ...).
 * @param name The event name. Ignored for SLICE_END and COUNTER events.
 * @param body Encoded TrackEvent field bytes to append, or nullptr.
 * @param body_size Size of `body` in bytes, or 0.
 * @param set_track_uuid Whether to attach the event to `leaf_track_uuid`
 *     (false leaves it on the sequence default track). True for any usingTrack.
 * @param leaf_track_uuid The uuid the event is attached to.
 * @param track_count Number of named track levels whose descriptors may need
 *     emitting (0 for a root track that already has a descriptor, e.g.
 * process).
 * @param track_uuids Per-level track uuids (length track_count).
 * @param track_parent_uuids Per-level parent uuids (length track_count).
 * @param track_names Per-level names (length track_count).
 * @param track_child_orderings Per-level child-ordering (0 = unset), length
 * count.
 * @param track_sibling_ranks Per-level sibling rank (0 = unset), length count.
 * @param track_name_static Whether track names are compile-time constants.
 * @param track_is_counter Whether the leaf level is a counter track.
 * @param interned_count Number of interned-string proto fields, or 0.
 * @param interned_field_ids Per-field track_event field ids (length count).
 * @param interned_type_ids Per-field InternedData type ids (length count).
 * @param interned_strs Per-field strings to intern (length count).
 */
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
                      const int32_t* track_child_orderings,
                      const int32_t* track_sibling_ranks,
                      bool track_name_static,
                      bool track_is_counter,
                      int32_t interned_count,
                      const int32_t* interned_field_ids,
                      const int32_t* interned_type_ids,
                      const char* const* interned_strs);

/**
 * @brief Gets the process track UUID.
 */
uint64_t get_process_track_uuid();

/**
 * @brief Gets the thread track UUID for a given PID.
 */
uint64_t get_thread_track_uuid(pid_t tid);

/**
 * @brief Represents a trace event category.
 */
class Category {
 public:
  explicit Category(const std::string& name);

  Category(const std::string& name, const std::vector<std::string>& tags);

  ~Category();

  void register_category();

  void unregister_category();

  bool is_category_enabled();

  static void delete_category(Category* category);

  const PerfettoTeCategory* get() const { return &category_; }

 private:
  DISALLOW_COPY_AND_ASSIGN(Category);
  PerfettoTeCategory category_;
  const std::string name_;
  const std::vector<std::string> tags_;
  std::vector<const char*> tags_data_;
};

class Session {
 public:
  Session(bool is_backend_in_process, void* buf, size_t len);
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  bool FlushBlocking(uint32_t timeout_ms);
  void StopBlocking();
  std::vector<uint8_t> ReadBlocking();

  static void delete_session(Session* session);

  struct PerfettoTracingSessionImpl* session_ = nullptr;
};

/**
 * @brief Activates a trigger.
 * @param name The name of the trigger.
 * @param ttl_ms The time-to-live of the trigger in milliseconds.
 */
void activate_trigger(const char* name, uint32_t ttl_ms);
}  // namespace sdk_for_jni
}  // namespace perfetto

#endif  // SRC_ANDROID_SDK_PERFETTO_SDK_FOR_JNI_TRACING_SDK_H_
