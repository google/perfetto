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

#include "perfetto/ext/trace_processor/importers/memory_tracker/graph_processor.h"

#include <stddef.h>

#include "perfetto/base/build_config.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {

using Edge = GlobalNodeGraph::Edge;
using Node = GlobalNodeGraph::Node;
using Process = GlobalNodeGraph::Process;

namespace {

const MemoryAllocatorNodeId kEmptyId;

}  // namespace

class GraphProcessorTest : public testing::Test {
 public:
  GraphProcessorTest() {}

  void MarkImplicitWeakParentsRecursively(Node* node) {
    GraphProcessor::MarkImplicitWeakParentsRecursively(node);
  }

  void MarkWeakOwnersAndChildrenRecursively(Node* node) {
    std::set<const Node*> visited;
    GraphProcessor::MarkWeakOwnersAndChildrenRecursively(node, &visited);
  }

  void RemoveWeakNodesRecursively(Node* node) {
    GraphProcessor::RemoveWeakNodesRecursively(node);
  }

  void AssignTracingOverhead(const std::string& allocator,
                             GlobalNodeGraph* global_graph,
                             Process* process) {
    GraphProcessor::AssignTracingOverhead(allocator, global_graph, process);
  }

  GlobalNodeGraph::Node::Entry AggregateNumericWithNameForNode(
      Node* node,
      const std::string& name) {
    return GraphProcessor::AggregateNumericWithNameForNode(node, name);
  }

  void AggregateNumericsRecursively(Node* node) {
    GraphProcessor::AggregateNumericsRecursively(node);
  }

  void PropagateNumericsAndDiagnosticsRecursively(Node* node) {
    GraphProcessor::PropagateNumericsAndDiagnosticsRecursively(node);
  }

  base::Optional<uint64_t> AggregateSizeForDescendantNode(Node* root,
                                                          Node* descendant) {
    return GraphProcessor::AggregateSizeForDescendantNode(root, descendant);
  }

  void CalculateSizeForNode(Node* node) {
    GraphProcessor::CalculateSizeForNode(node);
  }

  void CalculateNodeSubSizes(Node* node) {
    GraphProcessor::CalculateNodeSubSizes(node);
  }

  void CalculateNodeOwnershipCoefficient(Node* node) {
    GraphProcessor::CalculateNodeOwnershipCoefficient(node);
  }

  void CalculateNodeCumulativeOwnershipCoefficient(Node* node) {
    GraphProcessor::CalculateNodeCumulativeOwnershipCoefficient(node);
  }

  void CalculateNodeEffectiveSize(Node* node) {
    GraphProcessor::CalculateNodeEffectiveSize(node);
  }

 protected:
  GlobalNodeGraph graph;
};

