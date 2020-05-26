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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_ANDROID_PROBES_MODULE_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_ANDROID_PROBES_MODULE_H_

#include "perfetto/base/build_config.h"
#include "src/trace_processor/importers/proto/android_probes_parser.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/timestamped_trace_piece.h"

#include "protos/perfetto/config/trace_config.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace trace_processor {

class AndroidProbesModule : public ProtoImporterModuleBase<PERFETTO_BUILDFLAG(
                                PERFETTO_TP_ANDROID_PROBES)> {
 public:
  explicit AndroidProbesModule(TraceProcessorContext* context)
      : ProtoImporterModuleBase(context), parser_(context) {}

  ModuleResult ParsePacket(const protos::pbzero::TracePacket::Decoder& decoder,
                           const TimestampedTracePiece& ttp) {
    if (decoder.has_battery()) {
      parser_.ParseBatteryCounters(ttp.timestamp, decoder.battery());
      return ModuleResult::Handled();
    }

    if (decoder.has_power_rails()) {
      parser_.ParsePowerRails(ttp.timestamp, decoder.power_rails());
      return ModuleResult::Handled();
    }

    if (decoder.has_android_log()) {
      parser_.ParseAndroidLogPacket(decoder.android_log());
      return ModuleResult::Handled();
    }

    if (decoder.has_packages_list()) {
      parser_.ParseAndroidPackagesList(decoder.packages_list());
      return ModuleResult::Handled();
    }

    return ModuleResult::Ignored();
  }

  ModuleResult ParseTraceConfig(
      const protos::pbzero::TraceConfig::Decoder& decoder) {
    if (decoder.has_statsd_metadata()) {
      parser_.ParseStatsdMetadata(decoder.statsd_metadata());
      return ModuleResult::Handled();
    }
    return ModuleResult::Ignored();
  }

 private:
  AndroidProbesParser parser_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_ANDROID_PROBES_MODULE_H_
