/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "perfetto/ext/trace_processor/importers/memory_tracker/raw_process_memory_node.h"

#include <stddef.h>

#include "perfetto/base/build_config.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {

namespace {

const LevelOfDetail kLevelOfDetail = LevelOfDetail::kDetailed;

}  // namespace

TEST(RawProcessMemoryNodeTest, MoveConstructor) {
  const auto source = MemoryAllocatorNodeId(42);
  const auto target = MemoryAllocatorNodeId(4242);

  std::unique_ptr<RawMemoryGraphNode> mad1(
      new RawMemoryGraphNode("mad1", kLevelOfDetail, source));
  std::unique_ptr<RawMemoryGraphNode> mad2(
      new RawMemoryGraphNode("mad2", kLevelOfDetail, target));

  RawProcessMemoryNode::MemoryNodesMap nodesMap;
  nodesMap.emplace(mad1->absolute_name(), std::move(mad1));
  nodesMap.emplace(mad2->absolute_name(), std::move(mad2));

  std::unique_ptr<MemoryGraphEdge> edge(
      new MemoryGraphEdge(source, target, 10, false));

  RawProcessMemoryNode::AllocatorNodeEdgesMap edgesMap;
  edgesMap.emplace(edge->source, std::move(edge));

  RawProcessMemoryNode pmd1(kLevelOfDetail, std::move(edgesMap),
                            std::move(nodesMap));

  RawProcessMemoryNode pmd2(std::move(pmd1));

  EXPECT_EQ(1u, pmd2.allocator_nodes().count("mad1"));
  EXPECT_EQ(1u, pmd2.allocator_nodes().count("mad2"));
  EXPECT_EQ(LevelOfDetail::kDetailed, pmd2.level_of_detail());
  EXPECT_EQ(1u, pmd2.allocator_nodes_edges().size());
}

TEST(RawProcessMemoryNodeTest, MoveAssignment) {
  const auto source = MemoryAllocatorNodeId(42);
  const auto target = MemoryAllocatorNodeId(4242);

  std::unique_ptr<RawMemoryGraphNode> mad1(
      new RawMemoryGraphNode("mad1", kLevelOfDetail, source));
  std::unique_ptr<RawMemoryGraphNode> mad2(
      new RawMemoryGraphNode("mad2", kLevelOfDetail, target));

  RawProcessMemoryNode::MemoryNodesMap nodesMap;
  nodesMap.emplace(mad1->absolute_name(), std::move(mad1));
  nodesMap.emplace(mad2->absolute_name(), std::move(mad2));

  std::unique_ptr<MemoryGraphEdge> edge(
      new MemoryGraphEdge(source, target, 10, false));

  RawProcessMemoryNode::AllocatorNodeEdgesMap edgesMap;
  edgesMap.emplace(edge->source, std::move(edge));

  RawProcessMemoryNode pmd1(kLevelOfDetail, std::move(edgesMap),
                            std::move(nodesMap));

  RawProcessMemoryNode pmd2(LevelOfDetail::kBackground);

  pmd2 = std::move(pmd1);
  EXPECT_EQ(1u, pmd2.allocator_nodes().count("mad1"));
  EXPECT_EQ(1u, pmd2.allocator_nodes().count("mad2"));
  EXPECT_EQ(0u, pmd2.allocator_nodes().count("mad3"));
  EXPECT_EQ(LevelOfDetail::kDetailed, pmd2.level_of_detail());
  EXPECT_EQ(1u, pmd2.allocator_nodes_edges().size());
}

}  // namespace trace_processor
}  // namespace perfetto
