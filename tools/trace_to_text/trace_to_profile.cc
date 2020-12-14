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

#include "perfetto/trace_processor/trace_processor.h"
#include "src/profiling/symbolizer/local_symbolizer.h"
#include "src/profiling/symbolizer/symbolize_database.h"
#include "tools/trace_to_text/utils.h"

#include "perfetto/base/logging.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/temp_file.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/profiling/pprof_builder.h"
#include "src/profiling/symbolizer/symbolizer.h"

namespace {

constexpr const char* kDefaultTmp = "/tmp";

std::string GetTemp() {
  const char* tmp = nullptr;
  if ((tmp = getenv("TMPDIR")))
    return tmp;
  if ((tmp = getenv("TEMP")))
    return tmp;
  return kDefaultTmp;
}

}  // namespace

namespace perfetto {
namespace trace_to_text {
namespace {

void MaybeSymbolize(trace_processor::TraceProcessor* tp) {
  std::unique_ptr<profiling::Symbolizer> symbolizer =
      profiling::LocalSymbolizerOrDie(profiling::GetPerfettoBinaryPath(),
                                      getenv("PERFETTO_SYMBOLIZER_MODE"));
  if (!symbolizer)
    return;
  profiling::SymbolizeDatabase(
      tp, symbolizer.get(), [tp](const std::string& trace_proto) {
        std::unique_ptr<uint8_t[]> buf(new uint8_t[trace_proto.size()]);
        memcpy(buf.get(), trace_proto.data(), trace_proto.size());
        auto status = tp->Parse(std::move(buf), trace_proto.size());
        if (!status.ok()) {
          PERFETTO_DFATAL_OR_ELOG("Failed to parse: %s",
                                  status.message().c_str());
          return;
        }
      });
  tp->NotifyEndOfFile();
}

}  // namespace

int TraceToProfile(std::istream* input,
                   std::ostream* output,
                   uint64_t pid,
                   std::vector<uint64_t> timestamps) {
  std::vector<SerializedProfile> profiles;
  trace_processor::Config config;
  std::unique_ptr<trace_processor::TraceProcessor> tp =
      trace_processor::TraceProcessor::CreateInstance(config);

  if (!ReadTrace(tp.get(), input))
    return false;

  tp->NotifyEndOfFile();
  MaybeSymbolize(tp.get());

  TraceToPprof(tp.get(), &profiles, pid, timestamps);
  if (profiles.empty()) {
    return 0;
  }

  std::string temp_dir =
      GetTemp() + "/heap_profile-" + base::GetTimeFmt("%y%m%d%H%M%S");
  PERFETTO_CHECK(base::Mkdir(temp_dir));
  size_t itr = 0;
  for (const auto& profile : profiles) {
    std::string filename = temp_dir + "/heap_dump." + std::to_string(++itr) +
                           "." + std::to_string(profile.pid) + "." +
                           profile.heap_name + ".pb";
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
