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

#include "src/trace_processor/importers/proto/heap_graph_walker.h"

#include "perfetto/base/logging.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

class HeapGraphWalkerTestDelegate : public HeapGraphWalker::Delegate {
 public:
  ~HeapGraphWalkerTestDelegate() override = default;

  void MarkReachable(int64_t row) override { reachable_.emplace(row); }

  void SetRetained(int64_t row,
                   int64_t retained,
                   int64_t unique_retained) override {
    bool inserted;
    std::tie(std::ignore, inserted) = retained_.emplace(row, retained);
    PERFETTO_CHECK(inserted);
    std::tie(std::ignore, inserted) =
        unique_retained_.emplace(row, unique_retained);
    PERFETTO_CHECK(inserted);
  }

  bool Reachable(int64_t row) {
    return reachable_.find(row) != reachable_.end();
  }

  int64_t Retained(int64_t row) {
    auto it = retained_.find(row);
    PERFETTO_CHECK(it != retained_.end());
    return it->second;
  }

  int64_t UniqueRetained(int64_t row) {
    auto it = unique_retained_.find(row);
    PERFETTO_CHECK(it != unique_retained_.end());
    return it->second;
  }

 private:
  std::map<int64_t, int64_t> retained_;
  std::map<int64_t, int64_t> unique_retained_;
  std::set<int64_t> reachable_;
};

//     1     |
//    ^^     |
//   /  \    |
//   2   3   |
//   ^   ^   |
//    \ /    |
//     4     |
TEST(HeapGraphWalkerTest, Diamond) {
  HeapGraphWalkerTestDelegate delegate;
  HeapGraphWalker walker(&delegate);
  walker.AddNode(1, 1);
  walker.AddNode(2, 2);
  walker.AddNode(3, 3);
  walker.AddNode(4, 4);

  walker.AddEdge(2, 1);
  walker.AddEdge(3, 1);
  walker.AddEdge(4, 2);
  walker.AddEdge(4, 3);

  walker.MarkRoot(4);
  walker.CalculateRetained();

  EXPECT_EQ(delegate.Retained(1), 1);
  EXPECT_EQ(delegate.Retained(2), 3);
  EXPECT_EQ(delegate.Retained(3), 4);
  EXPECT_EQ(delegate.Retained(4), 10);

  EXPECT_EQ(delegate.UniqueRetained(1), 1);
  EXPECT_EQ(delegate.UniqueRetained(2), 2);
  EXPECT_EQ(delegate.UniqueRetained(3), 3);
  EXPECT_EQ(delegate.UniqueRetained(4), 10);
}

// 1     2  |
// ^     ^  |
// \    /   |
// 3<->4    |
TEST(HeapGraphWalkerTest, Loop) {
  HeapGraphWalkerTestDelegate delegate;
  HeapGraphWalker walker(&delegate);
  walker.AddNode(1, 1);
  walker.AddNode(2, 2);
  walker.AddNode(3, 3);
  walker.AddNode(4, 4);

  walker.AddEdge(3, 1);
  walker.AddEdge(3, 4);
  walker.AddEdge(4, 2);
  walker.AddEdge(4, 3);

  walker.MarkRoot(3);
  walker.CalculateRetained();

  EXPECT_EQ(delegate.Retained(1), 1);
  EXPECT_EQ(delegate.Retained(2), 2);
  EXPECT_EQ(delegate.Retained(3), 10);
  EXPECT_EQ(delegate.Retained(4), 10);

  EXPECT_EQ(delegate.UniqueRetained(1), 1);
  EXPECT_EQ(delegate.UniqueRetained(2), 2);
  EXPECT_EQ(delegate.UniqueRetained(3), 4);
  EXPECT_EQ(delegate.UniqueRetained(4), 6);
}

