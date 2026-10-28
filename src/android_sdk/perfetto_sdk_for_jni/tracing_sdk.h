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

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "perfetto/public/abi/track_event_hl_abi.h"
#include "perfetto/public/tracing_session.h"
#include "perfetto/public/track_event.h"

/**
 * The objects declared here are intended to be managed by Java.
 * This means the Java Garbage Collector is responsible for freeing the
 * underlying native resources.
 *
 * The static methods prefixed with `delete_` are special. They are designed to
 * be invoked by Java through the `PerfettoNativeMemoryCleaner` when the
 * corresponding Java object becomes unreachable.  These methods act as
 * callbacks to ensure proper deallocation of native resources.
 * These callbacks must match the cleaner's `void (*)(void*)` signature.
 */
namespace perfetto {
namespace sdk_for_jni {
/**
 * @brief Initializes the global perfetto instance.
 * @param backend_in_process use in-process or system backend
 */
void register_perfetto(bool backend_in_process = false);

/**
 * @brief Represents extra data associated with a trace event.
 * This class manages a collection of PerfettoTeHlExtra pointers.
 */
class Extra;

/**
 * @brief Emits a trace event.
 * @param type The type of the event.
 * @param cat The category of the event.
 * @param name The name of the event.
 * @param extra Pointer to Extra data.
 */
void trace_event(int type,
                 const PerfettoTeCategory* cat,
                 const char* name,
                 Extra* extra);

/**
 * @brief Gets the process track UUID.
 */
uint64_t get_process_track_uuid();

/**
 * @brief Gets the thread track UUID for a given PID.
 */
uint64_t get_thread_track_uuid(pid_t tid);

/**
 * @brief Holder for all the other classes in the file.
 */
class Extra {
 public:
  Extra();
  Extra(const Extra&) = delete;
  Extra& operator=(const Extra&) = delete;
  void push_extra(PerfettoTeHlExtra* extra);
  void pop_extra();
  void clear_extras();
  static void delete_extra(void* ptr);

  PerfettoTeHlExtra* const* get() const { return extras_.data(); }

 private:
  // These PerfettoTeHlExtra pointers are really pointers to all the other
  // types of extras: Category, DebugArg, Counter etc. Those objects are
  // individually managed by Java.
  std::vector<PerfettoTeHlExtra*> extras_;
};

/**
 * @brief Represents a trace event category.
 */
class Category {
 public:
  explicit Category(const std::string& name);

  Category(const std::string& name, const std::vector<std::string>& tags);

  ~Category();

  Category(const Category&) = delete;
  Category& operator=(const Category&) = delete;

  void register_category();

  void unregister_category();

  bool is_category_enabled();

  static void delete_category(void* ptr);

  const PerfettoTeCategory* get() const { return &category_; }

 private:
  PerfettoTeCategory category_;
  const std::string name_;
  const std::vector<std::string> tags_;
  std::vector<const char*> tags_data_;
};

/**
 * @brief Represents one end of a flow between two events.
 */
class Flow {
 public:
  Flow();
  Flow(const Flow&) = delete;
  Flow& operator=(const Flow&) = delete;

  void set_process_flow(uint64_t id);
  void set_process_terminating_flow(uint64_t id);
  static void delete_flow(void* ptr);

  const PerfettoTeHlExtraFlow* get() const { return &flow_; }

 private:
  PerfettoTeHlExtraFlow flow_;
};

/**
 * @brief Represents a named track.
 */
class NamedTrack {
 public:
  NamedTrack(uint64_t id,
             uint64_t parent_uuid,
             const std::string& name,
             bool is_name_static);
  NamedTrack(const NamedTrack&) = delete;
  NamedTrack& operator=(const NamedTrack&) = delete;

  static void delete_track(void* ptr);

  const PerfettoTeHlExtraNamedTrack* get() const { return &track_; }

 private:
  const std::string name_;
  PerfettoTeHlExtraNamedTrack track_;
};

// Root scope of a nested-track chain. Values match PerfettoTrack (Java) and are
// passed verbatim over JNI.
enum class RootType : int {
  kGlobal = 0,
  kProcess = 1,
  kThread = 2,
};

// One named level of a nested-track chain. The merge key is a oneof: the
// string one if set, the integer one otherwise. The enum-like fields mirror
// the PERFETTO_TE_HL_* ABI constants.
struct NestedLevel {
  std::string name;
  uint64_t id;
  int32_t sibling_order_rank;
  uint32_t child_ordering;
  uint32_t sibling_merge_behavior;
  std::optional<std::string> sibling_merge_key_str;
  uint64_t sibling_merge_key_int;
};

/**
 * @brief A nested chain of named tracks (the HL NESTED_TRACKS extra).
 *
 * The chain is, outermost first: an optional root (process or thread; global
 * roots have none) followed by one named level per entry in `levels`. The
 * native HL path derives the per-level uuids and emits a descriptor for each
 * level.
 */
class NestedTracks {
 public:
  NestedTracks(RootType root_type, std::vector<NestedLevel> levels);
  NestedTracks(const NestedTracks&) = delete;
  NestedTracks& operator=(const NestedTracks&) = delete;

  static void delete_track(void* ptr);

  const PerfettoTeHlExtraNestedTracks* get() const { return &extra_; }

