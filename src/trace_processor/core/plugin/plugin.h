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

#ifndef SRC_TRACE_PROCESSOR_CORE_PLUGIN_PLUGIN_H_
#define SRC_TRACE_PROCESSOR_CORE_PLUGIN_PLUGIN_H_

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "src/trace_processor/types/destructible.h"

struct sqlite3_module;

namespace perfetto::trace_processor::core::dataframe {
class Dataframe;
}  // namespace perfetto::trace_processor::core::dataframe

namespace perfetto::trace_processor {

class StaticTableFunction;
class TraceProcessorContext;
class TraceStorage;

// Lightweight struct for plugin dataframe registration.
struct PluginDataframe {
  core::dataframe::Dataframe* dataframe;
  std::string name;
};

namespace sqlite {
class ModuleStateManagerBase;
}  // namespace sqlite

// Registration entry for a sqlite virtual table module.
struct SqliteModuleRegistration {
  using Destructor = void (*)(void*);

  std::string name;
  const sqlite3_module* module = nullptr;
  void* context = nullptr;
  Destructor destructor = nullptr;
  bool is_state_manager = false;
};

// Helper to create a SqliteModuleRegistration with a non-owning context.
template <typename Module>
SqliteModuleRegistration MakeSqliteModule(std::string name,
                                          typename Module::Context* ctx) {
  SqliteModuleRegistration reg;
  reg.name = std::move(name);
  reg.module = &Module::kModule;
  reg.context = ctx;
  reg.is_state_manager = std::is_base_of_v<sqlite::ModuleStateManagerBase,
                                           typename Module::Context>;
  return reg;
}

// Helper to create a SqliteModuleRegistration with an owning context.
template <typename Module>
SqliteModuleRegistration MakeSqliteModule(
    std::string name,
    std::unique_ptr<typename Module::Context> ctx) {
  SqliteModuleRegistration reg;
  reg.name = std::move(name);
  reg.module = &Module::kModule;
  reg.context = ctx.release();
  reg.destructor = [](void* p) {
    delete static_cast<typename Module::Context*>(p);
  };
  reg.is_state_manager = std::is_base_of_v<sqlite::ModuleStateManagerBase,
                                           typename Module::Context>;
  return reg;
}

// Compile-time tag for plugin identity. Each Plugin subclass gets a unique
// runtime ID via the address of a per-instantiation static member.
template <typename T>
struct PluginTag {
  static inline constexpr char kTag = 0;
  static constexpr const void* Id() { return &kTag; }
};

// Non-templated base class for type-erased storage and virtual dispatch.
//
// Plugins must NOT store mutable state on the plugin object itself. State
// should either live in the Storage object (for data that persists beyond
// parsing) or inside the parser/importer that the plugin creates.
//
// TraceProcessorImpl calls these methods, passing both the context and the
// type-erased storage pointer. Subclasses should not override these directly;
// instead, override the typed methods on Plugin<>.
class PluginBase {
 public:
  virtual ~PluginBase();

  // Creates the plugin's opaque storage. Called once by TraceProcessorImpl;
  // the returned pointer is owned by TraceStorage and passed back into
  // all subsequent lifecycle methods.
  virtual std::unique_ptr<Destructible> CreateStorage(
      TraceProcessorContext* context);

