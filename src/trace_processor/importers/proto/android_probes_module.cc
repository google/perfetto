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

#include "src/trace_processor/importers/proto/android_probes_module.h"
#include "perfetto/base/build_config.h"
#include "src/trace_processor/importers/proto/android_probes_parser.h"
#include "src/trace_processor/timestamped_trace_piece.h"

#include "protos/perfetto/config/trace_config.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace trace_processor {

using perfetto::protos::pbzero::TracePacket;

AndroidProbesModule::AndroidProbesModule(TraceProcessorContext* context)
    : parser_(context) {
  RegisterForField(TracePacket::kBatteryFieldNumber, context);
  RegisterForField(TracePacket::kPowerRailsFieldNumber, context);
  RegisterForField(TracePacket::kAndroidLogFieldNumber, context);
  RegisterForField(TracePacket::kPackagesListFieldNumber, context);
}

void AndroidProbesModule::ParsePacket(const TracePacket::Decoder& decoder,
                                      const TimestampedTracePiece& ttp,
                                      uint32_t field_id) {
  switch (field_id) {
    case TracePacket::kBatteryFieldNumber:
      parser_.ParseBatteryCounters(ttp.timestamp, decoder.battery());
      return;
    case TracePacket::kPowerRailsFieldNumber:
      parser_.ParsePowerRails(ttp.timestamp, decoder.power_rails());
      return;
    case TracePacket::kAndroidLogFieldNumber:
      parser_.ParseAndroidLogPacket(decoder.android_log());
      return;
    case TracePacket::kPackagesListFieldNumber:
      parser_.ParseAndroidPackagesList(decoder.packages_list());
      return;
  }
}

void AndroidProbesModule::ParseTraceConfig(
    const protos::pbzero::TraceConfig::Decoder& decoder) {
  if (decoder.has_statsd_metadata()) {
    parser_.ParseStatsdMetadata(decoder.statsd_metadata());
  }
}

}  // namespace trace_processor
}  // namespace perfetto
