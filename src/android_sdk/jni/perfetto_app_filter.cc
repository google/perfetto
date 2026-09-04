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

#include "src/android_sdk/jni/perfetto_app_filter.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <string_view>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace perfetto {
namespace jni {
namespace {

// Allowlist of package names permitted to register for Perfetto tracing on
// Android devices. Matches PERFETTO_TRACING_ALLOWLIST in
// frameworks/base/core/java/android/app/ActivityThread.java.
constexpr const char* kAllowlistedPackages[] = {
    "com.google.android.youtube",
    "com.google.android.googlequicksearchbox",
    "com.android.systemui",
};

constexpr const char* kSystemServer = "system_server";

#if defined(__ANDROID__)
constexpr const char* kSyspropEnableAll =
    "persist.debug.perfetto.sdk_enable_tracing_all_apps";

std::string ReadProcessCmdline() {
  int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return "";
  }
  char buf[512];
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0) {
    return "";
  }
  buf[n] = '\0';
  // /proc/self/cmdline arguments are separated by '\0'. The first argument
  // argv[0] ends at the first null byte.
  return std::string(buf);
}

bool IsSyspropOverrideEnabled() {
  char val[PROP_VALUE_MAX] = {};
  int len = __system_property_get(kSyspropEnableAll, val);
  return len == 1 && val[0] == '1';
}
#endif

}  // namespace

bool IsAllowlistedProcess(std::string_view cmdline) {
  if (cmdline.empty()) {
    return false;
  }

  // Multi-process apps append ":process_name" (e.g.
  // "com.google.android.youtube:sandboxed_process0"). Strip the suffix to
  // obtain the package name.
  std::string_view package_name = cmdline;
  size_t colon_pos = package_name.find(':');
  if (colon_pos != std::string_view::npos) {
    package_name = package_name.substr(0, colon_pos);
  }

  if (package_name == kSystemServer) {
    return true;
  }

  for (const char* allowlisted : kAllowlistedPackages) {
    if (package_name == allowlisted) {
      return true;
    }
  }

  return false;
}

bool IsAppRegistrationAllowed(bool is_backend_in_process) {
  // Always enable for in-process backend (used for tests and benchmarks).
  if (is_backend_in_process) {
    return true;
  }

  // Always enable on host environments.
#if !defined(__ANDROID__)
  return true;
#else
  // 1. Check if the current process is system_server or an allowlisted app.
  // Using memoized GetProcessCmdline() avoids syscalls and sysprop lookups
  // for all production allowlisted targets.
  if (IsAllowlistedProcess(GetProcessCmdline())) {
    return true;
  }

  // 2. Fall back to system property override for non-allowlisted apps.
  return IsSyspropOverrideEnabled();
#endif
}

const std::string& GetProcessCmdline() {
#if defined(__ANDROID__)
  static const std::string* const kCmdline =
      new std::string(ReadProcessCmdline());
  return *kCmdline;
#else
  static const std::string* const kEmpty = new std::string();
  return *kEmpty;
#endif
}

}  // namespace jni
}  // namespace perfetto