  virtual void RegisterImporters(TraceProcessorContext* context,
                                 Destructible* storage);
  virtual void RegisterDataframes(TraceProcessorContext* context,
                                  Destructible* storage,
                                  std::vector<PluginDataframe>& tables);
  virtual void RegisterStaticTableFunctions(
      TraceProcessorContext* context,
      Destructible* storage,
      std::vector<std::unique_ptr<StaticTableFunction>>& fns);
  virtual void RegisterSqliteModules(
      TraceProcessorContext* context,
      Destructible* storage,
      std::vector<SqliteModuleRegistration>& modules);
  // Allows plugins to contribute SQL modules to the stdlib. Each entry is a
  // pair of (module_key, sql_content) where module_key follows the stdlib
  // naming convention (e.g., "etm.decode").
  virtual void RegisterSqlModules(
      TraceProcessorContext* context,
      Destructible* storage,
      std::vector<std::pair<std::string, std::string>>& modules);
};

// Templated subclass that provides identity, dependency tracking, and typed
// storage dispatch.
//
// Self: CRTP parameter (the concrete plugin class).
// Storage: plugin-owned state type, must inherit from Destructible.
//          Use void (or omit) if no storage is needed.
// Deps: prerequisite plugin classes.
//
// The template parameters serve three purposes:
// 1. Compile-time: forces #include of dep headers -> forces GN dep
// 2. Link-time: if dep's .cc isn't compiled, GN dep missing -> build fails
// 3. Runtime: topological sort verifies all deps are registered
template <typename Self, typename Storage = void, typename... Deps>
class Plugin : public PluginBase {
 public:
  static constexpr const void* kPluginId = PluginTag<Self>::Id();
  static constexpr std::array<const void*, sizeof...(Deps)> kDepIds = {
      PluginTag<Deps>::Id()...};

  // Override these in the concrete plugin class.
  virtual std::unique_ptr<Storage> CreatePluginStorage(TraceProcessorContext*) {
    return nullptr;
  }
  virtual void RegisterImporters(TraceProcessorContext*, Storage*) {}
  virtual void RegisterDataframes(TraceProcessorContext*,
                                  Storage*,
                                  std::vector<PluginDataframe>&) {}
  virtual void RegisterStaticTableFunctions(
      TraceProcessorContext*,
      Storage*,
      std::vector<std::unique_ptr<StaticTableFunction>>&) {}
  virtual void RegisterSqliteModules(TraceProcessorContext*,
                                     Storage*,
                                     std::vector<SqliteModuleRegistration>&) {}
  virtual void RegisterSqlModules(
      TraceProcessorContext*,
      Storage*,
      std::vector<std::pair<std::string, std::string>>&) {}

 private:
  std::unique_ptr<Destructible> CreateStorage(
      TraceProcessorContext* ctx) final {
    return static_cast<Self*>(this)->CreatePluginStorage(ctx);
  }
  void RegisterImporters(TraceProcessorContext* ctx, Destructible* s) final {
    static_cast<Self*>(this)->RegisterImporters(ctx, static_cast<Storage*>(s));
  }
  void RegisterDataframes(TraceProcessorContext* ctx,
                          Destructible* s,
                          std::vector<PluginDataframe>& tables) final {
    static_cast<Self*>(this)->RegisterDataframes(ctx, static_cast<Storage*>(s),
                                                 tables);
  }
  void RegisterStaticTableFunctions(
      TraceProcessorContext* ctx,
      Destructible* s,
      std::vector<std::unique_ptr<StaticTableFunction>>& fns) final {
    static_cast<Self*>(this)->RegisterStaticTableFunctions(
        ctx, static_cast<Storage*>(s), fns);
  }
  void RegisterSqliteModules(
      TraceProcessorContext* ctx,
      Destructible* s,
      std::vector<SqliteModuleRegistration>& modules) final {
    static_cast<Self*>(this)->RegisterSqliteModules(
        ctx, static_cast<Storage*>(s), modules);
  }
  void RegisterSqlModules(
      TraceProcessorContext* ctx,
      Destructible* s,
      std::vector<std::pair<std::string, std::string>>& modules) final {
    static_cast<Self*>(this)->RegisterSqlModules(ctx, static_cast<Storage*>(s),
                                                 modules);
  }
};

// Specialization for plugins with no storage.
template <typename Self, typename... Deps>
class Plugin<Self, void, Deps...> : public PluginBase {
 public:
  static constexpr const void* kPluginId = PluginTag<Self>::Id();
  static constexpr std::array<const void*, sizeof...(Deps)> kDepIds = {
      PluginTag<Deps>::Id()...};

