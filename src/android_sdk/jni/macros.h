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

#ifndef SRC_ANDROID_SDK_JNI_MACROS_H_
#define SRC_ANDROID_SDK_JNI_MACROS_H_
#include <android/log.h>
#include <jni.h>
#include <string_view>

// JNI parameter shim for natives declared @CriticalNative on the Java
// side. On Android device, ART recognises the annotation and calls the
// function with no JNIEnv*/jclass prefix. On host VMs (notably Ravenwood,
// where the perfetto SDK also runs) the annotation is invisible and the
// standard JNI ABI is used; the host build is paired with a Java-source
// preprocessor that strips @CriticalNative entirely, so every method
// binds as a plain native and this macro adds the env+clazz prefix to
// keep the C signatures matching.
//
//   static jlong foo(PERFETTO_JNI_HOST_PARAMS) { ... }
//   static void  bar(PERFETTO_JNI_HOST_PARAMS_COMMA jlong x) { ... }
//
// Expands to nothing on device (so the on-wire binary is unchanged) and
// to JNIEnv*,jclass[,] everywhere else.
//
// Use __ANDROID__ directly rather than PERFETTO_BUILDFLAG so this
// leaf JNI header doesn't pull in perfetto/base/build_config.h.
#if defined(__ANDROID__)
#define PERFETTO_JNI_HOST_PARAMS
#define PERFETTO_JNI_HOST_PARAMS_COMMA
#else
#define PERFETTO_JNI_HOST_PARAMS JNIEnv*, jclass
#define PERFETTO_JNI_HOST_PARAMS_COMMA JNIEnv*, jclass,
#endif

// This is a very basic check to make sure we get the
// 'PERFETTO_JNI_JARJAR_PREFIX' in the form of 'com/android/internal/'.
constexpr static bool IsValidJavaPackage(const std::string_view str) {
  if (str.empty())
    return false;
  if (str.front() == '/')
    return false;
  if (str.back() != '/')
    return false;
  for (const char c : str) {
    if (!((c >= 'a' && c <= 'z') || c == '/')) {
      return false;
    }
  }
  return true;
}

#define TO_STRING1(x) #x
#define TO_STRING(x) TO_STRING1(x)

#ifdef PERFETTO_JNI_JARJAR_PREFIX
static_assert(IsValidJavaPackage(TO_STRING(PERFETTO_JNI_JARJAR_PREFIX)));
#define TO_MAYBE_JAR_JAR_CLASS_NAME(className) \
  TO_STRING(PERFETTO_JNI_JARJAR_PREFIX) className
#else
#define TO_MAYBE_JAR_JAR_CLASS_NAME(className) className
#endif

// Registers |methods| on |class_name| without ever aborting the process.
//
// Unlike jniRegisterNativeMethods() (which calls __android_log_assert on any
// failure), this logs an error, clears any pending Java exception and returns
// false. This matters because these registrations run from JNI_OnLoad /
// JNI_OnLoad_Perfetto: in the ART static integration they run during runtime
// startup of every process (including zygote), so an abort there turns into a
// device bootloop. Callers must propagate the failure (e.g. by returning
// JNI_ERR from JNI_OnLoad) so that tracing is cleanly disabled instead.
inline bool PerfettoRegisterNativeMethods(JNIEnv* env,
                                          const char* class_name,
                                          const JNINativeMethod* methods,
                                          int num_methods) {
  jclass clazz = env->FindClass(class_name);
  if (clazz == nullptr) {
    env->ExceptionClear();
    __android_log_print(ANDROID_LOG_ERROR, "PerfettoJNI",
                        "Unable to find class '%s' for native registration",
                        class_name);
    return false;
  }
  jint res = env->RegisterNatives(clazz, methods, num_methods);
  env->DeleteLocalRef(clazz);
  if (res != JNI_OK) {
    env->ExceptionClear();
    __android_log_print(ANDROID_LOG_ERROR, "PerfettoJNI",
                        "Unable to register native methods for class '%s'",
                        class_name);
    return false;
  }
  return true;
}

#endif  // SRC_ANDROID_SDK_JNI_MACROS_H_
