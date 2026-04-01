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

#include "src/java_datasource/jni/perfetto_datasource_jni.h"

#include <android/log.h>
#include <jni.h>
#include <string.h>

#include "perfetto/public/abi/atomic.h"
#include "perfetto/public/abi/data_source_abi.h"
#include "perfetto/public/abi/stream_writer_abi.h"
#include "perfetto/public/stream_writer.h"
#include "src/android_sdk/jni/macros.h"
#include "src/android_sdk/nativehelper/JNIHelp.h"
#include "src/android_sdk/nativehelper/scoped_utf_chars.h"

#define LOG_TAG "PerfettoDataSourceJNI"

namespace perfetto {
namespace jni {

namespace {

// Per-data-source registration state. Stored as the user_arg for callbacks.
struct DsState {
  struct PerfettoDsImpl* ds_impl;
  // Points to the native atomic enabled flag. True when any instance is active.
  // Used by OnStop to decide whether to disable the Java fast path.
  PERFETTO_ATOMIC(bool) * enabled_ptr;
  // Global ref to the Java PerfettoDataSource object.
  jobject java_ds_global_ref;
  // Cached JVM pointer for callbacks from arbitrary threads.
  JavaVM* jvm;
  // Cached method IDs.
  jmethodID on_enabled_changed_mid;
  jmethodID on_setup_mid;
  jmethodID on_start_mid;
  jmethodID on_stop_mid;
  jmethodID on_flush_mid;
};

// Per-instance incremental state. Tracks whether the state was cleared
// so Java can know when to re-emit InternedData.
struct IncrState {
  bool was_cleared;
};

JNIEnv* GetJNIEnv(JavaVM* jvm) {
  JNIEnv* env = nullptr;
  jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  return env;
}

// ============================================================================
// Data source callbacks
// ============================================================================

void* OnSetup(struct PerfettoDsImpl*,
              PerfettoDsInstanceIndex inst_id,
              void* ds_config,
              size_t ds_config_size,
              void* user_arg,
              struct PerfettoDsOnSetupArgs*) {
  auto* state = static_cast<DsState*>(user_arg);
  JNIEnv* env = GetJNIEnv(state->jvm);
  if (env) {
    jbyteArray config = env->NewByteArray(static_cast<jsize>(ds_config_size));
    if (config) {
      env->SetByteArrayRegion(config, 0, static_cast<jsize>(ds_config_size),
                              reinterpret_cast<const jbyte*>(ds_config));
      env->CallVoidMethod(state->java_ds_global_ref, state->on_setup_mid,
                          static_cast<jint>(inst_id), config);
      env->DeleteLocalRef(config);
    }
  }
  return nullptr;
}

void OnStart(struct PerfettoDsImpl*,
             PerfettoDsInstanceIndex inst_id,
             void* user_arg,
             void*,
             struct PerfettoDsOnStartArgs*) {
  auto* state = static_cast<DsState*>(user_arg);
  JNIEnv* env = GetJNIEnv(state->jvm);
  if (env) {
    env->CallVoidMethod(state->java_ds_global_ref,
                        state->on_enabled_changed_mid, JNI_TRUE);
    env->CallVoidMethod(state->java_ds_global_ref, state->on_start_mid,
                        static_cast<jint>(inst_id));
  }
}

void OnStop(struct PerfettoDsImpl*,
            PerfettoDsInstanceIndex inst_id,
            void* user_arg,
            void*,
            struct PerfettoDsOnStopArgs*) {
  auto* state = static_cast<DsState*>(user_arg);
  JNIEnv* env = GetJNIEnv(state->jvm);
  if (env) {
    // Only disable the Java fast path if no instances remain active.
    // The native enabled_ptr is an atomic bool managed by the C SDK that
    // is true when at least one instance is active.
    if (!PERFETTO_ATOMIC_LOAD_EXPLICIT(*state->enabled_ptr,
                                       PERFETTO_MEMORY_ORDER_RELAXED)) {
      env->CallVoidMethod(state->java_ds_global_ref,
                          state->on_enabled_changed_mid, JNI_FALSE);
    }
    env->CallVoidMethod(state->java_ds_global_ref, state->on_stop_mid,
                        static_cast<jint>(inst_id));
  }
}

void OnFlush(struct PerfettoDsImpl*,
             PerfettoDsInstanceIndex inst_id,
             void* user_arg,
             void*,
             struct PerfettoDsOnFlushArgs*) {
  auto* state = static_cast<DsState*>(user_arg);
  JNIEnv* env = GetJNIEnv(state->jvm);
  if (env) {
    env->CallVoidMethod(state->java_ds_global_ref, state->on_flush_mid,
                        static_cast<jint>(inst_id));
  }
}

// Incremental state callbacks.

void* OnCreateIncr(struct PerfettoDsImpl*,
                   PerfettoDsInstanceIndex,
                   struct PerfettoDsTracerImpl*,
                   void*) {
  auto* incr = new IncrState();
  incr->was_cleared = true;  // New state counts as "cleared"
  return incr;
}

void OnDeleteIncr(void* obj) {
  delete static_cast<IncrState*>(obj);
}

bool OnClearIncr(void* obj, void*) {
  auto* incr = static_cast<IncrState*>(obj);
  incr->was_cleared = true;
  return true;
}

}  // namespace

// ============================================================================
// JNI methods
// ============================================================================

static jlong nativeRegister(JNIEnv* env,
                            jclass,
                            jobject java_ds,
                            jstring name) {
  ScopedUtfChars name_chars(env, name);
  if (name_chars.c_str() == nullptr) {
    return 0;
  }

  struct PerfettoDsImpl* ds_impl = PerfettoDsImplCreate();

  auto* state = new DsState();
  state->ds_impl = ds_impl;
  state->java_ds_global_ref = env->NewGlobalRef(java_ds);
  env->GetJavaVM(&state->jvm);

  jclass cls = env->GetObjectClass(java_ds);
  state->on_enabled_changed_mid =
      env->GetMethodID(cls, "onEnabledChanged", "(Z)V");
  state->on_setup_mid = env->GetMethodID(cls, "onSetup", "(I[B)V");
  state->on_start_mid = env->GetMethodID(cls, "onStart", "(I)V");
  state->on_stop_mid = env->GetMethodID(cls, "onStop", "(I)V");
  state->on_flush_mid = env->GetMethodID(cls, "onFlush", "(I)V");

  PerfettoDsSetOnSetupCallback(ds_impl, OnSetup);
  PerfettoDsSetOnStartCallback(ds_impl, OnStart);
  PerfettoDsSetOnStopCallback(ds_impl, OnStop);
  PerfettoDsSetOnFlushCallback(ds_impl, OnFlush);
  PerfettoDsSetOnCreateIncr(ds_impl, OnCreateIncr);
  PerfettoDsSetOnDeleteIncr(ds_impl, OnDeleteIncr);
  PerfettoDsSetOnClearIncr(ds_impl, OnClearIncr);
  PerfettoDsSetCbUserArg(ds_impl, state);

  // Build the DataSourceDescriptor proto: field 1 (name) as a
  // length-delimited string. The length is encoded as a single-byte varint,
  // which limits names to 127 bytes.
  size_t name_len = strlen(name_chars.c_str());
  if (name_len > 127) {
    __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                        "Data source name too long (max 127): %s",
                        name_chars.c_str());
    env->DeleteGlobalRef(state->java_ds_global_ref);
    delete state;
    return 0;
  }
  uint8_t desc[256];
  uint8_t* p = desc;
  *p++ = (1 << 3) | 2;  // field 1, wire type 2
  *p++ = static_cast<uint8_t>(name_len);
  memcpy(p, name_chars.c_str(), name_len);
  p += name_len;
  size_t desc_size = static_cast<size_t>(p - desc);

