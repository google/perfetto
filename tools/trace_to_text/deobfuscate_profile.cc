/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include <stdio.h>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/profiling/deobfuscator.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "tools/trace_to_text/deobfuscate_profile.h"
#include "tools/trace_to_text/utils.h"

namespace perfetto {
namespace trace_to_text {
namespace {

bool ParseFile(profiling::ProguardParser* p, FILE* f) {
  std::vector<std::string> lines;
  size_t n = 0;
  char* line = nullptr;
  ssize_t rd = 0;
  bool success = true;
  do {
    rd = getline(&line, &n, f);
    // Do not read empty line that terminates the output.
    if (rd > 1) {
      // Remove newline character.
      PERFETTO_DCHECK(line[rd - 1] == '\n');
      line[rd - 1] = '\0';
      success = p->AddLine(line);
    }
  } while (rd > 1 && success);
  free(line);
  return success;
}
}  // namespace

int DeobfuscateProfile(std::istream* input, std::ostream* output) {
  base::ignore_result(input);
  base::ignore_result(output);
  auto maybe_map = GetPerfettoProguardMapPath();
  if (!maybe_map) {
    PERFETTO_ELOG("No PERFETTO_PROGUARD_MAP specified.");
    return 1;
  }
  for (const ProguardMap& map : *maybe_map) {
    const char* filename = map.filename.c_str();
    base::ScopedFstream f(fopen(filename, "r"));
    if (!f) {
      PERFETTO_ELOG("Failed to open %s", filename);
      return 1;
    }
    profiling::ProguardParser parser;
    if (!ParseFile(&parser, *f)) {
      PERFETTO_ELOG("Failed to parse %s", filename);
      return 1;
    }
    std::map<std::string, profiling::ObfuscatedClass> obfuscation_map =
        parser.ConsumeMapping();

    // TODO(fmayer): right now, we don't use the profile we are given. We can
    // filter the output to only contain the classes actually seen in the
    // profile.
    MakeDeobfuscationPackets(map.package, obfuscation_map,
                             [output](const std::string& packet_proto) {
                               WriteTracePacket(packet_proto, output);
                             });
  }
  return 0;
}

}  // namespace trace_to_text
}  // namespace perfetto
