/*
 * Copyright (C) 2024 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_SQLITE_MODULE_LIFECYCLE_MANAGER_H_
#define SRC_TRACE_PROCESSOR_SQLITE_MODULE_LIFECYCLE_MANAGER_H_

#include <memory>
#include <string>
#include <string_view>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/flat_hash_map.h"

namespace perfetto::trace_processor::sqlite {

// Helper class which abstracts away management of per-vtab state of an SQLite
// virtual table module.
//
// SQLite has some subtle semantics around lifecycle of vtabs which makes state
// management complex. This class attempts to encapsulate some of that
// complexity as a central place where we can document the quirks.
//
// Usage of this class:
// struct MyModule : sqlite::Module<MyModule> {
//   struct Context {
//     // Store the manager in the context object.
//     ModuleStateManager<MyModule> manager.
//     ... (other fields)
//   }
//   struct Vtab : sqlite::Module<MyModule>::Vtab {
//     // Store the per-vtab-state pointer in the vtab object.
//     ModuleStateManager<MyModule>::PerVtabState* state;
//     ... (other fields)
//   }
//   static void OnCreate(...) {
//     ...
//     // Call OnCreate on the manager object and store the returned pointer.
//     tab->state = ctx->manager.OnCreate(argv);
//     ...
//   }
//   static void OnDestroy(...) {
//     ...
//     // Call OnDestroy with the stored state pointer.
//     sqlite::ModuleStateManager<MyModule>::OnDestroy(tab->state);
//     ...
//   }
//   // Do the same in OnConnect and OnDisconnect as in OnCreate and OnDestroy
//   // respectively.
//   static void OnConnect(...)
//   static void OnDisconnect(...)
// }
template <typename Module>
class ModuleStateManager {
 public:
  // Per-vtab state. The pointer to this class should be stored in the Vtab.
  struct PerVtabState {
   private:
    // The below fields should only be accessed by the manager, use GetState to
    // access the state from outside this class.
    friend class ModuleStateManager<Module>;

    ModuleStateManager* manager;
    bool disconnected = false;
    std::string table_name;
    std::unique_ptr<typename Module::State> state;
  };

  // Lifecycle method to be called from Module::Create.
  [[nodiscard]] PerVtabState* OnCreate(
      const char* const* argv,
      std::unique_ptr<typename Module::State> state) {
    auto it_and_inserted = state_by_name_.Insert(argv[2], nullptr);
    PERFETTO_CHECK(
        it_and_inserted.second ||
        (it_and_inserted.first && it_and_inserted.first->get()->disconnected));

    auto s = std::make_unique<PerVtabState>();
    auto* s_ptr = s.get();
    *it_and_inserted.first = std::move(s);

    s_ptr->manager = this;
    s_ptr->table_name = argv[2];
    s_ptr->state = std::move(state);
    return it_and_inserted.first->get();
  }

  // Lifecycle method to be called from Module::Connect.
  [[nodiscard]] PerVtabState* OnConnect(const char* const* argv) {
    auto* ptr = state_by_name_.Find(argv[2]);
    PERFETTO_CHECK(ptr);
    ptr->get()->disconnected = false;
    return ptr->get();
  }

  // Lifecycle method to be called from Module::Disconnect.
  static void OnDisconnect(PerVtabState* state) {
    auto* ptr = state->manager->state_by_name_.Find(state->table_name);
    PERFETTO_CHECK(ptr);
    ptr->get()->disconnected = true;
  }

  // Lifecycle method to be called from Module::Destroy.
  static void OnDestroy(PerVtabState* state) {
    PERFETTO_CHECK(state->manager->state_by_name_.Erase(state->table_name));
  }

  // Method to be called from module callbacks to extract the module state
  // from the manager state.
  static typename Module::State* GetState(PerVtabState* s) {
    return s->state.get();
  }

  // Looks up the state of a module by name. This function should only be called
  // for speculative lookups from outside the module implementation: use
  // |GetState| inside the sqlite::Module implementation.
  typename Module::State* FindStateByName(std::string_view name) {
    if (auto ptr = state_by_name_.Find(std::string(name)); ptr) {
      return GetState(ptr->get());
    }
    return nullptr;
  }

 private:
  base::FlatHashMap<std::string, std::unique_ptr<PerVtabState>> state_by_name_;
};

}  // namespace perfetto::trace_processor::sqlite

#endif  // SRC_TRACE_PROCESSOR_SQLITE_MODULE_LIFECYCLE_MANAGER_H_