//    1     |
//    ^\    |
//   /  v   |
//   3<-2   |
TEST(HeapGraphWalkerTest, Triangle) {
  HeapGraphWalkerTestDelegate delegate;
  HeapGraphWalker walker(&delegate);
  walker.AddNode(1, 1);
  walker.AddNode(2, 2);
  walker.AddNode(3, 3);

  walker.AddEdge(1, 2);
  walker.AddEdge(2, 3);
  walker.AddEdge(3, 1);

  walker.MarkRoot(1);
  walker.CalculateRetained();

  EXPECT_EQ(delegate.Retained(1), 6);
  EXPECT_EQ(delegate.Retained(2), 6);
  EXPECT_EQ(delegate.Retained(3), 6);

  EXPECT_EQ(delegate.UniqueRetained(1), 1);
  EXPECT_EQ(delegate.UniqueRetained(2), 2);
  EXPECT_EQ(delegate.UniqueRetained(3), 3);
}

// 1      |
// ^      |
// |      |
// 2  4   |
// ^  ^   |
// |  |   |
// 3  5   |
TEST(HeapGraphWalkerTest, Disconnected) {
  HeapGraphWalkerTestDelegate delegate;
  HeapGraphWalker walker(&delegate);
  walker.AddNode(1, 1);
  walker.AddNode(2, 2);
  walker.AddNode(3, 3);
  walker.AddNode(4, 4);
  walker.AddNode(5, 5);

  walker.AddEdge(2, 1);
  walker.AddEdge(3, 2);
  walker.AddEdge(5, 4);

  walker.MarkRoot(3);
  walker.CalculateRetained();

  EXPECT_EQ(delegate.Retained(1), 1);
  EXPECT_EQ(delegate.Retained(2), 3);
  EXPECT_EQ(delegate.Retained(3), 6);

  EXPECT_EQ(delegate.UniqueRetained(1), 1);
  EXPECT_EQ(delegate.UniqueRetained(2), 3);
  EXPECT_EQ(delegate.UniqueRetained(3), 6);

  EXPECT_TRUE(delegate.Reachable(1));
  EXPECT_TRUE(delegate.Reachable(2));
  EXPECT_TRUE(delegate.Reachable(3));
  EXPECT_FALSE(delegate.Reachable(4));
  EXPECT_FALSE(delegate.Reachable(5));
}

//      1      |
//      ^^     |
//     / \     |
//    2   3    |
//    ^  ^^    |
//    |/  |    |
//    4   5    |
//    ^   ^    |
//    \  /     |
//      6      |
TEST(HeapGraphWalkerTest, Complex) {
  HeapGraphWalkerTestDelegate delegate;
  HeapGraphWalker walker(&delegate);
  walker.AddNode(1, 1);
  walker.AddNode(2, 2);
  walker.AddNode(3, 3);
  walker.AddNode(4, 4);
  walker.AddNode(5, 5);
  walker.AddNode(6, 6);

  walker.AddEdge(2, 1);
  walker.AddEdge(3, 1);
  walker.AddEdge(4, 2);
  walker.AddEdge(4, 3);
  walker.AddEdge(5, 3);
  walker.AddEdge(6, 4);
  walker.AddEdge(6, 5);

  walker.MarkRoot(6);
  walker.CalculateRetained();

  EXPECT_EQ(delegate.Retained(1), 1);
  EXPECT_EQ(delegate.Retained(2), 3);
  EXPECT_EQ(delegate.Retained(3), 4);
  EXPECT_EQ(delegate.Retained(4), 10);
  EXPECT_EQ(delegate.Retained(5), 9);
  EXPECT_EQ(delegate.Retained(6), 21);

  EXPECT_EQ(delegate.UniqueRetained(1), 1);
  EXPECT_EQ(delegate.UniqueRetained(2), 2);
  EXPECT_EQ(delegate.UniqueRetained(3), 3);
  EXPECT_EQ(delegate.UniqueRetained(4), 6);
  EXPECT_EQ(delegate.UniqueRetained(5), 5);
  EXPECT_EQ(delegate.UniqueRetained(6), 21);
}