TEST_F(GraphProcessorTest, SmokeComputeMemoryGraph) {
  std::map<base::PlatformProcessId, std::unique_ptr<RawProcessMemoryNode>>
      process_nodes;

  std::unique_ptr<RawMemoryGraphNode> source(new RawMemoryGraphNode(
      "test1/test2/test3", LevelOfDetail::kDetailed, MemoryAllocatorNodeId(42),
      std::vector<RawMemoryGraphNode::MemoryNodeEntry>{
          {RawMemoryGraphNode::kNameSize, RawMemoryGraphNode::kUnitsBytes,
           10}}));

  std::unique_ptr<RawMemoryGraphNode> target(new RawMemoryGraphNode(
      "target", LevelOfDetail::kDetailed, MemoryAllocatorNodeId(4242)));

  std::unique_ptr<MemoryGraphEdge> edge(
      new MemoryGraphEdge(source->id(), target->id(), 10, false));
  RawProcessMemoryNode::AllocatorNodeEdgesMap edgesMap;
  edgesMap.emplace(edge->source, std::move(edge));

  RawProcessMemoryNode::MemoryNodesMap nodesMap;
  nodesMap.emplace(source->absolute_name(), std::move(source));
  nodesMap.emplace(target->absolute_name(), std::move(target));

  auto pmd = std::unique_ptr<RawProcessMemoryNode>(new RawProcessMemoryNode(
      LevelOfDetail::kDetailed, std::move(edgesMap), std::move(nodesMap)));
  process_nodes.emplace(1, std::move(pmd));

  auto global_node = GraphProcessor::CreateMemoryGraph(process_nodes);

  ASSERT_EQ(1u, global_node->process_node_graphs().size());

  auto id_to_node_it = global_node->process_node_graphs().find(1);
  auto* first_child = id_to_node_it->second->FindNode("test1");
  ASSERT_NE(first_child, nullptr);
  ASSERT_EQ(first_child->parent(), id_to_node_it->second->root());

  auto* second_child = first_child->GetChild("test2");
  ASSERT_NE(second_child, nullptr);
  ASSERT_EQ(second_child->parent(), first_child);

  auto* third_child = second_child->GetChild("test3");
  ASSERT_NE(third_child, nullptr);
  ASSERT_EQ(third_child->parent(), second_child);

  auto* direct = id_to_node_it->second->FindNode("test1/test2/test3");
  ASSERT_EQ(third_child, direct);

  ASSERT_EQ(third_child->entries()->size(), 1ul);

  auto size = third_child->entries()->find(RawMemoryGraphNode::kNameSize);
  ASSERT_EQ(10ul, size->second.value_uint64);

  auto& edges = global_node->edges();
  auto edge_it = edges.begin();
  ASSERT_EQ(std::distance(edges.begin(), edges.end()), 1l);
  ASSERT_EQ(edge_it->source(), direct);
  ASSERT_EQ(edge_it->target(), id_to_node_it->second->FindNode("target"));
  ASSERT_EQ(edge_it->priority(), 10);
}

TEST_F(GraphProcessorTest, ComputeSharedFootprintFromGraphSameImportance) {
  Process* global_process = graph.shared_memory_graph();
  Node* global_node = global_process->CreateNode(kEmptyId, "global/1", false);
  global_node->AddEntry("size", Node::Entry::ScalarUnits::kBytes, 100);

  Process* first = graph.CreateGraphForProcess(1);
  Node* shared_1 = first->CreateNode(kEmptyId, "shared_memory/1", false);

  Process* second = graph.CreateGraphForProcess(2);
  Node* shared_2 = second->CreateNode(kEmptyId, "shared_memory/2", false);

  graph.AddNodeOwnershipEdge(shared_1, global_node, 1);
  graph.AddNodeOwnershipEdge(shared_2, global_node, 1);

  auto pid_to_sizes = GraphProcessor::ComputeSharedFootprintFromGraph(graph);
  ASSERT_EQ(pid_to_sizes[1], 50ul);
  ASSERT_EQ(pid_to_sizes[2], 50ul);
}

TEST_F(GraphProcessorTest, ComputeSharedFootprintFromGraphSomeDiffImportance) {
  Process* global_process = graph.shared_memory_graph();

  Node* global_node = global_process->CreateNode(kEmptyId, "global/1", false);
  global_node->AddEntry("size", Node::Entry::ScalarUnits::kBytes, 100);

  Process* first = graph.CreateGraphForProcess(1);
  Node* shared_1 = first->CreateNode(kEmptyId, "shared_memory/1", false);

  Process* second = graph.CreateGraphForProcess(2);
  Node* shared_2 = second->CreateNode(kEmptyId, "shared_memory/2", false);

  Process* third = graph.CreateGraphForProcess(3);
  Node* shared_3 = third->CreateNode(kEmptyId, "shared_memory/3", false);

  Process* fourth = graph.CreateGraphForProcess(4);
  Node* shared_4 = fourth->CreateNode(kEmptyId, "shared_memory/4", false);

  Process* fifth = graph.CreateGraphForProcess(5);
  Node* shared_5 = fifth->CreateNode(kEmptyId, "shared_memory/5", false);

  graph.AddNodeOwnershipEdge(shared_1, global_node, 1);
  graph.AddNodeOwnershipEdge(shared_2, global_node, 2);
  graph.AddNodeOwnershipEdge(shared_3, global_node, 3);
  graph.AddNodeOwnershipEdge(shared_4, global_node, 3);
  graph.AddNodeOwnershipEdge(shared_5, global_node, 3);

  auto pid_to_sizes = GraphProcessor::ComputeSharedFootprintFromGraph(graph);
  ASSERT_EQ(pid_to_sizes[1], 0ul);
  ASSERT_EQ(pid_to_sizes[2], 0ul);
  ASSERT_EQ(pid_to_sizes[3], 33ul);
  ASSERT_EQ(pid_to_sizes[4], 33ul);
  ASSERT_EQ(pid_to_sizes[5], 33ul);
}

