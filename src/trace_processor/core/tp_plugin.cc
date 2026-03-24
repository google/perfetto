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

#include "src/trace_processor/core/tp_plugin.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/flat_hash_map.h"

namespace perfetto::trace_processor {

// Global linked list head for module registrations.
static TpPluginRegistration* g_tp_plugin_head = nullptr;

TpPluginRegistration::TpPluginRegistration(Factory f,
                                           const void* id,
                                           const void* const* deps,
                                           size_t n_deps)
    : next(g_tp_plugin_head),
      factory(f),
      plugin_id(id),
      dep_ids(deps),
      dep_count(n_deps) {
  g_tp_plugin_head = this;
}

TpPluginRegistration* GetTpPluginRegistrations() {
  return g_tp_plugin_head;
}

// Default no-op implementations.
TpPluginBase::~TpPluginBase() = default;
void TpPluginBase::RegisterImporters(TraceProcessorContext*) {}
void TpPluginBase::OnEventsFullyExtracted(TraceProcessorContext*) {}
void TpPluginBase::RegisterStaticTables(
    TraceStorage*,
    std::vector<PerfettoSqlEngine::StaticTable>&) {}
void TpPluginBase::RegisterStaticTableFunctions(
    TraceProcessorContext*,
    TraceStorage*,
    PerfettoSqlEngine*,
    std::vector<std::unique_ptr<StaticTableFunction>>&) {}
void TpPluginBase::RegisterFunctionsAndOperators(TraceStorage*,
                                                 PerfettoSqlEngine*) {}
std::string TpPluginBase::GetAfterEofSql() {
  return {};
}

std::vector<std::unique_ptr<TpPluginBase>> CreateTpPlugins() {
  // Collect all registrations.
  std::vector<TpPluginRegistration*> regs;
  for (auto* r = GetTpPluginRegistrations(); r; r = r->next) {
    regs.push_back(r);
  }

  // Build a map from plugin_id -> index for dependency resolution.
  base::FlatHashMap<const void*, size_t> id_to_idx;
  for (size_t i = 0; i < regs.size(); ++i) {
    id_to_idx[regs[i]->plugin_id] = i;
  }

  // Verify all dependencies exist.
  for (auto* r : regs) {
    for (size_t d = 0; d < r->dep_count; ++d) {
      PERFETTO_CHECK(id_to_idx.Find(r->dep_ids[d]) != nullptr);
    }
  }

  // Kahn's algorithm for topological sort.
  size_t n = regs.size();
  std::vector<size_t> in_degree(n, 0);

  // Build adjacency: for each module, which modules depend on it.
  std::vector<std::vector<size_t>> dependents(n);
  for (size_t i = 0; i < n; ++i) {
    for (size_t d = 0; d < regs[i]->dep_count; ++d) {
      size_t dep_idx = *id_to_idx.Find(regs[i]->dep_ids[d]);
      dependents[dep_idx].push_back(i);
      in_degree[i]++;
    }
  }

  // Start with modules that have no dependencies.
  std::vector<size_t> queue;
  for (size_t i = 0; i < n; ++i) {
    if (in_degree[i] == 0) {
      queue.push_back(i);
    }
  }

  std::vector<size_t> sorted_order;
  sorted_order.reserve(n);
  while (!queue.empty()) {
    size_t idx = queue.back();
    queue.pop_back();
    sorted_order.push_back(idx);
    for (size_t dep : dependents[idx]) {
      if (--in_degree[dep] == 0) {
        queue.push_back(dep);
      }
    }
  }

  // If we didn't process all modules, there's a cycle.
  PERFETTO_CHECK(sorted_order.size() == n);

  // Instantiate in dependency order.
  std::vector<std::unique_ptr<TpPluginBase>> modules;
  modules.reserve(n);
  for (size_t idx : sorted_order) {
    modules.push_back(regs[idx]->factory());
  }
  return modules;
}

}  // namespace perfetto::trace_processor