  bool ok =
      PerfettoDsImplRegister(ds_impl, &state->enabled_ptr, desc, desc_size);
  if (!ok) {
    __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                        "Failed to register data source: %s",
                        name_chars.c_str());
    env->DeleteGlobalRef(state->java_ds_global_ref);
    delete state;
    return 0;
  }

  return static_cast<jlong>(reinterpret_cast<uintptr_t>(ds_impl));
}

static jboolean nativeCheckAnyIncrementalStateCleared(jlong ds_ptr) {
  auto* ds_impl =
      reinterpret_cast<struct PerfettoDsImpl*>(static_cast<uintptr_t>(ds_ptr));
  bool any_cleared = false;

  struct PerfettoDsImplTracerIterator it =
      PerfettoDsImplTraceIterateBegin(ds_impl);
  while (it.tracer) {
    auto* incr = static_cast<IncrState*>(
        PerfettoDsImplGetIncrementalState(ds_impl, it.tracer, it.inst_id));
    if (incr && incr->was_cleared) {
      any_cleared = true;
      incr->was_cleared = false;
    }
    PerfettoDsImplTraceIterateNext(ds_impl, &it);
  }

  return any_cleared ? JNI_TRUE : JNI_FALSE;
}

static void nativeWritePacketToAllInstances(JNIEnv* env,
                                            jclass,
                                            jlong ds_ptr,
                                            jbyteArray buf,
                                            jint len) {
  if (len <= 0) {
    return;
  }

  auto* ds_impl =
      reinterpret_cast<struct PerfettoDsImpl*>(static_cast<uintptr_t>(ds_ptr));

  // Copy Java byte[] to a stack buffer to avoid holding a JNI critical
  // region across stream writer calls (which may involve IPC for chunk
  // transitions).
  static constexpr size_t kStackBufSize = 4096;
  uint8_t stack_buf[kStackBufSize];
  uint8_t* data;
  bool heap_allocated = false;

  if (static_cast<size_t>(len) <= kStackBufSize) {
    data = stack_buf;
  } else {
    data = new uint8_t[static_cast<size_t>(len)];
    heap_allocated = true;
  }

  env->GetByteArrayRegion(buf, 0, len, reinterpret_cast<jbyte*>(data));

  // Iterate all active instances and write the packet to each.
  struct PerfettoDsImplTracerIterator it =
      PerfettoDsImplTraceIterateBegin(ds_impl);
  while (it.tracer) {
    struct PerfettoStreamWriter writer =
        PerfettoDsTracerImplPacketBegin(it.tracer);
    PerfettoStreamWriterAppendBytes(&writer, data, static_cast<size_t>(len));
    PerfettoDsTracerImplPacketEnd(it.tracer, &writer);
    PerfettoDsImplTraceIterateNext(ds_impl, &it);
  }

  if (heap_allocated) {
    delete[] data;
  }
}