TEST_F(GraphProcessorTest, MarkWeakParentsSimple) {
  Process* process = graph.CreateGraphForProcess(1);
  Node* parent = process->CreateNode(kEmptyId, "parent", false);
  Node* first = process->CreateNode(kEmptyId, "parent/first", true);
  Node* second = process->CreateNode(kEmptyId, "parent/second", false);

  // Case where one child is not weak.
  parent->set_explicit(false);
  first->set_explicit(true);
  second->set_explicit(true);

  // The function should be a no-op.
  MarkImplicitWeakParentsRecursively(parent);
  ASSERT_FALSE(parent->is_weak());
  ASSERT_TRUE(first->is_weak());
  ASSERT_FALSE(second->is_weak());

  // Case where all children is weak.
  second->set_weak(true);

  // The function should mark parent as weak.
  MarkImplicitWeakParentsRecursively(parent);
  ASSERT_TRUE(parent->is_weak());
  ASSERT_TRUE(first->is_weak());
  ASSERT_TRUE(second->is_weak());
}

TEST_F(GraphProcessorTest, MarkWeakParentsComplex) {
  Process* process = graph.CreateGraphForProcess(1);

  // |first| is explicitly strong but |first_child| is implicitly so.
  Node* parent = process->CreateNode(kEmptyId, "parent", false);
  Node* first = process->CreateNode(kEmptyId, "parent/f", false);
  Node* first_child = process->CreateNode(kEmptyId, "parent/f/c", false);
  Node* first_gchild = process->CreateNode(kEmptyId, "parent/f/c/c", true);

  parent->set_explicit(false);
  first->set_explicit(true);
  first_child->set_explicit(false);
  first_gchild->set_explicit(true);

  // That should lead to |first_child| marked implicitly weak.
  MarkImplicitWeakParentsRecursively(parent);
  ASSERT_FALSE(parent->is_weak());
  ASSERT_FALSE(first->is_weak());
  ASSERT_TRUE(first_child->is_weak());
  ASSERT_TRUE(first_gchild->is_weak());

  // Reset and change so that first is now only implicitly strong.
  first->set_explicit(false);
  first_child->set_weak(false);

  // The whole chain should now be weak.
  MarkImplicitWeakParentsRecursively(parent);
  ASSERT_TRUE(parent->is_weak());
  ASSERT_TRUE(first->is_weak());
  ASSERT_TRUE(first_child->is_weak());
  ASSERT_TRUE(first_gchild->is_weak());
}

TEST_F(GraphProcessorTest, MarkWeakOwners) {
  Process* process = graph.CreateGraphForProcess(1);

  // Make only the ultimate owned node weak.
  Node* owner = process->CreateNode(kEmptyId, "owner", false);
  Node* owned = process->CreateNode(kEmptyId, "owned", false);
  Node* owned_2 = process->CreateNode(kEmptyId, "owned2", true);

  graph.AddNodeOwnershipEdge(owner, owned, 0);
  graph.AddNodeOwnershipEdge(owned, owned_2, 0);

  // Starting from leaf node should lead to everything being weak.
  MarkWeakOwnersAndChildrenRecursively(process->root());
  ASSERT_TRUE(owner->is_weak());
  ASSERT_TRUE(owned->is_weak());
  ASSERT_TRUE(owned_2->is_weak());
}

TEST_F(GraphProcessorTest, MarkWeakParent) {
  Process* process = graph.CreateGraphForProcess(1);
  Node* parent = process->CreateNode(kEmptyId, "parent", true);
  Node* child = process->CreateNode(kEmptyId, "parent/c", false);
  Node* child_2 = process->CreateNode(kEmptyId, "parent/c/c", false);

  // Starting from parent node should lead to everything being weak.
  MarkWeakOwnersAndChildrenRecursively(process->root());
  ASSERT_TRUE(parent->is_weak());
  ASSERT_TRUE(child->is_weak());
  ASSERT_TRUE(child_2->is_weak());
}

