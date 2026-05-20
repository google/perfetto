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

#include "src/android_sdk/jni/macros.h"
#include "src/android_sdk/jni/string_buffer.h"
#include "src/android_sdk/nativehelper/JNIHelp.h"
#include "src/android_sdk/perfetto_sdk_for_jni/tracing_sdk.h"

namespace perfetto {
namespace jni {

// Deepest track hierarchy passed in one emit. Deeper chains are clamped (a
// pathological case; real hierarchies are a handful of levels).
static constexpr jint kMaxTrackLevels = 16;

template <typename T>
inline static T* toPointer(jlong ptr) {
  return reinterpret_cast<T*>(static_cast<uintptr_t>(ptr));
}

// Copies the first `len` bytes of `body` into `stack_buf` (fast path), or via
// GetByteArrayElements for oversized bodies (returns the pointer to release in
// `*heap_out`). Returns the data pointer.
static const uint8_t* CopyBody(JNIEnv* env,
                               jbyteArray body,
                               jint len,
                               uint8_t* stack_buf,
                               jint stack_size,
                               jbyte** heap_out) {
  *heap_out = nullptr;
  if (len <= 0) {
    return nullptr;
  }
  if (len <= stack_size) {
    env->GetByteArrayRegion(body, 0, len, reinterpret_cast<jbyte*>(stack_buf));
    return stack_buf;
  }
  *heap_out = env->GetByteArrayElements(body, nullptr);
  return reinterpret_cast<const uint8_t*>(*heap_out);
}

// Shared emit. `name` and any track names are converted via the thread-local
// StringBuffer (GetStringRegion fast path: no Java-heap object, no native
// malloc); the body and the track chain are copied onto the stack. All
// conversions are allocation-free. Track arrays are only read when track_count
// or set_track_uuid require them.
static void emit(JNIEnv* env,
                 jint type,
                 jlong cat_ptr,
                 jstring name,
                 jbyteArray body,
                 jint body_len,
                 bool set_track_uuid,
                 jlong leaf_track_uuid,
                 jint track_count,
                 jlongArray track_uuids,
                 jlongArray track_parent_uuids,
                 jobjectArray track_names,
                 bool track_name_static,
                 bool track_is_counter) {
  auto* category = toPointer<sdk_for_jni::Category>(cat_ptr);
  std::string_view name_view = StringBuffer::utf16_to_ascii(env, name);

  uint64_t uuids[kMaxTrackLevels];
  uint64_t parent_uuids[kMaxTrackLevels];
  const char* names[kMaxTrackLevels];
  jint count = 0;
  if (track_count > 0) {
    count = track_count < kMaxTrackLevels ? track_count : kMaxTrackLevels;
    env->GetLongArrayRegion(track_uuids, 0, count,
                            reinterpret_cast<jlong*>(uuids));
    env->GetLongArrayRegion(track_parent_uuids, 0, count,
                            reinterpret_cast<jlong*>(parent_uuids));
    for (jint i = 0; i < count; i++) {
      jstring tn =
          static_cast<jstring>(env->GetObjectArrayElement(track_names, i));
      names[i] = StringBuffer::utf16_to_ascii(env, tn).data();
      env->DeleteLocalRef(tn);
    }
  }

  constexpr jint kStackBufSize = 4096;
  uint8_t stack_buf[kStackBufSize];
  jbyte* heap = nullptr;
  const uint8_t* data =
      CopyBody(env, body, body_len, stack_buf, kStackBufSize, &heap);

  sdk_for_jni::emit_track_event(
      category->get(), type, name_view.data(), data,
      body_len > 0 ? static_cast<size_t>(body_len) : 0, set_track_uuid,
      static_cast<uint64_t>(leaf_track_uuid), count, uuids, parent_uuids, names,
      track_name_static, track_is_counter);

  if (heap) {
    env->ReleaseByteArrayElements(body, heap, JNI_ABORT);
  }
  StringBuffer::reset();
}

// Common path: event on the sequence default track. No track arrays.
static void dev_perfetto_sdk_PerfettoEvent_native_emit(JNIEnv* env,
                                                       jclass,
                                                       jint type,
                                                       jlong cat_ptr,
                                                       jstring name,
                                                       jbyteArray body,
                                                       jint body_len) {
  emit(env, type, cat_ptr, name, body, body_len, /*set_track_uuid=*/false,
       /*leaf_track_uuid=*/0, /*track_count=*/0, nullptr, nullptr, nullptr,
       /*track_name_static=*/false, /*track_is_counter=*/false);
}

// Track path: event attached to a (possibly nested) track.
static void dev_perfetto_sdk_PerfettoEvent_native_emit_on_track(
    JNIEnv* env,
    jclass,
    jint type,
    jlong cat_ptr,
    jstring name,
    jbyteArray body,
    jint body_len,
    jlong leaf_track_uuid,
    jint track_count,
    jlongArray track_uuids,
    jlongArray track_parent_uuids,
    jobjectArray track_names,
    jboolean track_name_static,
    jboolean track_is_counter) {
  emit(env, type, cat_ptr, name, body, body_len, /*set_track_uuid=*/true,
       leaf_track_uuid, track_count, track_uuids, track_parent_uuids,
       track_names, track_name_static == JNI_TRUE, track_is_counter == JNI_TRUE);
}

static const JNINativeMethod gEventMethods[] = {
    {"native_emit", "(IJLjava/lang/String;[BI)V",
     (void*)dev_perfetto_sdk_PerfettoEvent_native_emit},
    {"native_emit_on_track",
     "(IJLjava/lang/String;[BIJI[J[J[Ljava/lang/String;ZZ)V",
     (void*)dev_perfetto_sdk_PerfettoEvent_native_emit_on_track},
};

int register_dev_perfetto_sdk_PerfettoEvent(JNIEnv* env) {
  int res = jniRegisterNativeMethods(
      env, TO_MAYBE_JAR_JAR_CLASS_NAME("dev/perfetto/sdk/PerfettoEvent"),
      gEventMethods, NELEM(gEventMethods));
  LOG_ALWAYS_FATAL_IF(res < 0,
                      "Unable to register PerfettoEvent native methods.");
  return 0;
}

}  // namespace jni
}  // namespace perfetto