//    1      |
//    ^^     |
//   /  \    |
//  2<-> 3   |
//  ^        |
//  |        |
//  4        |
TEST(HeapGraphWalkerTest, SharedInComponent) {
  HeapGraphWalkerTestDelegate delegate;
  HeapGraphWalker walker(&delegate);
  walker.AddNode(1, 1);
  walker.AddNode(2, 2);
  walker.AddNode(3, 3);
  walker.AddNode(4, 4);

  walker.AddEdge(2, 1);
  walker.AddEdge(2, 3);
  walker.AddEdge(3, 1);
  walker.AddEdge(3, 2);
  walker.AddEdge(4, 2);

  walker.MarkRoot(4);
  walker.CalculateRetained();

  EXPECT_EQ(delegate.Retained(1), 1);
  EXPECT_EQ(delegate.Retained(2), 6);
  EXPECT_EQ(delegate.Retained(3), 6);
  EXPECT_EQ(delegate.Retained(4), 10);

  EXPECT_EQ(delegate.UniqueRetained(1), 1);
  EXPECT_EQ(delegate.UniqueRetained(2), 2);
  EXPECT_EQ(delegate.UniqueRetained(3), 3);
  EXPECT_EQ(delegate.UniqueRetained(4), 10);
}

// 1 <- 2   |
// ^    ^   |
// |    |   |
// 3<-> 4   |
TEST(HeapGraphWalkerTest, TwoPaths) {
  HeapGraphWalkerTestDelegate delegate;
  HeapGraphWalker walker(&delegate);
  walker.AddNode(1, 1);
  walker.AddNode(2, 2);
  walker.AddNode(3, 3);
  walker.AddNode(4, 4);

  walker.AddEdge(2, 1);
  walker.AddEdge(3, 1);
  walker.AddEdge(3, 4);
  walker.AddEdge(4, 2);
  walker.AddEdge(4, 3);

  walker.MarkRoot(4);
  walker.CalculateRetained();

  EXPECT_EQ(delegate.Retained(1), 1);
  EXPECT_EQ(delegate.Retained(2), 3);
  EXPECT_EQ(delegate.Retained(3), 10);
  EXPECT_EQ(delegate.Retained(4), 10);

  EXPECT_EQ(delegate.UniqueRetained(1), 1);
  EXPECT_EQ(delegate.UniqueRetained(2), 2);
  EXPECT_EQ(delegate.UniqueRetained(3), 3);
  EXPECT_EQ(delegate.UniqueRetained(4), 6);
}

//    1     |
//   ^^     |
//  /  \    |
// 2    3   |
TEST(HeapGraphWalkerTest, Diverge) {
  HeapGraphWalkerTestDelegate delegate;
  HeapGraphWalker walker(&delegate);
  walker.AddNode(1, 1);
  walker.AddNode(2, 2);
  walker.AddNode(3, 3);

  walker.AddEdge(2, 1);
  walker.AddEdge(3, 1);

  walker.MarkRoot(2);
  walker.MarkRoot(3);
  walker.CalculateRetained();

  EXPECT_EQ(delegate.Retained(1), 1);
  EXPECT_EQ(delegate.Retained(2), 3);
  EXPECT_EQ(delegate.Retained(3), 4);

  EXPECT_EQ(delegate.UniqueRetained(1), 1);
  EXPECT_EQ(delegate.UniqueRetained(2), 2);
  EXPECT_EQ(delegate.UniqueRetained(3), 3);
}

//    1            |
//   ^^            |
//  /  \           |
// 2    3 (dead)   |
TEST(HeapGraphWalkerTest, Dead) {
  HeapGraphWalkerTestDelegate delegate;
  HeapGraphWalker walker(&delegate);
  walker.AddNode(1, 1);
  walker.AddNode(2, 2);
  walker.AddNode(3, 3);

  walker.AddEdge(2, 1);
  walker.AddEdge(3, 1);

  walker.MarkRoot(2);
  walker.CalculateRetained();

  EXPECT_EQ(delegate.Retained(1), 1);
  EXPECT_EQ(delegate.Retained(2), 3);

  EXPECT_EQ(delegate.UniqueRetained(1), 1);
  EXPECT_EQ(delegate.UniqueRetained(2), 3);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