TEST_F(GraphProcessorTest, MarkWeakParentOwner) {
  Process* process = graph.CreateGraphForProcess(1);

  // Make only the parent node weak.
  Node* parent = process->CreateNode(kEmptyId, "parent", true);
  Node* child = process->CreateNode(kEmptyId, "parent/c", false);
  Node* child_2 = process->CreateNode(kEmptyId, "parent/c/c", false);
  Node* owner = process->CreateNode(kEmptyId, "owner", false);

  graph.AddNodeOwnershipEdge(owner, parent, 0);

  // Starting from parent node should lead to everything being weak.
  MarkWeakOwnersAndChildrenRecursively(process->root());
  ASSERT_TRUE(parent->is_weak());
  ASSERT_TRUE(child->is_weak());
  ASSERT_TRUE(child_2->is_weak());
  ASSERT_TRUE(owner->is_weak());
}

TEST_F(GraphProcessorTest, RemoveWeakNodesRecursively) {
  Process* process = graph.CreateGraphForProcess(1);

  // Make only the child node weak.
  Node* parent = process->CreateNode(kEmptyId, "parent", false);
  Node* child = process->CreateNode(kEmptyId, "parent/c", true);
  process->CreateNode(kEmptyId, "parent/c/c", false);
  Node* owned = process->CreateNode(kEmptyId, "parent/owned", false);

  graph.AddNodeOwnershipEdge(child, owned, 0);

  // Starting from parent node should lead child and child_2 being
  // removed and owned to have the edge from it removed.
  RemoveWeakNodesRecursively(parent);

  ASSERT_EQ(parent->children()->size(), 1ul);
  ASSERT_EQ(parent->children()->begin()->second, owned);

  ASSERT_TRUE(owned->owned_by_edges()->empty());
}

TEST_F(GraphProcessorTest, RemoveWeakNodesRecursivelyBetweenGraphs) {
  Process* f_process = graph.CreateGraphForProcess(1);
  Process* s_process = graph.CreateGraphForProcess(2);

  // Make only the child node weak.
  Node* child = f_process->CreateNode(kEmptyId, "c", true);
  f_process->CreateNode(kEmptyId, "c/c", false);
  Node* owned = s_process->CreateNode(kEmptyId, "owned", false);

  graph.AddNodeOwnershipEdge(child, owned, 0);

  // Starting from root node should lead child and child_2 being
  // removed.
  RemoveWeakNodesRecursively(f_process->root());

  ASSERT_EQ(f_process->root()->children()->size(), 0ul);
  ASSERT_EQ(s_process->root()->children()->size(), 1ul);

  // This should be false until our next pass.
  ASSERT_FALSE(owned->owned_by_edges()->empty());

  RemoveWeakNodesRecursively(s_process->root());

  // We should now have cleaned up the owned node's edges.
  ASSERT_TRUE(owned->owned_by_edges()->empty());
}

TEST_F(GraphProcessorTest, AssignTracingOverhead) {
  Process* process = graph.CreateGraphForProcess(1);

  // Now add an allocator node.
  process->CreateNode(kEmptyId, "malloc", false);

  // If the tracing node does not exist, this should do nothing.
  AssignTracingOverhead("malloc", &graph, process);
  ASSERT_TRUE(process->root()->GetChild("malloc")->children()->empty());

  // Now add a tracing node.
  process->CreateNode(kEmptyId, "tracing", false);

  // This should now add a node with the allocator.
  AssignTracingOverhead("malloc", &graph, process);
  ASSERT_NE(process->FindNode("malloc/allocated_objects/tracing_overhead"),
            nullptr);
}

