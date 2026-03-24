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

#ifndef SRC_TRACE_PROCESSOR_CORE_TP_PLUGIN_H_
#define SRC_TRACE_PROCESSOR_CORE_TP_PLUGIN_H_

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"

namespace perfetto::trace_processor {

class StaticTableFunction;
class TraceProcessorContext;
class TraceStorage;

// Compile-time tag for module identity. Each TpPlugin subclass gets a unique
// runtime ID via the address of a per-instantiation static member.
// No RTTI required.
template <typename T>
struct TpPluginTag {
  static inline constexpr char kTag = 0;
  static constexpr const void* Id() { return &kTag; }
};

// Non-templated base class for type-erased storage and virtual dispatch.
// All methods have default no-op implementations so modules only override
// what they need.
class TpPluginBase {
 public:
  virtual ~TpPluginBase();

  // Called early during context setup (before parsing begins).
  // Module should create its tracker and store it in context, register
  // its ProtoImporterModule, create module-owned tables, etc.
  virtual void RegisterImporters(TraceProcessorContext* context);

  // Called after all events are sorted and parsed, before SQL engine init.
  // Module should finalize any tracker state.
  virtual void OnEventsFullyExtracted(TraceProcessorContext* context);

  // Register static tables into the engine.
  virtual void RegisterStaticTables(
      TraceStorage* storage,
      std::vector<PerfettoSqlEngine::StaticTable>& tables);

  // Register table functions.
  virtual void RegisterStaticTableFunctions(
      TraceProcessorContext* context,
      TraceStorage* storage,
      PerfettoSqlEngine* engine,
      std::vector<std::unique_ptr<StaticTableFunction>>& fns);

  // Register SQL functions, aggregate functions, virtual table modules.
  virtual void RegisterFunctionsAndOperators(TraceStorage* storage,
                                             PerfettoSqlEngine* engine);

  // SQL to execute after EOF (views, etc). Empty string means no SQL.
  virtual std::string GetAfterEofSql();
};

// Templated subclass that declares dependencies and provides identity.
// Self is the CRTP parameter (the concrete module class).
// Deps are prerequisite module classes whose headers must be #included.
//
// The template parameters serve three purposes:
// 1. Compile-time: forces #include of dep headers -> forces GN dep
// 2. Link-time: if dep's .cc isn't compiled, GN dep missing -> build fails
// 3. Runtime: topological sort verifies all deps are registered
template <typename Self, typename... Deps>
class TpPlugin : public TpPluginBase {
 public:
  // This module's unique identity (address of per-type static).
  static constexpr const void* kPluginId = TpPluginTag<Self>::Id();

  // Dependencies' identities for topological sort.
  static constexpr std::array<const void*, sizeof...(Deps)> kDepIds = {
      TpPluginTag<Deps>::Id()...};
};

// Registration entry in a global intrusive linked list.
// Created at static init time by PERFETTO_TP_REGISTER_PLUGIN.
struct TpPluginRegistration {
  using Factory = std::unique_ptr<TpPluginBase> (*)();

  TpPluginRegistration* next;
  Factory factory;
  const void* plugin_id;
  const void* const* dep_ids;
  size_t dep_count;

  TpPluginRegistration(Factory f,
                       const void* id,
                       const void* const* deps,
                       size_t n_deps);
};

// Returns the head of the global linked list of module registrations.
TpPluginRegistration* GetTpPluginRegistrations();

// Collects all registered modules, topologically sorts them by dependencies,
// and instantiates them in order. PERFETTO_FATAL if a dependency is missing.
std::vector<std::unique_ptr<TpPluginBase>> CreateTpPlugins();

// Macro to register a module. Place at file scope in the module's .cc file.
// The module's source_set in BUILD.gn controls whether it gets compiled.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
#define PERFETTO_TP_REGISTER_PLUGIN(ClassName)                                 \
  _Pragma("clang diagnostic push") _Pragma(                                    \
      "clang diagnostic ignored \"-Wglobal-constructors\"") static ::          \
      perfetto::trace_processor::TpPluginRegistration g_##ClassName##_reg(     \
          []() -> std::unique_ptr<::perfetto::trace_processor::TpPluginBase> { \
            return std::make_unique<ClassName>();                              \
          },                                                                   \
          ClassName::kPluginId, ClassName::kDepIds.data(),                     \
          ClassName::kDepIds.size());                                          \
  _Pragma("clang diagnostic pop")

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_CORE_TP_PLUGIN_H_
