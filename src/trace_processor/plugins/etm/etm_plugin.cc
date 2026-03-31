/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/trace_processor/plugins/etm/etm_plugin.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "src/trace_processor/importers/perf/perf_event.h"
#include "src/trace_processor/importers/perf/perf_tracker.h"
#include "src/trace_processor/plugins/etm/etm_decode_trace_vtable.h"
#include "src/trace_processor/plugins/etm/etm_iterate_range_vtable.h"
#include "src/trace_processor/plugins/etm/etm_tracker.h"
#include "src/trace_processor/plugins/etm/etm_v4_stream_demultiplexer.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

PERFETTO_TP_REGISTER_PLUGIN(EtmPlugin);

EtmPlugin::EtmPlugin() = default;
EtmPlugin::~EtmPlugin() = default;

EtmPlugin* EtmPlugin::Get(TraceProcessorContext* context) {
  return static_cast<EtmPlugin*>(context->etm_plugin);
}

void EtmPlugin::RegisterImporters(TraceProcessorContext* context) {
  // Create plugin-owned tables using the shared string pool.
  StringPool* pool = context->storage->mutable_string_pool();
  configuration_table_ =
      std::make_unique<tables::EtmV4ConfigurationTable>(pool);
  session_table_ = std::make_unique<tables::EtmV4SessionTable>(pool);
  chunk_table_ = std::make_unique<tables::EtmV4ChunkTable>(pool);

  // Create the ETM tracker.
  etm_tracker_ = std::make_unique<etm::EtmTracker>(context);

  // Store a pointer to this plugin in the context so importers can access it.
  context->etm_plugin = this;

  // Register "etm" in the modules table.
  context->storage->mutable_modules_table()->Insert(
      {context->storage->InternString("etm")});

  // Register ETM aux tokenizer factory for PerfTracker to consume.
  context->perf_aux_tokenizer_registrations.push_back(
      [](void* perf_tracker_ptr) {
        auto* pt = static_cast<perf_importer::PerfTracker*>(perf_tracker_ptr);
        pt->RegisterAuxTokenizer(
            PERF_AUXTRACE_CS_ETM, [](TraceProcessorContext* ctx,
                                     perf_importer::AuxtraceInfoRecord info) {
              return etm::CreateEtmV4StreamDemultiplexer(ctx, std::move(info));
            });
      });
}

void EtmPlugin::RegisterDataframes(TraceProcessorContext*,
                                   std::vector<PluginDataframe>& tables) {
  tables.push_back({&configuration_table_->dataframe(),
                    tables::EtmV4ConfigurationTable::Name()});
  tables.push_back(
      {&session_table_->dataframe(), tables::EtmV4SessionTable::Name()});
  tables.push_back(
      {&chunk_table_->dataframe(), tables::EtmV4ChunkTable::Name()});
}

void EtmPlugin::RegisterSqliteModules(
    TraceProcessorContext*,
    std::vector<SqliteModuleRegistration>& modules) {
  modules.push_back(MakeSqliteModule<etm::EtmDecodeChunkVtable>(
      "__intrinsic_etm_decode_chunk", this));
  modules.push_back(MakeSqliteModule<etm::EtmIterateRangeVtable>(
      "__intrinsic_etm_iterate_instruction_range", this));
}

}  // namespace perfetto::trace_processor