TEST_F(GraphProcessorTest, AggregateNumericWithNameForNode) {
  Process* process = graph.CreateGraphForProcess(1);

  Node* c1 = process->CreateNode(kEmptyId, "c1", false);
  Node* c2 = process->CreateNode(kEmptyId, "c2", false);
  Node* c3 = process->CreateNode(kEmptyId, "c3", false);

  c1->AddEntry("random_numeric", Node::Entry::ScalarUnits::kBytes, 100);
  c2->AddEntry("random_numeric", Node::Entry::ScalarUnits::kBytes, 256);
  c3->AddEntry("other_numeric", Node::Entry::ScalarUnits::kBytes, 1000);

  Node* root = process->root();
  Node::Entry entry = AggregateNumericWithNameForNode(root, "random_numeric");
  ASSERT_EQ(entry.value_uint64, 356ul);
  ASSERT_EQ(entry.units, Node::Entry::ScalarUnits::kBytes);
}

TEST_F(GraphProcessorTest, AggregateNumericsRecursively) {
  Process* process = graph.CreateGraphForProcess(1);

  Node* c1 = process->CreateNode(kEmptyId, "c1", false);
  Node* c2 = process->CreateNode(kEmptyId, "c2", false);
  Node* c2_c1 = process->CreateNode(kEmptyId, "c2/c1", false);
  Node* c2_c2 = process->CreateNode(kEmptyId, "c2/c2", false);
  Node* c3_c1 = process->CreateNode(kEmptyId, "c3/c1", false);
  Node* c3_c2 = process->CreateNode(kEmptyId, "c3/c2", false);

  // If an entry already exists in the parent, the child should not
  // ovewrite it. If nothing exists, then the child can aggregrate.
  c1->AddEntry("random_numeric", Node::Entry::ScalarUnits::kBytes, 100);
  c2->AddEntry("random_numeric", Node::Entry::ScalarUnits::kBytes, 256);
  c2_c1->AddEntry("random_numeric", Node::Entry::ScalarUnits::kBytes, 256);
  c2_c2->AddEntry("random_numeric", Node::Entry::ScalarUnits::kBytes, 256);
  c3_c1->AddEntry("random_numeric", Node::Entry::ScalarUnits::kBytes, 10);
  c3_c2->AddEntry("random_numeric", Node::Entry::ScalarUnits::kBytes, 10);

  Node* root = process->root();
  AggregateNumericsRecursively(root);
  ASSERT_EQ(root->entries()->size(), 1ul);

  auto entry = root->entries()->begin()->second;
  ASSERT_EQ(entry.value_uint64, 376ul);
  ASSERT_EQ(entry.units, Node::Entry::ScalarUnits::kBytes);
}

TEST_F(GraphProcessorTest, AggregateSizeForDescendantNode) {
  Process* process = graph.CreateGraphForProcess(1);

  Node* c1 = process->CreateNode(kEmptyId, "c1", false);
  Node* c2 = process->CreateNode(kEmptyId, "c2", false);
  Node* c2_c1 = process->CreateNode(kEmptyId, "c2/c1", false);
  Node* c2_c2 = process->CreateNode(kEmptyId, "c2/c2", false);
  Node* c3_c1 = process->CreateNode(kEmptyId, "c3/c1", false);
  Node* c3_c2 = process->CreateNode(kEmptyId, "c3/c2", false);

  c1->AddEntry("size", Node::Entry::ScalarUnits::kBytes, 100);
  c2_c1->AddEntry("size", Node::Entry::ScalarUnits::kBytes, 256);
  c2_c2->AddEntry("size", Node::Entry::ScalarUnits::kBytes, 256);
  c3_c1->AddEntry("size", Node::Entry::ScalarUnits::kBytes, 10);
  c3_c2->AddEntry("size", Node::Entry::ScalarUnits::kBytes, 10);

  graph.AddNodeOwnershipEdge(c2_c2, c3_c2, 0);

  // Aggregating root should give size of (100 + 256 + 10 * 2) = 376.
  // |c2_c2| is not counted because it is owns by |c3_c2|.
  Node* root = process->root();
  ASSERT_EQ(376ul, *AggregateSizeForDescendantNode(root, root));

  // Aggregating c2 should give size of (256 * 2) = 512. |c2_c2| is counted
  // because |c3_c2| is not a child of |c2|.
  ASSERT_EQ(512ul, *AggregateSizeForDescendantNode(c2, c2));
}

