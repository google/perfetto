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

#include "src/android_sdk/jni/dev_perfetto_sdk_PerfettoDataSource.h"

#include <jni.h>

#include <cstdint>

#include "perfetto/base/build_config.h"
#include "perfetto/public/abi/atomic.h"
#include "perfetto/public/data_source.h"
#include "perfetto/public/stream_writer.h"
#include "src/android_sdk/jni/macros.h"
#include "src/android_sdk/nativehelper/JNIHelp.h"
#include "src/android_sdk/nativehelper/scoped_utf_chars.h"

namespace perfetto {
namespace jni {
namespace {

// Per-data-source registration state, passed to the callbacks as user_arg.
struct DsState {
  jobject java_ds_global_ref;
  JavaVM* jvm;
  // Number of active instances. Lifecycle callbacks for one data source are
  // serialized on the muxer thread, so a plain int is enough. The Java fast
  // path is enabled while this is > 0 (the C SDK `enabled` atomic isn't yet
  // cleared during the last instance's OnStop, so we track this ourselves).
  int active_instances;
  jmethodID on_enabled_changed_mid;
  jmethodID on_setup_mid;
  jmethodID on_start_mid;
  jmethodID on_stop_mid;
  jmethodID on_flush_mid;
};

// Per-instance incremental state: remembers whether it was (re)created or
// cleared since Java last checked, so Java knows to re-emit interned data.
struct IncrState {
  bool was_cleared;
};

template <typename T>
inline T* toPointer(jlong ptr) {
  return reinterpret_cast<T*>(static_cast<uintptr_t>(ptr));
}

// Lifecycle callbacks fire on the producer/muxer thread, which is not a JVM
// thread, so attach it for the duration of the upcall.
struct ScopedEnv {
  JavaVM* jvm;
  JNIEnv* env = nullptr;
  bool attached = false;

  explicit ScopedEnv(JavaVM* vm) : jvm(vm) {
    if (jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) ==
        JNI_OK) {
      return;
    }
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
    if (jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
      attached = true;
    }
#else
    if (jvm->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr) ==
        JNI_OK) {
      attached = true;
    }
#endif
  }