 private:
  // named_[i] borrows level i's name and merge key, ptrs_ point into
  // named_/root_; nothing is resized after construction, so the pointers and
  // the strings they reference stay valid for the object's lifetime.
  std::vector<NestedLevel> levels_;
  PerfettoTeHlNestedTrack root_;
  std::vector<PerfettoTeHlNestedTrackNamed> named_;
  std::vector<PerfettoTeHlNestedTrack*> ptrs_;
  PerfettoTeHlExtraNestedTracks extra_;
};

/**
 * @brief Represents a registered track.
 */
class RegisteredTrack {
 public:
  RegisteredTrack(uint64_t id,
                  uint64_t parent_uuid,
                  const std::string& name,
                  bool is_counter,
                  bool is_name_static_,
                  bool is_state);

  ~RegisteredTrack();

  RegisteredTrack(const RegisteredTrack&) = delete;
  RegisteredTrack& operator=(const RegisteredTrack&) = delete;

  void register_track();
  void unregister_track();
  static void delete_track(void* ptr);

  const PerfettoTeHlExtraRegisteredTrack* get() const { return &track_; }

 private:
  PerfettoTeRegisteredTrack registered_track_;
  PerfettoTeHlExtraRegisteredTrack track_;
  const std::string name_;
  const uint64_t id_;
  const uint64_t parent_uuid_;
  const bool is_counter_;
  const bool is_name_static_;
  const bool is_state_;
};

/**
 * @brief Represents a counter track event.
 */
class Counter {
 public:
  Counter() {}
  Counter(const Counter&) = delete;
  Counter& operator=(const Counter&) = delete;

  static void delete_counter(void* ptr) { delete static_cast<Counter*>(ptr); }

  PerfettoTeHlExtraCounterUnion* get() { return &counter_; }

 private:
  PerfettoTeHlExtraCounterUnion counter_;
};

/**
 * @brief A correlation id linking an event with others in the same logical
 * operation (TrackEvent's correlation_id / correlation_id_str).
 */
class CorrelationId {
 public:
  CorrelationId() : correlation_id_{} {}
  CorrelationId(const CorrelationId&) = delete;
  CorrelationId& operator=(const CorrelationId&) = delete;

  static void delete_correlation_id(void* ptr) {
    delete static_cast<CorrelationId*>(ptr);
  }

  const char* set_str(std::string_view val) {
    str_ = val;
    return str_.c_str();
  }
  PerfettoTeHlExtraCorrelationIdUnion* get() { return &correlation_id_; }

 private:
  PerfettoTeHlExtraCorrelationIdUnion correlation_id_;
  std::string str_;
};

/**
 * @brief Represents a debug argument for a trace event.
 */
class DebugArg {
 public:
  explicit DebugArg(const std::string& name) : name_(name) {}
  DebugArg(const DebugArg&) = delete;
  DebugArg& operator=(const DebugArg&) = delete;

  static void delete_arg(void* ptr) { delete static_cast<DebugArg*>(ptr); }

  const char* name() const { return name_.c_str(); }
  const char* set_value(std::string_view val) {
    value_ = val;
    return value_.c_str();
  }
  PerfettoTeHlExtraDebugArgUnion* get() { return &arg_; }

 private:
  PerfettoTeHlExtraDebugArgUnion arg_;
  const std::string name_;
  std::string value_;
};

class ProtoField {
 public:
  ProtoField() {}
  ProtoField(const ProtoField&) = delete;
  ProtoField& operator=(const ProtoField&) = delete;
  static void delete_field(void* ptr) { delete static_cast<ProtoField*>(ptr); }

  const char* set_str(std::string_view val) {
    str_ = val;
    return str_.c_str();
  }
  PerfettoTeHlProtoFieldUnion* get() { return &arg_; }

 private:
  PerfettoTeHlProtoFieldUnion arg_;
  std::string str_;
};

class ProtoFieldNested {
 public:
  ProtoFieldNested();
  ProtoFieldNested(const ProtoFieldNested&) = delete;
  ProtoFieldNested& operator=(const ProtoFieldNested&) = delete;

  void add_field(PerfettoTeHlProtoField* field);
  void set_id(uint32_t id);
  static void delete_field(void* ptr);

  const PerfettoTeHlProtoFieldNested* get() const { return &field_; }

 private:
  PerfettoTeHlProtoFieldNested field_;
  // These PerfettoTeHlProtoField pointers are really pointers to all the other
  // types of protos: PerfettoTeHlProtoFieldVarInt,
  // PerfettoTeHlProtoFieldVarInt, PerfettoTeHlProtoFieldVarInt,
  // PerfettoTeHlProtoFieldNested. Those objects are individually managed by
  // Java.
  std::vector<PerfettoTeHlProtoField*> fields_;
};

class Proto {
 public:
  Proto();
  Proto(const Proto&) = delete;
  Proto& operator=(const Proto&) = delete;

  void add_field(PerfettoTeHlProtoField* field);
  void clear_fields();
  static void delete_proto(void* ptr);

  const PerfettoTeHlExtraProtoFields* get() const { return &proto_; }

 private:
  PerfettoTeHlExtraProtoFields proto_;
  // These PerfettoTeHlProtoField pointers are really pointers to all the other
  // types of protos: PerfettoTeHlProtoFieldVarInt,
  // PerfettoTeHlProtoFieldVarInt, PerfettoTeHlProtoFieldVarInt,
  // PerfettoTeHlProtoFieldNested. Those objects are individually managed by
  // Java.
  std::vector<PerfettoTeHlProtoField*> fields_;
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

  static void delete_session(void* ptr);

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