TEST_F(GraphProcessorTest, CalculateSizeForNode) {
  Process* process = graph.CreateGraphForProcess(1);

  Node* c1 = process->CreateNode(kEmptyId, "c1", false);
  Node* c2 = process->CreateNode(kEmptyId, "c2", false);
  Node* c2_c1 = process->CreateNode(kEmptyId, "c2/c1", false);
  Node* c2_c2 = process->CreateNode(kEmptyId, "c2/c2", false);
  Node* c3 = process->CreateNode(kEmptyId, "c3", false);
  Node* c3_c1 = process->CreateNode(kEmptyId, "c3/c1", false);
  Node* c3_c2 = process->CreateNode(kEmptyId, "c3/c2", false);

  c1->AddEntry("size", Node::Entry::ScalarUnits::kBytes, 600);
  c2_c1->AddEntry("size", Node::Entry::ScalarUnits::kBytes, 10);
  c2_c2->AddEntry("size", Node::Entry::ScalarUnits::kBytes, 10);
  c3->AddEntry("size", Node::Entry::ScalarUnits::kBytes, 600);
  c3_c1->AddEntry("size", Node::Entry::ScalarUnits::kBytes, 256);
  c3_c2->AddEntry("size", Node::Entry::ScalarUnits::kBytes, 256);

  graph.AddNodeOwnershipEdge(c2_c2, c3_c2, 0);

  // Compute size entry for |c2| since computations for |c2_c1| and |c2_c2|
  // are already complete.
  CalculateSizeForNode(c2);

  // Check that |c2| now has a size entry of 20 (sum of children).
  auto c2_entry = c2->entries()->begin()->second;
  ASSERT_EQ(c2_entry.value_uint64, 20ul);
  ASSERT_EQ(c2_entry.units, Node::Entry::ScalarUnits::kBytes);

  // Compute size entry for |c3_c2| which should not change in size.
  CalculateSizeForNode(c3_c2);

  // Check that |c3_c2| now has unchanged size.
  auto c3_c2_entry = c3_c2->entries()->begin()->second;
  ASSERT_EQ(c3_c2_entry.value_uint64, 256ul);
  ASSERT_EQ(c3_c2_entry.units, Node::Entry::ScalarUnits::kBytes);

  // Compute size entry for |c3| which should add an unspecified node.
  CalculateSizeForNode(c3);

  // Check that |c3| has unchanged size.
  auto c3_entry = c3->entries()->begin()->second;
  ASSERT_EQ(c3_entry.value_uint64, 600ul);
  ASSERT_EQ(c3_entry.units, Node::Entry::ScalarUnits::kBytes);

  // Check that the unspecified node is a child of |c3| and has size
  // 600 - 512 = 88.
  Node* c3_child = c3->children()->find("<unspecified>")->second;
  auto c3_child_entry = c3_child->entries()->begin()->second;
  ASSERT_EQ(c3_child_entry.value_uint64, 88ul);
  ASSERT_EQ(c3_child_entry.units, Node::Entry::ScalarUnits::kBytes);

  // Compute size entry for |root| which should aggregate children sizes.
  CalculateSizeForNode(process->root());

  // Check that |root| has been assigned a size of 600 + 10 + 600 = 1210.
  // Note that |c2_c2| is not counted because it ows |c3_c2| which is a
  // descendant of |root|.
  auto root_entry = process->root()->entries()->begin()->second;
  ASSERT_EQ(root_entry.value_uint64, 1210ul);
  ASSERT_EQ(root_entry.units, Node::Entry::ScalarUnits::kBytes);
}

