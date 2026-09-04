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

// Host shim for <sys/system_properties.h>. Provides the small surface used
// by perfetto SDK JNI sources so they compile cleanly on host.
// Only on the include path when libperfetto_jni is built for host via
// tools/run_android_sdk_host_test.

#ifndef SRC_ANDROID_SDK_JNI_HOST_STUBS_ANDROID_SYSTEM_PROPERTIES_H_
#define SRC_ANDROID_SDK_JNI_HOST_STUBS_ANDROID_SYSTEM_PROPERTIES_H_

#define PROP_VALUE_MAX 92

#ifdef __cplusplus
extern "C" {
#endif

static inline int __system_property_get(const char* name, char* value) {
  (void)name;
  if (value) {
    value[0] = '\0';
  }
  return 0;
}

#ifdef __cplusplus
}
#endif

#endif  // SRC_ANDROID_SDK_JNI_HOST_STUBS_ANDROID_SYSTEM_PROPERTIES_H_
