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

template <typename T>
inline static T* toPointer(jlong ptr) {
  return reinterpret_cast<T*>(static_cast<uintptr_t>(ptr));
}

// Emits a track event through the Low Level ABI path. `cat_ptr` is a pointer to
// the native sdk_for_jni::Category created by PerfettoTrace.Category.
//
// The name is converted via the thread-local StringBuffer (GetStringRegion fast
// path -- no Java-heap object and no native malloc, unlike GetStringUTFChars).
// `body`/`body_len`, when non-empty, is the Java-encoded TrackEvent body copied
// onto the stack. Both conversions are allocation-free.
static void dev_perfetto_sdk_PerfettoEvent_native_emit(JNIEnv* env,
                                                       jclass,
                                                       jint type,
                                                       jlong cat_ptr,
                                                       jstring name,
                                                       jbyteArray body,
                                                       jint body_len) {
  auto* category = toPointer<sdk_for_jni::Category>(cat_ptr);
  std::string_view name_view = StringBuffer::utf16_to_ascii(env, name);

  if (body_len <= 0) {
    sdk_for_jni::emit_track_event(category->get(), type, name_view.data(),
                                  nullptr, 0);
  } else {
    // The body is one trace event's worth of fields; it comfortably fits the
    // stack fast path. Copy out of the Java heap before touching the stream
    // writer (which may transition shared-memory chunks).
    constexpr jint kStackBufSize = 4096;
    uint8_t stack_buf[kStackBufSize];
    if (body_len <= kStackBufSize) {
      env->GetByteArrayRegion(body, 0, body_len,
                              reinterpret_cast<jbyte*>(stack_buf));
      sdk_for_jni::emit_track_event(category->get(), type, name_view.data(),
                                    stack_buf, static_cast<size_t>(body_len));
    } else {
      jbyte* heap = env->GetByteArrayElements(body, nullptr);
      sdk_for_jni::emit_track_event(category->get(), type, name_view.data(),
                                    heap, static_cast<size_t>(body_len));
      env->ReleaseByteArrayElements(body, heap, JNI_ABORT);
    }
  }

  StringBuffer::reset();
}

static const JNINativeMethod gEventMethods[] = {
    {"native_emit", "(IJLjava/lang/String;[BI)V",
     (void*)dev_perfetto_sdk_PerfettoEvent_native_emit},
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