TEST_F(GraphProcessorTest, CalculateNodeSubSizes) {
  Process* process_1 = graph.CreateGraphForProcess(1);
  Process* process_2 = graph.CreateGraphForProcess(2);

  Node* parent_1 = process_1->CreateNode(kEmptyId, "parent", false);
  Node* child_1 = process_1->CreateNode(kEmptyId, "parent/child", false);

  Node* parent_2 = process_2->CreateNode(kEmptyId, "parent", false);
  Node* child_2 = process_2->CreateNode(kEmptyId, "parent/child", false);

  graph.AddNodeOwnershipEdge(parent_1, parent_2, 0);

  process_1->root()->AddEntry("size", Node::Entry::ScalarUnits::kBytes, 4);
  parent_1->AddEntry("size", Node::Entry::ScalarUnits::kBytes, 4);
  child_1->AddEntry("size", Node::Entry::ScalarUnits::kBytes, 4);
  process_2->root()->AddEntry("size", Node::Entry::ScalarUnits::kBytes, 5);
  parent_2->AddEntry("size", Node::Entry::ScalarUnits::kBytes, 5);
  child_2->AddEntry("size", Node::Entry::ScalarUnits::kBytes, 5);

  // Each of these nodes should have owner/owned same as size itself.
  CalculateNodeSubSizes(child_1);
  ASSERT_EQ(child_1->not_owned_sub_size(), 4ul);
  ASSERT_EQ(child_1->not_owning_sub_size(), 4ul);
  CalculateNodeSubSizes(child_2);
  ASSERT_EQ(child_2->not_owned_sub_size(), 5ul);
  ASSERT_EQ(child_2->not_owning_sub_size(), 5ul);

  // These nodes should also have size of children.
  CalculateNodeSubSizes(parent_1);
  ASSERT_EQ(parent_1->not_owned_sub_size(), 4ul);
  ASSERT_EQ(parent_1->not_owning_sub_size(), 4ul);
  CalculateNodeSubSizes(parent_2);
  ASSERT_EQ(parent_2->not_owned_sub_size(), 5ul);
  ASSERT_EQ(parent_2->not_owning_sub_size(), 5ul);

  // These nodes should account for edge between parents.
  CalculateNodeSubSizes(process_1->root());
  ASSERT_EQ(process_1->root()->not_owned_sub_size(), 4ul);
  ASSERT_EQ(process_1->root()->not_owning_sub_size(), 0ul);
  CalculateNodeSubSizes(process_2->root());
  ASSERT_EQ(process_2->root()->not_owned_sub_size(), 1ul);
  ASSERT_EQ(process_2->root()->not_owning_sub_size(), 5ul);
}

TEST_F(GraphProcessorTest, CalculateNodeOwnershipCoefficient) {
  Process* process = graph.CreateGraphForProcess(1);

  Node* owned = process->CreateNode(kEmptyId, "owned", false);
  Node* owner_1 = process->CreateNode(kEmptyId, "owner1", false);
  Node* owner_2 = process->CreateNode(kEmptyId, "owner2", false);
  Node* owner_3 = process->CreateNode(kEmptyId, "owner3", false);
  Node* owner_4 = process->CreateNode(kEmptyId, "owner4", false);

  graph.AddNodeOwnershipEdge(owner_1, owned, 2);
  graph.AddNodeOwnershipEdge(owner_2, owned, 2);
  graph.AddNodeOwnershipEdge(owner_3, owned, 1);
  graph.AddNodeOwnershipEdge(owner_4, owned, 0);

  // Ensure the owned node has a size otherwise calculations will not happen.
  owned->AddEntry("size", Node::Entry::kBytes, 10);

  // Setup the owned/owning sub sizes.
  owned->add_not_owned_sub_size(10);
  owner_1->add_not_owning_sub_size(6);
  owner_2->add_not_owning_sub_size(7);
  owner_3->add_not_owning_sub_size(5);
  owner_4->add_not_owning_sub_size(8);

  // Perform the computation.
  CalculateNodeOwnershipCoefficient(owned);

  // Ensure that the coefficients are correct.
  ASSERT_DOUBLE_EQ(owned->owned_coefficient(), 2.0 / 10.0);
  ASSERT_DOUBLE_EQ(owner_1->owning_coefficient(), 3.0 / 6.0);
  ASSERT_DOUBLE_EQ(owner_2->owning_coefficient(), 4.0 / 7.0);
  ASSERT_DOUBLE_EQ(owner_3->owning_coefficient(), 0.0 / 5.0);
  ASSERT_DOUBLE_EQ(owner_4->owning_coefficient(), 1.0 / 8.0);
}

