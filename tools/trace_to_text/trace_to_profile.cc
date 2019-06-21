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

#include "tools/trace_to_text/trace_to_profile.h"

#include <string>
#include <vector>

#include "tools/trace_to_text/pprof_builder.h"

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/temp_file.h"
#include "perfetto/ext/base/utils.h"

namespace {

constexpr const char* kDefaultTmp = "/tmp";

std::string GetTemp() {
  const char* tmp = getenv("TMPDIR");
  if (tmp == nullptr)
    tmp = kDefaultTmp;
  return tmp;
}

}  // namespace

namespace perfetto {
namespace trace_to_text {

int TraceToProfile(std::istream* input, std::ostream* output) {
  std::vector<SerializedProfile> profiles;
  TraceToPprof(input, &profiles);
  if (profiles.empty()) {
    return 0;
  }

  std::string temp_dir = GetTemp() + "/heap_profile-XXXXXXX";
  PERFETTO_CHECK(mkdtemp(&temp_dir[0]));
  size_t itr = 0;
  for (const auto& profile : profiles) {
    std::string filename = temp_dir + "/heap_dump." + std::to_string(++itr) +
                           "." + std::to_string(profile.pid) + ".pb";
    base::ScopedFile fd(base::OpenFile(filename, O_CREAT | O_WRONLY, 0700));
    if (!fd)
      PERFETTO_FATAL("Failed to open %s", filename.c_str());
    PERFETTO_CHECK(base::WriteAll(*fd, profile.serialized.c_str(),
                                  profile.serialized.size()) ==
                   static_cast<ssize_t>(profile.serialized.size()));
  }
  *output << "Wrote profiles to " << temp_dir << std::endl;
  return 0;
}

}  // namespace trace_to_text
}  // namespace perfetto
