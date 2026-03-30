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

#include "src/trace_processor/core/plugin/plugin.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/no_destructor.h"

namespace perfetto::trace_processor {

// Global linked list head for plugin registrations.
static PluginRegistration* g_plugin_head = nullptr;

PluginRegistration::PluginRegistration(Factory f,
                                       const void* id,
                                       const void* const* deps,
                                       size_t n_deps)
    : next(g_plugin_head),
      factory(f),
      plugin_id(id),
      dep_ids(deps),
      dep_count(n_deps) {
  g_plugin_head = this;
}

PluginRegistration* GetPluginRegistrations() {
  return g_plugin_head;
}

// Default no-op implementations.
PluginBase::~PluginBase() = default;
std::unique_ptr<Destructible> PluginBase::CreateStorage(
    TraceProcessorContext*) {
  return nullptr;
}
void PluginBase::RegisterImporters(TraceProcessorContext*, Destructible*) {}
void PluginBase::RegisterDataframes(TraceProcessorContext*,
                                    Destructible*,
                                    std::vector<PluginDataframe>&) {}
void PluginBase::RegisterStaticTableFunctions(
    TraceProcessorContext*,
    Destructible*,
    std::vector<std::unique_ptr<StaticTableFunction>>&) {}
void PluginBase::RegisterSqliteModules(TraceProcessorContext*,
                                       Destructible*,
                                       std::vector<SqliteModuleRegistration>&) {
}
std::string PluginBase::GetAfterEofSql() {
  return {};
}

namespace {

// Returns the topologically sorted order of plugin registrations.
// The result is cached in a static local since the global linked list
// is fixed after static initialization.
std::vector<PluginRegistration*> TopologicalSort() {
  // Collect all registrations.
  std::vector<PluginRegistration*> regs;
  for (auto* r = GetPluginRegistrations(); r; r = r->next) {
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

  // Build adjacency: for each plugin, which plugins depend on it.
  std::vector<std::vector<size_t>> dependents(n);
  for (size_t i = 0; i < n; ++i) {
    for (size_t d = 0; d < regs[i]->dep_count; ++d) {
      size_t dep_idx = *id_to_idx.Find(regs[i]->dep_ids[d]);
      dependents[dep_idx].push_back(i);
      in_degree[i]++;
    }
  }

  // Start with plugins that have no dependencies.
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

  // If we didn't process all plugins, there's a cycle.
  PERFETTO_CHECK(sorted_order.size() == n);

  std::vector<PluginRegistration*> result;
  result.reserve(n);
  for (size_t idx : sorted_order) {
    result.push_back(regs[idx]);
  }
  return result;
}

const std::vector<PluginRegistration*>& GetSortedPluginRegistrations() {
  static base::NoDestructor<std::vector<PluginRegistration*>> sorted(
      TopologicalSort());
  return sorted.ref();
}

}  // namespace

std::vector<std::unique_ptr<PluginBase>> CreatePlugins() {
  const auto& sorted = GetSortedPluginRegistrations();
  std::vector<std::unique_ptr<PluginBase>> plugins;
  plugins.reserve(sorted.size());
  for (auto* reg : sorted) {
    plugins.push_back(reg->factory());
  }
  return plugins;
}

}  // namespace perfetto::trace_processor
