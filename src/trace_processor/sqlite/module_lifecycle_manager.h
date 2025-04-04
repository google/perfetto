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
#include <utility>
#include <vector>

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
    std::string table_name;
    std::unique_ptr<typename Module::State> state;

    // Tracks whether a `Commit` call has been seen while the vtab's state
    // was being tracked.
    bool create_committed = false;
  };

  // Lifecycle method to be called from Module::Create.
  [[nodiscard]] PerVtabState* OnCreate(
      const char* const* argv,
      std::unique_ptr<typename Module::State> state) {
    auto [it, inserted] =
        state_by_name_.Insert(argv[2], std::make_unique<PerVtabState>());
    PERFETTO_CHECK(inserted);

    auto* s_ptr = it->get();
    s_ptr->manager = this;
    s_ptr->table_name = argv[2];
    s_ptr->state = std::move(state);
    return s_ptr;
  }

  // Lifecycle method to be called from Module::Connect.
  [[nodiscard]] PerVtabState* OnConnect(const char* const* argv) {
    auto* ptr = state_by_name_.Find(argv[2]);
    PERFETTO_CHECK(ptr);
    return ptr->get();
  }

  // Lifecycle method to be called from Module::Disconnect.
  static void OnDisconnect(PerVtabState* state) {
    auto* ptr = state->manager->state_by_name_.Find(state->table_name);
    PERFETTO_CHECK(ptr);
    PERFETTO_CHECK(ptr->get() == state);
  }

  // Lifecycle method to be called from Module::Destroy.
  static void OnDestroy(PerVtabState* state) {
    auto* ptr = state->manager->state_by_name_.Find(state->table_name);
    PERFETTO_CHECK(ptr);
    PERFETTO_CHECK(ptr->get() == state);
    if (state->create_committed) {
      state->manager->destroyed_state_by_name_.Insert(state->table_name,
                                                      std::move(*ptr));
    }
    PERFETTO_CHECK(state->manager->state_by_name_.Erase(state->table_name));
  }

  // Called by the engine when a transaction is rolled back.
  //
  // This is used to undo the effects of all the destroys performed since a
  // previous rollback or commit.
  void OnRollback() {
    std::vector<std::string> to_erase;
    for (auto it = state_by_name_.GetIterator(); it; ++it) {
      if (!it.value()->create_committed) {
        to_erase.push_back(it.key());
      }
    }
    for (const auto& name : to_erase) {
      state_by_name_.Erase(name);
    }
    for (auto it = destroyed_state_by_name_.GetIterator(); it; ++it) {
      state_by_name_.Insert(it.key(), std::move(it.value()));
    }
    destroyed_state_by_name_.Clear();
  }

  // Called by the engine when a transaction is committed.
  //
  // This is used to finalize all the destroys performed since a previous
  // rollback or commit.
  void OnCommit() {
    for (auto it = state_by_name_.GetIterator(); it; ++it) {
      it.value()->create_committed = true;
    }
    destroyed_state_by_name_.Clear();
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
  using StateMap =
      base::FlatHashMap<std::string, std::unique_ptr<PerVtabState>>;
  StateMap state_by_name_;
  StateMap destroyed_state_by_name_;
};

}  // namespace perfetto::trace_processor::sqlite

#endif  // SRC_TRACE_PROCESSOR_SQLITE_MODULE_LIFECYCLE_MANAGER_H_
