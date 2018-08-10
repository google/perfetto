/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/base/test/utils.h"

#include <stdlib.h>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_MACOSX)
#include <limits.h>
#include <unistd.h>
#endif

namespace perfetto {
namespace base {

std::string GetTestDataPath(const std::string& path) {
  char const* test_data_root = getenv("TEST_DATA_ROOT");
  if (test_data_root) {
    return std::string(test_data_root) + path;
  }

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  char buf[PATH_MAX];
  ssize_t bytes = readlink("/proc/self/exe", buf, sizeof(buf));
  PERFETTO_CHECK(bytes != -1);
  // readlink does not null terminate.
  buf[bytes] = 0;
  std::string self_path = std::string(buf);
  // Cut binary name.
  self_path = self_path.substr(0, self_path.find_last_of("/"));
  return self_path + "/../../test/data/" + path;
#else
  // TODO(hjd): Implement on MacOS/Windows
  // Fall back to relative to root dir.
  return "test/data/" + path;
#endif
}

}  // namespace base
}  // namespace perfetto