TEST_F(GraphProcessorTest, CalculateNodeCumulativeOwnershipCoefficient) {
  Process* process = graph.CreateGraphForProcess(1);

  Node* c1 = process->CreateNode(kEmptyId, "c1", false);
  Node* c1_c1 = process->CreateNode(kEmptyId, "c1/c1", false);
  Node* c1_c2 = process->CreateNode(kEmptyId, "c1/c2", false);
  Node* owned = process->CreateNode(kEmptyId, "owned", false);

  graph.AddNodeOwnershipEdge(c1_c2, owned, 2);

  // Ensure all nodes have sizes otherwise calculations will not happen.
  c1_c1->AddEntry("size", Node::Entry::kBytes, 10);
  c1_c2->AddEntry("size", Node::Entry::kBytes, 10);
  owned->AddEntry("size", Node::Entry::kBytes, 10);

  // Setup the owned/owning cummulative coefficients.
  c1->set_cumulative_owning_coefficient(0.123);
  c1->set_cumulative_owned_coefficient(0.456);
  owned->set_cumulative_owning_coefficient(0.789);
  owned->set_cumulative_owned_coefficient(0.987);

  // Set owning and owned for the children.
  c1_c1->set_owning_coefficient(0.654);
  c1_c1->set_owned_coefficient(0.321);
  c1_c2->set_owning_coefficient(0.135);
  c1_c2->set_owned_coefficient(0.246);

  // Perform the computation and check our answers.
  CalculateNodeCumulativeOwnershipCoefficient(c1_c1);
  ASSERT_DOUBLE_EQ(c1_c1->cumulative_owning_coefficient(), 0.123);
  ASSERT_DOUBLE_EQ(c1_c1->cumulative_owned_coefficient(), 0.456 * 0.321);

  CalculateNodeCumulativeOwnershipCoefficient(c1_c2);
  ASSERT_DOUBLE_EQ(c1_c2->cumulative_owning_coefficient(), 0.135 * 0.789);
  ASSERT_DOUBLE_EQ(c1_c2->cumulative_owned_coefficient(), 0.456 * 0.246);
}

TEST_F(GraphProcessorTest, CalculateNodeEffectiveSize) {
  Process* process = graph.CreateGraphForProcess(1);

  Node* c1 = process->CreateNode(kEmptyId, "c1", false);
  Node* c1_c1 = process->CreateNode(kEmptyId, "c1/c1", false);
  Node* c1_c2 = process->CreateNode(kEmptyId, "c1/c2", false);

  // Ensure all nodes have sizes otherwise calculations will not happen.
  c1->AddEntry("size", Node::Entry::kBytes, 200);
  c1_c1->AddEntry("size", Node::Entry::kBytes, 32);
  c1_c2->AddEntry("size", Node::Entry::kBytes, 20);

  // Setup the owned/owning cummulative coefficients.
  c1_c1->set_cumulative_owning_coefficient(0.123);
  c1_c1->set_cumulative_owned_coefficient(0.456);
  c1_c2->set_cumulative_owning_coefficient(0.789);
  c1_c2->set_cumulative_owned_coefficient(0.987);

  // Perform the computation and check our answers.
  CalculateNodeEffectiveSize(c1_c1);
  const Node::Entry& entry_c1_c1 =
      c1_c1->entries()->find("effective_size")->second;
  uint64_t expected_c1_c1 = static_cast<int>(0.123 * 0.456 * 32);
  ASSERT_EQ(entry_c1_c1.value_uint64, expected_c1_c1);

  CalculateNodeEffectiveSize(c1_c2);
  const Node::Entry& entry_c1_c2 =
      c1_c2->entries()->find("effective_size")->second;
  uint64_t expected_c1_c2 = static_cast<int>(0.789 * 0.987 * 20);
  ASSERT_EQ(entry_c1_c2.value_uint64, expected_c1_c2);

  CalculateNodeEffectiveSize(c1);
  const Node::Entry& entry_c1 = c1->entries()->find("effective_size")->second;
  ASSERT_EQ(entry_c1.value_uint64, expected_c1_c1 + expected_c1_c2);
}

}  // namespace trace_processor
}  // namespace perfetto
