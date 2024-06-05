/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "src/trace_processor/importers/proto/winscope/surfaceflinger_transactions_parser.h"

#include "protos/perfetto/trace/android/surfaceflinger_transactions.pbzero.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/proto/args_parser.h"
#include "src/trace_processor/importers/proto/winscope/winscope.descriptor.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

SurfaceFlingerTransactionsParser::SurfaceFlingerTransactionsParser(
    TraceProcessorContext* context)
    : context_{context}, args_parser_{pool_} {
  pool_.AddFromFileDescriptorSet(kWinscopeDescriptor.data(),
                                 kWinscopeDescriptor.size());
}

void SurfaceFlingerTransactionsParser::Parse(int64_t timestamp,
                                             protozero::ConstBytes blob) {
  tables::SurfaceFlingerTransactionsTable::Row row;
  row.ts = timestamp;
  auto rowId = context_->storage->mutable_surfaceflinger_transactions_table()
                   ->Insert(row)
                   .id;

  ArgsTracker tracker(context_);
  auto inserter = tracker.AddArgsTo(rowId);
  ArgsParser writer(timestamp, inserter, *context_->storage.get());
  base::Status status =
      args_parser_.ParseMessage(blob, kTransactionTraceEntryProtoName,
                                nullptr /* parse all fields */, writer);
  if (!status.ok()) {
    context_->storage->IncrementStats(
        stats::winscope_sf_transactions_parse_errors);
  }
}

}  // namespace trace_processor
}  // namespace perfetto
