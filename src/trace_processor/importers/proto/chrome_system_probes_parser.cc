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

#include "src/trace_processor/importers/proto/chrome_system_probes_parser.h"

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/protozero/proto_decoder.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/storage/metadata.h"
#include "src/trace_processor/types/trace_processor_context.h"

#include "protos/perfetto/trace/ps/process_stats.pbzero.h"
#include "protos/perfetto/trace/ps/process_tree.pbzero.h"
#include "protos/perfetto/trace/sys_stats/sys_stats.pbzero.h"

namespace perfetto {
namespace trace_processor {

ChromeSystemProbesParser::ChromeSystemProbesParser(
    TraceProcessorContext* context)
    : context_(context),
      is_peak_rss_resettable_id_(
          context->storage->InternString("is_peak_rss_resettable")) {
  using ProcessStats = protos::pbzero::ProcessStats;
  proc_stats_process_names_
      [ProcessStats::Process::kChromePrivateFootprintKbFieldNumber] =
          context->storage->InternString("chrome.private_footprint_kb");
  proc_stats_process_names_
      [ProcessStats::Process::kChromePeakResidentSetKbFieldNumber] =
          context->storage->InternString("chrome.peak_resident_set_kb");
}

void ChromeSystemProbesParser::ParseProcessStats(int64_t ts, ConstBytes blob) {
  protos::pbzero::ProcessStats::Decoder stats(blob.data, blob.size);
  for (auto it = stats.processes(); it; ++it) {
    protozero::ProtoDecoder proc(*it);
    uint32_t pid = 0;
    for (auto fld = proc.ReadField(); fld.valid(); fld = proc.ReadField()) {
      if (fld.id() == protos::pbzero::ProcessStats::Process::kPidFieldNumber) {
        pid = fld.as_uint32();
        break;
      }
    }

    for (auto fld = proc.ReadField(); fld.valid(); fld = proc.ReadField()) {
      if (fld.id() == protos::pbzero::ProcessStats::Process::
                          kIsPeakRssResettableFieldNumber) {
        UniquePid upid = context_->process_tracker->GetOrCreateProcess(pid);
        context_->process_tracker->AddArgsTo(upid).AddArg(
            is_peak_rss_resettable_id_, Variadic::Boolean(fld.as_bool()));
        continue;
      }

      if (fld.id() >= proc_stats_process_names_.size())
        continue;
      const StringId& name = proc_stats_process_names_[fld.id()];
      if (name == StringId::Null())
        continue;
      UniquePid upid = context_->process_tracker->GetOrCreateProcess(pid);
      TrackId track =
          context_->track_tracker->InternProcessCounterTrack(name, upid);
      int64_t value = fld.as_int64() * 1024;
      context_->event_tracker->PushCounter(ts, static_cast<double>(value),
                                           track);
    }
  }
}

}  // namespace trace_processor
}  // namespace perfetto
