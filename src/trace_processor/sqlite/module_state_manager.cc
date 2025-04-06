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

#include "src/trace_processor/sqlite/module_state_manager.h"
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/hash.h"

namespace perfetto::trace_processor::sqlite {

ModuleStateManagerBase::PerVtabState* ModuleStateManagerBase::OnCreate(
    int argc,
    const char* const* argv,
    std::unique_ptr<void, void (*)(void*)> state) {
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

ModuleStateManagerBase::PerVtabState* ModuleStateManagerBase::OnConnect(
    int argc,
    const char* const* argv) {
  auto* ptr = state_by_args_hash_.Find(ComputeHash(argc, argv));
  PERFETTO_CHECK(ptr);
  return ptr->get();
}

void ModuleStateManagerBase::OnDisconnect(PerVtabState* state) {
  auto* ptr = state->manager->state_by_args_hash_.Find(state->argv_hash);
  PERFETTO_CHECK(ptr);
  PERFETTO_CHECK(ptr->get() == state);
}

void ModuleStateManagerBase::OnDestroy(PerVtabState* state) {
  auto* ptr = state->manager->state_by_args_hash_.Find(state->argv_hash);
  PERFETTO_CHECK(ptr);
  PERFETTO_CHECK(ptr->get() == state);
  state->lifecycle = PerVtabState::kDestroyedButNotCommitted;
}

void* ModuleStateManagerBase::GetState(PerVtabState* s) {
  return s->state.get();
}

void* ModuleStateManagerBase::FindStateByNameSlow(std::string_view name) {
  void* res = nullptr;
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

uint64_t ModuleStateManagerBase::ComputeHash(int argc,
                                             const char* const* argv) {
  base::Hasher hash;
  for (int i = 0; i < argc; i++) {
    hash.Update(argv[i]);
  }
  return hash.digest();
}

}  // namespace perfetto::trace_processor::sqlite
