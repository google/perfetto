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

#ifndef SRC_ANDROID_SDK_JNI_REGISTER_NATIVES_H_
#define SRC_ANDROID_SDK_JNI_REGISTER_NATIVES_H_

#include <android/log.h>
#include <jni.h>

#include <cstddef>

#include "perfetto/base/compiler.h"
#include "src/android_sdk/nativehelper/scoped_local_ref.h"

namespace perfetto {
namespace jni {

// A Java class and the native methods to bind to it.
struct NativeClass {
  const char* name;
  const JNINativeMethod* methods;
  int num_methods;
};

// Registers |methods| on |class_name| without ever aborting the process.
//
// Unlike jniRegisterNativeMethods() (which calls __android_log_assert on any
// failure), this logs an error, clears any pending Java exception and returns
// false. This matters because these registrations run from JNI_OnLoad /
// JNI_OnLoad_Perfetto: in the ART static integration they run during runtime
// startup of every process (including zygote), so an abort there turns into a
// device bootloop. Callers must propagate the failure (e.g. by returning
// JNI_ERR from JNI_OnLoad) so that tracing is cleanly disabled instead.
PERFETTO_WARN_UNUSED_RESULT inline bool RegisterNativeMethodsMayFail(
    JNIEnv* env,
    const char* class_name,
    const JNINativeMethod* methods,
    int num_methods) {
  ScopedLocalRef clazz(env, env->FindClass(class_name));
  if (clazz.get() == nullptr) {
    env->ExceptionClear();
    __android_log_print(ANDROID_LOG_ERROR, "PerfettoJNI",
                        "Unable to find class '%s' for native registration",
                        class_name);
    return false;
  }
  if (env->RegisterNatives(clazz.get(), methods, num_methods) != JNI_OK) {
    env->ExceptionClear();
    __android_log_print(ANDROID_LOG_ERROR, "PerfettoJNI",
                        "Unable to register native methods for class '%s'",
                        class_name);
    return false;
  }
  return true;
}

// Unregisters native methods from |class_name| without throwing or asserting.
inline void UnregisterNativeMethods(JNIEnv* env, const char* class_name) {
  ScopedLocalRef clazz(env, env->FindClass(class_name));
  if (clazz.get() == nullptr) {
    env->ExceptionClear();
    return;
  }
  env->UnregisterNatives(clazz.get());
  env->ExceptionClear();
}

// Registers every class in |classes|. Attempts all of them, even after a
// failure, so that every failing class gets logged.
PERFETTO_WARN_UNUSED_RESULT inline bool RegisterNativeClassesMayFail(
    JNIEnv* env,
    const NativeClass* classes,
    size_t num_classes) {
  bool ok = true;
  for (size_t i = 0; i < num_classes; i++) {
    ok &= RegisterNativeMethodsMayFail(env, classes[i].name, classes[i].methods,
                                       classes[i].num_methods);
  }
  return ok;
}

// Unregisters every class in |classes|.
inline void UnregisterNativeClasses(JNIEnv* env,
                                    const NativeClass* classes,
                                    size_t num_classes) {
  for (size_t i = 0; i < num_classes; i++) {
    UnregisterNativeMethods(env, classes[i].name);
  }
}

}  // namespace jni
}  // namespace perfetto

#endif  // SRC_ANDROID_SDK_JNI_REGISTER_NATIVES_H_
