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

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/hash.h"

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

    // The name of the vtab.
    std::string name;

    // A hash of all the arguments passed to the module from SQLite. This
    // acts as the unique identifier for the vtab state.
    uint64_t argv_hash;

    // A pointer to the manager object. Backreference for use by static
    // functions in this class.
    ModuleStateManager* manager;

    // The actual state object which will be used by the module.
    std::unique_ptr<typename Module::State> state;

    enum {
      kCommitted,
      kCreatedButNotCommitted,
      kDestroyedButNotCommitted
    } lifecycle;
  };

  // Lifecycle method to be called from Module::Create.
  [[nodiscard]] PerVtabState* OnCreate(
      int argc,
      const char* const* argv,
      std::unique_ptr<typename Module::State> state) {
    uint64_t hash = ComputeHash(argc, argv);
    auto [it, inserted] =
        state_by_args_hash_.Insert(hash, std::make_unique<PerVtabState>());

    // Note to future readers: if you find this check failing, that means that
    // multiple vtabs have been created with the same arguments inside a single
    // transaction. We explicitly choose not to handle this because it's very
    // difficult to do so correctly and we never expect this to be hit in normal
    // usage (both in terms of transactions and virtual table design).
    //
    // Specifically, the case this would happen is if we did:
    // ```sql
    // BEGIN;
    // -- xCreate will be called.
    // CREATE VIRTUAL TABLE t1 USING foo(arg);
    // -- xDestroy will be called.
    // DROP TABLE t1
    // -- xCreate will be called again with the same arguments.
    // -- Crash will happen here!
    // CREATE VIRTUAL TABLE t1 USING foo(arg);
    // ```
    //
    // You could say: let's instead just keep track of the destroyed state
    // in a separate map and then reinsert it into the main map on rollback.
    // Unfortunately, the problem with this is that it would break in the
    // presence of SAVEPOINTs. Consider:
    //
    // ```sql
    // BEGIN;
    // -- xCreate will be called.
    // CREATE VIRTUAL TABLE t1 USING foo(arg);
    // SAVEPOINT s1;
    // -- xDestroy will be called.
    // DROP TABLE t1;
    // -- Even though we have the same args as the previous instance of t1,
    // -- it has different state.
    // CREATE VIRTUAL TABLE t1 USING foo(arg);
    // INSERT INTO t1 VALUES (1);
    // -- SQLite does not provide a way for us to get a callback when a ROLLBACK
    // -- TO/release operation happens so this is totally transparent to us.
    // -- We don't even get a xDisconnect callback!
    // ROLLBACK TO s1;
    // RELEASE s1;
    // -- xConnect will happen here. But which instance of t1 should we use?
    // -- We have no way of knowing! So we instead just ban the situation where
    // -- two vtabs with the same args are created in a single transaction.
    // CREATE VIRTUAL TABLE t1 USING foo(arg);
    // ```
    //
    // The workaround for this: all virtual tables in trace processor should be
    // carefully designed such that the arguments known to SQLite uniquely
    // identify the state. That way, even if two tables have the same name (i.e.
    // argv[2]), they will have different state.
    PERFETTO_CHECK(inserted);

    auto* s_ptr = it->get();
    s_ptr->manager = this;
    s_ptr->name = std::string(argv[2]);
    s_ptr->argv_hash = hash;
    s_ptr->state = std::move(state);
    s_ptr->lifecycle = PerVtabState::kCreatedButNotCommitted;
    return s_ptr;
  }

  // Lifecycle method to be called from Module::Connect.
  [[nodiscard]] PerVtabState* OnConnect(int argc, const char* const* argv) {
    auto* ptr = state_by_args_hash_.Find(ComputeHash(argc, argv));
    PERFETTO_CHECK(ptr);
    return ptr->get();
  }

  // Lifecycle method to be called from Module::Disconnect.
  static void OnDisconnect(PerVtabState* state) {
    auto* ptr = state->manager->state_by_args_hash_.Find(state->argv_hash);
    PERFETTO_CHECK(ptr);
    PERFETTO_CHECK(ptr->get() == state);
  }

  // Lifecycle method to be called from Module::Destroy.
  static void OnDestroy(PerVtabState* state) {
    auto* ptr = state->manager->state_by_args_hash_.Find(state->argv_hash);
    PERFETTO_CHECK(ptr);
    PERFETTO_CHECK(ptr->get() == state);
    state->lifecycle = PerVtabState::kDestroyedButNotCommitted;
  }

  // Called by the engine when a transaction is committed.
  //
  // This is used to finalize all the destroys performed since a previous
  // rollback or commit.
  void OnCommit() {
    std::vector<uint64_t> to_erase;
    for (auto it = state_by_args_hash_.GetIterator(); it; ++it) {
      if (it.value()->lifecycle == PerVtabState::kDestroyedButNotCommitted) {
        to_erase.push_back(it.key());
      } else {
        it.value()->lifecycle = PerVtabState::kCommitted;
      }
    }
    for (const auto& hash : to_erase) {
      state_by_args_hash_.Erase(hash);
    }
  }

  // Called by the engine when a transaction is rolled back.
  //
  // This is used to undo the effects of all the destroys performed since a
  // previous rollback or commit.
  void OnRollback() {
    std::vector<uint64_t> to_erase;
    for (auto it = state_by_args_hash_.GetIterator(); it; ++it) {
      if (it.value()->lifecycle == PerVtabState::kCreatedButNotCommitted) {
        to_erase.push_back(it.key());
      } else {
        it.value()->lifecycle = PerVtabState::kCommitted;
      }
    }
    for (const auto& hash : to_erase) {
      state_by_args_hash_.Erase(hash);
    }
  }

  // Method to be called from module callbacks to extract the module state
  // from the manager state.
  static typename Module::State* GetState(PerVtabState* s) {
    return s->state.get();
  }

  // Looks up the state of a module by name in O(n) time. This function should
  // not be called in the performance sensitive contexts. It must also be called
  // in a case where there are not multiple vtabs with the same name. This can
  // happen inside a transaction context where we are executing a "CREATE OR
  // REPLACE" operation.
  //
  // This function should only be called for speculative lookups from outside
  // the module implementation: use |GetState| inside the sqlite::Module
  // implementation.
  typename Module::State* FindStateByNameSlow(std::string_view name) {
    typename Module::State* res = nullptr;
    for (auto it = state_by_args_hash_.GetIterator(); it; ++it) {
      if (it.value()->name == name) {
        // This means that there are multiple vtabs with the same name.
        // According to the precondition of this function, this is not allowed.
        PERFETTO_CHECK(!res);
        res = GetState(it.value().get());
      }
    }
    return res;
  }

 private:
  using StateMap = base::FlatHashMap<uint64_t,
                                     std::unique_ptr<PerVtabState>,
                                     base::AlreadyHashed<uint64_t>>;

  uint64_t ComputeHash(int argc, const char* const* argv) {
    base::Hasher hasher;
    for (int i = 0; i < argc; ++i) {
      hasher.Update(argv[i]);
    }
    return hasher.digest();
  }

  StateMap state_by_args_hash_;
};

}  // namespace perfetto::trace_processor::sqlite

#endif  // SRC_TRACE_PROCESSOR_SQLITE_MODULE_LIFECYCLE_MANAGER_H_
