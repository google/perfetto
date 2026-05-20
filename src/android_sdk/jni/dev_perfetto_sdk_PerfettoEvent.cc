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

#include "src/android_sdk/jni/dev_perfetto_sdk_PerfettoEvent.h"

#include <jni.h>

#include <cstdint>
#include <cstring>

#include "perfetto/base/build_config.h"
#include "src/android_sdk/jni/macros.h"
#include "src/android_sdk/nativehelper/JNIHelp.h"
#include "src/android_sdk/perfetto_sdk_for_jni/tracing_sdk.h"

namespace perfetto {
namespace jni {

// Deepest track hierarchy / most interned fields handled in one emit. Deeper
// inputs are clamped (pathological; real events use a handful).
static constexpr int32_t kMaxTrackLevels = 16;
static constexpr int32_t kMaxInternedFields = 16;

template <typename T>
inline static T* toPointer(jlong ptr) {
  return reinterpret_cast<T*>(static_cast<uintptr_t>(ptr));
}

// The frame is little-endian and only read on little-endian targets (every host
// JVM and Android ABI), so a plain memcpy reproduces the Java-written value.
static int32_t ReadI32(const uint8_t** p) {
  int32_t v;
  memcpy(&v, *p, sizeof(v));
  *p += sizeof(v);
  return v;
}

static uint64_t ReadU64(const uint8_t** p) {
  uint64_t v;
  memcpy(&v, *p, sizeof(v));
  *p += sizeof(v);
  return v;
}

// Reads a { len, bytes, NUL } string and returns a pointer to the bytes, which
// are NUL-terminated in place so they can be used directly as a C string. The
// cursor is advanced past the terminator.
static const char* ReadCStr(const uint8_t** p) {
  int32_t len = ReadI32(p);
  const char* s = reinterpret_cast<const char*>(*p);
  *p += len + 1;
  return s;
}

// Parses the off-heap buffer (protobuf body in [0, body_len), then the frame)
// and drives the LL emit. Touches no JVM state, so it is shared by the host and
// ART (@CriticalNative) entry points below.
static void EmitFromBuffer(jint type,
                           jlong cat_ptr,
                           jlong addr,
                           jint body_len,
                           jint /*frame_len*/) {
  auto* category = toPointer<sdk_for_jni::Category>(cat_ptr);
  const uint8_t* base = toPointer<const uint8_t>(addr);
  const void* body = base;
  size_t body_size = body_len > 0 ? static_cast<size_t>(body_len) : 0;

  const uint8_t* p = base + body_len;
  const char* name = ReadCStr(&p);
  uint8_t flags = *p++;
  bool set_track_uuid = flags & 1;
  bool track_is_counter = flags & 2;
  bool track_name_static = flags & 4;
  bool has_timestamp = flags & 8;
  struct PerfettoTeTimestamp explicit_ts;
  const struct PerfettoTeTimestamp* explicit_ts_ptr = nullptr;
  if (has_timestamp) {
    explicit_ts.clock_id = static_cast<uint32_t>(ReadI32(&p));
    explicit_ts.value = ReadU64(&p);
    explicit_ts_ptr = &explicit_ts;
  }
  uint64_t leaf_track_uuid = ReadU64(&p);

  uint64_t uuids[kMaxTrackLevels];
  uint64_t parent_uuids[kMaxTrackLevels];
  const char* names[kMaxTrackLevels];
  int32_t child_orderings[kMaxTrackLevels];
  int32_t sibling_ranks[kMaxTrackLevels];
  int32_t track_count = ReadI32(&p);
  int32_t stored_tracks = 0;
  for (int32_t i = 0; i < track_count; i++) {
    uint64_t uuid = ReadU64(&p);
    uint64_t parent = ReadU64(&p);
    uint8_t ordering = *p++;
    int32_t rank = ReadI32(&p);
    const char* tname = ReadCStr(&p);
    if (i < kMaxTrackLevels) {
      uuids[i] = uuid;
      parent_uuids[i] = parent;
      child_orderings[i] = ordering;
      sibling_ranks[i] = rank;
      names[i] = tname;
      stored_tracks++;
    }
  }

  int32_t ifield_ids[kMaxInternedFields];
  int32_t itype_ids[kMaxInternedFields];
  const char* istrs[kMaxInternedFields];
  int32_t interned_count = ReadI32(&p);
  int32_t stored_interned = 0;
  for (int32_t i = 0; i < interned_count; i++) {
    int32_t fid = ReadI32(&p);
    int32_t tid = ReadI32(&p);
    const char* s = ReadCStr(&p);
    if (i < kMaxInternedFields) {
      ifield_ids[i] = fid;
      itype_ids[i] = tid;
      istrs[i] = s;
      stored_interned++;
    }
  }

  sdk_for_jni::emit_track_event(
      category->get(), type, name, body, body_size, set_track_uuid,
      leaf_track_uuid, stored_tracks, uuids, parent_uuids, names,
      child_orderings, sibling_ranks, track_name_static, track_is_counter,
      stored_interned, ifield_ids, itype_ids, istrs, explicit_ts_ptr);
}

// native_emit is @CriticalNative on ART (no JNIEnv/jclass, primitives only).
// Host JVMs ignore the annotation and call it with the standard JNI signature,
// so the function ABI differs by platform; both forward to EmitFromBuffer.
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
static void dev_perfetto_sdk_PerfettoEvent_native_emit(jint type,
                                                       jlong cat_ptr,
                                                       jlong addr,
                                                       jint body_len,
                                                       jint frame_len) {
  EmitFromBuffer(type, cat_ptr, addr, body_len, frame_len);
}
#else
static void dev_perfetto_sdk_PerfettoEvent_native_emit(JNIEnv*,
                                                       jclass,
                                                       jint type,
                                                       jlong cat_ptr,
                                                       jlong addr,
                                                       jint body_len,
                                                       jint frame_len) {
  EmitFromBuffer(type, cat_ptr, addr, body_len, frame_len);
}
#endif

// Returns the stable native address of a direct ByteBuffer. Called once per
// buffer (and on growth), never on the hot path, so a normal @FastNative is
// fine.
static jlong dev_perfetto_sdk_EmitBuffer_nativeAddress(JNIEnv* env,
                                                       jclass,
                                                       jobject buffer) {
  return static_cast<jlong>(
      reinterpret_cast<uintptr_t>(env->GetDirectBufferAddress(buffer)));
}

static const JNINativeMethod gEventMethods[] = {
    {"native_emit", "(IJJII)V",
     (void*)dev_perfetto_sdk_PerfettoEvent_native_emit},
};

static const JNINativeMethod gBufferMethods[] = {
    {"nativeAddress", "(Ljava/nio/ByteBuffer;)J",
     (void*)dev_perfetto_sdk_EmitBuffer_nativeAddress},
};

int register_dev_perfetto_sdk_PerfettoEvent(JNIEnv* env) {
  int res = jniRegisterNativeMethods(
      env, TO_MAYBE_JAR_JAR_CLASS_NAME("dev/perfetto/sdk/PerfettoEvent"),
      gEventMethods, NELEM(gEventMethods));
  LOG_ALWAYS_FATAL_IF(res < 0,
                      "Unable to register PerfettoEvent native methods.");
  res = jniRegisterNativeMethods(
      env, TO_MAYBE_JAR_JAR_CLASS_NAME("dev/perfetto/sdk/EmitBuffer"),
      gBufferMethods, NELEM(gBufferMethods));
  LOG_ALWAYS_FATAL_IF(res < 0, "Unable to register EmitBuffer native methods.");
  return 0;
}

}  // namespace jni
}  // namespace perfetto
