/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "src/profiling/common/producer_support.h"

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/string_splitter.h"
#include "perfetto/tracing/core/data_source_config.h"

#include "src/traced/probes/packages_list/packages_list_parser.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#include <sys/system_properties.h>
#endif

namespace perfetto {
namespace profiling {

bool CanProfile(const DataSourceConfig& ds_config, uint64_t uid) {
// We restrict by !PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD) because a
// sideloaded heapprofd should not be restricted by this. Do note though that,
// at the moment, there isn't really a way to sideload a functioning heapprofd
// onto user builds.
#if !PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD) || \
    !PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  base::ignore_result(ds_config);
  base::ignore_result(uid);
  return true;
#else
  char buf[PROP_VALUE_MAX + 1] = {};
  int ret = __system_property_get("ro.build.type", buf);
  PERFETTO_CHECK(ret >= 0);
  return CanProfileAndroid(ds_config, uid, std::string(buf),
                           "/data/system/packages.list");
#endif
}

bool CanProfileAndroid(const DataSourceConfig& ds_config,
                       uint64_t uid,
                       const std::string& build_type,
                       const std::string& packages_list_path) {
  // These are replicated constants from libcutils android_filesystem_config.h
  constexpr auto kAidAppStart = 10000;     // AID_APP_START
  constexpr auto kAidAppEnd = 19999;       // AID_APP_END
  constexpr auto kAidUserOffset = 100000;  // AID_USER_OFFSET

  if (build_type != "user") {
    return true;
  }

  if (ds_config.enable_extra_guardrails()) {
    return false;  // no extra guardrails on user builds.
  }

  uint64_t uid_without_profile = uid % kAidUserOffset;
  if (uid_without_profile < kAidAppStart || kAidAppEnd < uid_without_profile) {
    // TODO(fmayer): relax this.
    return false;  // no native services on user.
  }

  std::string content;
  if (!base::ReadFile(packages_list_path, &content)) {
    PERFETTO_ELOG("Failed to read %s.", packages_list_path.c_str());
    return false;
  }
  for (base::StringSplitter ss(std::move(content), '\n'); ss.Next();) {
    Package pkg;
    if (!ReadPackagesListLine(ss.cur_token(), &pkg)) {
      PERFETTO_ELOG("Failed to parse packages.list.");
      return false;
    }
    if (pkg.uid == uid_without_profile &&
        (pkg.profileable_from_shell || pkg.debuggable)) {
      return true;
    }
  }
  return false;
}

}  // namespace profiling
}  // namespace perfetto
