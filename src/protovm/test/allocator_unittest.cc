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
  ASSERT_EQ(allocator_.GetMemoryUsageBytes(), 0u);

  // Allocate N nodes
  auto nodes = std::vector<OwnedPtr<Node>>{};
  for (size_t i = 0; i < kCapacity; ++i) {
    auto prev_memory_usage = allocator_.GetMemoryUsageBytes();
    auto node = allocator_.CreateNode<Node::Empty>();
    ASSERT_TRUE(node.IsOk());
    ASSERT_GT(allocator_.GetMemoryUsageBytes(), prev_memory_usage);
    nodes.push_back(std::move(*node));
  }

  // Failed node allocation (memory limit reached)
  {
    auto prev_memory_usage = allocator_.GetMemoryUsageBytes();
    auto node_fail = allocator_.CreateNode<Node::Empty>();
    ASSERT_FALSE(node_fail.IsOk());
    ASSERT_EQ(allocator_.GetMemoryUsageBytes(), prev_memory_usage);
  }

  // Delete one node
  {
    auto prev_memory_usage = allocator_.GetMemoryUsageBytes();
    allocator_.Delete(nodes.back().release());
    nodes.pop_back();
    ASSERT_LT(allocator_.GetMemoryUsageBytes(), prev_memory_usage);
  }

  // Successfull node allocation (verify that previous deletion actually freed
  // memory for one node)
  {
    auto prev_memory_usage = allocator_.GetMemoryUsageBytes();
    auto node_success = allocator_.CreateNode<Node::Empty>();
    ASSERT_TRUE(node_success.IsOk());
    ASSERT_GT(allocator_.GetMemoryUsageBytes(), prev_memory_usage);
    nodes.push_back(node_success->release());
  }

  // Delete all nodes
  for (auto& n : nodes) {
    auto prev_memory_usage = allocator_.GetMemoryUsageBytes();
    allocator_.Delete(n.release());
    ASSERT_LT(allocator_.GetMemoryUsageBytes(), prev_memory_usage);
  }

  ASSERT_EQ(allocator_.GetMemoryUsageBytes(), 0u);
}

TEST_F(AllocatorTest, BytesAllocationRespectsMemoryLimit) {
  auto bytes0 = std::vector<std::uint8_t>(kMemoryLimitBytes / 2);
  auto bytes1 = std::vector<std::uint8_t>(kMemoryLimitBytes - bytes0.size());

  // Successfully allocate copy0 and copy1 (reach memory limit)
  auto copy0 = allocator_.AllocateAndCopyBytes(
      protozero::ConstBytes{bytes0.data(), bytes0.size()});
  ASSERT_TRUE(copy0.IsOk());
  ASSERT_EQ(allocator_.GetMemoryUsageBytes(), bytes0.size());

  auto copy1 = allocator_.AllocateAndCopyBytes(
      protozero::ConstBytes{bytes1.data(), bytes1.size()});
  ASSERT_TRUE(copy1.IsOk());
  ASSERT_EQ(allocator_.GetMemoryUsageBytes(), bytes0.size() + bytes1.size());

  // Failed allocation
  // (verify that previous allocations affected memory usage)
  auto copy_fail = allocator_.AllocateAndCopyBytes(
      protozero::ConstBytes{bytes0.data(), bytes0.size()});
  ASSERT_FALSE(copy_fail.IsOk());
  ASSERT_EQ(allocator_.GetMemoryUsageBytes(), bytes0.size() + bytes1.size());

  // Delete copy1
  auto node1 = Node{std::move(*copy1)};
  allocator_.DeleteReferencedData(&node1);
  ASSERT_EQ(allocator_.GetMemoryUsageBytes(), bytes0.size());

  // Successfully allocate copy11
  // (verify that previous deletion affected memory usage)
  auto copy11 = allocator_.AllocateAndCopyBytes(
      protozero::ConstBytes{bytes1.data(), bytes1.size()});
  ASSERT_TRUE(copy11.IsOk());
  ASSERT_EQ(allocator_.GetMemoryUsageBytes(), bytes0.size() + bytes1.size());

  // Delete
  auto node0 = Node{std::move(*copy0)};
  allocator_.DeleteReferencedData(&node0);
  ASSERT_EQ(allocator_.GetMemoryUsageBytes(), bytes1.size());

  auto node11 = Node{std::move(*copy11)};
  allocator_.DeleteReferencedData(&node11);
  ASSERT_EQ(allocator_.GetMemoryUsageBytes(), 0u);
}

}  // namespace test
}  // namespace protovm
}  // namespace perfetto
