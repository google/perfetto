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

#ifndef SRC_ANDROID_SDK_JNI_PERFETTO_APP_FILTER_H_
#define SRC_ANDROID_SDK_JNI_PERFETTO_APP_FILTER_H_

#include <string>
#include <string_view>

namespace perfetto {
namespace jni {

// Returns true if the calling process is allowed to register with Perfetto.
// On host builds, this always returns true.
// On Android devices, registration is allowed if:
// 1. `is_backend_in_process` is true (in-process backend).
// 2. The process is system_server or in the app allowlist (YouTube,
//    Google Search App, System UI).
// 3. The sysprop `persist.debug.perfetto.sdk_enable_tracing_all_apps` is set to
//    "1".
bool IsAppRegistrationAllowed(bool is_backend_in_process);

// Helper to check if a process cmdline matches allowlisted apps or
// system_server. Visible for testing.
bool IsAllowlistedProcess(std::string_view cmdline);

// Returns the calling process's command line (argv[0]), memoized across calls.
const std::string& GetProcessCmdline();

}  // namespace jni
}  // namespace perfetto

#endif  // SRC_ANDROID_SDK_JNI_PERFETTO_APP_FILTER_H_
