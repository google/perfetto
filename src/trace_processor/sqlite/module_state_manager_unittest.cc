/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include <memory>
#include <string>
#include <utility>

#include "perfetto/base/compiler.h"
#include "src/trace_processor/sqlite/committed_state_manager.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::sqlite {
namespace {

// Exposes the protected lifecycle methods so tests can drive them
// without going through SQLite's vtab callbacks.
class TestStateManager : public ModuleStateManagerBase {
 public:
  explicit TestStateManager(CommittedStateManager& store)
      : ModuleStateManagerBase(store) {}

  using ModuleStateManagerBase::GetState;
  using ModuleStateManagerBase::OnConnect;
  using ModuleStateManagerBase::OnCreate;
  using ModuleStateManagerBase::OnDestroy;
};

struct Payload {
  int value = 0;
};

std::unique_ptr<void, void (*)(void*)> MakeErasedPayload(int v) {
  return std::unique_ptr<void, void (*)(void*)>(
      new Payload{v}, [](void* p) { delete static_cast<Payload*>(p); });
}

const char* const kArgvVt[] = {"db", "module", "vt1"};

// A manager with no local state falls back to peer-committed state.
TEST(ModuleStateManagerTest, ColdAttachReadsPeerCommittedState) {
  CommittedStateManager store;
  TestStateManager writer(store);
  TestStateManager reader(store);

  base::ignore_result(writer.OnCreate(3, kArgvVt, MakeErasedPayload(42)));
  writer.OnCommit();

  auto* peer_state = reader.OnConnect(3, kArgvVt);
  ASSERT_NE(peer_state, nullptr);
  auto* payload = static_cast<Payload*>(TestStateManager::GetState(peer_state));
  ASSERT_NE(payload, nullptr);
  EXPECT_EQ(payload->value, 42);
}

// An unknown vtab name with no peer committed state returns nullptr.
TEST(ModuleStateManagerTest, ColdAttachUnknownNameReturnsNull) {
  CommittedStateManager store;
  TestStateManager reader(store);

  EXPECT_EQ(reader.OnConnect(3, kArgvVt), nullptr);
}

// Uncommitted state on a peer is invisible until OnCommit.
TEST(ModuleStateManagerTest, ColdAttachIgnoresUncommittedState) {
  CommittedStateManager store;
  TestStateManager writer(store);
  TestStateManager reader(store);

  base::ignore_result(writer.OnCreate(3, kArgvVt, MakeErasedPayload(7)));
  EXPECT_EQ(reader.OnConnect(3, kArgvVt), nullptr);
}

// A peer's rolled-back CREATE publishes nothing.
TEST(ModuleStateManagerTest, ColdAttachAfterPeerRollback) {
  CommittedStateManager store;
  TestStateManager writer(store);
  TestStateManager reader(store);

  base::ignore_result(writer.OnCreate(3, kArgvVt, MakeErasedPayload(99)));
  writer.OnRollback();
  EXPECT_EQ(reader.OnConnect(3, kArgvVt), nullptr);
}

// A second OnConnect for the same vtab returns the cached local instance.
TEST(ModuleStateManagerTest, ColdAttachIsIdempotentOnSecondConnect) {
  CommittedStateManager store;
  TestStateManager writer(store);
  TestStateManager reader(store);

  base::ignore_result(writer.OnCreate(3, kArgvVt, MakeErasedPayload(1)));
  writer.OnCommit();

  auto* first = reader.OnConnect(3, kArgvVt);
  auto* second = reader.OnConnect(3, kArgvVt);
  EXPECT_EQ(first, second);
}

}  // namespace
}  // namespace perfetto::trace_processor::sqlite
