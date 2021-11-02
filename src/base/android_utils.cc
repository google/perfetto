/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "perfetto/ext/base/android_utils.h"

#include "perfetto/base/build_config.h"

#include <string>

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#include <sys/system_properties.h>
#endif

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"

namespace perfetto {
namespace base {

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)

std::string GetAndroidProp(const char* name) {
  std::string ret;
#if __ANDROID_API__ >= 26
  const prop_info* pi = __system_property_find(name);
  if (!pi) {
    return ret;
  }
  __system_property_read_callback(
      pi,
      [](void* dst_void, const char*, const char* value, uint32_t) {
        std::string& dst = *static_cast<std::string*>(dst_void);
        dst = value;
      },
      &ret);
#else  // __ANDROID_API__ < 26
  char value_buf[PROP_VALUE_MAX];
  int len = __system_property_get(name, value_buf);
  if (len > 0 && static_cast<size_t>(len) < sizeof(value_buf)) {
    ret = std::string(value_buf, static_cast<size_t>(len));
  }
#endif
  return ret;
}

#endif  // PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)

}  // namespace base
}  // namespace perfetto
