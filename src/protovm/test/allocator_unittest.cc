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

#include "perfetto/protozero/field.h"
#include "test/gtest_and_gmock.h"

#include "src/protovm/allocator.h"
#include "src/protovm/node.h"

namespace perfetto {
namespace protovm {
namespace test {

class AllocatorTest : public ::testing::Test {
 protected:
  static constexpr size_t kCapacity = 10;
  static constexpr size_t kMemoryLimitBytes = kCapacity * sizeof(Node);
  Allocator allocator_{kMemoryLimitBytes};
};

TEST_F(AllocatorTest, NodeAllocationRespectsMemoryLimit) {
  auto nodes = std::vector<OwnedPtr<Node>>{};

  for (size_t i = 0; i < kCapacity; ++i) {
    auto node = allocator_.CreateNode<Node::Empty>();
    ASSERT_TRUE(node.IsOk());
    nodes.push_back(std::move(*node));
  }

  auto node = allocator_.CreateNode<Node::Empty>();
  ASSERT_FALSE(node.IsOk());

  for (auto& n : nodes) {
    allocator_.Delete(n.release());
  }
}

TEST_F(AllocatorTest, BytesAllocationRespectsMemoryLimit) {
  auto bytes = std::vector<Node::Bytes>{};

  auto bytes0 = std::vector<std::uint8_t>(kMemoryLimitBytes / 2);
  auto bytes1 = std::vector<std::uint8_t>(kMemoryLimitBytes - bytes0.size());

  auto copy0 = allocator_.AllocateAndCopyBytes(
      protozero::ConstBytes{bytes0.data(), bytes0.size()});
  ASSERT_TRUE(copy0.IsOk());

  auto copy1 = allocator_.AllocateAndCopyBytes(
      protozero::ConstBytes{bytes1.data(), bytes1.size()});
  ASSERT_TRUE(copy1.IsOk());

  auto copy_fail = allocator_.AllocateAndCopyBytes(
      protozero::ConstBytes{bytes0.data(), bytes0.size()});
  ASSERT_FALSE(copy_fail.IsOk());

  auto node0 = Node{std::move(*copy0)};
  allocator_.DeleteReferencedData(&node0);

  auto node1 = Node{std::move(*copy1)};
  allocator_.DeleteReferencedData(&node1);
}

}  // namespace test
}  // namespace protovm
}  // namespace perfetto