static void nativeFlush(jlong ds_ptr) {
  auto* ds_impl =
      reinterpret_cast<struct PerfettoDsImpl*>(static_cast<uintptr_t>(ds_ptr));

  struct PerfettoDsImplTracerIterator it =
      PerfettoDsImplTraceIterateBegin(ds_impl);
  while (it.tracer) {
    PerfettoDsTracerImplFlush(it.tracer, nullptr, nullptr);
    PerfettoDsImplTraceIterateNext(ds_impl, &it);
  }
}

// ============================================================================
// JNI registration
// ============================================================================

static const JNINativeMethod gMethods[] = {
    {"nativeRegister",
     "(Ldev/perfetto/sdk/PerfettoDataSource;Ljava/lang/String;)J",
     reinterpret_cast<void*>(nativeRegister)},
    {"nativeCheckAnyIncrementalStateCleared", "(J)Z",
     reinterpret_cast<void*>(nativeCheckAnyIncrementalStateCleared)},
    {"nativeWritePacketToAllInstances", "(J[BI)V",
     reinterpret_cast<void*>(nativeWritePacketToAllInstances)},
    {"nativeFlush", "(J)V", reinterpret_cast<void*>(nativeFlush)},
};

int register_dev_perfetto_sdk_PerfettoDataSource(JNIEnv* env) {
  int res = jniRegisterNativeMethods(
      env, TO_MAYBE_JAR_JAR_CLASS_NAME("dev/perfetto/sdk/PerfettoDataSource"),
      gMethods, sizeof(gMethods) / sizeof(gMethods[0]));
  LOG_ALWAYS_FATAL_IF(res < 0,
                      "Unable to register PerfettoDataSource native methods.");
  return 0;
}

}  // namespace jni
}  // namespace perfetto

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
  JNIEnv* env;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
    return JNI_ERR;
  }
  perfetto::jni::register_dev_perfetto_sdk_PerfettoDataSource(env);
  return JNI_VERSION_1_6;
}