  ~ScopedEnv() {
    if (attached) {
      jvm->DetachCurrentThread();
    }
  }
};

void* OnSetup(struct PerfettoDsImpl*,
              PerfettoDsInstanceIndex inst_id,
              void* ds_config,
              size_t ds_config_size,
              void* user_arg,
              struct PerfettoDsOnSetupArgs*) {
  auto* state = static_cast<DsState*>(user_arg);
  ScopedEnv scoped(state->jvm);
  if (!scoped.env) {
    return nullptr;
  }
  JNIEnv* env = scoped.env;
  jbyteArray config = env->NewByteArray(static_cast<jsize>(ds_config_size));
  if (config) {
    env->SetByteArrayRegion(config, 0, static_cast<jsize>(ds_config_size),
                            reinterpret_cast<const jbyte*>(ds_config));
    env->CallVoidMethod(state->java_ds_global_ref, state->on_setup_mid,
                        static_cast<jint>(inst_id), config);
    env->DeleteLocalRef(config);
  }
  return nullptr;
}

void OnStart(struct PerfettoDsImpl*,
             PerfettoDsInstanceIndex inst_id,
             void* user_arg,
             void*,
             struct PerfettoDsOnStartArgs*) {
  auto* state = static_cast<DsState*>(user_arg);
  state->active_instances++;
  ScopedEnv scoped(state->jvm);
  if (!scoped.env) {
    return;
  }
  scoped.env->CallVoidMethod(state->java_ds_global_ref,
                             state->on_enabled_changed_mid, JNI_TRUE);
  scoped.env->CallVoidMethod(state->java_ds_global_ref, state->on_start_mid,
                             static_cast<jint>(inst_id));
}

void OnStop(struct PerfettoDsImpl*,
            PerfettoDsInstanceIndex inst_id,
            void* user_arg,
            void*,
            struct PerfettoDsOnStopArgs*) {
  auto* state = static_cast<DsState*>(user_arg);
  bool last = (--state->active_instances <= 0);
  ScopedEnv scoped(state->jvm);
  if (!scoped.env) {
    return;
  }
  // Disable the Java fast path only once the last instance has stopped.
  if (last) {
    scoped.env->CallVoidMethod(state->java_ds_global_ref,
                               state->on_enabled_changed_mid, JNI_FALSE);
  }
  scoped.env->CallVoidMethod(state->java_ds_global_ref, state->on_stop_mid,
                             static_cast<jint>(inst_id));
}

void OnFlush(struct PerfettoDsImpl*,
             PerfettoDsInstanceIndex inst_id,
             void* user_arg,
             void*,
             struct PerfettoDsOnFlushArgs*) {
  auto* state = static_cast<DsState*>(user_arg);
  ScopedEnv scoped(state->jvm);
  if (!scoped.env) {
    return;
  }
  scoped.env->CallVoidMethod(state->java_ds_global_ref, state->on_flush_mid,
                             static_cast<jint>(inst_id));
}

void* OnCreateIncr(struct PerfettoDsImpl*,
                   PerfettoDsInstanceIndex,
                   struct PerfettoDsTracerImpl*,
                   void*) {
  auto* incr = new IncrState();
  incr->was_cleared = true;  // a fresh state counts as "cleared"
  return incr;
}

void OnDeleteIncr(void* obj) {
  delete static_cast<IncrState*>(obj);
}

bool OnClearIncr(void* obj, void*) {
  static_cast<IncrState*>(obj)->was_cleared = true;
  return true;
}

// The work behind the @CriticalNative methods, shared by the host and ART entry
// points. None of these touch JVM state.

// Reads the already-encoded packet straight from the off-heap address and
// appends it to every active instance -- no allocation, no JNI accessors.
void WritePacket(jlong ds_ptr, jlong addr, jint len) {
  if (len <= 0) {
    return;
  }
  auto* ds = toPointer<struct PerfettoDs>(ds_ptr);
  const uint8_t* data = toPointer<const uint8_t>(addr);
  for (struct PerfettoDsTracerIterator it = PerfettoDsTraceIterateBegin(ds);
       it.impl.tracer != nullptr; PerfettoDsTraceIterateNext(ds, &it)) {
    struct PerfettoStreamWriter writer =
        PerfettoDsTracerImplPacketBegin(it.impl.tracer);
    PerfettoStreamWriterAppendBytes(&writer, data, static_cast<size_t>(len));
    PerfettoDsTracerImplPacketEnd(it.impl.tracer, &writer);
  }
}

bool CheckAnyIncrementalStateCleared(jlong ds_ptr) {
  auto* ds = toPointer<struct PerfettoDs>(ds_ptr);
  bool any_cleared = false;
  for (struct PerfettoDsTracerIterator it = PerfettoDsTraceIterateBegin(ds);
       it.impl.tracer != nullptr; PerfettoDsTraceIterateNext(ds, &it)) {
    auto* incr =
        static_cast<IncrState*>(PerfettoDsGetIncrementalState(ds, &it));
    if (incr && incr->was_cleared) {
      any_cleared = true;
      incr->was_cleared = false;
    }
  }
  return any_cleared;
}

void FlushAll(jlong ds_ptr) {
  auto* ds = toPointer<struct PerfettoDs>(ds_ptr);
  for (struct PerfettoDsTracerIterator it = PerfettoDsTraceIterateBegin(ds);
       it.impl.tracer != nullptr; PerfettoDsTraceIterateNext(ds, &it)) {
    PerfettoDsTracerImplFlush(it.impl.tracer, nullptr, nullptr);
  }
}

// ============================================================================
// JNI methods
// ============================================================================

jlong nativeRegister(JNIEnv* env, jclass, jobject java_ds, jstring name) {
  ScopedUtfChars name_chars(env, name);
  if (name_chars.c_str() == nullptr) {
    return 0;
  }

  auto* state = new DsState();
  state->java_ds_global_ref = env->NewGlobalRef(java_ds);
  env->GetJavaVM(&state->jvm);
  jclass cls = env->GetObjectClass(java_ds);
  state->on_enabled_changed_mid =
      env->GetMethodID(cls, "onEnabledChanged", "(Z)V");
  state->on_setup_mid = env->GetMethodID(cls, "onSetup", "(I[B)V");
  state->on_start_mid = env->GetMethodID(cls, "onStart", "(I)V");
  state->on_stop_mid = env->GetMethodID(cls, "onStop", "(I)V");
  state->on_flush_mid = env->GetMethodID(cls, "onFlush", "(I)V");

  auto* ds = new PerfettoDs PERFETTO_DS_INIT();
  struct PerfettoDsParams params = PerfettoDsParamsDefault();
  params.on_setup_cb = OnSetup;
  params.on_start_cb = OnStart;
  params.on_stop_cb = OnStop;
  params.on_flush_cb = OnFlush;
  params.on_create_incr_cb = OnCreateIncr;
  params.on_delete_incr_cb = OnDeleteIncr;
  params.on_clear_incr_cb = OnClearIncr;
  params.user_arg = state;

  if (!PerfettoDsRegister(ds, name_chars.c_str(), params)) {
    env->DeleteGlobalRef(state->java_ds_global_ref);
    delete state;
    delete ds;
    return 0;
  }
  return static_cast<jlong>(reinterpret_cast<uintptr_t>(ds));
}

// nativeCheckAnyIncrementalStateCleared / nativeWritePacket / nativeFlush are
// @CriticalNative on ART (primitives only, no JVM state), so the C ABI omits
// JNIEnv/jclass there; host JVMs ignore the annotation and call with the
// standard signature. nativeRegister stays a normal native (it needs JNIEnv).
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
jboolean nativeCheckAnyIncrementalStateCleared(jlong ds_ptr) {
  return CheckAnyIncrementalStateCleared(ds_ptr) ? JNI_TRUE : JNI_FALSE;
}
void nativeWritePacket(jlong ds_ptr, jlong addr, jint len) {
  WritePacket(ds_ptr, addr, len);
}
void nativeFlush(jlong ds_ptr) {
  FlushAll(ds_ptr);
}
#else
jboolean nativeCheckAnyIncrementalStateCleared(JNIEnv*, jclass, jlong ds_ptr) {
  return CheckAnyIncrementalStateCleared(ds_ptr) ? JNI_TRUE : JNI_FALSE;
}
void nativeWritePacket(JNIEnv*, jclass, jlong ds_ptr, jlong addr, jint len) {
  WritePacket(ds_ptr, addr, len);
}
void nativeFlush(JNIEnv*, jclass, jlong ds_ptr) {
  FlushAll(ds_ptr);
}
#endif

const JNINativeMethod gMethods[] = {
    {"nativeRegister",
     "(Ldev/perfetto/sdk/PerfettoDataSource;Ljava/lang/String;)J",
     (void*)nativeRegister},
    {"nativeCheckAnyIncrementalStateCleared", "(J)Z",
     (void*)nativeCheckAnyIncrementalStateCleared},
    {"nativeWritePacket", "(JJI)V", (void*)nativeWritePacket},
    {"nativeFlush", "(J)V", (void*)nativeFlush},
};

}  // namespace

int register_dev_perfetto_sdk_PerfettoDataSource(JNIEnv* env) {
  int res = jniRegisterNativeMethods(
      env, TO_MAYBE_JAR_JAR_CLASS_NAME("dev/perfetto/sdk/PerfettoDataSource"),
      gMethods, NELEM(gMethods));
  LOG_ALWAYS_FATAL_IF(res < 0,
                      "Unable to register PerfettoDataSource native methods.");
  return 0;
}

}  // namespace jni
}  // namespace perfetto