  virtual void RegisterImporters(TraceProcessorContext*) {}
  virtual void RegisterDataframes(TraceProcessorContext*,
                                  std::vector<PluginDataframe>&) {}
  virtual void RegisterStaticTableFunctions(
      TraceProcessorContext*,
      std::vector<std::unique_ptr<StaticTableFunction>>&) {}
  virtual void RegisterSqliteModules(TraceProcessorContext*,
                                     std::vector<SqliteModuleRegistration>&) {}
  virtual void RegisterSqlModules(
      TraceProcessorContext*,
      std::vector<std::pair<std::string, std::string>>&) {}

 private:
  void RegisterImporters(TraceProcessorContext* ctx, Destructible*) final {
    static_cast<Self*>(this)->RegisterImporters(ctx);
  }
  void RegisterDataframes(TraceProcessorContext* ctx,
                          Destructible*,
                          std::vector<PluginDataframe>& tables) final {
    static_cast<Self*>(this)->RegisterDataframes(ctx, tables);
  }
  void RegisterStaticTableFunctions(
      TraceProcessorContext* ctx,
      Destructible*,
      std::vector<std::unique_ptr<StaticTableFunction>>& fns) final {
    static_cast<Self*>(this)->RegisterStaticTableFunctions(ctx, fns);
  }
  void RegisterSqliteModules(
      TraceProcessorContext* ctx,
      Destructible*,
      std::vector<SqliteModuleRegistration>& modules) final {
    static_cast<Self*>(this)->RegisterSqliteModules(ctx, modules);
  }
  void RegisterSqlModules(
      TraceProcessorContext* ctx,
      Destructible*,
      std::vector<std::pair<std::string, std::string>>& modules) final {
    static_cast<Self*>(this)->RegisterSqlModules(ctx, modules);
  }
};

// Registration entry in a global intrusive linked list.
struct PluginRegistration {
  using Factory = std::unique_ptr<PluginBase> (*)();

  PluginRegistration* next;
  Factory factory;
  const void* plugin_id;
  const void* const* dep_ids;
  size_t dep_count;

  PluginRegistration(Factory f,
                     const void* id,
                     const void* const* deps,
                     size_t n_deps);
};

PluginRegistration* GetPluginRegistrations();

// Collects all registered plugins, topologically sorts them by dependencies,
// and instantiates them in order.
std::vector<std::unique_ptr<PluginBase>> CreatePlugins();

// Suppresses -Wglobal-constructors which is Clang-specific. GCC and MSVC
// do not have this warning so the macros are no-ops on those compilers.
#ifdef __clang__
#define PERFETTO_ALLOW_GLOBAL_CTORS_FOR_TP_PLUGIN_REGISTER \
  _Pragma("clang diagnostic push")                         \
      _Pragma("clang diagnostic ignored \"-Wglobal-constructors\"")
#define PERFETTO_END_ALLOW_GLOBAL_CTORS_FOR_TP_PLUGIN_REGISTER \
  _Pragma("clang diagnostic pop")
#else
#define PERFETTO_ALLOW_GLOBAL_CTORS_FOR_TP_PLUGIN_REGISTER
#define PERFETTO_END_ALLOW_GLOBAL_CTORS_FOR_TP_PLUGIN_REGISTER
#endif

#define PERFETTO_TP_REGISTER_PLUGIN(ClassName)                                \
  PERFETTO_ALLOW_GLOBAL_CTORS_FOR_TP_PLUGIN_REGISTER                          \
  static ::perfetto::trace_processor::PluginRegistration g_##ClassName##_reg( \
      []() -> std::unique_ptr<::perfetto::trace_processor::PluginBase> {      \
        return std::make_unique<ClassName>();                                 \
      },                                                                      \
      ClassName::kPluginId, ClassName::kDepIds.data(),                        \
      ClassName::kDepIds.size());                                             \
  PERFETTO_END_ALLOW_GLOBAL_CTORS_FOR_TP_PLUGIN_REGISTER

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_CORE_PLUGIN_PLUGIN_H_
