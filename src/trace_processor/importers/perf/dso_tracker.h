/*
 * Copyright (C) 2024 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PERF_DSO_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PERF_DSO_TRACKER_H_

#include <cstdint>
#include <string>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "protos/third_party/simpleperf/record_file.pbzero.h"
#include "src/trace_processor/importers/common/address_range.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/profiler_tables_py.h"
#include "src/trace_processor/types/destructible.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor::perf_importer {

// Keeps track of DSO symbols to symbolize frames at the end of the trace
// parsing.
// TODO(b/334978369): We could potentially use this class (or a similar one) to
// process the ModuleSymbols proto packets and consolidate all symbolization in
// one place.
class DsoTracker : public Destructible {
 public:
  static DsoTracker& GetOrCreate(TraceProcessorContext* context) {
    if (!context->perf_dso_tracker) {
      context->perf_dso_tracker.reset(new DsoTracker(context));
    }
    return static_cast<DsoTracker&>(*context->perf_dso_tracker);
  }
  ~DsoTracker() override;

  // Add symbol data contained in a `FileFeature` proto.
  void AddSimpleperfFile2(
      const third_party::simpleperf::proto::pbzero::FileFeature::Decoder& file);

  // Tries to symbolize any `STACK_PROFILE_FRAME` frame missing the `name`
  // attribute. This should be called at the end of parsing when all packets
  // have been processed and all tables updated.
  void SymbolizeFrames();

 private:
  struct Dso {
    uint64_t load_bias;
    AddressRangeMap<std::string> symbols;
  };

  explicit DsoTracker(TraceProcessorContext* context);

  void SymbolizeKernelFrame(tables::StackProfileFrameTable::RowReference frame);
  // Returns true it the frame was symbolized.
  bool TrySymbolizeFrame(tables::StackProfileFrameTable::RowReference frame);

  TraceProcessorContext* const context_;
  const tables::StackProfileMappingTable& mapping_table_;
  base::FlatHashMap<StringId, Dso> files_;
  AddressRangeMap<std::string> kernel_symbols_;
};

}  // namespace perfetto::trace_processor::perf_importer

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PERF_DSO_TRACKER_H_
