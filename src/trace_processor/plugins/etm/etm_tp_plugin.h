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

#ifndef SRC_TRACE_PROCESSOR_PLUGINS_ETM_ETM_TP_PLUGIN_H_
#define SRC_TRACE_PROCESSOR_PLUGINS_ETM_ETM_TP_PLUGIN_H_

#include <memory>
#include <string>
#include <vector>

#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/core/tp_plugin.h"
#include "src/trace_processor/plugins/etm/etm_tables_py.h"
#include "src/trace_processor/types/destructible.h"

namespace perfetto::trace_processor {

class TraceProcessorContext;
namespace etm {
class EtmTracker;
}  // namespace etm

// TpPlugin for Embedded Trace Macrocell (ETM) support.
// Owns the ETM-specific tables and data, registers ETM operators.
class EtmTpPlugin : public TpPlugin<EtmTpPlugin> {
 public:
  EtmTpPlugin();
  ~EtmTpPlugin() override;

  // Retrieves this module from TraceProcessorContext.
  static EtmTpPlugin* Get(TraceProcessorContext* context);

  // --- TpPluginBase overrides ---
  void RegisterImporters(TraceProcessorContext* context) override;
  void OnEventsFullyExtracted(TraceProcessorContext* context) override;
  void RegisterStaticTables(
      TraceStorage* storage,
      std::vector<PerfettoSqlEngine::StaticTable>& tables) override;
  void RegisterFunctionsAndOperators(TraceStorage* storage,
                                     PerfettoSqlEngine* engine) override;

  // --- Module-owned storage ---
  tables::EtmV4ConfigurationTable* mutable_configuration_table() {
    return configuration_table_.get();
  }
  const tables::EtmV4ConfigurationTable& configuration_table() const {
    return *configuration_table_;
  }

  tables::EtmV4SessionTable* mutable_session_table() {
    return session_table_.get();
  }
  const tables::EtmV4SessionTable& session_table() const {
    return *session_table_;
  }

  tables::EtmV4ChunkTable* mutable_chunk_table() { return chunk_table_.get(); }
  const tables::EtmV4ChunkTable& chunk_table() const { return *chunk_table_; }

  std::vector<std::unique_ptr<Destructible>>* mutable_configuration_data() {
    return &configuration_data_;
  }
  const std::vector<std::unique_ptr<Destructible>>& configuration_data() const {
    return configuration_data_;
  }

  std::vector<TraceBlobView>* mutable_chunk_data() { return &chunk_data_; }
  const std::vector<TraceBlobView>& chunk_data() const { return chunk_data_; }

  Destructible* target_memory() { return target_memory_.get(); }
  void set_target_memory(std::unique_ptr<Destructible> tm) {
    target_memory_ = std::move(tm);
  }

  etm::EtmTracker* etm_tracker() { return etm_tracker_.get(); }

 private:
  // Tables are created in RegisterImporters() when StringPool is available.
  std::unique_ptr<tables::EtmV4ConfigurationTable> configuration_table_;
  std::unique_ptr<tables::EtmV4SessionTable> session_table_;
  std::unique_ptr<tables::EtmV4ChunkTable> chunk_table_;

  // Side-channel data indexed by table row IDs.
  std::vector<std::unique_ptr<Destructible>> configuration_data_;
  std::vector<TraceBlobView> chunk_data_;
  std::unique_ptr<Destructible> target_memory_;
  std::unique_ptr<etm::EtmTracker> etm_tracker_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PLUGINS_ETM_ETM_TP_PLUGIN_H_
