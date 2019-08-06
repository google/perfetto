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

#include "tools/trace_to_text/symbolize_profile.h"

#include <vector>

#include "perfetto/base/logging.h"

#ifndef PERFETTO_NOLOCALSYMBOLIZE
#include "tools/trace_to_text/local_symbolizer.h"  // nogncheck
#endif

#include "tools/trace_to_text/symbolizer.h"
#include "tools/trace_to_text/trace_symbol_table.h"
#include "tools/trace_to_text/utils.h"

#include "perfetto/trace/profiling/profile_common.pb.h"
#include "perfetto/trace/profiling/profile_packet.pb.h"
#include "perfetto/trace/interned_data/interned_data.pb.h"

namespace perfetto {
namespace trace_to_text {
// Ingest profile, and emit a symbolization table for each sequence. This can
// be prepended to the profile to attach the symbol information.
int SymbolizeProfile(std::istream* input, std::ostream* output) {
  std::unique_ptr<Symbolizer> symbolizer;
  auto binary_path = GetPerfettoBinaryPath();
  if (!binary_path.empty()) {
#ifndef PERFETTO_NOLOCALSYMBOLIZE
    symbolizer.reset(new LocalSymbolizer(GetPerfettoBinaryPath()));
#else
    PERFETTO_FATAL("This build does not support local symbolization.");
#endif
  }

  if (!symbolizer)
    PERFETTO_FATAL("No symbolizer selected");

  return VisitCompletePacket(
      input, [&output, &symbolizer](
                 uint32_t seq_id,
                 const std::vector<protos::ProfilePacket>& packet_fragments,
                 const std::vector<protos::InternedData>& interned_data) {
        TraceSymbolTable symbol_table(symbolizer.get());
        if (!symbol_table.Visit(packet_fragments, interned_data))
          return false;
        symbol_table.Finalize();
        symbol_table.WriteResult(output, seq_id);
        return true;
      });
}

}  // namespace trace_to_text
}  // namespace perfetto
